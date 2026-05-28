#pragma once
#include "map.hpp"
#include "pc.hpp"

void voice_bridge_init();
void voice_bridge_final();

void voice_bridge_send_join(map_session_data* sd);
void voice_bridge_send_map_pos(map_session_data* sd);
void voice_bridge_send_leave(map_session_data* sd);
void voice_bridge_send_room_join(map_session_data* sd, int room_id);
void voice_bridge_send_room_leave(map_session_data* sd);
void voice_bridge_send_reload_config();
void voice_bridge_send_reload_db();
void voice_bridge_send_guild_war_state(bool active);
void voice_bridge_send_admin_mute(int char_id, int duration_sec);  // 0 = permanent
void voice_bridge_send_admin_unmute(int char_id);
void voice_bridge_send_admin_ban(int account_id, int duration_sec); // 0 = permanent
void voice_bridge_send_admin_unban(int account_id);
void voice_bridge_send_admin_ban_by_name(const char* char_name, int duration_sec);
void voice_bridge_send_admin_unban_by_name(const char* char_name);
// Returns true when the UDP packet was queued, false when the voice
// bridge is not ready (voice server unreachable / startup not complete).
// Script item paths should refund / refuse to consume when this is false.
bool voice_bridge_send_grant_license(int account_id, int duration_sec);
bool voice_bridge_send_revoke_license(int account_id);
void voice_bridge_send_block_by_name(int blocker_account_id, const char* blocked_name);
void voice_bridge_send_unblock_by_name(int blocker_account_id, const char* blocked_name);
