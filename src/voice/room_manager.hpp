#pragma once
#include "player.hpp"
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>
#include <shared_mutex>

class RoomManager {
public:
    // ── Player lifecycle ──────────────────────────────────────
    void player_join(Player* p);
    void player_leave(Player* p);

    // Called when position changes
    void player_move(Player* p, const std::string& map, int x, int y);

    // Called when party/guild changes (from HTTP API)
    void player_set_party(Player* p, int party_id);
    void player_set_guild(Player* p, int guild_id);
    void player_join_room(Player* p, const std::string& room_id);
    void player_leave_room(Player* p);

    // ── Audio routing ─────────────────────────────────────────
    // Returns nearby players with volume (from cache)
    std::vector<ListenerEntry> get_proximity_listeners(const Player* speaker) const;

    // Returns all members in party/guild/room except speaker
    std::vector<Player*> get_party_members(int party_id, int exclude_char_id) const;
    std::vector<Player*> get_guild_members(int guild_id, int exclude_char_id) const;
    std::vector<Player*> get_room_members(const std::string& room_id, int exclude_char_id) const;

    // ── Proximity cache (called from worker thread) ───────────
    void update_proximity_cache(float full_range, float max_range);

    // ── Lookup ────────────────────────────────────────────────
    Player* find_by_char_id(int char_id) const;
    Player* find_by_name(const std::string& name) const;

    size_t online_count() const;

private:
    mutable std::shared_mutex mtx_;

    std::unordered_set<Player*>                                  all_players_;
    std::unordered_map<int, Player*>                             by_char_id_;
    std::unordered_map<std::string, Player*>                     by_name_;
    std::unordered_map<std::string, std::unordered_set<Player*>> map_rooms_;
    std::unordered_map<int,         std::unordered_set<Player*>> party_rooms_;
    std::unordered_map<int,         std::unordered_set<Player*>> guild_rooms_;
    std::unordered_map<std::string, std::unordered_set<Player*>> custom_rooms_;
};

extern RoomManager g_rooms;
