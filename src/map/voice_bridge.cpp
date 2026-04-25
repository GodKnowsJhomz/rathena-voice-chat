#include "voice_bridge.hpp"
#include "../common/timer.hpp"

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   using socket_t = SOCKET;
   static constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
   inline void close_socket(socket_t s) { closesocket(s); }
   inline bool wsa_init() { WSADATA w{}; return WSAStartup(MAKEWORD(2,2),&w)==0; }
   inline void wsa_cleanup() { WSACleanup(); }
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
   using socket_t = int;
   static constexpr socket_t INVALID_SOCK = -1;
   inline void close_socket(socket_t s) { ::close(s); }
   inline bool wsa_init()    { return true; }
   inline void wsa_cleanup() {}
#endif

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace {
	socket_t    g_sock       = INVALID_SOCK;
	sockaddr_in g_voice_addr{};
	bool        g_ready      = false;

	bool voice_bridge_send_text(const std::string& s) {
		if (!g_ready || g_sock == INVALID_SOCK)
			return false;

		int rc = sendto(
			g_sock,
			s.c_str(),
			static_cast<int>(s.size()),
			0,
			reinterpret_cast<const sockaddr*>(&g_voice_addr),
			sizeof(g_voice_addr)
		);
		return rc == static_cast<int>(s.size());
	}

	// Last-sent position cache — keyed by char_id.
	// Periodic tick sends only when position changed OR keepalive interval elapsed.
	// Keepalive must be shorter than server's POSITION_STALE_MS (2000 ms) so
	// standing players don't lose proximity voice after 2 seconds.
	static constexpr t_tick KEEPALIVE_MS = 1500;

	// Auth advisory renewal — map server re-asserts each player's identity
	// every N ms so the voice server's advisory TTL (120 s) never lapses for
	// legitimately-logged-in characters.
	static constexpr t_tick AUTH_ADVISORY_RENEW_MS = 30000;

	struct LastPos {
		short  x        = -1;
		short  y        = -1;
		t_tick sent_at  = 0;   // gettick() of last UDP send
		t_tick auth_at  = 0;   // gettick() of last auth_advisory send
		char   map[MAP_NAME_LENGTH_EXT] = {};
	};
	std::unordered_map<int, LastPos> g_last_pos;

	bool should_send(map_session_data* sd, const LastPos& lp) {
		// Always send if position/map changed
		if (sd->x != lp.x || sd->y != lp.y) return true;
		const struct map_data* md = map_getmapdata(sd->m);
		const char* mapname = (md != nullptr) ? md->name : "";
		if (strncmp(mapname, lp.map, MAP_NAME_LENGTH_EXT) != 0) return true;
		// Keepalive: send even if standing still so server position stays fresh
		return (gettick() - lp.sent_at) >= KEEPALIVE_MS;
	}

	void update_last_pos(map_session_data* sd, LastPos& lp) {
		lp.x       = sd->x;
		lp.y       = sd->y;
		lp.sent_at = gettick();
		const struct map_data* md = map_getmapdata(sd->m);
		const char* mapname = (md != nullptr) ? md->name : "";
		strncpy(lp.map, mapname, MAP_NAME_LENGTH_EXT - 1);
		lp.map[MAP_NAME_LENGTH_EXT - 1] = '\0';
	}

}

static void voice_bridge_send_pos_common(map_session_data* sd);
static void voice_bridge_send_auth_advisory_internal(map_session_data* sd);

// ── Periodic position ping ────────────────────────────────────────────────────
static int voice_bridge_ping_player(map_session_data* sd, va_list) {
	if (!sd || sd->status.char_id <= 0)
		return 0;

	auto& lp = g_last_pos[sd->status.char_id];
	if (should_send(sd, lp)) {
		voice_bridge_send_pos_common(sd);
		update_last_pos(sd, lp);
	}

	// Auth advisory renewal — keeps voice server's advisory entry fresh
	if (lp.auth_at == 0 || (gettick() - lp.auth_at) >= AUTH_ADVISORY_RENEW_MS) {
		voice_bridge_send_auth_advisory_internal(sd);
		lp.auth_at = gettick();
	}
	return 0;
}

static TIMER_FUNC(voice_bridge_tick) {
	map_foreachpc(voice_bridge_ping_player);
	return 0;
}

void voice_bridge_init() {
	if (g_ready)
		return;

	if (!wsa_init())
		return;

	g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (g_sock == INVALID_SOCK) {
		wsa_cleanup();
		return;
	}

	std::memset(&g_voice_addr, 0, sizeof(g_voice_addr));
	g_voice_addr.sin_family = AF_INET;
	g_voice_addr.sin_port = htons(7001);
	inet_pton(AF_INET, "127.0.0.1", &g_voice_addr.sin_addr);

	g_ready = true;
	voice_bridge_send_guild_war_state(is_agit_start());

	// Send position for all online players every 1 second
	add_timer_interval(gettick() + 1000, voice_bridge_tick, 0, 0, 1000);
}

void voice_bridge_final() {
	if (g_sock != INVALID_SOCK) {
		close_socket(g_sock);
		g_sock = INVALID_SOCK;
	}
	if (g_ready) {
		wsa_cleanup();
		g_ready = false;
	}
}

static void voice_bridge_send_pos_common(map_session_data* sd) {
	if (!g_ready || !sd)
		return;

	const struct map_data* md = map_getmapdata(sd->m);
	const char* mapname = (md != nullptr) ? md->name : "";
	const int war_map = map_flag_gvg(sd->m) ? 1 : 0;

	char buf[256];
	std::snprintf(
		buf, sizeof(buf),
		"{\"type\":\"map_pos\",\"char_id\":%d,\"map\":\"%s\",\"x\":%d,\"y\":%d,\"level\":%d,\"job\":%d,\"group_id\":%d,\"war_map\":%s}",
		sd->status.char_id,
		mapname,
		sd->x,
		sd->y,
		(int)sd->status.base_level,
		(int)sd->status.class_,
		(int)sd->group_id,
		war_map ? "true" : "false"
	);

	voice_bridge_send_text(buf);
}

// Send `auth_advisory` so the voice server can verify WS auth from the DLL
// matches the authoritative (char_id, account_id) pair from the map server.
// login_id1 is also forwarded as an opaque RO session token for future stricter
// verification — currently informational only.
static void voice_bridge_send_auth_advisory_internal(map_session_data* sd) {
	if (!g_ready || !sd) return;

	char buf[160];
	std::snprintf(
		buf, sizeof(buf),
		"{\"type\":\"auth_advisory\",\"char_id\":%d,\"account_id\":%d,\"login_id1\":%u}",
		sd->status.char_id,
		sd->status.account_id,
		(unsigned int)sd->login_id1
	);
	voice_bridge_send_text(buf);
}

void voice_bridge_send_join(map_session_data* sd) {
	if (!sd) return;
	// Advisory BEFORE position so the voice server accepts subsequent WS auth
	voice_bridge_send_auth_advisory_internal(sd);
	voice_bridge_send_pos_common(sd);
	auto& lp = g_last_pos[sd->status.char_id];
	update_last_pos(sd, lp);
	lp.auth_at = gettick();
}

void voice_bridge_send_map_pos(map_session_data* sd) {
	if (!sd) return;
	voice_bridge_send_pos_common(sd);
	update_last_pos(sd, g_last_pos[sd->status.char_id]);
}

void voice_bridge_send_room_join(map_session_data* sd, int room_id) {
	if (!g_ready || !sd || room_id <= 0)
		return;

	char buf[128];
	std::snprintf(buf, sizeof(buf),
		"{\"type\":\"chat_join\",\"char_id\":%d,\"room_id\":%d}",
		sd->status.char_id, room_id);

	voice_bridge_send_text(buf);
}

void voice_bridge_send_room_leave(map_session_data* sd) {
	if (!g_ready || !sd)
		return;

	char buf[64];
	std::snprintf(buf, sizeof(buf),
		"{\"type\":\"chat_leave\",\"char_id\":%d}",
		sd->status.char_id);

	voice_bridge_send_text(buf);
}

void voice_bridge_send_reload_config() {
	voice_bridge_send_text("{\"type\":\"reload_config\"}");
}

void voice_bridge_send_guild_war_state(bool active) {
	char buf[64];
	std::snprintf(buf, sizeof(buf),
		"{\"type\":\"guild_war_state\",\"active\":%s}",
		active ? "true" : "false");
	voice_bridge_send_text(buf);
}

void voice_bridge_send_leave(map_session_data* sd) {
	if (!g_ready || !sd)
		return;

	g_last_pos.erase(sd->status.char_id);  // clean up cache on logout

	char buf[128];

	// Tell voice server this char_id is no longer logged in — future WS auth
	// with this char_id will be rejected until a new auth_advisory arrives.
	std::snprintf(buf, sizeof(buf),
		"{\"type\":\"auth_revoke\",\"char_id\":%d}",
		sd->status.char_id);
	voice_bridge_send_text(buf);

	std::snprintf(
		buf, sizeof(buf),
		"{\"type\":\"map_leave\",\"char_id\":%d}",
		sd->status.char_id
	);

	voice_bridge_send_text(buf);
}
