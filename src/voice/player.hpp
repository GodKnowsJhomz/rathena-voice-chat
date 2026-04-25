#pragma once
#include <string>
#include <vector>
#include <utility>
#include <cmath>
#include <limits>
#include <cstdint>

// Channel types (matches binary protocol byte 0)
enum class Channel : uint8_t {
    Normal  = 0x00,
    Party   = 0x01,
    Guild   = 0x02,
    Room    = 0x03,
    Whisper = 0x04
};

struct WhisperSession {
    std::string session_id;
    int         peer_char_id;
};

struct Player;
using ListenerEntry = std::pair<Player*, float>; // player, volume

struct Player {
    // WebSocket handle (uWS::WebSocket<false, true, UserData>*)
    // stored as void* to avoid template dependency
    void* ws = nullptr;

    // Identity
    int         account_id = 0;
    int         char_id    = 0;
    std::string char_name;

    // Position
    std::string map;
    int x = 0;
    int y = 0;

    // Groups
    int         party_id = 0;   // 0 = no party
    int         guild_id = 0;   // 0 = no guild
    std::string room_id;        // "" = no custom room

    // Whisper sessions (can have multiple)
    std::vector<WhisperSession> whisper_sessions;

    // State
    bool authenticated = false;
    bool muted         = false;
    bool ptt_active    = false;

    // Cached proximity listeners — updated by ProximityWorker every 500ms
    std::vector<ListenerEntry> cached_listeners;

    // ── Helpers ──────────────────────────────────────────────

    float distance_to(const Player& o) const {
        if (map != o.map) return std::numeric_limits<float>::infinity();
        float dx = static_cast<float>(x - o.x);
        float dy = static_cast<float>(y - o.y);
        return std::sqrt(dx * dx + dy * dy);
    }

    float volume_for(const Player& o, float full_r, float max_r) const {
        float d = distance_to(o);
        if (d <= full_r) return 1.0f;
        if (d >= max_r)  return 0.0f;
        return 1.0f - (d - full_r) / (max_r - full_r);
    }

    // Find whisper session by session_id
    WhisperSession* find_whisper(const std::string& sid) {
        for (auto& s : whisper_sessions)
            if (s.session_id == sid) return &s;
        return nullptr;
    }

    // Find whisper session by peer char_id
    WhisperSession* find_whisper_by_peer(int peer_id) {
        for (auto& s : whisper_sessions)
            if (s.peer_char_id == peer_id) return &s;
        return nullptr;
    }
};
