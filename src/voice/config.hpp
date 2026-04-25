#pragma once
#include <string>
#include <vector>
#include <unordered_set>
#include <atomic>

// One entry in blocked_maps — all specified conditions must match (AND logic)
struct BlockedMapRule {
    std::string map_name;
    int  min_level = -1;                    // -1 = no restriction
    int  max_level = -1;                    // -1 = no restriction
    std::unordered_set<int> jobs;           // empty = any job
    std::unordered_set<int> group_ids;      // empty = any group

    // Returns true if this rule blocks the given player
    bool matches(int level, int job, int group_id) const {
        if (min_level >= 0 && level < min_level) return false;
        if (max_level >= 0 && level > max_level) return false;
        if (!jobs.empty()      && jobs.count(job)           == 0) return false;
        if (!group_ids.empty() && group_ids.count(group_id) == 0) return false;
        return true;
    }
};

struct Config {
    std::string ip          = "0.0.0.0";
    int         port        = 7000;
    int         api_port    = 7001;
    float       full_range  = 5.0f;
    float       max_range   = 15.0f;
    int         prox_update_ms = 500;
    int         whisper_timeout = 300;
    int         log_level   = 2;

    std::vector<BlockedMapRule>  blocked_maps;
    std::unordered_set<int>      whisper_bypass_groups; // group_ids that skip whisper accept

    // Returns true if voice should be blocked for this player on this map
    bool is_map_blocked(const std::string& map, int level = -1, int job = -1, int group_id = -1) const {
        for (const auto& rule : blocked_maps)
            if (rule.map_name == map && rule.matches(level, job, group_id))
                return true;
        return false;
    }

    bool is_whisper_bypass(int group_id) const {
        return whisper_bypass_groups.count(group_id) > 0;
    }

    static Config load(const std::string& path);
};

extern Config g_config;
extern std::string         g_conf_path;
extern std::atomic<bool>   g_reload_requested;

#define LOG_ERROR 1
#define LOG_INFO  2
#define LOG_DEBUG 3

#define LOG(level, ...) do { \
    if (g_config.log_level >= level) { \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while(0)
