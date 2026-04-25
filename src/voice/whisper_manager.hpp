#pragma once
#include "player.hpp"
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>
#include <mutex>
#include <chrono>

struct WhisperSessionState {
    std::string session_id;
    int  char_id_a;   // requester
    int  char_id_b;   // target
    bool accepted    = false;
    std::chrono::steady_clock::time_point created_at;
};

class WhisperManager {
public:
    // A requests whisper to B → returns session_id (pending)
    std::string request(int char_id_a, int char_id_b);

    // B accepts
    bool accept(const std::string& session_id, int char_id_b);

    // B rejects
    bool reject(const std::string& session_id, int char_id_b);

    // End session (either side)
    bool end(const std::string& session_id, int char_id);

    // Check if session is valid and accepted
    bool is_active(const std::string& session_id) const;

    // Get peer char_id for a session
    int  get_peer(const std::string& session_id, int my_char_id) const;

    // Cleanup expired pending sessions (silent discard)
    void cleanup_expired(int timeout_seconds);

    // Like cleanup_expired but returns {char_id_a, char_id_b} pairs so the
    // caller can notify both sides before dropping the sessions.
    std::vector<std::pair<int,int>> collect_expired(int timeout_seconds);

private:
    mutable std::mutex mtx_;
    std::unordered_map<std::string, WhisperSessionState> sessions_;

    static std::string generate_id();
};

extern WhisperManager g_whisper;
