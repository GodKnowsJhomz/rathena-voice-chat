#include "whisper_manager.hpp"
#include "config.hpp"
#include <random>
#include <sstream>
#include <iomanip>

WhisperManager g_whisper;

std::string WhisperManager::generate_id() {
    static std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << "w_" << std::hex << std::setw(16) << std::setfill('0') << dist(rng);
    return oss.str();
}

std::string WhisperManager::request(int char_id_a, int char_id_b) {
    std::lock_guard lock(mtx_);
    // ยกเลิก pending เดิมถ้ามี
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        auto& s = it->second;
        if (s.char_id_a == char_id_a && s.char_id_b == char_id_b && !s.accepted)
            it = sessions_.erase(it);
        else
            ++it;
    }
    auto sid = generate_id();
    WhisperSessionState st;
    st.session_id = sid;
    st.char_id_a  = char_id_a;
    st.char_id_b  = char_id_b;
    st.accepted   = false;
    st.created_at = std::chrono::steady_clock::now();
    sessions_[sid] = st;
    return sid;
}

bool WhisperManager::accept(const std::string& sid, int char_id_b) {
    std::lock_guard lock(mtx_);
    auto it = sessions_.find(sid);
    if (it == sessions_.end()) return false;
    if (it->second.char_id_b != char_id_b) return false;
    it->second.accepted = true;
    return true;
}

bool WhisperManager::reject(const std::string& sid, int char_id_b) {
    std::lock_guard lock(mtx_);
    auto it = sessions_.find(sid);
    if (it == sessions_.end()) return false;
    if (it->second.char_id_b != char_id_b) return false;
    sessions_.erase(it);
    return true;
}

bool WhisperManager::end(const std::string& sid, int char_id) {
    std::lock_guard lock(mtx_);
    auto it = sessions_.find(sid);
    if (it == sessions_.end()) return false;
    auto& s = it->second;
    if (s.char_id_a != char_id && s.char_id_b != char_id) return false;
    sessions_.erase(it);
    return true;
}

bool WhisperManager::is_active(const std::string& sid) const {
    std::lock_guard lock(mtx_);
    auto it = sessions_.find(sid);
    return it != sessions_.end() && it->second.accepted;
}

int WhisperManager::get_peer(const std::string& sid, int my_char_id) const {
    std::lock_guard lock(mtx_);
    auto it = sessions_.find(sid);
    if (it == sessions_.end()) return 0;
    auto& s = it->second;
    if (s.char_id_a == my_char_id) return s.char_id_b;
    if (s.char_id_b == my_char_id) return s.char_id_a;
    return 0;
}

void WhisperManager::cleanup_expired(int timeout_sec) {
    if (timeout_sec <= 0) return;
    std::lock_guard lock(mtx_);
    auto now = std::chrono::steady_clock::now();
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
            now - it->second.created_at).count();
        if (age > timeout_sec)
            it = sessions_.erase(it);
        else
            ++it;
    }
}

std::vector<std::pair<int,int>> WhisperManager::collect_expired(int timeout_sec) {
    std::vector<std::pair<int,int>> result;
    if (timeout_sec <= 0) return result;
    std::lock_guard lock(mtx_);
    auto now = std::chrono::steady_clock::now();
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
            now - it->second.created_at).count();
        if (age > timeout_sec) {
            result.emplace_back(it->second.char_id_a, it->second.char_id_b);
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
    return result;
}
