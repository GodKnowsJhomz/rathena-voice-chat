#include <App.h>
#include "whisper_manager.hpp"
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <string>
#include <string_view>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <atomic>
#include <functional>
#include <chrono>
#ifdef _WIN32
#  include <windows.h>
#endif
#include <mysql.h>

static inline uint32_t tick_ms() {
#ifdef _WIN32
    return static_cast<uint32_t>(GetTickCount());
#else
    static const auto _start = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - _start).count());
#endif
}
#include "config.hpp"

using json = nlohmann::json;

// ── Config ────────────────────────────────────────────────────────────────────
struct SrvConfig {
    std::string voice_ip             = "0.0.0.0";
    int         voice_port           = 7000;
    float       proximity_full_range = 1.0f;
    float       proximity_max_range  = 14.0f;
    int         proximity_update_ms  = 50;
    int         whisper_timeout      = 300;
    int         db_refresh_s         = 5;
    int         log_level            = 2;
    int         max_targets_normal   = 64;
    int         max_targets_group    = 128;
    int         max_targets_room     = 64;
    int         audio_backpressure_kb = 64;
    bool        war_mode_enabled     = true;
    bool        war_allow_whisper    = true;

    std::string db_host       = "127.0.0.1";
    int         db_port       = 3306;
    std::string db_user       = "ragnarok";
    std::string db_pass       = "";
    std::string db_name       = "ragnarok";
    std::string db_char_table = "char";
} g_cfg;

// Hard cutoff guard for practical "14 cells exact" behavior.
// Why: position and audio arrive on separate packets, so at the edge of range
// the server can still momentarily use a stale position from ~1 tick earlier.
// Using a small guard keeps practical hearing closer to what the player sees.
static constexpr float    PROXIMITY_EDGE_GUARD_CELLS = 0.75f;
static constexpr uint32_t POSITION_STALE_MS          = 2000; // ping every 1s, allow 2s grace

static int parse_kv_file(const char* path,
                         std::function<bool(const std::string&, const std::string&)> handler)
{
    std::ifstream f(path);
    if (!f.is_open()) return -1;

    auto trim = [](std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        size_t b = s.find_last_not_of(" \t\r\n");
        s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
    };

    int matched = 0;
    std::string line;
    while (std::getline(f, line)) {
        auto cm = line.find("//");
        if (cm != std::string::npos) line = line.substr(0, cm);
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        trim(key); trim(val);
        if (key.empty()) continue;
        try {
            if (handler(key, val)) matched++;
        } catch (...) {
            printf("[Config] bad value: %s = %s\n", key.c_str(), val.c_str());
        }
    }
    return matched;
}

static void load_voice_conf(const char* path) {
    std::ifstream test(path);
    if (!test.is_open()) {
        printf("[Config] ERROR: cannot open %s\n", path);
        printf("[Config] Please create config file manually.\n");
        std::exit(1);
    }

    int n = parse_kv_file(path, [](const std::string& key, const std::string& val) -> bool {
        if      (key == "voice_ip") { if (!val.empty()) g_cfg.voice_ip = val; return true; }
        else if (key == "voice_port") { if (!val.empty()) g_cfg.voice_port = std::stoi(val); return true; }
        else if (key == "proximity_full_range" || key == "voice_proximity_full_range") {
            if (!val.empty()) g_cfg.proximity_full_range = std::stof(val); return true;
        }
        else if (key == "proximity_max_range" || key == "voice_proximity_max_range") {
            if (!val.empty()) g_cfg.proximity_max_range = std::stof(val); return true;
        }
        else if (key == "proximity_update_ms" || key == "voice_proximity_update_ms") {
            if (!val.empty()) g_cfg.proximity_update_ms = std::stoi(val); return true;
        }
        else if (key == "whisper_timeout" || key == "voice_whisper_timeout") {
            if (!val.empty()) g_cfg.whisper_timeout = std::stoi(val); return true;
        }
        else if (key == "db_refresh_s" || key == "voice_db_refresh_s") {
            if (!val.empty()) g_cfg.db_refresh_s = std::stoi(val); return true;
        }
        else if (key == "log_level" || key == "voice_log_level") {
            if (!val.empty()) g_cfg.log_level = std::stoi(val); return true;
        }
        else if (key == "voice_max_targets_normal") {
            if (!val.empty()) g_cfg.max_targets_normal = std::stoi(val); return true;
        }
        else if (key == "voice_max_targets_group") {
            if (!val.empty()) g_cfg.max_targets_group = std::stoi(val); return true;
        }
        else if (key == "voice_max_targets_room") {
            if (!val.empty()) g_cfg.max_targets_room = std::stoi(val); return true;
        }
        else if (key == "voice_audio_backpressure_kb") {
            if (!val.empty()) g_cfg.audio_backpressure_kb = std::stoi(val); return true;
        }
        else if (key == "voice_war_mode_enabled") {
            if (!val.empty()) g_cfg.war_mode_enabled = (std::stoi(val) != 0); return true;
        }
        else if (key == "voice_war_allow_whisper") {
            if (!val.empty()) g_cfg.war_allow_whisper = (std::stoi(val) != 0); return true;
        }
        return false;
    });

    printf("[Config] loaded %s (%d keys)\n", path, n);
}

static void load_inter_conf(const char* path) {
    int n = parse_kv_file(path, [](const std::string& key, const std::string& val) -> bool {
        if      (key == "voice_server_ip")   { if (!val.empty()) { g_cfg.db_host = val;            return true; } }
        else if (key == "voice_server_port") { if (!val.empty()) { g_cfg.db_port = std::stoi(val); return true; } }
        else if (key == "voice_server_id")   { if (!val.empty()) { g_cfg.db_user = val;            return true; } }
        else if (key == "voice_server_pw")   { g_cfg.db_pass = val;                                 return true; }
        else if (key == "voice_server_db")   { if (!val.empty()) { g_cfg.db_name = val;            return true; } }
        else if (key == "char_db")           { if (!val.empty()) { g_cfg.db_char_table = val;      return true; } }
        return false;
    });

    if (n < 0) printf("[Config] WARNING: cannot open %s — DB defaults will be used\n", path);
    else       printf("[Config] loaded %s (%d keys)\n", path, n);
}

namespace con {
    static void init() {
#ifdef _WIN32
        HANDLE hcon = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(hcon, &mode))
            SetConsoleMode(hcon, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        SetConsoleOutputCP(CP_UTF8);
#endif
    }
    static const char* RESET  = "\033[0m";
    static const char* GREEN  = "\033[32m";
    static const char* RED    = "\033[31m";
    static const char* YELLOW = "\033[33m";
    static const char* CYAN   = "\033[36m";
    static const char* WHITE  = "\033[97m";
    static const char* GRAY   = "\033[90m";
}

#undef LOG_ERROR
#undef LOG_INFO
#undef LOG_DEBUG

#define LOG_ERROR(fmt, ...)   do { if(g_cfg.log_level>=1) printf("%s[Error]%s: "   fmt "\n", con::RED,    con::RESET, ##__VA_ARGS__); } while(0)
#define LOG_WARNING(fmt, ...) do { if(g_cfg.log_level>=2) printf("%s[Warning]%s: " fmt "\n", con::YELLOW, con::RESET, ##__VA_ARGS__); } while(0)
#define LOG_INFO(fmt, ...)    do { if(g_cfg.log_level>=2) printf("%s[Info]%s: "    fmt "\n", con::WHITE,  con::RESET, ##__VA_ARGS__); } while(0)
#define LOG_STATUS(fmt, ...)  do { if(g_cfg.log_level>=2) printf("%s[Status]%s: "  fmt "\n", con::CYAN,   con::RESET, ##__VA_ARGS__); } while(0)
#define LOG_NOTICE(fmt, ...)  do { if(g_cfg.log_level>=2) printf("%s[Notice]%s: "  fmt "\n", con::GREEN,  con::RESET, ##__VA_ARGS__); } while(0)
#define LOG_DEBUG(fmt, ...)   do { if(g_cfg.log_level>=3) printf("%s[Debug]%s: "   fmt "\n", con::GRAY,   con::RESET, ##__VA_ARGS__); } while(0)

// ── Database ──────────────────────────────────────────────────────────────────
static MYSQL* g_db = nullptr;
static std::mutex g_db_mtx;

static bool db_connect() {
    if (g_db) {
        mysql_close(g_db);
        g_db = nullptr;
    }
    g_db = mysql_init(nullptr);
    if (!g_db) {
        LOG_ERROR("mysql_init failed");
        return false;
    }

    unsigned int timeout = 5;
    mysql_options(g_db, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    bool reconnect = 1;
    mysql_options(g_db, MYSQL_OPT_RECONNECT, &reconnect);
#if !defined(MARIADB_BASE_VERSION) && !defined(MARIADB_VERSION_ID) && MYSQL_VERSION_ID >= 50710
    {
        unsigned int md = SSL_MODE_DISABLED;
        mysql_options(g_db, MYSQL_OPT_SSL_MODE, &md);
    }
#endif

    if (!mysql_real_connect(g_db,
            g_cfg.db_host.c_str(),
            g_cfg.db_user.c_str(),
            g_cfg.db_pass.c_str(),
            g_cfg.db_name.c_str(),
            static_cast<unsigned int>(g_cfg.db_port),
            nullptr, 0))
    {
        LOG_ERROR("DB connect failed: %s", mysql_error(g_db));
        mysql_close(g_db);
        g_db = nullptr;
        return false;
    }

    if (mysql_set_character_set(g_db, "utf8mb4") != 0) {
        LOG_WARNING("mysql_set_character_set(utf8mb4) failed: %s", mysql_error(g_db));
    }

    LOG_NOTICE("DB connected  %s@%s:%d/%s",
               g_cfg.db_user.c_str(), g_cfg.db_host.c_str(),
               g_cfg.db_port, g_cfg.db_name.c_str());
    return true;
}

struct CharInfo {
    std::string char_name;
    int party_id = 0;
    int guild_id = 0;
    bool ok      = false;
};

static CharInfo db_get_char_info(int char_id) {
    std::lock_guard<std::mutex> lock(g_db_mtx);
    if (!g_db) return {};

    if (mysql_ping(g_db) != 0) {
        LOG_WARNING("DB ping failed, reconnecting...");
        if (!db_connect()) return {};
    }

    std::string query = "SELECT `name`,`party_id`,`guild_id` FROM `"
                      + g_cfg.db_char_table
                      + "` WHERE `char_id`=" + std::to_string(char_id) + " LIMIT 1";

    if (mysql_query(g_db, query.c_str()) != 0) {
        LOG_ERROR("DB query error: %s", mysql_error(g_db));
        if (!db_connect()) return {};
        if (mysql_query(g_db, query) != 0) return {};
    }

    MYSQL_RES* res = mysql_store_result(g_db);
    if (!res) return {};

    CharInfo info;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (row) {
        info.char_name = row[0] ? row[0] : "";
        info.party_id  = row[1] ? std::atoi(row[1]) : 0;
        info.guild_id  = row[2] ? std::atoi(row[2]) : 0;
        info.ok        = true;
    }
    mysql_free_result(res);
    return info;
}

// Lookup char_id by exact character name (case-insensitive depends on DB collation)
static int db_lookup_char_by_name(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_db_mtx);
    if (!g_db) return 0;
    if (mysql_ping(g_db) != 0) {
        LOG_WARNING("DB ping failed, reconnecting...");
        if (!db_connect()) return 0;
    }
    char escaped[128] = {};
    mysql_real_escape_string(g_db, escaped, name.c_str(),
                             (unsigned long)std::min(name.size(), sizeof(escaped) / 2 - 1));
    std::string query = "SELECT `char_id` FROM `"
                      + g_cfg.db_char_table
                      + "` WHERE `name`='" + escaped + "' LIMIT 1";
    if (mysql_query(g_db, query.c_str()) != 0) return 0;
    MYSQL_RES* res = mysql_store_result(g_db);
    if (!res) return 0;
    int char_id = 0;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (row && row[0]) char_id = std::atoi(row[0]);
    mysql_free_result(res);
    return char_id;
}

struct ClientSession {
    uWS::WebSocket<false, true, ClientSession>* ws = nullptr;
    int account_id = 0;
    int char_id = 0;
    std::string char_name;
    std::string ip;
    std::string map;
    int x = 0;
    int y = 0;
    int party_id = 0;
    int guild_id = 0;
    int  level    = 0;
    int  job      = 0;
    int  group_id = 0;
    bool authed = false;
    bool muted = false;
    bool deafened = false;
    bool ptt = false;

    // Advisory grace window — in a cold-start login the DLL's WS auth can race
    // ahead of the map server's UDP auth_advisory. We accept provisionally and
    // require the advisory to arrive within ADVISORY_GRACE_MS or we kick. If
    // it arrives with a mismatched account_id (real spoof), we kick immediately.
    bool     awaiting_advisory        = false;
    uint32_t advisory_wait_tick       = 0;
    static constexpr uint32_t ADVISORY_GRACE_MS = 15000;

    // Set when an auth_revoke deferred-kick has already been scheduled for
    // this session. Double-delivery of auth_revoke (map_quit + map_deliddb)
    // would otherwise call ws->end() twice, and the second call operates on
    // a session whose close handler is mid-flight. We do NOT touch `authed`
    // for this — the close handler needs authed=true to decrement
    // g_player_count correctly.
    bool  kicking                  = false;
    uint64_t session_id = 0;
    time_t db_refresh_tick = 0;
    uint32_t last_position_ms = 0;
    int         chat_room_id  = 0;
    uint8_t     rx_channel    = 0;
    bool        war_map       = false;
    bool        client_war_state_sent = false;
    bool        client_war_map        = false;
    int         client_war_recommend  = -2;
    std::string whisper_sid;    // active or pending whisper session ID

    // Token bucket rate limiter for audio packets
    // Normal speech = 50 packets/sec (20ms frames). Allow burst up to 100.
    // Refill rate: 60 tokens/sec  →  slightly above normal to absorb jitter
    // Bucket max:  100 tokens     →  ~2 sec burst
    static constexpr float RATE_REFILL = 60.0f;   // tokens per second
    static constexpr float RATE_MAX    = 100.0f;   // bucket capacity
    float    rate_tokens    = RATE_MAX;
    uint32_t rate_last_tick = 0;

    // Flood-ban tracking — consecutive drops over a sustained period.
    // A legitimate client that just hits rate_limit_check() briefly will not
    // accumulate violations because the counter only ticks when drops are
    // back-to-back (within 1 s). A DLL that sustains 500 pkt/s will rack up
    // violations fast and get kicked.
    static constexpr int   FLOOD_VIOLATION_THRESHOLD = 30; // drops in a row
    int      flood_violations    = 0;
    uint32_t last_violation_tick = 0;
    uint64_t audio_sent_packets      = 0;
    uint64_t audio_sent_bytes        = 0;
    uint64_t audio_backpressure_drops = 0;
    uint32_t audio_last_pressure_log = 0;

    bool rate_limit_check() {
        uint32_t now = tick_ms();
        if (rate_last_tick == 0) rate_last_tick = now;
        float dt = (now - rate_last_tick) / 1000.0f;
        rate_last_tick = now;
        rate_tokens = std::min(RATE_MAX, rate_tokens + dt * RATE_REFILL);
        if (rate_tokens < 1.0f) {
            // Consecutive drops within 1 s → flood; gap > 1 s → reset counter.
            if (last_violation_tick != 0 && now - last_violation_tick < 1000)
                flood_violations++;
            else
                flood_violations = 1;
            last_violation_tick = now;
            return false;
        }
        rate_tokens -= 1.0f;
        return true;
    }
    bool is_flooding() const { return flood_violations >= FLOOD_VIOLATION_THRESHOLD; }

    // Whisper spam guard — token bucket, 5 whispers / 30 s.
    // Stops one player from mass-calling random strangers to stalk/spam.
    static constexpr float WHISPER_MAX    = 5.0f;
    static constexpr float WHISPER_REFILL = 5.0f / 30.0f;  // 1 whisper per 6 s
    float    whisper_tokens    = WHISPER_MAX;
    uint32_t whisper_last_tick = 0;

    bool whisper_rate_check() {
        uint32_t now = tick_ms();
        if (whisper_last_tick == 0) whisper_last_tick = now;
        float dt = (now - whisper_last_tick) / 1000.0f;
        whisper_last_tick = now;
        whisper_tokens = std::min(WHISPER_MAX, whisper_tokens + dt * WHISPER_REFILL);
        if (whisper_tokens < 1.0f) return false;
        whisper_tokens -= 1.0f;
        return true;
    }
};

struct PendingPos {
    std::string map;
    int x = 0;
    int y = 0;
    bool war_map = false;
    uint32_t ms = 0;
};

// ── Session lock — split from the old single g_mtx ────────────────────────────
// Audio routing (hot path) acquires a shared_lock (reader) so multiple routing
// operations don't block each other, and the UDP position writer only briefly
// stalls readers while updating x/y/map.  All mutations (auth, close, position
// update, whisper) acquire a unique_lock (exclusive writer).
static std::shared_mutex g_session_mtx;

static std::unordered_map<void*, ClientSession*>      g_by_ws;
static std::unordered_map<int,   ClientSession*>      g_by_char_id;
static std::unordered_map<int,   PendingPos>          g_pending_pos;
static int g_player_count = 0;

// ── Auth advisory (populated by Map Server via UDP) ──────────────────────────
// Map server sends an advisory for each logged-in player with their trusted
// (account_id) tied to the char_id. WS auth must match or it's rejected.
// This blocks attackers who only know a public char_id but not the paired
// account_id of a currently-logged-in player.
struct AuthAdvisory {
    int      account_id = 0;
    uint32_t login_id1  = 0;   // optional: RO session token, for future stricter check
    uint32_t tick       = 0;   // tick_ms() of last refresh
};
static std::unordered_map<int, AuthAdvisory> g_auth_advisories; // by char_id
static constexpr uint32_t AUTH_ADVISORY_TTL_MS = 120000; // 2 minutes
static std::atomic<bool> g_war_active{false};

// ── IP flood-ban list (populated when a client trips FLOOD_VIOLATION_THRESHOLD)
struct FloodBan { uint32_t until_tick = 0; };
static std::unordered_map<std::string, FloodBan> g_flood_bans;
static constexpr uint32_t FLOOD_BAN_DURATION_MS = 300000; // 5 minutes

// ── Pre-built lookup indexes (maintained under g_session_mtx) ────────────────
// Replaces O(n) full-scan with O(members_in_channel) per audio packet.
static std::unordered_map<std::string, std::unordered_set<ClientSession*>> g_by_map;
static std::unordered_map<int,         std::unordered_set<ClientSession*>> g_by_party;
static std::unordered_map<int,         std::unordered_set<ClientSession*>> g_by_guild;
static std::unordered_map<int,         std::unordered_set<ClientSession*>> g_by_room;

// Call under g_session_mtx (exclusive)
static void idx_insert(ClientSession* s) {
    if (!s->map.empty())       g_by_map[s->map].insert(s);
    if (s->party_id > 0)       g_by_party[s->party_id].insert(s);
    if (s->guild_id > 0)       g_by_guild[s->guild_id].insert(s);
    if (s->chat_room_id > 0)   g_by_room[s->chat_room_id].insert(s);
}

static void idx_remove(ClientSession* s) {
    if (!s->map.empty())     { auto it = g_by_map.find(s->map);           if (it != g_by_map.end())    it->second.erase(s); }
    if (s->party_id > 0)     { auto it = g_by_party.find(s->party_id);    if (it != g_by_party.end())  it->second.erase(s); }
    if (s->guild_id > 0)     { auto it = g_by_guild.find(s->guild_id);    if (it != g_by_guild.end())  it->second.erase(s); }
    if (s->chat_room_id > 0) { auto it = g_by_room.find(s->chat_room_id); if (it != g_by_room.end())   it->second.erase(s); }
}

static void idx_set_map(ClientSession* s, const std::string& v) {
    if (s->map == v) return;
    if (!s->map.empty()) { auto it = g_by_map.find(s->map); if (it != g_by_map.end()) it->second.erase(s); }
    s->map = v;
    if (!v.empty()) g_by_map[v].insert(s);
}

static void idx_set_party(ClientSession* s, int v) {
    if (s->party_id == v) return;
    if (s->party_id > 0) { auto it = g_by_party.find(s->party_id); if (it != g_by_party.end()) it->second.erase(s); }
    s->party_id = v;
    if (v > 0) g_by_party[v].insert(s);
}

static void idx_set_guild(ClientSession* s, int v) {
    if (s->guild_id == v) return;
    if (s->guild_id > 0) { auto it = g_by_guild.find(s->guild_id); if (it != g_by_guild.end()) it->second.erase(s); }
    s->guild_id = v;
    if (v > 0) g_by_guild[v].insert(s);
}

static void idx_set_room(ClientSession* s, int v) {
    if (s->chat_room_id == v) return;
    if (s->chat_room_id > 0) { auto it = g_by_room.find(s->chat_room_id); if (it != g_by_room.end()) it->second.erase(s); }
    s->chat_room_id = v;
    if (v > 0) g_by_room[v].insert(s);
}


static bool session_matches(const ClientSession* s, const json& j) {
    if (!s) return false;
    if (s->session_id == 0) return true;
    uint64_t sid = 0;
    try { sid = j.value("session_id", static_cast<uint64_t>(0)); } catch (...) { sid = 0; }
    return sid != 0 && sid == s->session_id;
}

// ── Opus packet sanity check (defense against garbage/crash-inducing payloads) ─
// Only accepts 10 ms / 20 ms frames — matches what our encoder produces and what
// Opus decoders in the wild widely support. Rejects 2.5/5/40/60 ms frames and
// packets that can't even fit their declared code 1/3 layout.
//
// This is NOT a full Opus decode; we don't link libopus on the server. The goal
// is to drop obvious trash before broadcasting to every listener (each listener
// would otherwise run the garbage through opus_decode() → potential crash).
static bool validate_opus_packet(const unsigned char* p, size_t n) {
    if (n < 1) return false;
    const uint8_t toc    = p[0];
    const uint8_t config = toc >> 3;     // 5 bits  [3..7]
    const uint8_t code   = toc & 0x03;   // 2 bits  [0..1]

    // Bitmask of all 10 ms and 20 ms configs (RFC 6716 Table 2).
    constexpr uint32_t VALID_CONFIGS =
        (1u<<0)  | (1u<<1)  |  // SILK-NB   10/20 ms
        (1u<<4)  | (1u<<5)  |  // SILK-MB   10/20 ms
        (1u<<8)  | (1u<<9)  |  // SILK-WB   10/20 ms
        (1u<<12) | (1u<<13) |  // Hybrid-SWB 10/20 ms
        (1u<<14) | (1u<<15) |  // Hybrid-FB  10/20 ms
        (1u<<18) | (1u<<19) |  // CELT-NB    10/20 ms
        (1u<<22) | (1u<<23) |  // CELT-WB    10/20 ms
        (1u<<26) | (1u<<27) |  // CELT-SWB   10/20 ms
        (1u<<30) | (1u<<31);   // CELT-FB    10/20 ms
    if (!((VALID_CONFIGS >> config) & 1u)) return false;

    // code 1 = 2 CBR frames packed back-to-back → payload must be odd-sized+1
    if (code == 1 && n < 3)             return false;
    // code 2 = 2 VBR frames → needs at least TOC + length prefix byte
    if (code == 2 && n < 2)             return false;
    // code 3 = arbitrary frames → TOC + frame-count byte at minimum
    if (code == 3 && n < 2)             return false;

    return true;
}

static uint32_t read_be_u32(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0]) << 24)
         | (static_cast<uint32_t>(p[1]) << 16)
         | (static_cast<uint32_t>(p[2]) << 8)
         |  static_cast<uint32_t>(p[3]);
}

static void write_be_u32(unsigned char* p, uint32_t v) {
    p[0] = static_cast<unsigned char>((v >> 24) & 0xFF);
    p[1] = static_cast<unsigned char>((v >> 16) & 0xFF);
    p[2] = static_cast<unsigned char>((v >> 8) & 0xFF);
    p[3] = static_cast<unsigned char>(v & 0xFF);
}

static void write_be_f32(unsigned char* p, float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    write_be_u32(p, bits);
}

static bool has_fresh_position(const ClientSession& s) {
    const uint32_t now = tick_ms();
    return s.last_position_ms != 0 && (now - s.last_position_ms) <= POSITION_STALE_MS;
}

static float calc_volume(const ClientSession& from, const ClientSession& to) {
    if (from.map.empty() || to.map.empty() || from.map != to.map)
        return 0.0f;

    if (!has_fresh_position(from) || !has_fresh_position(to)) {
        LOG_DEBUG("proximity stale-pos  from=%s age=%lu  to=%s age=%lu",
                  from.char_name.c_str(),
                  from.last_position_ms ? (unsigned long)(tick_ms() - from.last_position_ms) : 999999UL,
                  to.char_name.c_str(),
                  to.last_position_ms ? (unsigned long)(tick_ms() - to.last_position_ms) : 999999UL);
        return 0.0f;
    }

    const int dx = from.x - to.x;
    const int dy = from.y - to.y;
    const float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy));

    const float full_dist = g_cfg.proximity_full_range;
    const float max_dist  = g_cfg.proximity_max_range;
    const float hard_cut  = max_dist - PROXIMITY_EDGE_GUARD_CELLS;

    LOG_DEBUG("proximity  %s(%d,%d) -> %s(%d,%d)  dist=%.2f  full=%.2f  max=%.2f  cut=%.2f",
              from.char_name.c_str(), from.x, from.y,
              to.char_name.c_str(),   to.x,   to.y,
              dist, full_dist, max_dist, hard_cut);

    if (dist >= hard_cut)  return 0.0f;
    if (dist <= full_dist) return 1.0f;

    const float t   = (dist - full_dist) / (hard_cut - full_dist);
    const float vol = 0.5f * (1.0f + std::cos(t * 3.14159265f));
    return vol;
}

static bool should_forward(uint8_t channel, uint32_t gid, const ClientSession& from, const ClientSession& to) {
    if (!to.authed) return false;
    if (to.deafened) return false;
    if (from.char_id == to.char_id) return false;

    // Block voice on restricted maps — check per-player level/job/group_id
    if (g_config.is_map_blocked(from.map, from.level, from.job, from.group_id) ||
        g_config.is_map_blocked(to.map,   to.level,   to.job,   to.group_id))
        return false;

    const bool war_restricted = g_cfg.war_mode_enabled &&
        g_war_active.load(std::memory_order_relaxed) &&
        (from.war_map || to.war_map);
    if (war_restricted) {
        if (channel == 0 || channel == 3) return false;
        if (channel == 4 && !g_cfg.war_allow_whisper) return false;
    }

    // Proximity is blocked at room boundaries (ch0 during war already blocked above).
    if (channel == 0 && (from.chat_room_id != 0 || to.chat_room_id != 0))
        return false;

    // ch0/ch1/ch2: sender and receiver must both be tuned to the same channel.
    // Prevents a modified client from broadcasting on a channel it is not tuned to,
    // and prevents a receiver tuned to party from hearing proximity audio.
    if (channel <= 2 && (from.rx_channel != channel || to.rx_channel != channel))
        return false;

    switch (channel) {
        case 0: return calc_volume(from, to) > 0.0f;
        case 1: return from.party_id != 0 && from.party_id == to.party_id;
        case 2: return from.guild_id != 0 && from.guild_id == to.guild_id;
        case 3: return from.chat_room_id != 0 && from.chat_room_id == to.chat_room_id;
        case 4: // Whisper — both must share the same active session
            return !from.whisper_sid.empty()
                && from.whisper_sid == to.whisper_sid
                && g_whisper.is_active(from.whisper_sid);
        default: return false;
    }
}

// ── UDP Position Receiver (from Map Server) ───────────────────────────────────
// Map Server sends position via UDP directly to voice server
// Format: {"type":"map_pos","char_id":123,"map":"prontera","x":150,"y":100,"party_id":1,"guild_id":2}
#ifdef _WIN32
#  include <winsock2.h>
#  pragma comment(lib, "ws2_32.lib")
   using sock_len_t = int;
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  define SOCKET         int
#  define INVALID_SOCKET (-1)
#  define SOCKET_ERROR   (-1)
#  define closesocket(s) ::close(s)
   using sock_len_t = socklen_t;
#endif

// Global uWS loop pointer – set once from the uWS thread before UDP starts
static uWS::Loop* g_uws_loop = nullptr;

// Helper: queue a JSON send onto the uWS event loop (thread-safe).
// We capture stable session identity (char_id + session_id) instead of a raw
// websocket pointer so a disconnect/reconnect cannot leave us sending through
// a stale uWS object after the target session is gone.
static void send_json_deferred(ClientSession* target, json msg) {
    if (!g_uws_loop) return;
    if (!target || target->char_id == 0) return;
    const int target_char_id = target->char_id;
    const uint64_t target_session_id = target->session_id;
    std::string payload = msg.dump();
    g_uws_loop->defer([target_char_id, target_session_id, payload = std::move(payload)]() {
        std::shared_lock<std::shared_mutex> lock(g_session_mtx);
        auto it = g_by_char_id.find(target_char_id);
        if (it == g_by_char_id.end() || !it->second) return;
        ClientSession* current = it->second;
        if (current->session_id != target_session_id || !current->ws) return;
        current->ws->send(payload, uWS::OpCode::TEXT);
    });
}

static bool is_war_restricted_session(const ClientSession& s) {
    return g_cfg.war_mode_enabled &&
        g_war_active.load(std::memory_order_relaxed) &&
        s.war_map;
}

static int recommended_war_channel(const ClientSession& s) {
    if (!is_war_restricted_session(s)) return 0;
    if (s.guild_id > 0) return 2;
    if (s.party_id > 0) return 1;
    return -1;
}

static json make_war_state_json(const ClientSession& s) {
    return json{
        {"type", "war_state"},
        {"active", is_war_restricted_session(s)},
        {"recommended_channel", recommended_war_channel(s)}
    };
}

// Call while holding g_session_mtx. The actual websocket send is deferred to
// the uWS loop and only happens when the visible client state changed.
static void maybe_send_war_state_locked(ClientSession* s) {
    if (!s || !s->authed) return;

    const bool active = is_war_restricted_session(*s);
    const int recommended = recommended_war_channel(*s);
    if (s->client_war_state_sent &&
        s->client_war_map == active &&
        s->client_war_recommend == recommended)
        return;

    s->client_war_state_sent = true;
    s->client_war_map = active;
    s->client_war_recommend = recommended;
    send_json_deferred(s, make_war_state_json(*s));
}

static SOCKET g_udp_sock = INVALID_SOCKET;
static std::thread g_udp_thread;
static std::atomic<bool> g_udp_running{false};

static void udp_position_loop() {
    char buf[1024];
    sockaddr_in from_addr{};
    sock_len_t from_len = sizeof(from_addr);

    // Periodic maintenance — runs every ~10 seconds inside the UDP loop.
    // Cleans up: stale pending positions and timed-out whisper sessions.
    static constexpr uint32_t MAINTENANCE_INTERVAL_MS = 10000;
    static constexpr uint32_t PENDING_POS_TTL_MS      = 30000;
    uint32_t last_maintenance = tick_ms();

    while (g_udp_running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(g_udp_sock, &readfds);

        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms timeout

        int ret = select(static_cast<int>(g_udp_sock) + 1, &readfds, nullptr, nullptr, &tv);

        // Config reload requested by SIGUSR1 — defer to uWS thread so it's safe
        if (g_reload_requested.load() && g_uws_loop) {
            g_reload_requested.store(false);
            g_uws_loop->defer([]() {
                g_config = Config::load(g_conf_path);
                LOG_INFO("Config reloaded from %s", g_conf_path.c_str());
            });
        }

        // ── Periodic maintenance ──────────────────────────────────────────────
        uint32_t now_maint = tick_ms();
        if (now_maint - last_maintenance >= MAINTENANCE_INTERVAL_MS) {
            last_maintenance = now_maint;

            // Sessions whose advisory grace window expired — collected under
            // the lock, kicked after releasing it (ws->end must run on uWS loop).
            std::vector<std::pair<int, uint64_t>> advisory_timeout_kicks;

            // 1. Pending position TTL — drop positions for players who never authed
            {
                std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                for (auto it = g_pending_pos.begin(); it != g_pending_pos.end(); ) {
                    if (now_maint - it->second.ms > PENDING_POS_TTL_MS) {
                        LOG_DEBUG("pending_pos TTL expired char_id=%d", it->first);
                        it = g_pending_pos.erase(it);
                    } else {
                        ++it;
                    }
                }

                // Expire stale auth advisories (map server should renew every ~30 s)
                for (auto it = g_auth_advisories.begin(); it != g_auth_advisories.end(); ) {
                    if (now_maint - it->second.tick > AUTH_ADVISORY_TTL_MS) {
                        LOG_DEBUG("auth_advisory TTL expired char_id=%d", it->first);
                        it = g_auth_advisories.erase(it);
                    } else {
                        ++it;
                    }
                }

                // Expire IP flood bans
                for (auto it = g_flood_bans.begin(); it != g_flood_bans.end(); ) {
                    if (now_maint >= it->second.until_tick) {
                        LOG_INFO("flood_ban expired ip=%s", it->first.c_str());
                        it = g_flood_bans.erase(it);
                    } else {
                        ++it;
                    }
                }

                // Kick provisional sessions whose advisory never arrived within
                // the grace window. Collect here; actual ws->end() has to run
                // on the uWS loop, so defer after releasing the lock.
                for (auto& kv : g_by_char_id) {
                    ClientSession* s = kv.second;
                    if (!s || !s->awaiting_advisory) continue;
                    if (now_maint - s->advisory_wait_tick
                        > ClientSession::ADVISORY_GRACE_MS) {
                        advisory_timeout_kicks.push_back({ s->char_id, s->session_id });
                    }
                }
            }

            if (!advisory_timeout_kicks.empty() && g_uws_loop) {
                std::vector<std::pair<int, uint64_t>> victims = std::move(advisory_timeout_kicks);
                g_uws_loop->defer([victims]() {
                    for (const auto& [char_id, session_id] : victims) {
                        ClientSession* s = nullptr;
                        {
                            std::shared_lock<std::shared_mutex> lock(g_session_mtx);
                            auto it = g_by_char_id.find(char_id);
                            if (it == g_by_char_id.end() || !it->second || !it->second->ws) continue;
                            if (it->second->session_id != session_id) continue;
                            s = it->second;
                        }
                        LOG_WARNING("auth timeout — advisory never arrived  char_id=%d ip=%s (kick)",
                                    s->char_id, s->ip.c_str());
                        s->ws->send(json{{"type","error"},{"message","no active map session"}}.dump(),
                                    uWS::OpCode::TEXT);
                        s->ws->end(1008, "no advisory");
                    }
                });
            }

            // 2. Whisper timeout — notify both peers then drop expired sessions
            if (g_cfg.whisper_timeout > 0 && g_uws_loop) {
                auto expired = g_whisper.collect_expired(
                    static_cast<int>(g_cfg.whisper_timeout));
                if (!expired.empty()) {
                    g_uws_loop->defer([expired = std::move(expired)]() {
                        std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                        const std::string payload =
                            json{{"type","whisper_ended"},{"reason","timeout"}}.dump();
                        for (auto& [a, b] : expired) {
                            for (int cid : {a, b}) {
                                auto it = g_by_char_id.find(cid);
                                if (it != g_by_char_id.end() && it->second && it->second->ws) {
                                    it->second->ws->send(payload, uWS::OpCode::TEXT);
                                    it->second->whisper_sid.clear();
                                }
                            }
                            LOG_NOTICE("whisper timeout: char_ids %d and %d", a, b);
                        }
                    });
                }
            }
        }

        if (ret <= 0) continue; // timeout or error

        int recv_len = recvfrom(g_udp_sock, buf, sizeof(buf) - 1, 0,
                                reinterpret_cast<sockaddr*>(&from_addr), &from_len);
        if (recv_len <= 0) continue;

        buf[recv_len] = '\0';

        // Parse JSON
        json j;
        try {
            j = json::parse(buf);
        } catch (...) {
            continue;
        }

        std::string type = j.value("type", "");

        if (type == "reload_config") {
            g_uws_loop->defer([]() {
                g_config = Config::load(g_conf_path);
                LOG_INFO("Config reloaded via @reloadvoiceconf");
            });
            continue;
        }

        if (type == "guild_war_state") {
            const bool active = j.value("active", false);
            g_war_active.store(active, std::memory_order_relaxed);
            {
                std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                for (auto& kv : g_by_char_id) {
                    maybe_send_war_state_locked(kv.second);
                }
            }
            LOG_NOTICE("guild_war_state active=%d", active ? 1 : 0);
            continue;
        }

        if (type == "auth_advisory") {
            // From Map Server — authoritative info on who is currently logged in.
            int cid = j.value("char_id",    0);
            int aid = j.value("account_id", 0);
            uint32_t l1 = j.value("login_id1", 0u);
            if (cid > 0 && aid > 0) {
                // Holds the lock only briefly to update the advisory map + check
                // for matching provisional sessions. Any WS kick is deferred
                // onto the uWS loop because ws->end() must run there.
                int spoof_char_id = 0;
                uint64_t spoof_session_id = 0;
                ClientSession* confirm_target = nullptr;
                int spoof_aid_advisory = 0;
                int spoof_aid_claimed  = 0;
                {
                    std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                    auto& adv = g_auth_advisories[cid];
                    adv.account_id = aid;
                    adv.login_id1  = l1;
                    adv.tick       = tick_ms();
                    LOG_DEBUG("auth_advisory char_id=%d account_id=%d l1=%u", cid, aid, l1);

                    // If there's a provisional session waiting for this advisory,
                    // either confirm it (matching account_id) or flag it as spoof.
                    auto sit = g_by_char_id.find(cid);
                    if (sit != g_by_char_id.end() && sit->second) {
                        ClientSession* s = sit->second;
                        if (s->awaiting_advisory) {
                            if (s->account_id == aid) {
                                s->awaiting_advisory = false;
                                confirm_target = s;
                            } else {
                                spoof_char_id      = s->char_id;
                                spoof_session_id   = s->session_id;
                                spoof_aid_advisory = aid;
                                spoof_aid_claimed  = s->account_id;
                            }
                        } else if (s->account_id != aid) {
                            // Already-authed session but advisory says a different
                            // account_id now owns this char_id → someone else logged
                            // in as that char (stale session) or a spoof. Kick.
                            spoof_char_id      = s->char_id;
                            spoof_session_id   = s->session_id;
                            spoof_aid_advisory = aid;
                            spoof_aid_claimed  = s->account_id;
                        }
                    }
                }

                if (confirm_target) {
                    LOG_INFO("auth confirmed via late advisory  char_id=%d aid=%d", cid, aid);
                }
                if (spoof_char_id > 0 && g_uws_loop) {
                    int cid_cap = cid;
                    int adv_cap = spoof_aid_advisory;
                    int clm_cap = spoof_aid_claimed;
                    int target_char_id = spoof_char_id;
                    uint64_t target_session_id = spoof_session_id;
                    g_uws_loop->defer([target_char_id, target_session_id, cid_cap, adv_cap, clm_cap]() {
                        ClientSession* target = nullptr;
                        {
                            std::shared_lock<std::shared_mutex> lock(g_session_mtx);
                            auto it = g_by_char_id.find(target_char_id);
                            if (it == g_by_char_id.end() || !it->second || !it->second->ws) return;
                            if (it->second->session_id != target_session_id) return;
                            target = it->second;
                        }
                        LOG_WARNING("auth SPOOF (late advisory) — char_id=%d claimed aid=%d but advisory aid=%d",
                                    cid_cap, clm_cap, adv_cap);
                        target->ws->send(json{{"type","error"},{"message","credentials mismatch"}}.dump(),
                                         uWS::OpCode::TEXT);
                        target->ws->end(1008, "spoof");
                    });
                }
            }
            continue;
        }

        if (type == "auth_revoke") {
            // Map server says this char is no longer logged in (char-select,
            // @quit, disconnect). Drop the advisory AND kick the live WS so
            // the DLL's reconnect loop picks up a clean session under the
            // new char_id.
            //
            // Earlier we bailed on the WS kick because map.c calls
            // voice_bridge_send_leave() twice (map_quit + map_deliddb) for
            // the same char_id — two UDP packets, two deferred kicks, and
            // the second one dereferenced a pointer whose session had
            // already been freed by the first one's close handler.
            //
            // Fix: don't capture the session pointer at all. Capture the
            // char_id, then look the session up fresh inside the deferred
            // lambda under g_session_mtx. Duplicate packets become a harmless
            // miss on the second lookup (session already gone).
            int cid = j.value("char_id", 0);
            if (cid <= 0) continue;

            {
                std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                g_auth_advisories.erase(cid);
            }

            if (g_uws_loop) {
                g_uws_loop->defer([cid]() {
                    // Step 1: lookup + set `kicking` flag under the session lock.
                    // We deliberately keep `authed` untouched here — the close
                    // handler below reads it to decrement g_player_count, and if
                    // we flipped it to false the online counter would leak.
                    ClientSession* target = nullptr;
                    bool spoof_mismatch = false;
                    {
                        std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                        auto it = g_by_char_id.find(cid);
                        if (it == g_by_char_id.end() || !it->second) return;  // already gone
                        if (it->second->kicking || !it->second->ws) return;    // already kicked
                        if (!it->second->authed) return;                       // never fully authed
                        target = it->second;
                        target->kicking = true;   // second auth_revoke will see this and bail
                    }
                    // Step 2: close the WS OUTSIDE the lock. ws->end() triggers
                    // our close handler synchronously on this same uWS loop
                    // thread, and that handler re-acquires g_session_mtx — holding
                    // it here would deadlock. Since we're on the uWS thread and
                    // the close handler only runs when we unwind, `target` stays
                    // valid across these two calls.
                    LOG_INFO("auth_revoke char_id=%d — map server reports logoff, closing WS", cid);
                    target->ws->send(json{{"type","error"},{"message","map session ended"}}.dump(),
                                     uWS::OpCode::TEXT);
                    target->ws->end(1000, "map logoff");
                });
            }
            continue;
        }

        if (type == "chat_join") {
            int char_id = j.value("char_id", 0);
            int room_id = j.value("room_id", 0);
            if (char_id > 0 && room_id > 0) {
                std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                auto it = g_by_char_id.find(char_id);
                if (it != g_by_char_id.end() && it->second && it->second->authed) {
                    idx_set_room(it->second, room_id);
                    send_json_deferred(it->second, json{{"type","room_joined"},{"room_id",room_id}});
                    LOG_NOTICE("chat_join char_id=%d room_id=%d", char_id, room_id);
                }
            }
            continue;
        }

        if (type == "chat_leave") {
            int char_id = j.value("char_id", 0);
            if (char_id > 0) {
                std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                auto it = g_by_char_id.find(char_id);
                if (it != g_by_char_id.end() && it->second && it->second->authed) {
                    idx_set_room(it->second, 0);
                    send_json_deferred(it->second, json{{"type","room_left"}});
                    LOG_NOTICE("chat_leave char_id=%d", char_id);
                }
            }
            continue;
        }

        if (type == "map_leave") {
            int char_id = j.value("char_id", 0);
            if (char_id > 0) {
                std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                auto it = g_by_char_id.find(char_id);
                if (it != g_by_char_id.end() && it->second && it->second->authed) {
                    ClientSession* s = it->second;
                    // idx_set_map removes from old map bucket and sets s->map = ""
                    idx_set_map(s, "");
                    s->x = 0;
                    s->y = 0;
                    s->last_position_ms = 0;  // stale-position guard → proximity = 0
                    LOG_DEBUG("map_leave char_id=%d — position cleared", char_id);
                }
            }
            continue;
        }

        if (type != "map_pos") continue;

        int char_id = j.value("char_id", 0);
        if (char_id <= 0) continue;

        std::string new_map = j.value("map", "");
        if (new_map.size() > 64) new_map.resize(64);
        const int new_x     = j.value("x",        0);
        const int new_y     = j.value("y",        0);
        const int new_level = j.value("level",    0);
        const int new_job   = j.value("job",      0);
        const int new_gid   = j.value("group_id", 0);
        const bool new_war_map = j.value("war_map", false);

        // Use unique_lock (not lock_guard) so we can unlock before the DB call below.
        std::unique_lock<std::shared_mutex> lock(g_session_mtx);
        auto it = g_by_char_id.find(char_id);
        if (it == g_by_char_id.end() || !it->second) {
            // Session not yet created — cache position for when auth arrives
            LOG_DEBUG("UDP map_pos char_id=%d not in session — cached %s(%d,%d)", char_id, new_map.c_str(), new_x, new_y);
            g_pending_pos[char_id] = { new_map, new_x, new_y, new_war_map, tick_ms() };
            continue;
        }

        ClientSession* s = it->second;
        LOG_DEBUG("UDP pos char_id=%d %s(%d,%d) lv=%d job=%d grp=%d war=%d", char_id, new_map.c_str(), new_x, new_y, new_level, new_job, new_gid, new_war_map ? 1 : 0);
        idx_set_map(s, new_map);
        s->x        = new_x;
        s->y        = new_y;
        s->level    = new_level;
        s->job      = new_job;
        s->group_id = new_gid;
        s->war_map  = new_war_map;
        s->last_position_ms = tick_ms();

        // Push own position back to the DLL so it can do stereo panning
        // without needing to read memory offsets for CHAR_X / CHAR_Y.
        if (s->authed) {
            send_json_deferred(s, json{
                {"type", "your_pos"},
                {"x",    new_x},
                {"y",    new_y},
                {"map",  new_map}
            });
        }

        // DB refresh — snapshot char_id and bump tick NOW (under lock) so rapid
        // back-to-back map_pos bursts for the same player don't all fire simultaneous
        // queries. Then release the exclusive lock BEFORE the blocking MySQL call
        // so audio-routing threads (shared_lock readers) are never stalled waiting
        // for a DB round-trip that can easily take 5-50 ms.
        time_t now_t = time(nullptr);
        bool need_refresh = (now_t - s->db_refresh_tick >= g_cfg.db_refresh_s);
        const int refresh_char_id = s->char_id;
        if (need_refresh)
            s->db_refresh_tick = now_t;  // prevent duplicate queries while lock is released
        maybe_send_war_state_locked(s);
        lock.unlock();  // release exclusive lock before blocking DB call

        if (need_refresh) {
            CharInfo ci = db_get_char_info(refresh_char_id);  // no lock held here
            if (ci.ok) {
                std::lock_guard<std::shared_mutex> lock2(g_session_mtx);
                auto it2 = g_by_char_id.find(refresh_char_id);
                if (it2 != g_by_char_id.end() && it2->second) {
                    ClientSession* s2 = it2->second;
                    if (ci.party_id != s2->party_id || ci.guild_id != s2->guild_id) {
                        LOG_DEBUG("DB refresh char_id=%d party %d->%d guild %d->%d",
                                  refresh_char_id, s2->party_id, ci.party_id, s2->guild_id, ci.guild_id);
                        idx_set_party(s2, ci.party_id);  // keeps g_by_party in sync
                        idx_set_guild(s2, ci.guild_id);  // keeps g_by_guild in sync
                    }
                }
            }
        }
    }
}

static bool init_udp_receiver() {
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOG_ERROR("WSAStartup failed");
        return false;
    }
#endif

    g_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_udp_sock == INVALID_SOCKET) {
        LOG_ERROR("UDP socket creation failed");
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }

    // Bind to port 7001 (same port used by voice_bridge)
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(7001);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(g_udp_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        LOG_ERROR("UDP bind failed on port 7001");
        closesocket(g_udp_sock);
        g_udp_sock = INVALID_SOCKET;
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }

#ifdef _WIN32
    u_long nonblock = 1;
    ioctlsocket(g_udp_sock, FIONBIO, &nonblock);
#else
    fcntl(g_udp_sock, F_SETFL, fcntl(g_udp_sock, F_GETFL, 0) | O_NONBLOCK);
#endif

    g_udp_running = true;
    g_udp_thread = std::thread(udp_position_loop);

    LOG_NOTICE("UDP position receiver started on port 7001");
    return true;
}

static void stop_udp_receiver() {
    g_udp_running = false;
    if (g_udp_thread.joinable()) {
        g_udp_thread.join();
    }
    if (g_udp_sock != INVALID_SOCKET) {
        closesocket(g_udp_sock);
        g_udp_sock = INVALID_SOCKET;
    }
#ifdef _WIN32
    WSACleanup();
#endif
}

static float volume_for(uint8_t channel, const ClientSession& from, const ClientSession& to) {
    if (channel == 0)
        return calc_volume(from, to);
    return 1.0f;
}

static std::string normalize_ip(std::string_view raw) {
    const std::string s(raw);
    const std::string prefix = "0000:0000:0000:0000:0000:ffff:";
    if (s.size() > prefix.size() && s.substr(0, prefix.size()) == prefix) {
        std::string hex = s.substr(prefix.size());
        // hex must be "XXXX:XXXX" — 9 chars minimum; reject malformed input
        if (hex.size() < 9) return s;
        std::string h = hex.substr(0, 4) + hex.substr(5, 4);
        try {
            unsigned int a = std::stoul(h.substr(0, 2), nullptr, 16);
            unsigned int b = std::stoul(h.substr(2, 2), nullptr, 16);
            unsigned int c = std::stoul(h.substr(4, 2), nullptr, 16);
            unsigned int d = std::stoul(h.substr(6, 2), nullptr, 16);
            char buf[20];
            snprintf(buf, sizeof(buf), "%u.%u.%u.%u", a, b, c, d);
            return buf;
        } catch (...) { return s; }
    }
    return s;
}

static void send_json(uWS::WebSocket<false, true, ClientSession>* ws, const json& j) {
    ws->send(j.dump(), uWS::OpCode::TEXT);
}

static size_t audio_backpressure_limit_bytes() {
    int kb = g_cfg.audio_backpressure_kb;
    if (kb < 16) kb = 16;
    if (kb > 1024) kb = 1024;
    return static_cast<size_t>(kb) * 1024u;
}

static size_t audio_target_limit(uint8_t channel) {
    int limit = 0;
    if (channel == 0) limit = g_cfg.max_targets_normal;
    else if (channel == 3) limit = g_cfg.max_targets_room;
    else if (channel == 1 || channel == 2) limit = g_cfg.max_targets_group;
    else return 0; // whisper and unknown channels stay uncapped here
    return limit > 0 ? static_cast<size_t>(limit) : 0;
}

static void trim_audio_targets(uint8_t channel,
                               int sender_char_id,
                               std::vector<std::pair<ClientSession*, float>>& targets) {
    const size_t limit = audio_target_limit(channel);
    if (limit == 0 || targets.size() <= limit) return;

    // Keep the loudest/nearest listeners first. For party/guild this is stable
    // enough because their volume is normally equal; the cap is only a safety net.
    std::sort(targets.begin(), targets.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    const size_t old_size = targets.size();
    targets.resize(limit);
    LOG_WARNING("audio target cap char_id=%d ch=%u targets=%zu cap=%zu dropped=%zu",
                sender_char_id, static_cast<unsigned>(channel),
                old_size, limit, old_size - limit);
}

static void clear_existing_whisper(ClientSession* s) {
    if (!s || s->whisper_sid.empty()) return;

    const std::string old_sid = s->whisper_sid;
    const int peer_id = g_whisper.get_peer(old_sid, s->char_id);
    g_whisper.end(old_sid, s->char_id);
    s->whisper_sid.clear();

    // Notify s that their old session ended. Without this, the DLL keeps
    // channel_=Whisper and then when the new whisper_active arrives it
    // saves pre_whisper_channel_=Whisper, permanently trapping the player
    // in Whisper once the new call ends.
    if (s->ws) send_json(s->ws, json{{"type","whisper_ended"},{"sid",old_sid}});

    auto it = g_by_char_id.find(peer_id);
    if (it != g_by_char_id.end() && it->second) {
        ClientSession* peer = it->second;
        if (peer->whisper_sid == old_sid)
            peer->whisper_sid.clear();
        if (peer->ws) send_json(peer->ws, json{{"type","whisper_ended"},{"sid",old_sid}});
    }
}

// Receiver packet layout:
//   [4 char_id][4 vol][4 x][4 y][24 name][2 seq] + opus   = 42 bytes header
static bool send_audio_to(ClientSession* to, int sender_char_id,
                          const std::string& sender_name,
                          float volume, int sender_x, int sender_y,
                          uint16_t seq,
                          const char* pcm, size_t pcm_bytes) {
    if (!to || !to->ws || pcm_bytes == 0 || volume <= 0.0f)
        return false;

    const size_t pressure_limit = audio_backpressure_limit_bytes();
    const unsigned int buffered = to->ws->getBufferedAmount();
    if (buffered > pressure_limit) {
        to->audio_backpressure_drops++;
        uint32_t now = tick_ms();
        if (to->audio_last_pressure_log == 0 || now - to->audio_last_pressure_log > 5000) {
            to->audio_last_pressure_log = now;
            LOG_WARNING("audio backpressure drop to char_id=%d buffered=%u limit=%zu drops=%llu",
                        to->char_id, buffered, pressure_limit,
                        static_cast<unsigned long long>(to->audio_backpressure_drops));
        }
        return false;
    }

    std::string out;
    out.resize(42 + pcm_bytes, '\0');

    auto* h = reinterpret_cast<unsigned char*>(&out[0]);
    write_be_u32(h,      static_cast<uint32_t>(sender_char_id));
    write_be_f32(h + 4,  volume);
    write_be_u32(h + 8,  static_cast<uint32_t>(sender_x));
    write_be_u32(h + 12, static_cast<uint32_t>(sender_y));
    size_t name_len = std::min(sender_name.size(), static_cast<size_t>(23));
    std::memcpy(h + 16, sender_name.c_str(), name_len);
    h[40] = static_cast<unsigned char>((seq >> 8) & 0xFF);
    h[41] = static_cast<unsigned char>( seq       & 0xFF);
    std::memcpy(&out[42], pcm, pcm_bytes);

    auto status = to->ws->send(out, uWS::OpCode::BINARY);
    if (status == uWS::WebSocket<false, true, ClientSession>::DROPPED) {
        to->audio_backpressure_drops++;
        return false;
    }

    to->audio_sent_packets++;
    to->audio_sent_bytes += out.size();
    return true;
}

void run_server() {
    con::init();
    load_voice_conf("conf/voice_athena.conf");
    load_inter_conf("conf/inter_athena.conf");

    printf("\n");
    printf("%s ==========================================%s\n", con::CYAN,  con::RESET);
    printf("%s   Voice Server v1.0  (voice_athena)      %s\n", con::WHITE, con::RESET);
    printf("%s ==========================================%s\n", con::CYAN,  con::RESET);
    printf("\n");

    LOG_STATUS("Bind        %s:%d", g_cfg.voice_ip.c_str(), g_cfg.voice_port);
    LOG_INFO("Proximity   full=%.0f cell  max=%.0f cell  update=%dms  guard=%.2f",
             g_cfg.proximity_full_range, g_cfg.proximity_max_range,
             g_cfg.proximity_update_ms, PROXIMITY_EDGE_GUARD_CELLS);
    LOG_INFO("Whisper     timeout=%ds%s", g_cfg.whisper_timeout,
             g_cfg.whisper_timeout == 0 ? " (no timeout)" : "");
    LOG_INFO("Audio scale targets normal=%d group=%d room=%d  backpressure=%dKB",
             g_cfg.max_targets_normal, g_cfg.max_targets_group,
             g_cfg.max_targets_room, g_cfg.audio_backpressure_kb);
    LOG_INFO("DB          %s@%s:%d/%s  table=%s  refresh=%ds",
             g_cfg.db_user.c_str(), g_cfg.db_host.c_str(),
             g_cfg.db_port, g_cfg.db_name.c_str(),
             g_cfg.db_char_table.c_str(), g_cfg.db_refresh_s);
    LOG_INFO("Log level   %d", g_cfg.log_level);
    printf("\n");

    if (!db_connect()) {
        LOG_WARNING("DB not available — party/guild channels will not work until connected");
    }

    // Start UDP receiver for position from Map Server
    if (!init_udp_receiver()) {
        LOG_WARNING("UDP receiver failed to start — positions from Map Server will not work");
    }

    uWS::App().ws<ClientSession>("/*", {
        .maxPayloadLength = 2048,
        .maxBackpressure = 64 * 1024,
        .closeOnBackpressureLimit = false,
        .open = [](auto* ws) {
            // Capture the uWS loop once so UDP thread can defer sends
            if (!g_uws_loop) g_uws_loop = uWS::Loop::get();
            auto* s = ws->getUserData();
            s->ws = ws;
            std::string ip = normalize_ip(ws->getRemoteAddressAsText());
            bool ip_banned = false;

            // IP flood-ban check — refuse connection if this IP is currently banned
            {
                std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                auto bit = g_flood_bans.find(ip);
                if (bit != g_flood_bans.end()) {
                    if (tick_ms() < bit->second.until_tick) {
                        uint32_t remain = (bit->second.until_tick - tick_ms()) / 1000;
                        LOG_WARNING("IP banned %s — refusing connection (%u s left)", ip.c_str(), remain);
                        ip_banned = true;
                    }
                    // stale entry — drop (maintenance will also sweep these)
                    if (!ip_banned)
                        g_flood_bans.erase(bit);
                }
                if (!ip_banned) {
                    s->ip = std::move(ip);
                    g_by_ws[ws] = s;
                }
            }
            if (ip_banned) {
                ws->end(1008, "ip banned");
                return;
            }
            LOG_STATUS("connection from %s", s->ip.c_str());
        },

        .message = [](auto* ws, std::string_view message, uWS::OpCode opCode) {
            auto* s = ws->getUserData();

            if (opCode == uWS::OpCode::TEXT) {
                json j;
                try {
                    j = json::parse(message);
                } catch (...) {
                    send_json(ws, json{{"type", "error"}, {"message", "invalid json"}});
                    return;
                }

                const std::string type = j.value("type", "");

                if (type == "auth") {
                    // ── Reject re-auth on an already-authed session ───────────
                    // The DLL sometimes forgets to close its WebSocket when the
                    // user logs out to char-select and picks a different char.
                    // It then sends a second `auth` frame on the same WS with
                    // the new char_id. If we let that through we'd overwrite
                    // `s->char_id` in place and leave orphan entries in
                    // g_by_char_id pointing to the same session (online count
                    // keeps climbing: 1 → 2 → 3 …). Kick instead so the DLL
                    // reconnects cleanly — the new WS will then run through
                    // the "one char per account" stale-session sweep below and
                    // see the old WS as a separate `other` pointer.
                    const int      new_aid = j.value("account_id", 0);
                    const int      new_cid = j.value("char_id", 0);
                    const uint64_t new_sid = j.value("session_id", static_cast<uint64_t>(0));

                    if (s->authed) {
                        LOG_INFO("re-auth on authed session char_id=%d → new char_id=%d — kicking for clean reconnect",
                                    s->char_id, new_cid);
                        s->kicking = true;   // prevent auth_revoke lambda from double-ending
                        send_json(ws, json{{"type","error"},{"message","re-auth on same connection not allowed"}});
                        ws->end(1000, "reauth");
                        return;
                    }

                    s->account_id = new_aid;
                    s->char_id    = new_cid;
                    s->session_id = new_sid;

                    if (s->account_id <= 0 || s->char_id <= 0 || s->session_id == 0) {
                        send_json(ws, json{{"type", "error"}, {"message", "missing account_id or char_id"}});
                        return;
                    }

                    // ── Verify against Map Server advisory (anti-spoofing) ────
                    // Map server broadcasts (char_id, account_id) on login + every
                    // 30 s. On cold start the DLL's WS auth can arrive before the
                    // first UDP advisory packet does; we accept provisionally and
                    // wait up to ADVISORY_GRACE_MS for the advisory. The maintenance
                    // loop kicks sessions whose advisory never arrives. A mismatched
                    // advisory on arrival still causes an immediate kick.
                    bool spoof_mismatch = false;
                    {
                        std::shared_lock<std::shared_mutex> lock(g_session_mtx);
                        auto ait = g_auth_advisories.find(s->char_id);
                        if (ait == g_auth_advisories.end()) {
                            s->awaiting_advisory  = true;
                            s->advisory_wait_tick = tick_ms();
                            LOG_INFO("auth provisional — waiting for advisory  char_id=%d ip=%s (grace %ums)",
                                     s->char_id, s->ip.c_str(), ClientSession::ADVISORY_GRACE_MS);
                        } else if (ait->second.account_id != s->account_id) {
                            LOG_WARNING("auth SPOOF attempt — char_id=%d claimed aid=%d but advisory aid=%d (ip=%s)",
                                        s->char_id, s->account_id, ait->second.account_id, s->ip.c_str());
                            spoof_mismatch = true;
                        }
                    }
                    if (spoof_mismatch) {
                        send_json(ws, json{{"type","error"},{"message","credentials mismatch"}});
                        ws->end(1008, "spoof");
                        return;
                    }

                    CharInfo ci = db_get_char_info(s->char_id);
                    if (!ci.ok) {
                        send_json(ws, json{{"type", "error"}, {"message", "char_id not found in DB"}});
                        return;
                    }

                    s->char_name = ci.char_name;
                    s->party_id  = ci.party_id;
                    s->guild_id  = ci.guild_id;
                    s->db_refresh_tick = time(nullptr);

                    s->authed = true;
                    std::vector<std::pair<ClientSession*, std::string>> sessions_to_close;
                    // Capture initial position to send your_pos after lock is released.
                    // Values may be from pending_pos (position arrived before auth) or
                    // from a prior reconnect (position already on session).
                    std::string init_map;
                    int init_x = 0, init_y = 0;
                    {
                        std::lock_guard<std::shared_mutex> lock(g_session_mtx);

                        // Apply any position that arrived before auth
                        auto pit = g_pending_pos.find(s->char_id);
                        if (pit != g_pending_pos.end()) {
                            const PendingPos& pp = pit->second;
                            s->map = pp.map;
                            s->x   = pp.x;
                            s->y   = pp.y;
                            s->war_map = pp.war_map;
                            s->last_position_ms = pp.ms;
                            LOG_DEBUG("auth applied pending pos char_id=%d %s(%d,%d)", s->char_id, pp.map.c_str(), pp.x, pp.y);
                            g_pending_pos.erase(pit);
                        }
                        // Snapshot position for your_pos (read while holding the lock)
                        init_map = s->map;
                        init_x   = s->x;
                        init_y   = s->y;

                        // ── One active char per account ───────────────────
                        // An RO account can only own one logged-in character at
                        // a time, so any OTHER session with the same account_id
                        // (but a different char_id) is a stale ghost — usually
                        // a DLL that forgot to close its previous WS after the
                        // user switched character without restarting the client.
                        // Kick those before we replace the char_id slot below.
                        int  stale_account_kicks = 0;
                        for (auto& kv : g_by_char_id) {
                            ClientSession* other = kv.second;
                            if (!other || other == s) continue;
                            if (other->account_id != s->account_id) continue;
                            if (other->char_id    == s->char_id)    continue;
                            LOG_WARNING("account_id=%d already online as char_id=%d sid=%llu — kicking stale session (new char_id=%d sid=%llu)",
                                        s->account_id, other->char_id,
                                        static_cast<unsigned long long>(other->session_id),
                                        s->char_id,
                                        static_cast<unsigned long long>(s->session_id));
                            if (other->authed) {
                                other->authed = false;
                                stale_account_kicks++;   // will decrement g_player_count below
                            }
                            if (other->ws) {
                                send_json_deferred(other,
                                    json{{"type","error"},{"message","account logged in as different character"}});
                                sessions_to_close.push_back({ other, "stale account session" });
                            }
                            idx_remove(other);
                            // Do NOT erase from g_by_char_id here — the kicked
                            // session's own .close handler will erase its own
                            // (char_id → session) entry. If we erased here we'd
                            // race with that handler.
                        }
                        if (stale_account_kicks > 0) {
                            g_player_count -= stale_account_kicks;
                            if (g_player_count < 0) g_player_count = 0;
                        }

                        bool replacing = false;
                        auto it_old = g_by_char_id.find(s->char_id);
                        if (it_old != g_by_char_id.end() && it_old->second && it_old->second != s) {
                            ClientSession* old = it_old->second;
                            replacing = old->authed; // only counts if old was actually online
                            LOG_WARNING("duplicate char_id=%d old_session=%llu new_session=%llu — closing old connection",
                                        s->char_id,
                                        static_cast<unsigned long long>(old->session_id),
                                        static_cast<unsigned long long>(s->session_id));
                            // Mark old session as no longer authed so its close handler
                            // won't decrement g_player_count (we handle the count here).
                            old->authed = false;
                            if (old->ws) {
                                send_json_deferred(old,
                                    json{{"type","error"},{"message","session replaced by new login"}});
                                sessions_to_close.push_back({ old, "replaced" });
                            }
                            idx_remove(old);
                        }
                        g_by_char_id[s->char_id] = s;
                        idx_insert(s);   // add to channel indexes

                        // If we replaced an existing authed session, count stays the same.
                        if (!replacing) g_player_count++;
                    }
                    for (const auto& [target, reason] : sessions_to_close) {
                        if (target && target->ws) {
                            target->ws->end(1000, reason);
                        }
                    }

                    // (g_player_count already updated inside the lock above)
                    LOG_NOTICE("(char_id=%d aid=%d sid=%llu name=%s ip=%s) party=%d guild=%d  [online: %d]",
                               s->char_id, s->account_id, static_cast<unsigned long long>(s->session_id),
                               s->char_name.c_str(), s->ip.c_str(),
                               s->party_id, s->guild_id, g_player_count);
                    send_json(ws, json{{"type", "auth_ok"}});
                    send_json(ws, make_war_state_json(*s));
                    // Push initial position so the DLL knows where the player is
                    // immediately — without this it stays at (0,0) until the next
                    // map_pos UDP update, making stereo panning wrong on first connect.
                    if (!init_map.empty()) {
                        send_json(ws, json{
                            {"type", "your_pos"},
                            {"x",    init_x},
                            {"y",    init_y},
                            {"map",  init_map}
                        });
                    }
                    return;
                }

                if (!s->authed) {
                    send_json(ws, json{{"type", "error"}, {"message", "not authenticated"}});
                    return;
                }

                if (type == "ping") {
                    send_json(ws, json{
                        {"type", "pong"},
                        {"t", j.value("t", 0u)}
                    });
                    return;
                }

                // position / party_update / guild_update are all handled
                // server-side: position from Map Server via UDP, party/guild from DB.
                // Silently ignore if DLL sends these for backward-compat.

                if (type == "set_channel") {
                    uint8_t ch = static_cast<uint8_t>(j.value("channel", 0));
                    if (ch > 4) {
                        send_json(ws, json{{"type","error"},{"message","invalid channel"}});
                        return;
                    }
                    s->rx_channel = ch;
                    send_json(ws, json{{"type", "channel_ack"}, {"channel", ch}});
                    LOG_DEBUG("set_channel char_id=%d ch=%d", s->char_id, ch);
                    return;
                }

                // ── Whisper ───────────────────────────────────────────────────
                if (type == "whisper_lookup") {
                    std::string name = j.value("name", "");
                    if (name.empty()) return;
                    // Spam guard: cap whisper attempts (5 per 30 s)
                    if (!s->whisper_rate_check()) {
                        send_json(ws, json{{"type","whisper_lookup_fail"},{"reason","rate_limited"}});
                        LOG_WARNING("whisper_lookup rate-limited char_id=%d name=%s", s->char_id, name.c_str());
                        return;
                    }
                    // DB lookup runs synchronously — acceptable for whisper (rare event)
                    int target_id = db_lookup_char_by_name(name);
                    if (target_id <= 0) {
                        send_json(ws, json{{"type","whisper_lookup_fail"},{"reason","not found"}});
                        LOG_NOTICE("whisper_lookup '%s' — not found", name.c_str());
                        return;
                    }
                    // Check target is online
                    {
                        std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                        auto it = g_by_char_id.find(target_id);
                        if (it == g_by_char_id.end() || !it->second || !it->second->authed) {
                            send_json(ws, json{{"type","whisper_lookup_fail"},{"reason","offline"}});
                            LOG_NOTICE("whisper_lookup '%s' (id=%d) — offline", name.c_str(), target_id);
                            return;
                        }
                        // Found online — proceed as whisper_request
                        ClientSession* target = it->second;
                        clear_existing_whisper(s);
                        std::string sid = g_whisper.request(s->char_id, target_id);
                        s->whisper_sid = sid;
                        if (g_config.is_whisper_bypass(s->group_id)) {
                            g_whisper.accept(sid, target_id);
                            target->whisper_sid = sid;
                            send_json(target->ws, json{
                                {"type","whisper_active"},
                                {"sid", sid},
                                {"peer_name", s->char_name}
                            });
                            send_json(ws, json{{"type","whisper_active"},{"sid",sid},{"peer_name",target->char_name}});
                            LOG_NOTICE("whisper_lookup bypass (GM) '%s' → char_id=%d, active", name.c_str(), target_id);
                        } else {
                            send_json(target->ws, json{
                                {"type","whisper_incoming"},
                                {"sid", sid},
                                {"from_char_id", s->char_id},
                                {"from_name",    s->char_name}
                            });
                            send_json(ws, json{{"type","whisper_calling"},{"sid",sid},{"target_name",target->char_name}});
                            LOG_NOTICE("whisper_lookup '%s' → char_id=%d, calling", name.c_str(), target_id);
                        }
                    }
                    return;
                }

                if (type == "whisper_request") {
                    int target_id = j.value("target_char_id", 0);
                    if (target_id <= 0) return;
                    // Spam guard: cap whisper attempts (5 per 30 s)
                    if (!s->whisper_rate_check()) {
                        send_json(ws, json{{"type","whisper_unavailable"},{"reason","rate_limited"}});
                        LOG_WARNING("whisper_request rate-limited char_id=%d target=%d", s->char_id, target_id);
                        return;
                    }
                    std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                    auto it = g_by_char_id.find(target_id);
                    if (it == g_by_char_id.end() || !it->second || !it->second->authed) {
                        send_json(ws, json{{"type","whisper_unavailable"},{"reason","offline"}});
                        return;
                    }
                    ClientSession* target = it->second;
                    // Cancel any previous pending session from this sender
                    clear_existing_whisper(s);
                    std::string sid = g_whisper.request(s->char_id, target_id);
                    s->whisper_sid = sid;

                    if (g_config.is_whisper_bypass(s->group_id)) {
                        // GM bypass — auto-accept without notifying target to confirm
                        g_whisper.accept(sid, target_id);
                        target->whisper_sid = sid;
                        send_json(target->ws, json{
                            {"type","whisper_active"},
                            {"sid", sid},
                            {"peer_name", s->char_name}
                        });
                        send_json(ws, json{{"type","whisper_active"},{"sid",sid},{"peer_name",target->char_name}});
                        LOG_NOTICE("whisper_bypass (GM) %s→%s sid=%s", s->char_name.c_str(), target->char_name.c_str(), sid.c_str());
                    } else {
                        send_json(target->ws, json{
                            {"type","whisper_incoming"},
                            {"sid", sid},
                            {"from_char_id", s->char_id},
                            {"from_name",    s->char_name}
                        });
                        send_json(ws, json{{"type","whisper_calling"},{"sid",sid},{"target_name",target->char_name}});
                        LOG_NOTICE("whisper_request %s→%s sid=%s", s->char_name.c_str(), target->char_name.c_str(), sid.c_str());
                    }
                    return;
                }

                if (type == "whisper_accept") {
                    std::string sid = j.value("sid", "");
                    if (!g_whisper.accept(sid, s->char_id)) return;
                    // get_peer uses its own mutex — no g_session_mtx needed yet.
                    int peer_id = g_whisper.get_peer(sid, s->char_id);
                    std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                    // Write whisper_sid inside the exclusive lock so the UDP audio-
                    // routing path (shared_lock) never sees a torn std::string.
                    s->whisper_sid = sid;
                    auto it = g_by_char_id.find(peer_id);
                    if (it != g_by_char_id.end() && it->second) {
                        send_json(it->second->ws, json{
                            {"type","whisper_active"},{"sid",sid},{"peer_name",s->char_name}
                        });
                    }
                    send_json(ws, json{{"type","whisper_active"},{"sid",sid},{"peer_name", it != g_by_char_id.end() && it->second ? it->second->char_name : ""}});
                    LOG_NOTICE("whisper_accept sid=%s char_id=%d", sid.c_str(), s->char_id);
                    return;
                }

                if (type == "whisper_reject") {
                    std::string sid = j.value("sid", "");
                    int peer_id = g_whisper.get_peer(sid, s->char_id);
                    if (!g_whisper.reject(sid, s->char_id)) return;
                    std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                    // Clear inside the lock — UDP routing reads whisper_sid under shared_lock.
                    s->whisper_sid.clear();
                    auto it = g_by_char_id.find(peer_id);
                    if (it != g_by_char_id.end() && it->second) {
                        send_json(it->second->ws, json{{"type","whisper_rejected"},{"sid",sid}});
                        it->second->whisper_sid.clear();
                    }
                    LOG_NOTICE("whisper_reject sid=%s", sid.c_str());
                    return;
                }

                if (type == "whisper_end") {
                    std::string sid = j.value("sid", "");
                    int peer_id = g_whisper.get_peer(sid, s->char_id);
                    if (!g_whisper.end(sid, s->char_id)) return;
                    std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                    // Clear inside the lock — UDP routing reads whisper_sid under shared_lock.
                    s->whisper_sid.clear();
                    auto it = g_by_char_id.find(peer_id);
                    if (it != g_by_char_id.end() && it->second) {
                        send_json(it->second->ws, json{{"type","whisper_ended"},{"sid",sid}});
                        it->second->whisper_sid.clear();
                    }
                    LOG_NOTICE("whisper_end sid=%s", sid.c_str());
                    return;
                }

                if (type == "mute") {
                    if (!session_matches(s, j)) {
                        LOG_DEBUG("ignore mute with wrong session char_id=%d sid=%llu", s->char_id, static_cast<unsigned long long>(s->session_id));
                        return;
                    }
                    s->muted = j.value("value", false);
                    return;
                }

                if (type == "deafen") {
                    if (!session_matches(s, j)) {
                        LOG_DEBUG("ignore deafen with wrong session char_id=%d sid=%llu", s->char_id, static_cast<unsigned long long>(s->session_id));
                        return;
                    }
                    s->deafened = j.value("value", false);
                    if (s->deafened) {
                        s->muted = true;
                    }
                    return;
                }

                if (type == "ptt") {
                    if (!session_matches(s, j)) {
                        LOG_DEBUG("ignore ptt with wrong session char_id=%d sid=%llu", s->char_id, static_cast<unsigned long long>(s->session_id));
                        return;
                    }
                    s->ptt = j.value("value", false);
                    return;
                }

                return;
            }

            if (opCode == uWS::OpCode::BINARY) {
                if (!s->authed) {
                    send_json(ws, json{{"type", "error"}, {"message", "binary before auth"}});
                    return;
                }

                // Header: 1 byte channel + 4 bytes gid + 2 bytes seq = 7 bytes; Opus payload: 1..1500 bytes
                if (message.size() < 8 || message.size() > 1507) {
                    LOG_WARNING("audio bad size char_id=%d size=%zu (drop)", s->char_id, message.size());
                    return;
                }

                const auto* p = reinterpret_cast<const unsigned char*>(message.data());
                const uint8_t channel = p[0];
                if (channel > 4) {
                    LOG_WARNING("audio invalid channel char_id=%d ch=%d (drop)", s->char_id, channel);
                    return;
                }
                const uint32_t gid = read_be_u32(p + 1);
                const uint16_t seq = static_cast<uint16_t>((p[5] << 8) | p[6]);
                const char* pcm = reinterpret_cast<const char*>(p + 7);
                const size_t pcm_bytes = message.size() - 7;

                // Opus TOC sanity — reject malformed / garbage payloads before
                // fanning out to every listener (libopus in listeners can crash
                // on intentionally-crafted bad packets).
                if (!validate_opus_packet(p + 7, pcm_bytes)) {
                    LOG_WARNING("audio invalid opus TOC char_id=%d ip=%s size=%zu (drop)",
                                s->char_id, s->ip.c_str(), pcm_bytes);
                    return;
                }

                if (!s->rate_limit_check()) {
                    LOG_WARNING("rate limit drop char_id=%d violations=%d", s->char_id, s->flood_violations);
                    if (s->is_flooding()) {
                        // Sustained flood (~30 consecutive drops) → ban IP for 5 min
                        LOG_ERROR("FLOOD BAN char_id=%d ip=%s — kicking and banning IP for 5 min",
                                  s->char_id, s->ip.c_str());
                        {
                            std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                            g_flood_bans[s->ip] = { tick_ms() + FLOOD_BAN_DURATION_MS };
                        }
                        send_json(ws, json{{"type","error"},{"message","flood detected — banned"}});
                        ws->end(1008, "flood ban");
                    }
                    return;
                }

                // Enforce client-declared TX state on the server too so a buggy
                // or modified DLL cannot keep relaying voice while locally muted
                // or after releasing push-to-talk.
                if (s->muted || s->deafened || !s->ptt) {
                    LOG_DEBUG("audio drop due to tx state char_id=%d muted=%d deafened=%d ptt=%d",
                              s->char_id, s->muted ? 1 : 0, s->deafened ? 1 : 0, s->ptt ? 1 : 0);
                    return;
                }

                LOG_DEBUG("audio rx char_id=%d ch=%d gid=%u seq=%u opus_bytes=%zu pos=%s(%d,%d) fresh=%d",
                          s->char_id, channel, gid, (unsigned)seq, pcm_bytes,
                          s->map.c_str(), s->x, s->y, has_fresh_position(*s) ? 1 : 0);

                // Build target list using pre-built indexes — O(channel_members)
                // instead of O(all_sessions).  Much faster at 3000+ players.
                // Uses shared_lock: concurrent audio routing reads don't block
                // each other, and the UDP writer only briefly stalls them.
                std::vector<std::pair<ClientSession*, float>> targets;
                {
                    std::shared_lock<std::shared_mutex> lock(g_session_mtx); // read-only

                    auto collect = [&](const std::unordered_set<ClientSession*>& set) {
                        targets.reserve(targets.size() + set.size());
                        for (ClientSession* to : set) {
                            if (!to) continue;
                            if (!should_forward(channel, gid, *s, *to)) continue;
                            float vol = volume_for(channel, *s, *to);
                            if (vol > 0.0f) targets.push_back({to, vol});
                        }
                    };

                    switch (channel) {
                        case 0: { // Normal proximity — only same-map players
                            auto it = g_by_map.find(s->map);
                            if (it != g_by_map.end()) collect(it->second);
                            break;
                        }
                        case 1: { // Party
                            if (s->party_id > 0) {
                                auto it = g_by_party.find(s->party_id);
                                if (it != g_by_party.end()) collect(it->second);
                            }
                            break;
                        }
                        case 2: { // Guild
                            if (s->guild_id > 0) {
                                auto it = g_by_guild.find(s->guild_id);
                                if (it != g_by_guild.end()) collect(it->second);
                            }
                            break;
                        }
                        case 3: { // Room
                            if (s->chat_room_id != 0) {
                                auto it = g_by_room.find(s->chat_room_id);
                                if (it != g_by_room.end()) collect(it->second);
                            }
                            break;
                        }
                        case 4: { // Whisper — forward only to the paired peer
                            if (!s->whisper_sid.empty()) {
                                int peer_id = g_whisper.get_peer(s->whisper_sid, s->char_id);
                                auto it = g_by_char_id.find(peer_id);
                                if (it != g_by_char_id.end() && it->second) {
                                    ClientSession* to = it->second;
                                    if (should_forward(channel, gid, *s, *to)) {
                                        float vol = volume_for(channel, *s, *to);
                                        if (vol > 0.0f) targets.push_back({to, vol});
                                    }
                                }
                            }
                            break;
                        }
                        default: break;
                    }

                    // trim + send under the shared_lock so that the raw ClientSession*
                    // pointers in targets[] remain valid for the duration of send_audio_to.
                    // If we released the lock first, the close handler could free a session
                    // between the lock release and the ws->send() call (use-after-free).
                    trim_audio_targets(channel, s->char_id, targets);
                    LOG_DEBUG("audio targets=%zu for char_id=%d", targets.size(), s->char_id);

                    size_t sent = 0;
                    for (auto& [to, vol] : targets) {
                        if (send_audio_to(to, s->char_id, s->char_name, vol, s->x, s->y, seq, pcm, pcm_bytes))
                            sent++;
                    }
                    if (sent != targets.size()) {
                        LOG_DEBUG("audio partial fanout char_id=%d targets=%zu sent=%zu dropped=%zu",
                                  s->char_id, targets.size(), sent, targets.size() - sent);
                    }
                }
                return;
            }
        },

        .close = [](auto* ws, int code, std::string_view) {
            auto* s = ws->getUserData();

            // Notify whisper peer on disconnect
            {
                std::lock_guard<std::shared_mutex> lk2(g_session_mtx);
                if (!s->whisper_sid.empty()) {
                    int peer_id = g_whisper.get_peer(s->whisper_sid, s->char_id);
                    g_whisper.end(s->whisper_sid, s->char_id);
                    auto it = g_by_char_id.find(peer_id);
                    if (it != g_by_char_id.end() && it->second) {
                        send_json_deferred(it->second, json{{"type","whisper_ended"},{"sid",s->whisper_sid}});
                        it->second->whisper_sid.clear();
                    }
                }
            }

            {
                std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                idx_remove(s);   // remove from all channel indexes
                g_by_ws.erase(ws);
                if (s->char_id != 0) {
                    auto it = g_by_char_id.find(s->char_id);
                    if (it != g_by_char_id.end() && it->second == s)
                        g_by_char_id.erase(it);
                }
            }

            if (s->authed) g_player_count--;
            if (g_player_count < 0) g_player_count = 0;

            const char* reason = (code == 1000) ? "normal"
                               : (code == 1001) ? "going away"
                               : (code == 1006) ? "lost connection"
                               : (code == 1011) ? "server error"
                               : "unknown";
            LOG_INFO("(char_id=%d aid=%d sid=%llu ip=%s) disconnected [%s]  [online: %d]",
                     s->char_id, s->account_id, static_cast<unsigned long long>(s->session_id), s->ip.c_str(), reason, g_player_count);
        }
    }).listen(g_cfg.voice_ip.c_str(), g_cfg.voice_port, [](auto* token) {
        if (token) LOG_STATUS("Listening on %s:%d", g_cfg.voice_ip.c_str(), g_cfg.voice_port);
        else       LOG_ERROR("Failed to listen on %s:%d", g_cfg.voice_ip.c_str(), g_cfg.voice_port);
    }).run();
}
