#include "room_manager.hpp"
#include "config.hpp"
#include <algorithm>
#include <mutex>

RoomManager g_rooms;

// ── Player lifecycle ─────────────────────────────────────────────

void RoomManager::player_join(Player* p) {
    std::unique_lock lock(mtx_);
    all_players_.insert(p);
    by_char_id_[p->char_id]  = p;
    if (!p->char_name.empty()) by_name_[p->char_name] = p;
}

void RoomManager::player_leave(Player* p) {
    std::unique_lock lock(mtx_);

    // Remove from all rooms
    if (!p->map.empty())     map_rooms_[p->map].erase(p);
    if (p->party_id)         party_rooms_[p->party_id].erase(p);
    if (p->guild_id)         guild_rooms_[p->guild_id].erase(p);
    if (!p->room_id.empty()) custom_rooms_[p->room_id].erase(p);

    by_char_id_.erase(p->char_id);
    if (!p->char_name.empty()) by_name_.erase(p->char_name);
    all_players_.erase(p);
}

void RoomManager::player_move(Player* p, const std::string& map, int x, int y) {
    std::unique_lock lock(mtx_);
    // Leave old map
    if (!p->map.empty())
        map_rooms_[p->map].erase(p);
    // Update position
    p->map = map;
    p->x   = x;
    p->y   = y;
    // Join new map
    map_rooms_[map].insert(p);
}

void RoomManager::player_set_party(Player* p, int party_id) {
    std::unique_lock lock(mtx_);
    if (p->party_id) party_rooms_[p->party_id].erase(p);
    p->party_id = party_id;
    if (party_id) party_rooms_[party_id].insert(p);
}

void RoomManager::player_set_guild(Player* p, int guild_id) {
    std::unique_lock lock(mtx_);
    if (p->guild_id) guild_rooms_[p->guild_id].erase(p);
    p->guild_id = guild_id;
    if (guild_id) guild_rooms_[guild_id].insert(p);
}

void RoomManager::player_join_room(Player* p, const std::string& room_id) {
    std::unique_lock lock(mtx_);
    if (!p->room_id.empty()) custom_rooms_[p->room_id].erase(p);
    p->room_id = room_id;
    if (!room_id.empty()) custom_rooms_[room_id].insert(p);
}

void RoomManager::player_leave_room(Player* p) {
    player_join_room(p, "");
}

// ── Audio routing ────────────────────────────────────────────────

std::vector<ListenerEntry> RoomManager::get_proximity_listeners(const Player* speaker) const {
    std::shared_lock lock(mtx_);
    return speaker->cached_listeners;
}

std::vector<Player*> RoomManager::get_party_members(int party_id, int exclude_char_id) const {
    std::shared_lock lock(mtx_);
    std::vector<Player*> result;
    auto it = party_rooms_.find(party_id);
    if (it == party_rooms_.end()) return result;
    for (auto* p : it->second)
        if (p->char_id != exclude_char_id) result.push_back(p);
    return result;
}

std::vector<Player*> RoomManager::get_guild_members(int guild_id, int exclude_char_id) const {
    std::shared_lock lock(mtx_);
    std::vector<Player*> result;
    auto it = guild_rooms_.find(guild_id);
    if (it == guild_rooms_.end()) return result;
    for (auto* p : it->second)
        if (p->char_id != exclude_char_id) result.push_back(p);
    return result;
}

std::vector<Player*> RoomManager::get_room_members(const std::string& room_id, int exclude_char_id) const {
    std::shared_lock lock(mtx_);
    std::vector<Player*> result;
    auto it = custom_rooms_.find(room_id);
    if (it == custom_rooms_.end()) return result;
    for (auto* p : it->second)
        if (p->char_id != exclude_char_id) result.push_back(p);
    return result;
}

// ── Proximity cache ──────────────────────────────────────────────

void RoomManager::update_proximity_cache(float full_range, float max_range) {
    std::unique_lock lock(mtx_);
    for (auto& [map_name, players] : map_rooms_) {
        for (auto* speaker : players) {
            speaker->cached_listeners.clear();
            for (auto* listener : players) {
                if (listener == speaker) continue;
                float vol = speaker->volume_for(*listener, full_range, max_range);
                if (vol > 0.0f)
                    speaker->cached_listeners.emplace_back(listener, vol);
            }
        }
    }
}

// ── Lookup ───────────────────────────────────────────────────────

Player* RoomManager::find_by_char_id(int char_id) const {
    std::shared_lock lock(mtx_);
    auto it = by_char_id_.find(char_id);
    return (it != by_char_id_.end()) ? it->second : nullptr;
}

Player* RoomManager::find_by_name(const std::string& name) const {
    std::shared_lock lock(mtx_);
    auto it = by_name_.find(name);
    return (it != by_name_.end()) ? it->second : nullptr;
}

size_t RoomManager::online_count() const {
    std::shared_lock lock(mtx_);
    return all_players_.size();
}
