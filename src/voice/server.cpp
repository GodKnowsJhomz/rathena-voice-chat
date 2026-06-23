#include "voice_transport.hpp"
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
#include <random>
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
    std::string voice_api_ip         = "127.0.0.1";
    int         voice_api_port       = 7001;
    std::string voice_bridge_secret;
    float       proximity_full_range = 1.0f;
    float       proximity_max_range  = 14.0f;
    int         proximity_update_ms  = 50;
    int         whisper_timeout      = 300;
    int         db_refresh_s         = 5;
    int         log_level            = 2;
    int         max_targets_normal   = 64;
    int         max_targets_group    = 128;
    int         max_targets_room     = 64;
    int         audio_backpressure_kb = 128;
    int         speaking_hat_timeout_ms = 900;
    bool        war_mode_enabled     = true;
    bool        war_allow_whisper    = true;
    bool        voice_license_required = false; // gate audio TX behind a voice_licenses entry
    bool        voice_block_bidirectional = false; // A blocks B → neither hears the other
    int         voice_block_alert_threshold = 0;   // distinct blockers to alert GMs (0 = off)
    bool        voice_ignore_sync = false;          // text /ex ignore also blocks voice
    bool        login_id1_strict  = true;

    std::string db_host       = "127.0.0.1";
    int         db_port       = 3306;
    std::string db_user       = "ragnarok";
    std::string db_pass       = "";
    std::string db_name       = "ragnarok";
    std::string db_char_table = "char";

    std::string client_secret = "";
} g_cfg;

// Hard cutoff guard for practical "14 cells exact" behavior.
// Why: position and audio arrive on separate packets, so at the edge of range
// the server can still momentarily use a stale position from ~1 tick earlier.
// Using a small guard keeps practical hearing closer to what the player sees.
static constexpr float    PROXIMITY_EDGE_GUARD_CELLS = 0.75f;
// Used only as an index-health guard. Proximity itself uses the most recent
// map_pos as the current truth until a newer map_pos or disconnect arrives.
static constexpr uint32_t POSITION_STALE_MS          = 10000;

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
        if (key == "import") {
            if (!val.empty()) {
                int sub = parse_kv_file(val.c_str(), handler);
                if (sub < 0)
                    printf("[Config] WARNING: import not found: %s\n", val.c_str());
                else
                    matched += sub;
            }
            continue;
        }
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
        else if (key == "voice_api_ip" || key == "voice_api_bind_ip") {
            if (!val.empty()) g_cfg.voice_api_ip = val; return true;
        }
        else if (key == "voice_api_port") {
            if (!val.empty()) g_cfg.voice_api_port = std::stoi(val); return true;
        }
        else if (key == "voice_bridge_secret") {
            g_cfg.voice_bridge_secret = val; return true;
        }
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
        else if (key == "voice_speaking_hat_timeout_ms" || key == "voice_speaking_timeout_ms") {
            if (!val.empty()) g_cfg.speaking_hat_timeout_ms = std::stoi(val); return true;
        }
        else if (key == "voice_war_mode_enabled") {
            if (!val.empty()) g_cfg.war_mode_enabled = (std::stoi(val) != 0); return true;
        }
        else if (key == "voice_war_allow_whisper") {
            if (!val.empty()) g_cfg.war_allow_whisper = (std::stoi(val) != 0); return true;
        }
        else if (key == "voice_license_required") {
            if (!val.empty()) g_cfg.voice_license_required = (std::stoi(val) != 0); return true;
        }
        else if (key == "voice_block_bidirectional") {
            if (!val.empty()) g_cfg.voice_block_bidirectional = (std::stoi(val) != 0); return true;
        }
        else if (key == "voice_block_alert_threshold") {
            if (!val.empty()) g_cfg.voice_block_alert_threshold = std::stoi(val); return true;
        }
        else if (key == "voice_ignore_sync") {
            if (!val.empty()) g_cfg.voice_ignore_sync = (std::stoi(val) != 0); return true;
        }
        else if (key == "voice_login_id1_strict") {
            if (!val.empty()) g_cfg.login_id1_strict = (std::stoi(val) != 0); return true;
        }
        else if (key == "voice_client_secret") {
            g_cfg.client_secret = val; return true;
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
    int party_id   = 0;
    int guild_id   = 0;
    int account_id = 0;
    bool ok        = false;
};

static CharInfo db_get_char_info(int char_id) {
    std::lock_guard<std::mutex> lock(g_db_mtx);
    if (!g_db) return {};

    if (mysql_ping(g_db) != 0) {
        LOG_WARNING("DB ping failed, reconnecting...");
        if (!db_connect()) return {};
    }

    std::string query = "SELECT `name`,`party_id`,`guild_id`,`account_id` FROM `"
                      + g_cfg.db_char_table
                      + "` WHERE `char_id`=" + std::to_string(char_id) + " LIMIT 1";

    if (mysql_query(g_db, query.c_str()) != 0) {
        LOG_ERROR("DB query error: %s", mysql_error(g_db));
        if (!db_connect()) return {};
		if (mysql_query(g_db, query.c_str()) != 0) return {};
    }

    MYSQL_RES* res = mysql_store_result(g_db);
    if (!res) return {};

    CharInfo info;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (row) {
        info.char_name  = row[0] ? row[0] : "";
        info.party_id   = row[1] ? std::atoi(row[1]) : 0;
        info.guild_id   = row[2] ? std::atoi(row[2]) : 0;
        info.account_id = row[3] ? std::atoi(row[3]) : 0;
        info.ok         = true;
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

static int db_lookup_account_id_by_name(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_db_mtx);
    if (!g_db) return 0;
    if (mysql_ping(g_db) != 0) {
        LOG_WARNING("DB ping failed, reconnecting...");
        if (!db_connect()) return 0;
    }
    char escaped[128] = {};
    mysql_real_escape_string(g_db, escaped, name.c_str(),
                             (unsigned long)std::min(name.size(), sizeof(escaped) / 2 - 1));
    std::string query = "SELECT `account_id` FROM `"
                      + g_cfg.db_char_table
                      + "` WHERE `name`='" + escaped + "' LIMIT 1";
    if (mysql_query(g_db, query.c_str()) != 0) return 0;
    MYSQL_RES* res = mysql_store_result(g_db);
    if (!res) return 0;
    int aid = 0;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (row && row[0]) aid = std::atoi(row[0]);
    mysql_free_result(res);
    return aid;
}

// ── Voice ban DB ──────────────────────────────────────────────────────────────
static void db_ensure_ban_table() {
    std::lock_guard<std::mutex> lock(g_db_mtx);
    if (!g_db) return;
    const char* sql =
        "CREATE TABLE IF NOT EXISTS `voice_bans` ("
        " `account_id`   INT          NOT NULL,"
        " `banned_by`    VARCHAR(24)  NOT NULL DEFAULT '',"
        " `banned_at`    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        " `banned_until` DATETIME     NULL DEFAULT NULL,"
        " PRIMARY KEY (`account_id`)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    if (mysql_query(g_db, sql) != 0)
        LOG_ERROR("voice_bans table create failed: %s", mysql_error(g_db));
}

static void db_load_bans(std::unordered_map<int, time_t>& out) {
    std::lock_guard<std::mutex> lock(g_db_mtx);
    if (!g_db) return;
    const char* sql =
        "SELECT `account_id`, UNIX_TIMESTAMP(`banned_until`) "
        "FROM `voice_bans` "
        "WHERE `banned_until` IS NULL OR `banned_until` > NOW()";
    if (mysql_query(g_db, sql) != 0) {
        LOG_ERROR("db_load_bans: %s", mysql_error(g_db));
        return;
    }
    MYSQL_RES* res = mysql_store_result(g_db);
    if (!res) return;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        if (!row[0]) continue;
        int aid = std::atoi(row[0]);
        time_t until = (row[1] && row[1][0]) ? (time_t)std::stoll(row[1]) : 0;
        out[aid] = until;
    }
    mysql_free_result(res);
    LOG_INFO("voice_bans loaded %zu entry(ies)", out.size());
}

static void db_insert_ban(int account_id, const std::string& banned_by, time_t until) {
    std::lock_guard<std::mutex> lock(g_db_mtx);
    if (!g_db) return;
    char by_esc[100] = {};
    mysql_real_escape_string(g_db, by_esc, banned_by.c_str(),
                             (unsigned long)std::min(banned_by.size(), sizeof(by_esc) / 2 - 1));
    std::string sql;
    if (until == 0) {
        sql = "INSERT INTO `voice_bans` (`account_id`,`banned_by`) VALUES ("
            + std::to_string(account_id) + ",'" + by_esc + "') "
            "ON DUPLICATE KEY UPDATE `banned_by`='" + by_esc + "',`banned_until`=NULL";
    } else {
        sql = "INSERT INTO `voice_bans` (`account_id`,`banned_by`,`banned_until`) VALUES ("
            + std::to_string(account_id) + ",'" + by_esc + "',FROM_UNIXTIME("
            + std::to_string((long long)until) + ")) "
            "ON DUPLICATE KEY UPDATE `banned_by`='" + by_esc + "',`banned_until`=FROM_UNIXTIME("
            + std::to_string((long long)until) + ")";
    }
    if (mysql_query(g_db, sql.c_str()) != 0)
        LOG_ERROR("db_insert_ban: %s", mysql_error(g_db));
}

static void db_delete_ban(int account_id) {
    std::lock_guard<std::mutex> lock(g_db_mtx);
    if (!g_db) return;
    std::string sql = "DELETE FROM `voice_bans` WHERE `account_id`=" + std::to_string(account_id);
    if (mysql_query(g_db, sql.c_str()) != 0)
        LOG_ERROR("db_delete_ban: %s", mysql_error(g_db));
}

// Maintenance-only delete that ignores rows which have been re-issued after
// the in-memory expiry sweep but before this DELETE runs.
static void db_delete_expired_ban(int account_id) {
    std::lock_guard<std::mutex> lock(g_db_mtx);
    if (!g_db) return;
    std::string sql = "DELETE FROM `voice_bans` WHERE `account_id`="
        + std::to_string(account_id)
        + " AND `banned_until` IS NOT NULL AND `banned_until` <= NOW()";
    if (mysql_query(g_db, sql.c_str()) != 0)
        LOG_ERROR("db_delete_expired_ban: %s", mysql_error(g_db));
}

// Called at startup to clean up rows that elapsed while the voice server
// was offline. Without this they would stay in the DB forever because
// db_load_bans / db_load_licenses skip expired rows (so the maintenance
// sweep never sees them in memory).
static void db_purge_expired_rows() {
    std::lock_guard<std::mutex> lock(g_db_mtx);
    if (!g_db) return;
    const char* q1 = "DELETE FROM `voice_bans`     WHERE `banned_until` IS NOT NULL AND `banned_until` <= NOW()";
    const char* q2 = "DELETE FROM `voice_licenses` WHERE `expires_at`   IS NOT NULL AND `expires_at`   <= NOW()";
    if (mysql_query(g_db, q1) != 0)
        LOG_ERROR("db_purge_expired_rows (bans): %s", mysql_error(g_db));
    else if (mysql_affected_rows(g_db) > 0)
        LOG_INFO("startup cleanup: purged %llu expired voice_bans rows",
                 (unsigned long long)mysql_affected_rows(g_db));
    if (mysql_query(g_db, q2) != 0)
        LOG_ERROR("db_purge_expired_rows (licenses): %s", mysql_error(g_db));
    else if (mysql_affected_rows(g_db) > 0)
        LOG_INFO("startup cleanup: purged %llu expired voice_licenses rows",
                 (unsigned long long)mysql_affected_rows(g_db));
}

// ── Voice license DB ──────────────────────────────────────────────────────────
static void db_ensure_license_table() {
    std::lock_guard<std::mutex> lock(g_db_mtx);
    if (!g_db) return;
    const char* sql =
        "CREATE TABLE IF NOT EXISTS `voice_licenses` ("
        "  `account_id` INT NOT NULL,"
        "  `granted_by` VARCHAR(24) NOT NULL DEFAULT '',"
        "  `granted_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "  `expires_at` DATETIME NULL DEFAULT NULL,"
        "  PRIMARY KEY (`account_id`)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    if (mysql_query(g_db, sql) != 0)
        LOG_ERROR("db_ensure_license_table: %s", mysql_error(g_db));
}

static void db_load_licenses(std::unordered_map<int, time_t>& out) {
    std::lock_guard<std::mutex> lock(g_db_mtx);
    if (!g_db) return;
    const char* sql =
        "SELECT `account_id`, UNIX_TIMESTAMP(`expires_at`) "
        "FROM `voice_licenses` "
        "WHERE `expires_at` IS NULL OR `expires_at` > NOW()";
    if (mysql_query(g_db, sql) != 0) {
        LOG_ERROR("db_load_licenses: %s", mysql_error(g_db));
        return;
    }
    MYSQL_RES* res = mysql_store_result(g_db);
    if (!res) return;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        if (!row[0]) continue;
        int aid = std::atoi(row[0]);
        time_t until = (row[1] && row[1][0]) ? (time_t)std::stoll(row[1]) : 0;
        out[aid] = until;
    }
    mysql_free_result(res);
    LOG_INFO("voice_licenses loaded %zu entry(ies)", out.size());
}

static void db_insert_license(int account_id, const std::string& granted_by, time_t until) {
    std::lock_guard<std::mutex> lock(g_db_mtx);
    if (!g_db) return;
    char by_esc[100] = {};
    mysql_real_escape_string(g_db, by_esc, granted_by.c_str(),
                             (unsigned long)std::min(granted_by.size(), sizeof(by_esc) / 2 - 1));
    std::string sql;
    if (until == 0) {
        sql = "INSERT INTO `voice_licenses` (`account_id`,`granted_by`) VALUES ("
            + std::to_string(account_id) + ",'" + by_esc + "') "
            "ON DUPLICATE KEY UPDATE `granted_by`='" + by_esc + "',`expires_at`=NULL";
    } else {
        sql = "INSERT INTO `voice_licenses` (`account_id`,`granted_by`,`expires_at`) VALUES ("
            + std::to_string(account_id) + ",'" + by_esc + "',FROM_UNIXTIME("
            + std::to_string((long long)until) + ")) "
            "ON DUPLICATE KEY UPDATE `granted_by`='" + by_esc + "',`expires_at`=FROM_UNIXTIME("
            + std::to_string((long long)until) + ")";
    }
    if (mysql_query(g_db, sql.c_str()) != 0)
        LOG_ERROR("db_insert_license: %s", mysql_error(g_db));
}

static void db_delete_license(int account_id) {
    std::lock_guard<std::mutex> lock(g_db_mtx);
    if (!g_db) return;
    std::string sql = "DELETE FROM `voice_licenses` WHERE `account_id`=" + std::to_string(account_id);
    if (mysql_query(g_db, sql.c_str()) != 0)
        LOG_ERROR("db_delete_license: %s", mysql_error(g_db));
}

// ── Voice block DB (player-driven block list) ────────────────────────────────
static void db_ensure_block_table() {
    std::lock_guard<std::mutex> lock(g_db_mtx);
    if (!g_db) return;
    const char* sql =
        "CREATE TABLE IF NOT EXISTS `voice_blocks` ("
        "  `blocker_account_id` INT NOT NULL,"
        "  `blocked_account_id` INT NOT NULL,"
        "  `blocked_name`       VARCHAR(24) NOT NULL DEFAULT '',"
        "  `created_at`         DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "  PRIMARY KEY (`blocker_account_id`, `blocked_account_id`),"
        "  INDEX (`blocker_account_id`)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    if (mysql_query(g_db, sql) != 0)
        LOG_ERROR("db_ensure_block_table: %s", mysql_error(g_db));
}

static void db_load_blocks(std::unordered_map<int, std::unordered_set<int>>& out) {
    std::lock_guard<std::mutex> lock(g_db_mtx);
    if (!g_db) return;
    const char* sql = "SELECT `blocker_account_id`, `blocked_account_id` FROM `voice_blocks`";
    if (mysql_query(g_db, sql) != 0) {
        LOG_ERROR("db_load_blocks: %s", mysql_error(g_db));
        return;
    }
    MYSQL_RES* res = mysql_store_result(g_db);
    if (!res) return;
    size_t total = 0;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        if (!row[0] || !row[1]) continue;
        int blocker = std::atoi(row[0]);
        int blocked = std::atoi(row[1]);
        if (blocker > 0 && blocked > 0) {
            out[blocker].insert(blocked);
            ++total;
        }
    }
    mysql_free_result(res);
    LOG_INFO("voice_blocks loaded %zu entry(ies) across %zu blocker(s)", total, out.size());
}

static void db_insert_block(int blocker_aid, int blocked_aid, const std::string& blocked_name) {
    std::lock_guard<std::mutex> lock(g_db_mtx);
    if (!g_db) return;
    char name_esc[100] = {};
    mysql_real_escape_string(g_db, name_esc, blocked_name.c_str(),
                             (unsigned long)std::min(blocked_name.size(), sizeof(name_esc) / 2 - 1));
    std::string sql =
        "INSERT INTO `voice_blocks` (`blocker_account_id`,`blocked_account_id`,`blocked_name`) VALUES ("
        + std::to_string(blocker_aid) + "," + std::to_string(blocked_aid) + ",'" + name_esc + "') "
        "ON DUPLICATE KEY UPDATE `blocked_name`='" + name_esc + "'";
    if (mysql_query(g_db, sql.c_str()) != 0)
        LOG_ERROR("db_insert_block: %s", mysql_error(g_db));
}

static void db_delete_block(int blocker_aid, int blocked_aid) {
    std::lock_guard<std::mutex> lock(g_db_mtx);
    if (!g_db) return;
    std::string sql = "DELETE FROM `voice_blocks` WHERE `blocker_account_id`="
        + std::to_string(blocker_aid)
        + " AND `blocked_account_id`=" + std::to_string(blocked_aid);
    if (mysql_query(g_db, sql.c_str()) != 0)
        LOG_ERROR("db_delete_block: %s", mysql_error(g_db));
}

// Same as db_delete_license but only matches expired rows. Used by the
// maintenance sweep so a race-condition re-grant that lands between the
// in-memory erase and this DELETE does not nuke a freshly-issued license.
static void db_delete_expired_license(int account_id) {
    std::lock_guard<std::mutex> lock(g_db_mtx);
    if (!g_db) return;
    std::string sql = "DELETE FROM `voice_licenses` WHERE `account_id`="
        + std::to_string(account_id)
        + " AND `expires_at` IS NOT NULL AND `expires_at` <= NOW()";
    if (mysql_query(g_db, sql.c_str()) != 0)
        LOG_ERROR("db_delete_expired_license: %s", mysql_error(g_db));
}

struct ClientSession;
using VoiceSocket = VoiceTcp::Connection<false, true, ClientSession>;

struct ClientSession {
    VoiceSocket* ws = nullptr;
    int account_id = 0;
    int char_id = 0;
    uint32_t login_id1 = 0;
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

    // Advisory grace window — in a cold-start login the DLL's auth can race
    // ahead of the map server's UDP auth_advisory. We accept provisionally and
    // require the advisory to arrive within ADVISORY_GRACE_MS or we kick. If
    // it arrives with a mismatched account_id (real spoof), we kick immediately.
    bool     awaiting_advisory        = false;
    uint32_t advisory_wait_tick       = 0;
    bool     auth_ok_pending          = false;
    static constexpr uint32_t ADVISORY_GRACE_MS = 90000;

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
    uint32_t    last_nearby_ms        = 0;  // last time nearby_players was broadcast to this client
    uint64_t    last_nearby_hash      = 0;  // last nearby_players payload signature

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
    uint64_t udp_token = 0;
    bool     udp_ready = false;
    sockaddr_in udp_addr{};
    uint32_t udp_last_seen_ms = 0;
    uint64_t udp_sent_packets = 0;
    uint64_t udp_recv_packets = 0;
    std::mutex audio_route_mtx;
    bool     have_rx_seq = false;
    uint16_t last_rx_seq = 0;
    uint32_t last_rx_packet_ms = 0;
    std::atomic<bool>     speaking_hat_on{false};
    std::atomic<uint32_t> last_speaking_audio_ms{0};

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
    int level = 0;
    int job = 0;
    int group_id = 0;
    bool war_map = false;
    uint32_t ms = 0;
};

struct SessionKick {
    int char_id = 0;
    uint64_t session_id = 0;
    std::string reason;
};

// ── Session lock — split from the old single g_mtx ────────────────────────────
// Audio routing (hot path) acquires a shared_lock (reader) so multiple routing
// operations don't block each other, and the UDP position writer only briefly
// stalls readers while updating x/y/map.  All mutations (auth, close, position
// update, whisper) acquire a unique_lock (exclusive writer).
static std::shared_mutex g_session_mtx;
static std::shared_mutex g_voice_db_config_mtx;

static std::unordered_map<void*, ClientSession*>      g_by_ws;
static std::unordered_map<int,   ClientSession*>      g_by_char_id;
static std::unordered_map<int,   PendingPos>          g_pending_pos;
static int g_player_count = 0;

// ── Auth advisory (populated by Map Server via UDP) ──────────────────────────
// Map server sends an advisory for each logged-in player with their trusted
// (account_id) tied to the char_id. Client auth must match or it's rejected.
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

// ── Duplicate-char replacement flap dampener ─────────────────────────────────
// Two clients of the SAME account+char (e.g. an autotrade shop running on a
// VPS plus the player's home client, each behind a different IP) will both
// pass auth — their account_id matches the map-server advisory — and then
// fight over the char_id: each new connection replaces the other on every
// reconnect, flapping many times per second. It is harmless (no leak: every
// replace calls idx_remove + the close handler's identity guard) but it spams
// the log and churns CPU/UDP. We can't pick a "true" owner (both are valid for
// that account), but we CAN stop the thrash: track how often a char_id is
// replaced; once it exceeds the threshold inside the window, keep whoever
// currently holds the slot and bounce the newcomer with a long backoff so the
// connection stabilizes instead of ping-ponging. A genuinely dead holder is
// still reclaimed once its TCP connection closes (close handler frees the
// slot), so this never permanently locks out a legitimate client.
struct ReplaceFlap {
    uint32_t window_start_tick = 0;
    int      count             = 0;
};
static std::unordered_map<int, ReplaceFlap> g_replace_flap; // by char_id
static constexpr uint32_t FLAP_WINDOW_MS = 10000; // rolling window
static constexpr int      FLAP_THRESHOLD = 3;     // replaces in window before damping

// ── Per-account flood-ban list (populated when a client trips FLOOD_VIOLATION_THRESHOLD)
// Keyed on account_id so families sharing a NAT/router are not collaterally banned.
struct FloodBan { uint32_t until_tick = 0; };
static std::unordered_map<int, FloodBan> g_flood_bans;
static constexpr uint32_t FLOOD_BAN_DURATION_MS = 300000; // 5 minutes

// ── Admin mute (char_id → unmute time_t, 0 = permanent until @voiceunmute)
static std::unordered_map<int, time_t> g_admin_muted;  // not persisted, clears on restart

// ── Admin ban (account_id → expiry time_t, 0 = permanent)
static std::unordered_map<int, time_t> g_admin_banned; // persisted in voice_bans table

// ── Voice license (account_id → expires time_t, 0 = permanent)
// Only enforced when g_cfg.voice_license_required is true.
static std::unordered_map<int, time_t> g_voice_licenses; // persisted in voice_licenses table

// ── Voice block list (blocker_account_id → set of blocked account_ids)
// Player-driven asymmetric block: blocker doesn't hear blocked.
// Stored in voice_blocks DB table.
static std::unordered_map<int, std::unordered_set<int>> g_voice_blocks;
// Accounts we've already raised a toxic-player alert for (avoid re-alerting
// on every subsequent block once the threshold is crossed). Cleared on restart.
static std::unordered_set<int> g_block_alerted;

static uint64_t make_udp_token() {
    static std::mutex mtx;
    static std::mt19937_64 rng([] {
        std::random_device rd;
        uint64_t seed = (static_cast<uint64_t>(rd()) << 32) ^ rd() ^ tick_ms();
        return seed;
    }());
    std::lock_guard<std::mutex> lock(mtx);
    uint64_t v = 0;
    while (v == 0) v = rng();
    return v;
}

// ── Pre-built lookup indexes (maintained under g_session_mtx) ────────────────
// Replaces O(n) full-scan with O(members_in_channel) per audio packet.
static std::unordered_map<std::string, std::unordered_set<ClientSession*>> g_by_map;
static std::unordered_map<int,         std::unordered_set<ClientSession*>> g_by_party;
static std::unordered_map<int,         std::unordered_set<ClientSession*>> g_by_guild;
static std::unordered_map<int,         std::unordered_set<ClientSession*>> g_by_room;
static std::unordered_map<std::string, std::unordered_map<int64_t, std::unordered_set<ClientSession*>>> g_by_spatial;

static constexpr int SPATIAL_CELL_SIZE = 16;

static int spatial_cell(int v) {
    return v >= 0 ? (v / SPATIAL_CELL_SIZE) : ((v - SPATIAL_CELL_SIZE + 1) / SPATIAL_CELL_SIZE);
}

static int64_t spatial_key(int cx, int cy) {
    return (static_cast<int64_t>(cx) << 32) ^ static_cast<uint32_t>(cy);
}

static int64_t spatial_key_for_pos(int x, int y) {
    return spatial_key(spatial_cell(x), spatial_cell(y));
}

static bool spatial_indexable(const ClientSession* s) {
    return s && !s->map.empty() &&
        s->last_position_ms != 0 &&
        (tick_ms() - s->last_position_ms) <= POSITION_STALE_MS;
}

static void spatial_remove(ClientSession* s) {
    if (!s || s->map.empty() || s->last_position_ms == 0) return;
    auto map_it = g_by_spatial.find(s->map);
    if (map_it == g_by_spatial.end()) return;
    const int64_t key = spatial_key_for_pos(s->x, s->y);
    auto cell_it = map_it->second.find(key);
    if (cell_it == map_it->second.end()) return;
    cell_it->second.erase(s);
    if (cell_it->second.empty())
        map_it->second.erase(cell_it);
    if (map_it->second.empty())
        g_by_spatial.erase(map_it);
}

static void spatial_insert(ClientSession* s) {
    if (!spatial_indexable(s)) return;
    g_by_spatial[s->map][spatial_key_for_pos(s->x, s->y)].insert(s);
}

// Call under g_session_mtx (exclusive)
static void idx_insert(ClientSession* s) {
    if (!s->map.empty())       g_by_map[s->map].insert(s);
    spatial_insert(s);
    if (s->party_id > 0)       g_by_party[s->party_id].insert(s);
    if (s->guild_id > 0)       g_by_guild[s->guild_id].insert(s);
    if (s->chat_room_id > 0)   g_by_room[s->chat_room_id].insert(s);
}

static void idx_remove(ClientSession* s) {
    spatial_remove(s);
    if (!s->map.empty())     { auto it = g_by_map.find(s->map);           if (it != g_by_map.end())    it->second.erase(s); }
    if (s->party_id > 0)     { auto it = g_by_party.find(s->party_id);    if (it != g_by_party.end())  it->second.erase(s); }
    if (s->guild_id > 0)     { auto it = g_by_guild.find(s->guild_id);    if (it != g_by_guild.end())  it->second.erase(s); }
    if (s->chat_room_id > 0) { auto it = g_by_room.find(s->chat_room_id); if (it != g_by_room.end())   it->second.erase(s); }
}

static void idx_set_map(ClientSession* s, const std::string& v) {
    if (s->map == v) return;
    spatial_remove(s);
    if (!s->map.empty()) { auto it = g_by_map.find(s->map); if (it != g_by_map.end()) it->second.erase(s); }
    s->map = v;
    s->last_nearby_hash = 0;
    if (!v.empty()) g_by_map[v].insert(s);
    spatial_insert(s);
}

static void idx_set_position(ClientSession* s, const std::string& map, int x, int y, uint32_t now) {
    spatial_remove(s);
    if (s->map != map) {
        if (!s->map.empty()) {
            auto old_it = g_by_map.find(s->map);
            if (old_it != g_by_map.end()) old_it->second.erase(s);
        }
        s->map = map;
        s->last_nearby_hash = 0;
        if (!s->map.empty()) g_by_map[s->map].insert(s);
    }
    s->x = x;
    s->y = y;
    s->last_position_ms = now;
    spatial_insert(s);
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

static bool voice_db_map_blocked(const std::string& map, int level, int job, int group_id) {
    std::shared_lock<std::shared_mutex> lock(g_voice_db_config_mtx);
    return g_config.is_map_blocked(map, level, job, group_id);
}

static bool voice_db_whisper_bypass(int group_id) {
    std::shared_lock<std::shared_mutex> lock(g_voice_db_config_mtx);
    return g_config.is_whisper_bypass(group_id);
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

static uint64_t read_be_u64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | static_cast<uint64_t>(p[i]);
    return v;
}

static void write_be_u32(unsigned char* p, uint32_t v) {
    p[0] = static_cast<unsigned char>((v >> 24) & 0xFF);
    p[1] = static_cast<unsigned char>((v >> 16) & 0xFF);
    p[2] = static_cast<unsigned char>((v >> 8) & 0xFF);
    p[3] = static_cast<unsigned char>(v & 0xFF);
}

static void write_be_u64(unsigned char* p, uint64_t v) {
    for (int i = 7; i >= 0; --i)
        p[7 - i] = static_cast<unsigned char>((v >> (i * 8)) & 0xFF);
}

static void write_be_f32(unsigned char* p, float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    write_be_u32(p, bits);
}

static float calc_volume(const ClientSession& from, const ClientSession& to) {
    if (from.map.empty() || to.map.empty() || from.map != to.map)
        return 0.0f;

    if (from.last_position_ms == 0 || to.last_position_ms == 0)
        return 0.0f;

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

template <typename Fn>
static void for_each_spatial_candidate(const ClientSession& from, Fn&& fn) {
    auto map_it = g_by_spatial.find(from.map);
    if (map_it == g_by_spatial.end()) return;

    const int cx = spatial_cell(from.x);
    const int cy = spatial_cell(from.y);
    const int radius = static_cast<int>(std::ceil((g_cfg.proximity_max_range + PROXIMITY_EDGE_GUARD_CELLS) /
                                                  static_cast<float>(SPATIAL_CELL_SIZE))) + 1;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            auto cell_it = map_it->second.find(spatial_key(cx + dx, cy + dy));
            if (cell_it == map_it->second.end()) continue;
            for (ClientSession* other : cell_it->second)
                fn(other);
        }
    }
}

static bool should_forward(uint8_t channel, uint32_t gid, const ClientSession& from, const ClientSession& to) {
    if (!to.authed) return false;
    if (to.deafened) return false;
    if (from.char_id == to.char_id) return false;

    // Voice-banned sender — audio silenced, can still connect and hear
    if (from.account_id > 0) {
        auto bit = g_admin_banned.find(from.account_id);
        if (bit != g_admin_banned.end()) {
            if (bit->second == 0 || time(nullptr) < bit->second)
                return false;
        }
    }

    // License gate — when enabled, sender must have an active license entry
    if (g_cfg.voice_license_required && from.account_id > 0) {
        auto lit = g_voice_licenses.find(from.account_id);
        const bool has_license = lit != g_voice_licenses.end() &&
                                 (lit->second == 0 || time(nullptr) < lit->second);
        if (!has_license) return false;
    }

    // Receiver-side personal block list — drop if `to` blocked `from`'s account
    if (to.account_id > 0 && from.account_id > 0) {
        auto bit = g_voice_blocks.find(to.account_id);
        if (bit != g_voice_blocks.end() && bit->second.count(from.account_id))
            return false;
        // Bidirectional: also drop if `from` blocked `to` (so neither hears the other)
        if (g_cfg.voice_block_bidirectional) {
            auto fit = g_voice_blocks.find(from.account_id);
            if (fit != g_voice_blocks.end() && fit->second.count(to.account_id))
                return false;
        }
    }

    // GM-muted sender — no one hears them
    {
        auto mit = g_admin_muted.find(from.char_id);
        if (mit != g_admin_muted.end()) {
            if (mit->second == 0 || time(nullptr) < mit->second)
                return false;
        }
    }

    // Block voice on restricted maps — check per-player level/job/group_id
    if (voice_db_map_blocked(from.map, from.level, from.job, from.group_id) ||
        voice_db_map_blocked(to.map,   to.level,   to.job,   to.group_id))
        return false;

    const bool war_restricted = g_cfg.war_mode_enabled &&
        g_war_active.load(std::memory_order_relaxed) &&
        (from.war_map || to.war_map);
    if (war_restricted) {
        if (channel == 0 || channel == 3) return false;
        if (channel == 4 && !g_cfg.war_allow_whisper) return false;
    }

    // Players inside a chat room are isolated: only Room (3) and Whisper (4) audio
    // may cross the room boundary.  This is enforced server-side regardless of what
    // rx_channel the client reports, so a slow/buggy DLL cannot leak Party/Guild/Normal
    // audio in or out of a room.
    if (channel != 3 && channel != 4) {
        if (from.chat_room_id != 0 || to.chat_room_id != 0)
            return false;
    }

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
#  include <ws2tcpip.h>
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

// Global server loop pointer, set once from the raw TCP server thread before UDP starts.
static std::atomic<VoiceTcp::Loop*> g_voice_loop{nullptr};

static SOCKET g_voice_udp_sock = INVALID_SOCKET;

// Helper: queue a JSON send onto the server loop (thread-safe).
// We capture stable session identity (char_id + session_id) instead of a raw
// connection pointer so a disconnect/reconnect cannot leave us sending through
// a stale connection object after the target session is gone.
static void send_json_deferred(ClientSession* target, json msg) {
    if (!g_voice_loop.load()) return;
    if (!target || target->char_id == 0) return;
    const int target_char_id = target->char_id;
    const uint64_t target_session_id = target->session_id;
    std::string payload = msg.dump();
    g_voice_loop.load()->defer([target_char_id, target_session_id, payload = std::move(payload)]() {
        std::shared_lock<std::shared_mutex> lock(g_session_mtx);
        auto it = g_by_char_id.find(target_char_id);
        if (it == g_by_char_id.end() || !it->second) return;
        ClientSession* current = it->second;
        if (current->session_id != target_session_id || !current->ws) return;
        current->ws->send(payload, VoiceTcp::OpCode::TEXT);
    });
}

static void kick_session_deferred(int target_char_id,
                                  uint64_t target_session_id,
                                  json msg,
                                  std::string reason,
                                  int code = 1000) {
    if (!g_voice_loop.load() || target_char_id <= 0 || target_session_id == 0)
        return;

    std::string payload = msg.dump();
    g_voice_loop.load()->defer([target_char_id, target_session_id,
                                payload = std::move(payload),
                                reason = std::move(reason), code]() {
        std::lock_guard<std::shared_mutex> lock(g_session_mtx);
        auto it = g_by_char_id.find(target_char_id);
        if (it == g_by_char_id.end() || !it->second || !it->second->ws)
            return;
        ClientSession* current = it->second;
        if (current->session_id != target_session_id)
            return;
        current->kicking = true;
        current->ws->send(payload, VoiceTcp::OpCode::TEXT);
        current->ws->end(code, reason);
    });
}

static constexpr uint32_t NEARBY_BROADCAST_INTERVAL_MS = 100;
static void send_nearby_players_deferred(ClientSession* s) {
    if (!s || !s->authed || s->map.empty()) return;

    std::vector<std::pair<int, std::string>> nearby;
    for_each_spatial_candidate(*s, [&](ClientSession* other) {
        if (!other || other->char_id == s->char_id || !other->authed) return;
        if (calc_volume(*s, *other) > 0.0f)
            nearby.push_back({other->char_id, other->char_name});
    });
    std::sort(nearby.begin(), nearby.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    uint64_t hash = UINT64_C(1469598103934665603);
    for (const auto& [id, name] : nearby) {
        hash ^= static_cast<uint32_t>(id);
        hash *= UINT64_C(1099511628211);
    }
    if (hash == s->last_nearby_hash)
        return;
    s->last_nearby_hash = hash;

    json players = json::array();
    for (const auto& [id, name] : nearby)
        players.push_back({{"id", id}, {"name", name}});
    send_json_deferred(s, json{{"type", "nearby_players"}, {"players", players}});
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

// Call while holding g_session_mtx. The actual socket send is deferred to
// the server loop and only happens when the visible client state changed.
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
static std::atomic<bool> g_server_stop_requested{false};
static std::atomic<bool> g_server_shutdown_deferred{false};
static VoiceTcp::App* g_app = nullptr;
static std::mutex g_map_bridge_addr_mtx;
static sockaddr_in g_map_bridge_addr{};
static bool g_map_bridge_addr_valid = false;

static void stop_udp_receiver();

static std::string json_escape_copy(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out.push_back(ch); break;
        }
    }
    return out;
}

static void send_map_speaking_hat(int char_id, bool speaking) {
    if (char_id <= 0 || g_udp_sock == INVALID_SOCKET)
        return;

    sockaddr_in to{};
    {
        std::lock_guard<std::mutex> lock(g_map_bridge_addr_mtx);
        if (!g_map_bridge_addr_valid)
            return;
        to = g_map_bridge_addr;
    }

    std::string payload = std::string("{\"type\":\"speaking_hat\",\"char_id\":") +
        std::to_string(char_id) + ",\"speaking\":" + (speaking ? "true" : "false");
    if (!g_cfg.voice_bridge_secret.empty())
        payload += ",\"bridge_secret\":\"" + json_escape_copy(g_cfg.voice_bridge_secret) + "\"";
    payload += "}";

    sendto(g_udp_sock, payload.c_str(), static_cast<int>(payload.size()), 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

// Notify the map server that a player has been blocked by many distinct
// accounts (possible toxic player). Map server broadcasts to online GMs.
static void send_map_block_alert(int blocked_char_id, const std::string& blocked_name, int count) {
    if (g_udp_sock == INVALID_SOCKET)
        return;
    sockaddr_in to{};
    {
        std::lock_guard<std::mutex> lock(g_map_bridge_addr_mtx);
        if (!g_map_bridge_addr_valid)
            return;
        to = g_map_bridge_addr;
    }
    std::string payload = std::string("{\"type\":\"block_alert\",\"char_id\":")
        + std::to_string(blocked_char_id)
        + ",\"name\":\"" + json_escape_copy(blocked_name) + "\""
        + ",\"count\":" + std::to_string(count);
    if (!g_cfg.voice_bridge_secret.empty())
        payload += ",\"bridge_secret\":\"" + json_escape_copy(g_cfg.voice_bridge_secret) + "\"";
    payload += "}";
    sendto(g_udp_sock, payload.c_str(), static_cast<int>(payload.size()), 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

static void set_session_speaking_hat(ClientSession* s, bool speaking) {
    if (!s || s->char_id <= 0)
        return;

    bool expected = !speaking;
    if (s->speaking_hat_on.compare_exchange_strong(expected, speaking))
        send_map_speaking_hat(s->char_id, speaking);
}

static std::string udp_addr_to_ip(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN] = {};
    const char* ok = inet_ntop(AF_INET, const_cast<in_addr*>(&addr.sin_addr), ip, sizeof(ip));
    return ok ? std::string(ip) : std::string{};
}

static bool udp_source_allowed(const sockaddr_in& from_addr) {
    const std::string ip = udp_addr_to_ip(from_addr);
    return ip == g_cfg.voice_api_ip;
}

static bool is_loopback_ip(const std::string& ip) {
    return ip == "127.0.0.1" || ip == "localhost" || ip.rfind("127.", 0) == 0;
}

static bool udp_secret_allowed(const json& j) {
    if (g_cfg.voice_bridge_secret.empty())
        return true;
    return j.value("bridge_secret", "") == g_cfg.voice_bridge_secret;
}

static void reload_voice_db_config() {
    Config db_cfg = Config::load_voice_db(g_conf_path);
    if (!db_cfg.voice_db_valid) {
        LOG_ERROR("Voice DB reload failed; keeping previous blocked map rules");
        return;
    }
    std::unique_lock<std::shared_mutex> lock(g_voice_db_config_mtx);
    g_config.blocked_maps = std::move(db_cfg.blocked_maps);
    g_config.whisper_bypass_groups = std::move(db_cfg.whisper_bypass_groups);
    g_config.voice_db_valid = true;
}

// Reload voice config and notify currently-connected sessions of any
// state changes that should take effect immediately (e.g., toggling
// voice_license_required on/off should not require players to relog).
//
// Must run on the server loop thread (i.e. via g_voice_loop->defer).
static void reload_voice_conf_and_apply() {
    const bool prev_license_required = g_cfg.voice_license_required;
    load_voice_conf(g_conf_path.c_str());
    const bool now_license_required = g_cfg.voice_license_required;
    if (prev_license_required == now_license_required)
        return; // nothing session-visible to update

    std::shared_lock<std::shared_mutex> lock(g_session_mtx);
    if (now_license_required) {
        // Newly required — anyone without an active license should be told.
        const std::string payload = json{{"type","license_required"}}.dump();
        const std::string ok      = json{{"type","license_granted"}}.dump();
        const time_t now_t = time(nullptr);
        for (auto& kv : g_by_char_id) {
            ClientSession* s = kv.second;
            if (!s || !s->authed || !s->ws || s->kicking) continue;
            auto lit = g_voice_licenses.find(s->account_id);
            const bool has = lit != g_voice_licenses.end() &&
                             (lit->second == 0 || now_t < lit->second);
            s->ws->send(has ? ok : payload, VoiceTcp::OpCode::TEXT);
        }
    } else {
        // License mode disabled — anyone we had marked as no_license should be cleared.
        const std::string payload = json{{"type","license_granted"}}.dump();
        for (auto& kv : g_by_char_id) {
            ClientSession* s = kv.second;
            if (!s || !s->authed || !s->ws || s->kicking) continue;
            s->ws->send(payload, VoiceTcp::OpCode::TEXT);
        }
    }
    LOG_INFO("voice_license_required %s — notified active sessions",
             now_license_required ? "ENABLED" : "DISABLED");
}

void request_server_stop() {
    g_server_stop_requested.store(true);

    VoiceTcp::Loop* loop = g_voice_loop.load();
    if (loop) {
        if (g_server_shutdown_deferred.exchange(true))
            return;
        loop->defer([]() {
            LOG_INFO("Shutdown requested");
            stop_udp_receiver();
            {
                std::lock_guard<std::mutex> lock(g_db_mtx);
                if (g_db) {
                    mysql_close(g_db);
                    g_db = nullptr;
                }
            }
            if (g_app)
                g_app->close();
        });
    } else {
        stop_udp_receiver();
    }
}

static void udp_position_loop() {
    char buf[4096];
    sockaddr_in from_addr{};
    sock_len_t from_len = sizeof(from_addr);

    // Periodic maintenance — runs every ~10 seconds inside the UDP loop.
    // Cleans up: stale pending positions and timed-out whisper sessions.
    static constexpr uint32_t MAINTENANCE_INTERVAL_MS = 10000;
    static constexpr uint32_t PENDING_POS_TTL_MS      = 30000;
    uint32_t last_maintenance = tick_ms();
    uint32_t last_speaking_maintenance = last_maintenance;

    while (g_udp_running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(g_udp_sock, &readfds);

        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms timeout

        int ret = select(static_cast<int>(g_udp_sock) + 1, &readfds, nullptr, nullptr, &tv);

        // Config reload requested by SIGUSR1 — defer to server thread so it's safe
        if (g_reload_requested.load() && g_voice_loop.load()) {
            g_reload_requested.store(false);
            g_voice_loop.load()->defer([]() {
                reload_voice_conf_and_apply();
                reload_voice_db_config();
                LOG_INFO("Voice config and DB reloaded from %s", g_conf_path.c_str());
            });
        }

        // ── Periodic maintenance ──────────────────────────────────────────────
        uint32_t now_maint = tick_ms();
        if (now_maint - last_speaking_maintenance >= 250) {
            last_speaking_maintenance = now_maint;
            std::vector<int> speaking_hat_off;
            {
                std::shared_lock<std::shared_mutex> lock(g_session_mtx);
                const uint32_t speaking_timeout = static_cast<uint32_t>(std::max(100, g_cfg.speaking_hat_timeout_ms));
                for (auto& kv : g_by_char_id) {
                    ClientSession* s = kv.second;
                    if (!s || !s->speaking_hat_on.load()) continue;
                    uint32_t last_audio = s->last_speaking_audio_ms.load();
                    if (last_audio == 0 || now_maint - last_audio >= speaking_timeout) {
                        s->speaking_hat_on.store(false);
                        speaking_hat_off.push_back(s->char_id);
                    }
                }
            }
            for (int char_id : speaking_hat_off)
                send_map_speaking_hat(char_id, false);
        }

        if (now_maint - last_maintenance >= MAINTENANCE_INTERVAL_MS) {
            last_maintenance = now_maint;

            // Sessions whose advisory grace window expired — collected under
                // the lock, kicked after releasing it (ws->end must run on the server loop).
            std::vector<std::pair<int, uint64_t>> advisory_timeout_kicks;
            // Admin ban expiries — DB delete + DLL notify happen after lock release.
            // We capture account_id (not ws*) for the notify path so the defer
            // can re-look-up the session under the lock and avoid use-after-free
            // if the player disconnected between expiry and notify execution.
            std::vector<int> expired_admin_bans;
            std::vector<int> expired_notify_account_ids;
            // Voice license expiries — same pattern as admin bans
            std::vector<int> expired_licenses;
            std::vector<int> expired_license_notify_account_ids;

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

                // Expire stale auth advisories (map-server bridge renews every 5 s;
                // TTL is 2 min so a brief bridge hiccup never drops a live advisory)
                for (auto it = g_auth_advisories.begin(); it != g_auth_advisories.end(); ) {
                    if (now_maint - it->second.tick > AUTH_ADVISORY_TTL_MS) {
                        LOG_DEBUG("auth_advisory TTL expired char_id=%d", it->first);
                        it = g_auth_advisories.erase(it);
                    } else {
                        ++it;
                    }
                }

                // Expire stale replace-flap counters. The close handler clears
                // these when the holder leaves cleanly, but a counter can linger
                // if the last contender was bounced (no holder ever closed). Drop
                // any whose rolling window has long since elapsed so the map can't
                // grow unbounded across a long uptime.
                for (auto it = g_replace_flap.begin(); it != g_replace_flap.end(); ) {
                    if (now_maint - it->second.window_start_tick > FLAP_WINDOW_MS * 2) {
                        it = g_replace_flap.erase(it);
                    } else {
                        ++it;
                    }
                }

                // Expire per-account flood bans
                for (auto it = g_flood_bans.begin(); it != g_flood_bans.end(); ) {
                    if ((int32_t)(now_maint - it->second.until_tick) >= 0) {
                        LOG_INFO("flood_ban expired account_id=%d", it->first);
                        it = g_flood_bans.erase(it);
                    } else {
                        ++it;
                    }
                }

                // Expire admin mutes
                {
                    time_t now_t = time(nullptr);
                    for (auto it = g_admin_muted.begin(); it != g_admin_muted.end(); ) {
                        if (it->second != 0 && now_t >= it->second) {
                            LOG_INFO("admin_mute expired char_id=%d", it->first);
                            it = g_admin_muted.erase(it);
                        } else ++it;
                    }
                    // Expire admin bans — collect here; DB delete + notify
                    // happen after we release the session lock.
                    for (auto it = g_admin_banned.begin(); it != g_admin_banned.end(); ) {
                        if (it->second != 0 && now_t >= it->second) {
                            const int aid = it->first;
                            LOG_INFO("admin_ban expired account_id=%d", aid);
                            expired_admin_bans.push_back(aid);
                            // Note: only schedule a notify if a session is currently
                            // online for this aid. The defer re-validates under the lock.
                            for (auto& kv : g_by_char_id) {
                                ClientSession* s = kv.second;
                                if (s && s->authed && s->account_id == aid) {
                                    expired_notify_account_ids.push_back(aid);
                                    break;
                                }
                            }
                            it = g_admin_banned.erase(it);
                        } else ++it;
                    }
                    // Expire voice licenses
                    for (auto it = g_voice_licenses.begin(); it != g_voice_licenses.end(); ) {
                        if (it->second != 0 && now_t >= it->second) {
                            const int aid = it->first;
                            LOG_INFO("voice_license expired account_id=%d", aid);
                            expired_licenses.push_back(aid);
                            for (auto& kv : g_by_char_id) {
                                ClientSession* s = kv.second;
                                if (s && s->authed && s->account_id == aid) {
                                    expired_license_notify_account_ids.push_back(aid);
                                    break;
                                }
                            }
                            it = g_voice_licenses.erase(it);
                        } else ++it;
                    }
                }

                // Kick provisional sessions whose advisory never arrived within
                // the grace window. Collect here; actual ws->end() has to run
                // on the server loop, so defer after releasing the lock.
                for (auto& kv : g_by_char_id) {
                    ClientSession* s = kv.second;
                    if (!s || s->kicking || !s->awaiting_advisory) continue;
                    if (now_maint - s->advisory_wait_tick
                        > ClientSession::ADVISORY_GRACE_MS) {
                        advisory_timeout_kicks.push_back({ s->char_id, s->session_id });
                    }
                }
            }

            // ── Now that g_session_mtx is released, run DB writes / WS notifies ─
            for (int aid : expired_admin_bans)
                db_delete_expired_ban(aid);
            if (!expired_notify_account_ids.empty() && g_voice_loop.load()) {
                auto aids = std::move(expired_notify_account_ids);
                g_voice_loop.load()->defer([aids = std::move(aids)]() {
                    // Re-look up under the lock so the ws pointer cannot be freed
                    // mid-send by a concurrent disconnect.
                    std::shared_lock<std::shared_mutex> lock(g_session_mtx);
                    const std::string payload =
                        json{{"type","admin_unbanned"}}.dump();
                    for (int aid : aids) {
                        for (auto& kv : g_by_char_id) {
                            ClientSession* s = kv.second;
                            if (s && s->authed && s->account_id == aid && s->ws && !s->kicking) {
                                s->ws->send(payload, VoiceTcp::OpCode::TEXT);
                                break;
                            }
                        }
                    }
                });
            }
            for (int aid : expired_licenses)
                db_delete_expired_license(aid);
            if (!expired_license_notify_account_ids.empty() && g_voice_loop.load()
                && g_cfg.voice_license_required) {
                auto aids = std::move(expired_license_notify_account_ids);
                g_voice_loop.load()->defer([aids = std::move(aids)]() {
                    std::shared_lock<std::shared_mutex> lock(g_session_mtx);
                    const std::string payload =
                        json{{"type","license_required"}}.dump();
                    for (int aid : aids) {
                        for (auto& kv : g_by_char_id) {
                            ClientSession* s = kv.second;
                            if (s && s->authed && s->account_id == aid && s->ws && !s->kicking) {
                                s->ws->send(payload, VoiceTcp::OpCode::TEXT);
                                break;
                            }
                        }
                    }
                });
            }

            if (!advisory_timeout_kicks.empty() && g_voice_loop.load()) {
                std::vector<std::pair<int, uint64_t>> victims = std::move(advisory_timeout_kicks);
                g_voice_loop.load()->defer([victims]() {
                    for (const auto& [char_id, session_id] : victims) {
                        // Keep lookup + send/end under the session lock so close_cb
                        // cannot delete the session between lookup and socket close.
                        int log_char_id = 0;
                        std::string log_ip;
                        {
                            std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                            auto it = g_by_char_id.find(char_id);
                            if (it == g_by_char_id.end() || !it->second || !it->second->ws) continue;
                            if (it->second->session_id != session_id) continue;
                            if (it->second->kicking) continue;
                            if (!it->second->awaiting_advisory) continue;
                            if (tick_ms() - it->second->advisory_wait_tick
                                <= ClientSession::ADVISORY_GRACE_MS) continue;
                            log_char_id = it->second->char_id;
                            log_ip      = it->second->ip;
                            it->second->kicking = true;
                            it->second->ws->send(json{{"type","error"},{"message","no active map session"}}.dump(),
                                                 VoiceTcp::OpCode::TEXT);
                            it->second->ws->end(1008, "no advisory");
                        }
                        LOG_WARNING("auth timeout — advisory never arrived  char_id=%d ip=%s (kick)",
                                    log_char_id, log_ip.c_str());
                    }
                });
            }

            // 2. Whisper timeout — notify both peers then drop expired sessions
            if (g_cfg.whisper_timeout > 0 && g_voice_loop.load()) {
                auto expired = g_whisper.collect_expired(
                    static_cast<int>(g_cfg.whisper_timeout));
                if (!expired.empty()) {
                    g_voice_loop.load()->defer([expired = std::move(expired)]() {
                        std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                        for (auto& e : expired) {
                            const std::string payload =
                                json{{"type","whisper_ended"},{"sid",e.sid},{"reason","timeout"}}.dump();
                            for (int cid : {e.char_id_a, e.char_id_b}) {
                                auto it = g_by_char_id.find(cid);
                                if (it == g_by_char_id.end() || !it->second) continue;
                                ClientSession* s = it->second;
                                if (s->whisper_sid != e.sid) continue;
                                s->whisper_sid.clear();
                                if (s->ws) s->ws->send(payload, VoiceTcp::OpCode::TEXT);
                            }
                            LOG_NOTICE("whisper timeout: char_ids %d and %d sid=%s",
                                       e.char_id_a, e.char_id_b, e.sid.c_str());
                        }
                    });
                }
            }
        }

        if (ret <= 0) continue; // timeout or error

        int recv_len = recvfrom(g_udp_sock, buf, sizeof(buf) - 1, 0,
                                reinterpret_cast<sockaddr*>(&from_addr), &from_len);
        if (recv_len <= 0) continue;

        if (!udp_source_allowed(from_addr)) {
            LOG_WARNING("UDP control rejected from unexpected source %s", udp_addr_to_ip(from_addr).c_str());
            continue;
        }

        buf[recv_len] = '\0';

        // Parse JSON
        json j;
        try {
            j = json::parse(buf);
        } catch (...) {
            continue;
        }

        if (!udp_secret_allowed(j)) {
            LOG_WARNING("UDP control rejected from %s: bad bridge secret", udp_addr_to_ip(from_addr).c_str());
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(g_map_bridge_addr_mtx);
            g_map_bridge_addr = from_addr;
            g_map_bridge_addr_valid = true;
        }

        std::string type = j.value("type", "");

        if (type == "reload_config") {
            g_voice_loop.load()->defer([]() {
                reload_voice_conf_and_apply();
                reload_voice_db_config();
                LOG_INFO("Voice config and DB reloaded");
            });
            continue;
        }

        if (type == "reload_voice_conf") {
            g_voice_loop.load()->defer([]() {
                reload_voice_conf_and_apply();
                LOG_INFO("Voice config reloaded");
            });
            continue;
        }

        if (type == "reload_voice_db") {
            g_voice_loop.load()->defer([]() {
                reload_voice_db_config();
                LOG_INFO("Voice DB reloaded");
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

        if (type == "admin_mute") {
            int cid = j.value("char_id", 0);
            int dur = j.value("duration", 0); // seconds, 0 = permanent
            if (cid > 0) {
                time_t until = (dur > 0) ? (time(nullptr) + dur) : 0;
                std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                g_admin_muted[cid] = until;
                if (dur > 0)
                    LOG_NOTICE("admin_mute char_id=%d duration=%ds", cid, dur);
                else
                    LOG_NOTICE("admin_mute char_id=%d permanent", cid);
            }
            continue;
        }

        if (type == "admin_unmute") {
            int cid = j.value("char_id", 0);
            if (cid > 0) {
                std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                g_admin_muted.erase(cid);
                LOG_NOTICE("admin_unmute char_id=%d", cid);
            }
            continue;
        }

        if (type == "admin_ban") {
            int aid = j.value("account_id", 0);
            int dur = j.value("duration", 0);
            if (aid > 0) {
                time_t until = (dur > 0) ? (time(nullptr) + dur) : 0;
                bool has_session = false;
                {
                    std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                    g_admin_banned[aid] = until;
                    for (auto& kv : g_by_char_id) {
                        ClientSession* s = kv.second;
                        if (s && s->authed && s->account_id == aid) {
                            has_session = true; break;
                        }
                    }
                }
                db_insert_ban(aid, j.value("banned_by", "admin"), until);
                if (has_session && g_voice_loop.load()) {
                    g_voice_loop.load()->defer([aid]() {
                        std::shared_lock<std::shared_mutex> lock(g_session_mtx);
                        for (auto& kv : g_by_char_id) {
                            ClientSession* s = kv.second;
                            if (s && s->authed && s->account_id == aid && s->ws && !s->kicking) {
                                s->ws->send(json{{"type","admin_banned"}}.dump(), VoiceTcp::OpCode::TEXT);
                                break;
                            }
                        }
                    });
                }
                LOG_NOTICE("admin_ban account_id=%d dur=%ds", aid, dur);
            }
            continue;
        }

        if (type == "admin_unban") {
            int aid = j.value("account_id", 0);
            if (aid > 0) {
                bool has_session = false;
                {
                    std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                    g_admin_banned.erase(aid);
                    for (auto& kv : g_by_char_id) {
                        ClientSession* s = kv.second;
                        if (s && s->authed && s->account_id == aid) {
                            has_session = true; break;
                        }
                    }
                }
                db_delete_ban(aid);
                if (has_session && g_voice_loop.load()) {
                    g_voice_loop.load()->defer([aid]() {
                        std::shared_lock<std::shared_mutex> lock(g_session_mtx);
                        for (auto& kv : g_by_char_id) {
                            ClientSession* s = kv.second;
                            if (s && s->authed && s->account_id == aid && s->ws && !s->kicking) {
                                s->ws->send(json{{"type","admin_unbanned"}}.dump(), VoiceTcp::OpCode::TEXT);
                                break;
                            }
                        }
                    });
                }
                LOG_NOTICE("admin_unban account_id=%d", aid);
            }
            continue;
        }

        if (type == "admin_ban_by_name") {
            const std::string name = j.value("char_name", "");
            int dur = j.value("duration", 0);
            if (!name.empty()) {
                int aid = db_lookup_account_id_by_name(name);
                if (aid <= 0) {
                    LOG_WARNING("admin_ban_by_name: char '%s' not found", name.c_str());
                } else {
                    time_t until = (dur > 0) ? (time(nullptr) + dur) : 0;
                    bool has_session = false;
                    {
                        std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                        g_admin_banned[aid] = until;
                        for (auto& kv : g_by_char_id) {
                            ClientSession* s = kv.second;
                            if (s && s->authed && s->account_id == aid) {
                                has_session = true; break;
                            }
                        }
                    }
                    db_insert_ban(aid, j.value("banned_by", "admin"), until);
                    if (has_session && g_voice_loop.load()) {
                        g_voice_loop.load()->defer([aid]() {
                            std::shared_lock<std::shared_mutex> lock(g_session_mtx);
                            for (auto& kv : g_by_char_id) {
                                ClientSession* s = kv.second;
                                if (s && s->authed && s->account_id == aid && s->ws && !s->kicking) {
                                    s->ws->send(json{{"type","admin_banned"}}.dump(), VoiceTcp::OpCode::TEXT);
                                    break;
                                }
                            }
                        });
                    }
                    LOG_NOTICE("admin_ban_by_name '%s' account_id=%d dur=%ds", name.c_str(), aid, dur);
                }
            }
            continue;
        }

        if (type == "admin_unban_by_name") {
            const std::string name = j.value("char_name", "");
            if (!name.empty()) {
                int aid = db_lookup_account_id_by_name(name);
                if (aid <= 0) {
                    LOG_WARNING("admin_unban_by_name: char '%s' not found", name.c_str());
                } else {
                    bool has_session = false;
                    {
                        std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                        g_admin_banned.erase(aid);
                        for (auto& kv : g_by_char_id) {
                            ClientSession* s = kv.second;
                            if (s && s->authed && s->account_id == aid) {
                                has_session = true; break;
                            }
                        }
                    }
                    db_delete_ban(aid);
                    if (has_session && g_voice_loop.load()) {
                        g_voice_loop.load()->defer([aid]() {
                            std::shared_lock<std::shared_mutex> lock(g_session_mtx);
                            for (auto& kv : g_by_char_id) {
                                ClientSession* s = kv.second;
                                if (s && s->authed && s->account_id == aid && s->ws && !s->kicking) {
                                    s->ws->send(json{{"type","admin_unbanned"}}.dump(), VoiceTcp::OpCode::TEXT);
                                    break;
                                }
                            }
                        });
                    }
                    LOG_NOTICE("admin_unban_by_name '%s' account_id=%d", name.c_str(), aid);
                }
            }
            continue;
        }

        if (type == "grant_license") {
            int aid = j.value("account_id", 0);
            int dur = j.value("duration", 0); // 0 = permanent
            if (aid > 0) {
                time_t until = (dur > 0) ? (time(nullptr) + dur) : 0;
                bool has_session = false;
                {
                    std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                    g_voice_licenses[aid] = until;
                    for (auto& kv : g_by_char_id) {
                        ClientSession* s = kv.second;
                        if (s && s->authed && s->account_id == aid) {
                            has_session = true; break;
                        }
                    }
                }
                db_insert_license(aid, j.value("granted_by", "item"), until);
                if (has_session && g_voice_loop.load()) {
                    g_voice_loop.load()->defer([aid]() {
                        std::shared_lock<std::shared_mutex> lock(g_session_mtx);
                        for (auto& kv : g_by_char_id) {
                            ClientSession* s = kv.second;
                            if (s && s->authed && s->account_id == aid && s->ws && !s->kicking) {
                                s->ws->send(json{{"type","license_granted"}}.dump(), VoiceTcp::OpCode::TEXT);
                                break;
                            }
                        }
                    });
                }
                if (dur > 0)
                    LOG_NOTICE("grant_license account_id=%d duration=%ds", aid, dur);
                else
                    LOG_NOTICE("grant_license account_id=%d permanent", aid);
            }
            continue;
        }

        if (type == "revoke_license") {
            int aid = j.value("account_id", 0);
            if (aid > 0) {
                bool has_session = false;
                {
                    std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                    g_voice_licenses.erase(aid);
                    for (auto& kv : g_by_char_id) {
                        ClientSession* s = kv.second;
                        if (s && s->authed && s->account_id == aid) {
                            has_session = true; break;
                        }
                    }
                }
                db_delete_license(aid);
                if (has_session && g_voice_loop.load() && g_cfg.voice_license_required) {
                    g_voice_loop.load()->defer([aid]() {
                        std::shared_lock<std::shared_mutex> lock(g_session_mtx);
                        for (auto& kv : g_by_char_id) {
                            ClientSession* s = kv.second;
                            if (s && s->authed && s->account_id == aid && s->ws && !s->kicking) {
                                s->ws->send(json{{"type","license_required"}}.dump(), VoiceTcp::OpCode::TEXT);
                                break;
                            }
                        }
                    });
                }
                LOG_NOTICE("revoke_license account_id=%d", aid);
            }
            continue;
        }

        // ── Ignore-list sync (text /ex → voice block) ─────────────────────
        // Sent by the map server when a player adds/removes a name from their
        // text ignore list. Only honored when voice_ignore_sync is enabled.
        if (type == "block_by_name") {
            if (!g_cfg.voice_ignore_sync) continue;
            int blocker_aid = j.value("blocker_account_id", 0);
            std::string name = j.value("name", "");
            if (blocker_aid <= 0 || name.empty()) continue;
            int blocked_aid = db_lookup_account_id_by_name(name);
            if (blocked_aid <= 0 || blocked_aid == blocker_aid) continue;
            {
                std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                g_voice_blocks[blocker_aid].insert(blocked_aid);
            }
            db_insert_block(blocker_aid, blocked_aid, name);
            LOG_INFO("ignore-sync block: aid=%d blocked aid=%d (%s)",
                     blocker_aid, blocked_aid, name.c_str());
            continue;
        }

        if (type == "unblock_by_name") {
            if (!g_cfg.voice_ignore_sync) continue;
            int blocker_aid = j.value("blocker_account_id", 0);
            std::string name = j.value("name", "");
            if (blocker_aid <= 0 || name.empty()) continue;
            int blocked_aid = db_lookup_account_id_by_name(name);
            if (blocked_aid <= 0) continue;
            {
                std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                auto bit = g_voice_blocks.find(blocker_aid);
                if (bit != g_voice_blocks.end()) {
                    bit->second.erase(blocked_aid);
                    if (bit->second.empty()) g_voice_blocks.erase(bit);
                }
            }
            db_delete_block(blocker_aid, blocked_aid);
            LOG_INFO("ignore-sync unblock: aid=%d unblocked aid=%d (%s)",
                     blocker_aid, blocked_aid, name.c_str());
            continue;
        }

        if (type == "auth_advisory") {
            // From Map Server — authoritative info on who is currently logged in.
            int cid = j.value("char_id",    0);
            int aid = j.value("account_id", 0);
            uint32_t l1 = j.value("login_id1", 0u);
            if (cid > 0 && aid > 0) {
                // Holds the lock only briefly to update the advisory map + check
                // for matching provisional sessions. Any connection kick is deferred
                // onto the server loop because ws->end() must run there.
                int spoof_char_id = 0;
                uint64_t spoof_session_id = 0;
                ClientSession* confirm_target = nullptr;
                int spoof_aid_advisory = 0;
                int spoof_aid_claimed  = 0;
                uint32_t spoof_l1_advisory = 0;
                uint32_t spoof_l1_claimed  = 0;
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
                        const bool l1_mismatch =
                            (l1 != 0 && l1 != s->login_id1 &&
                             (g_cfg.login_id1_strict || s->login_id1 != 0));
                        if (s->kicking) {
                        } else if (s->awaiting_advisory) {
                            if (s->account_id == aid && !l1_mismatch) {
                                s->awaiting_advisory = false;
                                confirm_target = s;
                                if (s->auth_ok_pending) {
                                    s->auth_ok_pending = false;
                                    if (s->udp_token == 0) s->udp_token = make_udp_token();
                                    send_json_deferred(s, json{
                                        {"type", "auth_ok"},
                                        {"udp_port", g_voice_udp_sock != INVALID_SOCKET ? g_cfg.voice_port : 0},
                                        {"udp_token", s->udp_token}});
                                    auto bit = g_admin_banned.find(s->account_id);
                                    if (bit != g_admin_banned.end()) {
                                        const time_t now = time(nullptr);
                                        if (bit->second == 0 || now < bit->second)
                                            send_json_deferred(s, json{{"type", "admin_banned"}});
                                    }
                                    if (g_cfg.voice_license_required) {
                                        auto lit = g_voice_licenses.find(s->account_id);
                                        const bool has = lit != g_voice_licenses.end() &&
                                                         (lit->second == 0 || time(nullptr) < lit->second);
                                        if (!has)
                                            send_json_deferred(s, json{{"type", "license_required"}});
                                    }
                                    auto blk = g_voice_blocks.find(s->account_id);
                                    if (blk != g_voice_blocks.end() && !blk->second.empty()) {
                                        json arr = json::array();
                                        for (auto& kv2 : g_by_char_id) {
                                            ClientSession* o = kv2.second;
                                            if (!o || !o->authed) continue;
                                            if (blk->second.count(o->account_id))
                                                arr.push_back({{"char_id", o->char_id}, {"name", o->char_name}});
                                        }
                                        send_json_deferred(s, json{{"type", "your_blocks"}, {"blocked", arr}});
                                    }
                                    send_json_deferred(s, make_war_state_json(*s));
                                    if (!s->map.empty()) {
                                        send_json_deferred(s, json{
                                            {"type", "your_pos"}, {"x", s->x}, {"y", s->y}, {"map", s->map}});
                                        send_nearby_players_deferred(s);
                                    }
                                    LOG_NOTICE("(char_id=%d aid=%d name=%s) confirmed via advisory  [online: %d]",
                                               s->char_id, s->account_id, s->char_name.c_str(), g_player_count);
                                }
                            } else {
                                spoof_char_id      = s->char_id;
                                spoof_session_id   = s->session_id;
                                spoof_aid_advisory = aid;
                                spoof_aid_claimed  = s->account_id;
                                spoof_l1_advisory  = l1;
                                spoof_l1_claimed   = s->login_id1;
                            }
                        } else if (s->account_id != aid || l1_mismatch) {
                            // Already-authed session but advisory says a different
                            // account_id (or login_id1) now owns this char_id → someone
                            // else logged in as that char (stale session) or a spoof. Kick.
                            spoof_char_id      = s->char_id;
                            spoof_session_id   = s->session_id;
                            spoof_aid_advisory = aid;
                            spoof_aid_claimed  = s->account_id;
                            spoof_l1_advisory  = l1;
                            spoof_l1_claimed   = s->login_id1;
                        }
                    }
                }

                if (confirm_target) {
                    LOG_INFO("auth confirmed via late advisory  char_id=%d aid=%d", cid, aid);
                }
                if (spoof_char_id > 0 && g_voice_loop.load()) {
                    int cid_cap = cid;
                    int adv_cap = spoof_aid_advisory;
                    int clm_cap = spoof_aid_claimed;
                    uint32_t l1adv_cap = spoof_l1_advisory;
                    uint32_t l1clm_cap = spoof_l1_claimed;
                    int target_char_id = spoof_char_id;
                    uint64_t target_session_id = spoof_session_id;
                    g_voice_loop.load()->defer([target_char_id, target_session_id, cid_cap, adv_cap, clm_cap, l1adv_cap, l1clm_cap]() {
                        {
                            std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                            auto it = g_by_char_id.find(target_char_id);
                            if (it == g_by_char_id.end() || !it->second || !it->second->ws) return;
                            if (it->second->session_id != target_session_id) return;
                            it->second->kicking = true;
                            it->second->ws->send(json{{"type","error"},{"message","credentials mismatch"}}.dump(),
                                                 VoiceTcp::OpCode::TEXT);
                            it->second->ws->end(1008, "spoof");
                        }
                        // Distinguish the two mismatch causes — same aid with differing
                        // login_id1 is the "login ซ้อน" (newer login took over) case, not
                        // an account_id spoof, so don't print identical aids as if it were.
                        if (clm_cap == adv_cap) {
                            LOG_WARNING("auth SPOOF (late advisory) — char_id=%d aid=%d login_id1 mismatch claimed=%u advisory=%u",
                                        cid_cap, clm_cap, l1clm_cap, l1adv_cap);
                        } else {
                            LOG_WARNING("auth SPOOF (late advisory) — char_id=%d claimed aid=%d but advisory aid=%d",
                                        cid_cap, clm_cap, adv_cap);
                        }
                    });
                }
            }
            continue;
        }

        if (type == "auth_revoke") {
            // Map server says this char is no longer logged in (char-select,
            // @quit, disconnect). Drop the advisory AND kick the live connection so
            // the DLL's reconnect loop picks up a clean session under the
            // new char_id.
            //
            // Earlier we bailed on the connection kick because map.c calls
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

            if (g_voice_loop.load()) {
                g_voice_loop.load()->defer([cid]() {
                    // Look up the session, set kicking=true, and close it
                    // — all under the lock. We then call ws->send/end while still holding
                    // the lock to prevent a concurrent client_loop thread from running
                    // delete_cb (which frees the session object) between the lookup and
                    // our ws->send() call.
                    //
                    // NOTE: the .close callback runs on the client_loop thread, NOT on
                    // the server loop thread. ws->end() only closes the socket + joins
                    // send_thread; it does NOT call close_cb synchronously. There is
                    // therefore no deadlock risk from holding g_session_mtx here.
                    //
                    // We deliberately leave `authed` untouched — the close handler reads
                    // it to decrement g_player_count; flipping it here would leak the
                    // online counter.
                    {
                        std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                        auto it = g_by_char_id.find(cid);
                        if (it == g_by_char_id.end() || !it->second) return;  // already gone
                        if (it->second->kicking || !it->second->ws) return;    // already kicked
                        if (!it->second->authed) return;                       // never fully authed
                        it->second->kicking = true;   // second auth_revoke will see this and bail
                        it->second->ws->send(json{{"type","error"},{"message","map session ended"}}.dump(),
                                             VoiceTcp::OpCode::TEXT);
                        it->second->ws->end(1000, "map logoff");
                    }
                    LOG_INFO("auth_revoke char_id=%d — map server reports logoff, closing connection", cid);
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
                uint64_t session_id = 0;
                {
                    std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                    auto it = g_by_char_id.find(char_id);
                    if (it != g_by_char_id.end() && it->second && it->second->authed) {
                        ClientSession* s = it->second;
                        // idx_set_map removes from old map bucket and sets s->map = ""
                        idx_set_map(s, "");
                        s->x = 0;
                        s->y = 0;
                        s->last_position_ms = 0;  // stale-position guard -> proximity = 0
                        if (!s->kicking)
                            session_id = s->session_id;
                        LOG_DEBUG("map_leave char_id=%d - position cleared", char_id);
                    }
                }
                if (session_id != 0) {
                    kick_session_deferred(char_id, session_id,
                        json{{"type","error"},{"message","map session ended"}},
                        "map leave");
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
            g_pending_pos[char_id] = { new_map, new_x, new_y, new_level, new_job, new_gid, new_war_map, tick_ms() };
            continue;
        }

        ClientSession* s = it->second;
        LOG_DEBUG("UDP pos char_id=%d %s(%d,%d) lv=%d job=%d grp=%d war=%d", char_id, new_map.c_str(), new_x, new_y, new_level, new_job, new_gid, new_war_map ? 1 : 0);
        const uint32_t now_pos = tick_ms();
        idx_set_position(s, new_map, new_x, new_y, now_pos);
        s->level    = new_level;
        s->job      = new_job;
        s->group_id = new_gid;
        s->war_map  = new_war_map;

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

        if (s->authed && tick_ms() - s->last_nearby_ms >= NEARBY_BROADCAST_INTERVAL_MS) {
            s->last_nearby_ms = tick_ms();
            send_nearby_players_deferred(s);
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

    // Boost the kernel UDP receive buffer. The map-server sends one
    // position ping per online player every second plus a fresh auth
    // advisory every 5 s; with 200+ players a Linux default rmem (~208 KB)
    // routinely overflows during login bursts, silently dropping advisories
    // and triggering "advisory never arrived" kicks on the next 15 s tick.
    // 4 MB is overkill for steady state but absorbs short bursts cleanly.
    {
        int bufsz = 4 * 1024 * 1024;
        setsockopt(g_udp_sock, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&bufsz), sizeof(bufsz));
        setsockopt(g_udp_sock, SOL_SOCKET, SO_SNDBUF,
                   reinterpret_cast<const char*>(&bufsz), sizeof(bufsz));
    }

    // Bind to the private map-server bridge/API port.
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(g_cfg.voice_api_port));
    if (inet_pton(AF_INET, g_cfg.voice_api_ip.c_str(), &addr.sin_addr) != 1) {
        LOG_ERROR("Invalid voice_api_ip: %s", g_cfg.voice_api_ip.c_str());
        closesocket(g_udp_sock);
        g_udp_sock = INVALID_SOCKET;
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }

    if (bind(g_udp_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        LOG_ERROR("UDP bind failed on %s:%d", g_cfg.voice_api_ip.c_str(), g_cfg.voice_api_port);
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

    LOG_NOTICE("UDP control receiver started on %s:%d", g_cfg.voice_api_ip.c_str(), g_cfg.voice_api_port);
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

static void send_json(VoiceSocket* ws, const json& j) {
    ws->send(j.dump(), VoiceTcp::OpCode::TEXT);
}

static constexpr uint32_t VOICE_UDP_MAGIC = 0x56554450u; // "VUDP"
static constexpr uint8_t  VOICE_UDP_VERSION = 1;
static constexpr uint8_t  VOICE_UDP_HELLO = 1;
static constexpr uint8_t  VOICE_UDP_VOICE = 2;
static constexpr uint8_t  VOICE_UDP_VOICE_FWD = 3;
static constexpr size_t   VOICE_UDP_CLIENT_HEADER = 33;
static constexpr size_t   VOICE_UDP_FWD_PREFIX = 6;
static constexpr uint32_t VOICE_UDP_ENDPOINT_TTL_MS = 30000;

static std::thread g_voice_udp_thread;
static std::atomic<bool> g_voice_udp_running{false};

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
        send_json_deferred(peer, json{{"type","whisper_ended"},{"sid",old_sid}});
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

    const uint32_t now = tick_ms();
    if (g_voice_udp_sock != INVALID_SOCKET && to->udp_ready &&
        to->udp_last_seen_ms != 0 &&
        now - to->udp_last_seen_ms <= VOICE_UDP_ENDPOINT_TTL_MS) {
        std::string udp;
        udp.resize(VOICE_UDP_FWD_PREFIX + out.size(), '\0');
        auto* uh = reinterpret_cast<unsigned char*>(&udp[0]);
        write_be_u32(uh, VOICE_UDP_MAGIC);
        uh[4] = VOICE_UDP_VERSION;
        uh[5] = VOICE_UDP_VOICE_FWD;
        std::memcpy(&udp[VOICE_UDP_FWD_PREFIX], out.data(), out.size());

        int rc = sendto(g_voice_udp_sock, udp.data(), static_cast<int>(udp.size()), 0,
                        reinterpret_cast<const sockaddr*>(&to->udp_addr),
                        sizeof(to->udp_addr));
        if (rc == static_cast<int>(udp.size())) {
            to->udp_sent_packets++;
            to->audio_sent_packets++;
            to->audio_sent_bytes += udp.size();
            return true;
        }
    }

    const size_t pressure_limit = audio_backpressure_limit_bytes();
    const unsigned int buffered = to->ws->getBufferedAmount();
    if (buffered > pressure_limit) {
        to->audio_backpressure_drops++;
        if (to->audio_last_pressure_log == 0 || now - to->audio_last_pressure_log > 5000) {
            to->audio_last_pressure_log = now;
            LOG_WARNING("audio backpressure drop to char_id=%d buffered=%u limit=%zu drops=%llu",
                        to->char_id, buffered, pressure_limit,
                        static_cast<unsigned long long>(to->audio_backpressure_drops));
        }
        return false;
    }

    auto status = to->ws->send(out, VoiceTcp::OpCode::BINARY);
    if (status != VoiceSocket::SUCCESS) {
        to->audio_backpressure_drops++;
        return false;
    }

    to->audio_sent_packets++;
    to->audio_sent_bytes += out.size();
    return true;
}

struct AudioRouteFlood { bool ban = false; int account_id = 0; };

// Core audio routing. The CALLER MUST hold g_session_mtx (shared) for the whole
// call so that `s` and every target session stay alive while we read/forward.
// Returns flood info: the g_flood_bans write needs an EXCLUSIVE lock, which the
// caller applies *after* releasing the shared lock (taking it here would deadlock).
static AudioRouteFlood route_audio_packet_locked(ClientSession* s, uint8_t channel, uint32_t gid,
                                                 uint16_t seq, const char* pcm, size_t pcm_bytes) {
    AudioRouteFlood flood;
    if (!s || !s->authed || !pcm || pcm_bytes == 0)
        return flood;

    std::lock_guard<std::mutex> route_lock(s->audio_route_mtx);

    const uint32_t packet_now = tick_ms();
    if (s->have_rx_seq) {
        if (s->last_rx_packet_ms != 0 && packet_now - s->last_rx_packet_ms > 500) {
            s->have_rx_seq = false;
        } else {
            const int16_t delta = static_cast<int16_t>(seq - s->last_rx_seq);
            if (delta <= 0) {
                LOG_DEBUG("audio duplicate/reorder drop char_id=%d seq=%u last=%u",
                          s->char_id, static_cast<unsigned>(seq), static_cast<unsigned>(s->last_rx_seq));
                return flood;
            }
        }
    }
    s->last_rx_seq = seq;
    s->have_rx_seq = true;
    s->last_rx_packet_ms = packet_now;

    if (!s->rate_limit_check()) {
        LOG_WARNING("rate limit drop char_id=%d violations=%d", s->char_id, s->flood_violations);
        if (s->is_flooding()) {
            LOG_ERROR("FLOOD BAN char_id=%d account_id=%d ip=%s — kicking and banning account for 5 min",
                      s->char_id, s->account_id, s->ip.c_str());
            // Kick now — `s` is alive under the caller's shared lock. The
            // g_flood_bans write needs an EXCLUSIVE lock, so defer it to the
            // caller (return the account_id below).
            if (s->ws) {
                send_json(s->ws, json{{"type","flood_banned"},{"duration_ms", FLOOD_BAN_DURATION_MS}});
                s->ws->end(1008, "flood ban");
            }
            flood.ban = true;
            flood.account_id = s->account_id;
        }
        return flood;
    }

    if (s->muted || s->deafened || !s->ptt) {
        LOG_DEBUG("audio drop due to tx state char_id=%d muted=%d deafened=%d ptt=%d",
                  s->char_id, s->muted ? 1 : 0, s->deafened ? 1 : 0, s->ptt ? 1 : 0);
        return flood;
    }

    LOG_DEBUG("audio rx char_id=%d ch=%d gid=%u seq=%u opus_bytes=%zu pos=%s(%d,%d) fresh=%d",
              s->char_id, channel, gid, (unsigned)seq, pcm_bytes,
              s->map.c_str(), s->x, s->y, s->last_position_ms != 0 ? 1 : 0);

    s->last_speaking_audio_ms.store(tick_ms());
    set_session_speaking_hat(s, true);

    std::vector<std::pair<ClientSession*, float>> targets;
    {
        // Caller already holds g_session_mtx (shared); the channel-index maps
        // and target sessions below are read under that lock.

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
            case 0:
                for_each_spatial_candidate(*s, [&](ClientSession* to) {
                    if (!to) return;
                    if (!should_forward(channel, gid, *s, *to)) return;
                    float vol = volume_for(channel, *s, *to);
                    if (vol > 0.0f) targets.push_back({to, vol});
                });
                break;
            case 1:
                if (s->party_id > 0) {
                    auto it = g_by_party.find(s->party_id);
                    if (it != g_by_party.end()) collect(it->second);
                }
                break;
            case 2:
                if (s->guild_id > 0) {
                    auto it = g_by_guild.find(s->guild_id);
                    if (it != g_by_guild.end()) collect(it->second);
                }
                break;
            case 3:
                if (s->chat_room_id != 0) {
                    auto it = g_by_room.find(s->chat_room_id);
                    if (it != g_by_room.end()) collect(it->second);
                }
                break;
            case 4:
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
            default:
                break;
        }

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
    return flood;
}

// Apply a flood ban that route_audio_packet_locked deferred (it needs an
// EXCLUSIVE lock, which cannot be taken while the shared routing lock is held).
static void apply_deferred_flood_ban(const AudioRouteFlood& flood) {
    if (!flood.ban) return;
    std::lock_guard<std::shared_mutex> lock(g_session_mtx);
    g_flood_bans[flood.account_id] = { tick_ms() + FLOOD_BAN_DURATION_MS };
}

// TCP entry point. `s` is the calling connection's own user_data (same thread),
// so it is guaranteed alive; a shared lock keeps the target sessions alive too.
static void route_audio_packet(ClientSession* s, uint8_t channel, uint32_t gid,
                               uint16_t seq, const char* pcm, size_t pcm_bytes) {
    AudioRouteFlood flood;
    {
        std::shared_lock<std::shared_mutex> lock(g_session_mtx);
        flood = route_audio_packet_locked(s, channel, gid, seq, pcm, pcm_bytes);
    }
    apply_deferred_flood_ban(flood);
}

// UDP entry point. The sender session was looked up cross-thread, so it must be
// re-found under the shared lock (and the session id re-verified) to guarantee
// it is still alive before any field is read — the connection thread could have
// freed it after the UDP receiver released its lock.
static void route_audio_packet_by_id(int char_id, uint64_t session_id,
                                     uint8_t channel, uint32_t gid, uint16_t seq,
                                     const char* pcm, size_t pcm_bytes) {
    AudioRouteFlood flood;
    {
        std::shared_lock<std::shared_mutex> lock(g_session_mtx);
        auto it = g_by_char_id.find(char_id);
        if (it == g_by_char_id.end() || !it->second ||
            it->second->session_id != session_id || !it->second->authed)
            return;
        flood = route_audio_packet_locked(it->second, channel, gid, seq, pcm, pcm_bytes);
    }
    apply_deferred_flood_ban(flood);
}

static void voice_udp_send_hello_ack(const sockaddr_in& to) {
    if (g_voice_udp_sock == INVALID_SOCKET) return;
    unsigned char packet[6] = {};
    write_be_u32(packet, VOICE_UDP_MAGIC);
    packet[4] = VOICE_UDP_VERSION;
    packet[5] = VOICE_UDP_HELLO;
    sendto(g_voice_udp_sock, reinterpret_cast<const char*>(packet), sizeof(packet), 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

static ClientSession* bind_or_find_udp_session(const unsigned char* p, size_t n,
                                               const sockaddr_in& from) {
    if (n < 26) return nullptr;
    const uint64_t sid = read_be_u64(p + 6);
    const int char_id = static_cast<int>(read_be_u32(p + 14));
    const uint64_t token = read_be_u64(p + 18);
    if (sid == 0 || char_id <= 0 || token == 0) return nullptr;

    auto it = g_by_char_id.find(char_id);
    if (it == g_by_char_id.end() || !it->second) return nullptr;
    ClientSession* s = it->second;
    if (!s->authed || s->session_id != sid || s->udp_token != token)
        return nullptr;

    s->udp_addr = from;
    s->udp_ready = true;
    s->udp_last_seen_ms = tick_ms();
    return s;
}

static void voice_udp_loop() {
    LOG_NOTICE("UDP voice receiver started on %s:%d", g_cfg.voice_ip.c_str(), g_cfg.voice_port);
    while (g_voice_udp_running.load()) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(g_voice_udp_sock, &readfds);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        int ret = select(static_cast<int>(g_voice_udp_sock) + 1, &readfds, nullptr, nullptr, &tv);
        if (ret <= 0 || !FD_ISSET(g_voice_udp_sock, &readfds))
            continue;

        unsigned char buf[1600] = {};
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        int n = recvfrom(g_voice_udp_sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n < 6) continue;
        if (read_be_u32(buf) != VOICE_UDP_MAGIC || buf[4] != VOICE_UDP_VERSION)
            continue;

        const uint8_t type = buf[5];
        if (type == VOICE_UDP_HELLO) {
            std::lock_guard<std::shared_mutex> lock(g_session_mtx);
            ClientSession* s = bind_or_find_udp_session(buf, static_cast<size_t>(n), from);
            if (s) {
                voice_udp_send_hello_ack(from);
                LOG_DEBUG("udp hello char_id=%d", s->char_id);
            }
            continue;
        }

        if (type != VOICE_UDP_VOICE || n < static_cast<int>(VOICE_UDP_CLIENT_HEADER + 1))
            continue;

        int      s_char_id = 0;
        uint64_t s_sid     = 0;
        uint16_t seq = 0;
        uint8_t channel = 0;
        uint32_t gid = 0;
        const unsigned char* opus = buf + VOICE_UDP_CLIENT_HEADER;
        const size_t opus_len = static_cast<size_t>(n) - VOICE_UDP_CLIENT_HEADER;
        {
            // Exclusive: bind_or_find updates the session's UDP endpoint.
            // Capture char_id + session_id so we never deref the raw pointer
            // after releasing the lock (the session could be freed by its own
            // connection thread). Routing re-finds it under a shared lock.
            std::lock_guard<std::shared_mutex> lock(g_session_mtx);
            ClientSession* s = bind_or_find_udp_session(buf, static_cast<size_t>(n), from);
            if (!s) continue;
            s_char_id = s->char_id;
            s_sid     = s->session_id;
            seq = static_cast<uint16_t>((buf[26] << 8) | buf[27]);
            channel = buf[28];
            gid = read_be_u32(buf + 29);
            s->udp_recv_packets++;
        }

        if (channel > 4) {
            LOG_WARNING("udp audio invalid channel char_id=%d ch=%d (drop)", s_char_id, channel);
            continue;
        }
        if (opus_len > 1500 || !validate_opus_packet(opus, opus_len)) {
            LOG_WARNING("udp audio invalid opus char_id=%d size=%zu (drop)", s_char_id, opus_len);
            continue;
        }

        route_audio_packet_by_id(s_char_id, s_sid, channel, gid, seq,
                                 reinterpret_cast<const char*>(opus), opus_len);
    }
    LOG_NOTICE("UDP voice receiver stopped");
}

static bool init_voice_udp_receiver() {
    if (g_voice_udp_sock != INVALID_SOCKET)
        return true;

    g_voice_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_voice_udp_sock == INVALID_SOCKET) {
        LOG_WARNING("UDP voice socket create failed");
        return false;
    }

    int yes = 1;
    setsockopt(g_voice_udp_sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));

    // Boost RX buffer for the live voice frame port. With ~200 online
    // players each transmitting opus at ~50 packets/s peak we can see
    // 10k pkt/s incoming during pile-ups; the default rmem is too small
    // to absorb that burst and the kernel silently drops frames, which
    // shows up as choppy/dropped audio rather than disconnects.
    {
        int bufsz = 4 * 1024 * 1024;
        setsockopt(g_voice_udp_sock, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&bufsz), sizeof(bufsz));
        setsockopt(g_voice_udp_sock, SOL_SOCKET, SO_SNDBUF,
                   reinterpret_cast<const char*>(&bufsz), sizeof(bufsz));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(g_cfg.voice_port));
    if (g_cfg.voice_ip.empty() || g_cfg.voice_ip == "0.0.0.0" || g_cfg.voice_ip == "*")
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    else
        inet_pton(AF_INET, g_cfg.voice_ip.c_str(), &addr.sin_addr);

    if (bind(g_voice_udp_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        LOG_WARNING("UDP voice bind failed on %s:%d", g_cfg.voice_ip.c_str(), g_cfg.voice_port);
        closesocket(g_voice_udp_sock);
        g_voice_udp_sock = INVALID_SOCKET;
        return false;
    }

#ifdef _WIN32
    u_long nonblock = 1;
    ioctlsocket(g_voice_udp_sock, FIONBIO, &nonblock);
#else
    fcntl(g_voice_udp_sock, F_SETFL, fcntl(g_voice_udp_sock, F_GETFL, 0) | O_NONBLOCK);
#endif

    g_voice_udp_running.store(true);
    g_voice_udp_thread = std::thread(voice_udp_loop);
    return true;
}

static void stop_voice_udp_receiver() {
    g_voice_udp_running.store(false);
    if (g_voice_udp_thread.joinable())
        g_voice_udp_thread.join();
    if (g_voice_udp_sock != INVALID_SOCKET) {
        closesocket(g_voice_udp_sock);
        g_voice_udp_sock = INVALID_SOCKET;
    }
}

void run_server() {
    con::init();
    load_voice_conf(g_conf_path.c_str());
    load_inter_conf("conf/inter_athena.conf");

    printf("\n");
    printf("%s ==========================================%s\n", con::CYAN,  con::RESET);
    printf("%s   Voice Server v1.0  (voice_athena)      %s\n", con::WHITE, con::RESET);
    printf("%s ==========================================%s\n", con::CYAN,  con::RESET);
    printf("\n");

    LOG_STATUS("Raw TCP     %s:%d", g_cfg.voice_ip.c_str(), g_cfg.voice_port);
    LOG_STATUS("Bridge API  %s:%d  secret=%s",
               g_cfg.voice_api_ip.c_str(), g_cfg.voice_api_port,
               g_cfg.voice_bridge_secret.empty() ? "disabled" : "enabled");
    if (g_cfg.voice_bridge_secret.empty() && !is_loopback_ip(g_cfg.voice_api_ip)) {
        LOG_WARNING("voice_bridge_secret is empty while Bridge API is not loopback-only");
    }
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
    LOG_INFO("Client auth secret: %s", g_cfg.client_secret.empty() ? "disabled (open)" : "enabled");
    printf("\n");

    if (!db_connect()) {
        LOG_WARNING("DB not available — party/guild channels will not work until connected");
    } else {
        db_ensure_ban_table();
        db_ensure_license_table();
        db_ensure_block_table();
        db_purge_expired_rows();
        db_load_bans(g_admin_banned);
        db_load_licenses(g_voice_licenses);
        db_load_blocks(g_voice_blocks);
    }

    // Capture the server loop before any clients connect so Ctrl+C/SIGTERM can
    // always defer shutdown work onto the event-loop thread.
    g_voice_loop.store(VoiceTcp::Loop::get());
    if (g_server_stop_requested.load())
        request_server_stop();

    // Start UDP receiver for position from Map Server
    if (!init_udp_receiver()) {
        LOG_WARNING("UDP receiver failed to start — positions from Map Server will not work");
    }
    if (!init_voice_udp_receiver()) {
        LOG_WARNING("UDP voice receiver failed to start — clients will fall back to TCP voice");
    }

    VoiceTcp::App app;
    g_app = &app;
    app.connection<ClientSession>("/*", {
        .maxPayloadLength = 2048,
        .maxBackpressure = 64 * 1024,
        .closeOnBackpressureLimit = false,
        .open = [](auto* ws) {
            auto* s = ws->getUserData();
            s->ws = ws;
            {
                std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                s->ip = normalize_ip(ws->getRemoteAddressAsText());
                g_by_ws[ws] = s;
            }
            LOG_STATUS("connection from %s", s->ip.c_str());
        },

        .message = [](auto* ws, std::string_view message, VoiceTcp::OpCode opCode) {
            auto* s = ws->getUserData();

            if (opCode == VoiceTcp::OpCode::TEXT) {
                json j;
                try {
                    j = json::parse(message);
                } catch (...) {
                    send_json(ws, json{{"type", "error"}, {"message", "invalid json"}});
                    return;
                }

                const std::string type = j.value("type", "");

                if (type == "auth") {
                    if (!g_cfg.client_secret.empty()) {
                        if (j.value("secret", "") != g_cfg.client_secret) {
                            LOG_WARNING("auth REJECTED — bad or missing secret  ip=%s", s->ip.c_str());
                            send_json(ws, json{{"type","error"},{"message","unauthorized"}});
                            ws->end(1008, "bad secret");
                            return;
                        }
                    }

                    // ── Reject re-auth on an already-authed session ───────────
                    // The DLL sometimes forgets to close its connection when the
                    // user logs out to char-select and picks a different char.
                    // It then sends a second `auth` frame on the same connection with
                    // the new char_id. If we let that through we'd overwrite
                    // `s->char_id` in place and leave orphan entries in
                    // g_by_char_id pointing to the same session (online count
                    // keeps climbing: 1 → 2 → 3 …). Kick instead so the DLL
                    // reconnects cleanly — the new connection will then run through
                    // the "one char per account" stale-session sweep below and
                    // see the old connection as a separate `other` pointer.
                    const int      new_aid = j.value("account_id", 0);
                    const int      new_cid = j.value("char_id", 0);
                    const uint64_t new_sid = j.value("session_id", static_cast<uint64_t>(0));
                    const uint32_t new_l1  = j.value("login_id1", 0u);

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
                    s->login_id1  = new_l1;

                    if (s->account_id <= 0 || s->char_id <= 0 || s->session_id == 0) {
                        send_json(ws, json{{"type", "error"}, {"message", "missing account_id or char_id"}});
                        return;
                    }

                    // ── Verify against Map Server advisory (anti-spoofing) ────
                    // Map server broadcasts (char_id, account_id) on login + renews
                    // every 5 s. On cold start the DLL's auth can arrive before the
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
                        } else if (ait->second.login_id1 != 0 &&
                                   ait->second.login_id1 != s->login_id1 &&
                                   (g_cfg.login_id1_strict || s->login_id1 != 0)) {
                            LOG_WARNING("auth SPOOF attempt — char_id=%d aid=%d login_id1 mismatch claimed=%u advisory=%u (ip=%s)",
                                        s->char_id, s->account_id, s->login_id1, ait->second.login_id1, s->ip.c_str());
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

                    // ── Per-account flood ban check ───────────────────────
                    {
                        std::shared_lock<std::shared_mutex> lock(g_session_mtx);
                        auto fit = g_flood_bans.find(s->account_id);
                        if (fit != g_flood_bans.end()) {
                            const uint32_t now_ms   = tick_ms();
                            const uint32_t until_ms = fit->second.until_tick;
                            // wrap-safe: ban still active iff signed (until - now) > 0
                            if ((int32_t)(until_ms - now_ms) > 0) {
                                const uint32_t remain_ms = until_ms - now_ms;
                                LOG_WARNING("flood-banned account_id=%d — refusing auth (%u s left)",
                                            s->account_id, remain_ms / 1000);
                                send_json(ws, json{{"type","flood_banned"},{"duration_ms", remain_ms}});
                                ws->end(1008, "flood ban");
                                return;
                            }
                        }
                    }

                    s->authed = true;
                    std::vector<SessionKick> sessions_to_close;
                    bool flap_bounced = false;   // set if the flap dampener refuses this newcomer
                    // Capture initial position to send your_pos after lock is released.
                    // Values may be from pending_pos (position arrived before auth) or
                    // from a prior reconnect (position already on session).
                    std::string init_map;
                    int init_x = 0, init_y = 0;
                    int online_snapshot = 0;
                    bool hold_for_advisory = false;   // decided under the lock below
                    {
                        std::lock_guard<std::shared_mutex> lock(g_session_mtx);

                        // Apply any position that arrived before auth
                        auto pit = g_pending_pos.find(s->char_id);
                        if (pit != g_pending_pos.end()) {
                            const PendingPos& pp = pit->second;
                            s->map = pp.map;
                            s->x   = pp.x;
                            s->y   = pp.y;
                            s->level    = pp.level;
                            s->job      = pp.job;
                            s->group_id = pp.group_id;
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
                        // a DLL that forgot to close its previous connection after the
                        // user switched character without restarting the client.
                        //
                        // IMPORTANT: only evict ghosts if THIS session is already
                        // CONFIRMED (a matching map-server advisory arrived = the
                        // char is really logged into the game). A still-provisional
                        // session may itself be the ghost — e.g. the same account
                        // open on two machines with different chars. Letting a
                        // provisional session kick the real one caused an endless
                        // reconnect ping-pong (each side kicking the other on
                        // connect). Provisional sessions whose advisory never
                        // arrives are removed by the advisory-grace timeout instead.
                        int  stale_account_kicks = 0;
                        if (!s->awaiting_advisory) {
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
                                sessions_to_close.push_back({ other->char_id, other->session_id, "stale account session" });
                            }
                            idx_remove(other);
                            // Do NOT erase from g_by_char_id here — the kicked
                            // session's own .close handler will erase its own
                            // (char_id → session) entry. If we erased here we'd
                            // race with that handler.
                        }
                        }
                        if (stale_account_kicks > 0) {
                            g_player_count -= stale_account_kicks;
                            if (g_player_count < 0) g_player_count = 0;
                        }

                        bool replacing = false;
                        auto it_old = g_by_char_id.find(s->char_id);
                        if (it_old != g_by_char_id.end() && it_old->second && it_old->second != s) {
                            ClientSession* old = it_old->second;

                            // ── Flap dampener ────────────────────────────────
                            // Count replacements for this char_id within a rolling
                            // window. Once we exceed the threshold AND a live holder
                            // is present, refuse to replace — keep the holder and
                            // bounce this newcomer instead (handled after the lock).
                            const uint32_t now_flap = tick_ms();
                            auto& fl = g_replace_flap[s->char_id];
                            if (now_flap - fl.window_start_tick > FLAP_WINDOW_MS) {
                                fl.window_start_tick = now_flap;
                                fl.count = 0;
                            }

                            if (old->authed && fl.count >= FLAP_THRESHOLD) {
                                flap_bounced = true;
                            } else {
                                if (old->authed) fl.count++;
                                replacing = old->authed; // only counts if old was actually online
                                LOG_WARNING("duplicate char_id=%d old_session=%llu new_session=%llu — closing old connection",
                                            s->char_id,
                                            static_cast<unsigned long long>(old->session_id),
                                            static_cast<unsigned long long>(s->session_id));
                                // Mark old session as no longer authed so its close handler
                                // won't decrement g_player_count (we handle the count here).
                                old->authed = false;
                                if (old->ws) {
                                    old->kicking = true;
                                    old->ws->send(json{{"type","error"},{"message","session replaced by new login"}}.dump(),
                                                 VoiceTcp::OpCode::TEXT);
                                    old->ws->end(1000, "replaced");
                                }
                                idx_remove(old);
                            }
                        }

                        if (flap_bounced) {
                            // Do NOT take the slot, index, or player-count. Revert our
                            // provisional auth and mark kicking so the close handler is
                            // a no-op. The actual ws->end() happens after the lock.
                            s->authed  = false;
                            s->kicking = true;
                        } else {
                            g_by_char_id[s->char_id] = s;

                            // Advisory may have arrived during the DB call above (which
                            // runs without any lock).  If so, confirm the session now so
                            // the maintenance loop doesn't kick it after ADVISORY_GRACE_MS.
                            if (s->awaiting_advisory) {
                                auto ait = g_auth_advisories.find(s->char_id);
                                if (ait != g_auth_advisories.end() && ait->second.account_id == s->account_id) {
                                    s->awaiting_advisory = false;
                                    LOG_DEBUG("auth advisory arrived during DB query — confirmed char_id=%d", s->char_id);
                                }
                            }

                            if (s->awaiting_advisory) {
                                s->auth_ok_pending = true;
                                hold_for_advisory  = true;
                            }

                            idx_insert(s);   // add to channel indexes

                            // If we replaced an existing authed session, count stays the same.
                            if (!replacing) g_player_count++;
                            online_snapshot = g_player_count;
                        }
                    }
                    for (const auto& kick : sessions_to_close) {
                        kick_session_deferred(kick.char_id, kick.session_id,
                            json{{"type","error"},{"message",kick.reason}},
                            kick.reason);
                    }

                    // Flap dampener tripped: a live holder already owns this
                    // char_id and it is being contested too fast. Bounce this
                    // newcomer with a backoff hint and do NOT proceed to auth_ok.
                    if (flap_bounced) {
                        LOG_WARNING("char_id=%d auth bounced — replacement flap dampener active (keeping current holder, ip=%s)",
                                    s->char_id, s->ip.c_str());
                        send_json(ws, json{{"type","error"},{"message","session contested"}});
                        ws->end(1008, "flap dampener");
                        return;
                    }

                    if (hold_for_advisory) {
                        LOG_INFO("auth held — waiting for advisory before auth_ok  char_id=%d aid=%d ip=%s",
                                 s->char_id, s->account_id, s->ip.c_str());
                        return;
                    }

                    // (g_player_count already updated inside the lock above)
                    LOG_NOTICE("(char_id=%d aid=%d sid=%llu name=%s ip=%s) party=%d guild=%d  [online: %d]",
                               s->char_id, s->account_id, static_cast<unsigned long long>(s->session_id),
                               s->char_name.c_str(), s->ip.c_str(),
                               s->party_id, s->guild_id, online_snapshot);
                    if (s->udp_token == 0)
                        s->udp_token = make_udp_token();
                    send_json(ws, json{
                        {"type", "auth_ok"},
                        {"udp_port", g_voice_udp_sock != INVALID_SOCKET ? g_cfg.voice_port : 0},
                        {"udp_token", s->udp_token}
                    });
                    // Notify DLL if this account is currently voice-banned,
                    // and (if license mode is on) whether they hold a license.
                    {
                        std::shared_lock<std::shared_mutex> lock(g_session_mtx);
                        auto bit = g_admin_banned.find(s->account_id);
                        if (bit != g_admin_banned.end()) {
                            const time_t now = time(nullptr);
                            if (bit->second == 0 || now < bit->second)
                                send_json(ws, json{{"type", "admin_banned"}});
                        }
                        if (g_cfg.voice_license_required) {
                            auto lit = g_voice_licenses.find(s->account_id);
                            const bool has = lit != g_voice_licenses.end() &&
                                             (lit->second == 0 || time(nullptr) < lit->second);
                            if (!has)
                                send_json(ws, json{{"type", "license_required"}});
                        }
                        // Send the player's block list — char_ids currently online
                        // for accounts on their block list (offline accounts can't
                        // be heard anyway and don't need UI flagging).
                        auto blk = g_voice_blocks.find(s->account_id);
                        if (blk != g_voice_blocks.end() && !blk->second.empty()) {
                            json arr = json::array();
                            for (auto& kv : g_by_char_id) {
                                ClientSession* o = kv.second;
                                if (!o || !o->authed) continue;
                                if (blk->second.count(o->account_id)) {
                                    arr.push_back({
                                        {"char_id", o->char_id},
                                        {"name",    o->char_name}
                                    });
                                }
                            }
                            send_json(ws, json{{"type","your_blocks"},{"blocked", arr}});
                        }
                    }
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
                        std::shared_lock<std::shared_mutex> nl(g_session_mtx);
                        send_nearby_players_deferred(s);
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
                    // Check target is online; collect data under lock, send outside.
                    json  to_target, to_self;
                    bool  offline = false, bypass = false;
                    {
                        std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                        auto it = g_by_char_id.find(target_id);
                        if (it == g_by_char_id.end() || !it->second || !it->second->authed) {
                            offline = true;
                        } else {
                            ClientSession* target = it->second;
                            clear_existing_whisper(s);
                            std::string sid = g_whisper.request(s->char_id, target_id);
                            s->whisper_sid = sid;
                            bypass    = voice_db_whisper_bypass(s->group_id);
                            if (bypass) {
                                g_whisper.accept(sid, target_id);
                                target->whisper_sid = sid;
                                to_target = json{{"type","whisper_active"},{"sid",sid},{"peer_name",s->char_name}};
                                to_self   = json{{"type","whisper_active"},{"sid",sid},{"peer_name",target->char_name}};
                            } else {
                                to_target = json{{"type","whisper_incoming"},{"sid",sid},{"from_char_id",s->char_id},{"from_name",s->char_name}};
                                to_self   = json{{"type","whisper_calling"},{"sid",sid},{"target_name",target->char_name}};
                            }
                            send_json_deferred(target, to_target);
                        }
                    }
                    if (offline) {
                        send_json(ws, json{{"type","whisper_lookup_fail"},{"reason","offline"}});
                        LOG_NOTICE("whisper_lookup '%s' (id=%d) — offline", name.c_str(), target_id);
                        return;
                    }
                    send_json(ws,        to_self);
                    if (bypass)
                        LOG_NOTICE("whisper_lookup bypass (GM) '%s' → char_id=%d, active", name.c_str(), target_id);
                    else
                        LOG_NOTICE("whisper_lookup '%s' → char_id=%d, calling", name.c_str(), target_id);
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
                    // Collect data under lock, send outside — avoids holding
                    // g_session_mtx while a TCP write blocks on a slow client.
                    json  to_target, to_self;
                    bool  offline = false;
                    bool  bypass  = false;
                    std::string log_src, log_dst, log_sid;
                    {
                        std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                        auto it = g_by_char_id.find(target_id);
                        if (it == g_by_char_id.end() || !it->second || !it->second->authed) {
                            offline = true;
                        } else {
                            ClientSession* target = it->second;
                            clear_existing_whisper(s);
                            std::string sid = g_whisper.request(s->char_id, target_id);
                            s->whisper_sid = sid;
                            bypass     = voice_db_whisper_bypass(s->group_id);
                            log_src    = s->char_name;
                            log_dst    = target->char_name;
                            log_sid    = sid;
                            if (bypass) {
                                g_whisper.accept(sid, target_id);
                                target->whisper_sid = sid;
                                to_target = json{{"type","whisper_active"},{"sid",sid},{"peer_name",s->char_name}};
                                to_self   = json{{"type","whisper_active"},{"sid",sid},{"peer_name",target->char_name}};
                            } else {
                                to_target = json{{"type","whisper_incoming"},{"sid",sid},{"from_char_id",s->char_id},{"from_name",s->char_name}};
                                to_self   = json{{"type","whisper_calling"},{"sid",sid},{"target_name",target->char_name}};
                            }
                            send_json_deferred(target, to_target);
                        }
                    }
                    if (offline) {
                        send_json(ws, json{{"type","whisper_unavailable"},{"reason","offline"}});
                        return;
                    }
                    send_json(ws,        to_self);
                    if (bypass)
                        LOG_NOTICE("whisper_bypass (GM) %s→%s sid=%s", log_src.c_str(), log_dst.c_str(), log_sid.c_str());
                    else
                        LOG_NOTICE("whisper_request %s→%s sid=%s", log_src.c_str(), log_dst.c_str(), log_sid.c_str());
                    return;
                }

                if (type == "whisper_accept") {
                    std::string sid = j.value("sid", "");
                    if (!g_whisper.accept(sid, s->char_id)) return;
                    int peer_id = g_whisper.get_peer(sid, s->char_id);
                    std::string  peer_name;
                    {
                        std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                        s->whisper_sid = sid;
                        auto it = g_by_char_id.find(peer_id);
                        if (it != g_by_char_id.end() && it->second) {
                            peer_name = it->second->char_name;
                            send_json_deferred(it->second, json{{"type","whisper_active"},{"sid",sid},{"peer_name",s->char_name}});
                        }
                    }
                    send_json(ws, json{{"type","whisper_active"},{"sid",sid},{"peer_name",peer_name}});
                    LOG_NOTICE("whisper_accept sid=%s char_id=%d", sid.c_str(), s->char_id);
                    return;
                }

                if (type == "whisper_reject") {
                    std::string sid = j.value("sid", "");
                    int peer_id = g_whisper.get_peer(sid, s->char_id);
                    if (!g_whisper.reject(sid, s->char_id)) return;
                    {
                        std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                        s->whisper_sid.clear();
                        auto it = g_by_char_id.find(peer_id);
                        if (it != g_by_char_id.end() && it->second) {
                            it->second->whisper_sid.clear();
                            send_json_deferred(it->second, json{{"type","whisper_rejected"},{"sid",sid}});
                        }
                    }
                    LOG_NOTICE("whisper_reject sid=%s", sid.c_str());
                    return;
                }

                if (type == "whisper_end") {
                    std::string sid = j.value("sid", "");
                    int peer_id = g_whisper.get_peer(sid, s->char_id);
                    if (!g_whisper.end(sid, s->char_id)) return;
                    {
                        std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                        s->whisper_sid.clear();
                        auto it = g_by_char_id.find(peer_id);
                        if (it != g_by_char_id.end() && it->second) {
                            it->second->whisper_sid.clear();
                            send_json_deferred(it->second, json{{"type","whisper_ended"},{"sid",sid}});
                        }
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

                if (type == "block_add") {
                    // Player wants to block another player's voice. The target
                    // is identified by char_id; we resolve to account_id so the
                    // block follows across char switches.
                    // Online-only: avoids a DB query on the WS event loop, which a
                    // modified client could otherwise spam with random char_ids to
                    // stall the server. The block UI only ever shows online players.
                    int target_cid = j.value("target_char_id", 0);
                    if (target_cid <= 0 || target_cid == s->char_id) return;

                    int target_aid = 0;
                    std::string target_name;
                    {
                        std::shared_lock<std::shared_mutex> rl(g_session_mtx);
                        auto it = g_by_char_id.find(target_cid);
                        if (it != g_by_char_id.end() && it->second && it->second->authed) {
                            target_aid  = it->second->account_id;
                            target_name = it->second->char_name;
                        }
                    }
                    if (target_aid <= 0 || target_aid == s->account_id) return;

                    int distinct_blockers = 0;
                    bool fire_alert = false;
                    bool newly_blocked = false;
                    {
                        std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                        // insert() returns false if already present — skip the DB
                        // write and ack so a spam client can't hammer the DB.
                        newly_blocked = g_voice_blocks[s->account_id].insert(target_aid).second;
                        if (newly_blocked && g_cfg.voice_block_alert_threshold > 0 &&
                            !g_block_alerted.count(target_aid)) {
                            for (auto& kv : g_voice_blocks)
                                if (kv.second.count(target_aid)) ++distinct_blockers;
                            if (distinct_blockers >= g_cfg.voice_block_alert_threshold) {
                                g_block_alerted.insert(target_aid);
                                fire_alert = true;
                            }
                        }
                    }
                    if (!newly_blocked) return; // already blocked — nothing to do
                    db_insert_block(s->account_id, target_aid, target_name);
                    send_json(ws, json{
                        {"type",         "block_added"},
                        {"target_char_id", target_cid},
                        {"target_name",   target_name}
                    });
                    LOG_INFO("block_add: aid=%d blocked aid=%d (%s)",
                             s->account_id, target_aid, target_name.c_str());
                    if (fire_alert) {
                        LOG_WARNING("TOXIC ALERT: account_id=%d (%s) blocked by %d distinct players",
                                    target_aid, target_name.c_str(), distinct_blockers);
                        send_map_block_alert(target_cid, target_name, distinct_blockers);
                    }
                    return;
                }

                if (type == "block_remove") {
                    // Online-only resolution (same DoS-avoidance rationale as block_add).
                    // The block UI lists players that were online at block/auth time,
                    // so an unblock target is normally still reachable. If they went
                    // offline, the unblock can be retried once they are online again.
                    int target_cid = j.value("target_char_id", 0);
                    if (target_cid <= 0) return;

                    int target_aid = 0;
                    {
                        std::shared_lock<std::shared_mutex> rl(g_session_mtx);
                        auto it = g_by_char_id.find(target_cid);
                        if (it != g_by_char_id.end() && it->second && it->second->authed)
                            target_aid = it->second->account_id;
                    }
                    if (target_aid <= 0) return;

                    bool was_blocked = false;
                    {
                        std::lock_guard<std::shared_mutex> lock(g_session_mtx);
                        auto bit = g_voice_blocks.find(s->account_id);
                        if (bit != g_voice_blocks.end()) {
                            was_blocked = bit->second.erase(target_aid) > 0;
                            if (bit->second.empty()) g_voice_blocks.erase(bit);
                        }
                    }
                    if (!was_blocked) return; // wasn't blocked — nothing to do
                    db_delete_block(s->account_id, target_aid);
                    send_json(ws, json{
                        {"type",          "block_removed"},
                        {"target_char_id", target_cid}
                    });
                    LOG_INFO("block_remove: aid=%d unblocked aid=%d",
                             s->account_id, target_aid);
                    return;
                }

                return;
            }

            if (opCode == VoiceTcp::OpCode::BINARY) {
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

                route_audio_packet(s, channel, gid, seq, pcm, pcm_bytes);
                return;
            }
        },

        .close = [](auto* ws, int code, std::string_view) {
            auto* s = ws->getUserData();
            int log_char_id = 0;
            int log_account_id = 0;
            int log_online = 0;
            uint64_t log_session_id = 0;
            std::string log_ip;
            int speaking_hat_off_char_id = 0;

            {
                std::lock_guard<std::shared_mutex> lock(g_session_mtx);

                // Notify whisper peer on disconnect
                if (!s->whisper_sid.empty()) {
                    int peer_id = g_whisper.get_peer(s->whisper_sid, s->char_id);
                    g_whisper.end(s->whisper_sid, s->char_id);
                    auto it = g_by_char_id.find(peer_id);
                    if (it != g_by_char_id.end() && it->second) {
                        send_json_deferred(it->second, json{{"type","whisper_ended"},{"sid",s->whisper_sid}});
                        it->second->whisper_sid.clear();
                    }
                }

                idx_remove(s);   // remove from all channel indexes
                g_by_ws.erase(ws);
                if (s->char_id != 0) {
                    auto it = g_by_char_id.find(s->char_id);
                    if (it != g_by_char_id.end() && it->second == s) {
                        g_by_char_id.erase(it);
                        // This session was the *current* holder of the slot (not a
                        // session that had already been replaced). Its genuine
                        // departure ends any contention, so clear the flap counter:
                        // a normal map-change reconnect, or the legit client taking
                        // over after the rival quit, must start from a clean slate
                        // and never inherit the rival's strike count.
                        g_replace_flap.erase(s->char_id);
                    }
                }

                if (s->authed)
                    g_player_count--;
                if (g_player_count < 0)
                    g_player_count = 0;

                log_char_id = s->char_id;
                log_account_id = s->account_id;
                log_session_id = s->session_id;
                log_ip = s->ip;
                log_online = g_player_count;
                if (s->speaking_hat_on.exchange(false))
                    speaking_hat_off_char_id = s->char_id;
            }

            if (speaking_hat_off_char_id > 0)
                send_map_speaking_hat(speaking_hat_off_char_id, false);

            const char* reason = (code == 1000) ? "normal"
                               : (code == 1001) ? "going away"
                               : (code == 1006) ? "lost connection"
                               : (code == 1011) ? "server error"
                               : "unknown";
            LOG_INFO("(char_id=%d aid=%d sid=%llu ip=%s) disconnected [%s]  [online: %d]",
                     log_char_id, log_account_id, static_cast<unsigned long long>(log_session_id), log_ip.c_str(), reason, log_online);
        }
    }).listen(g_cfg.voice_ip.c_str(), g_cfg.voice_port, [](auto* token) {
        if (token) LOG_STATUS("Listening on %s:%d", g_cfg.voice_ip.c_str(), g_cfg.voice_port);
        else       LOG_ERROR("Failed to listen on %s:%d", g_cfg.voice_ip.c_str(), g_cfg.voice_port);
    }).run();
    stop_voice_udp_receiver();
    stop_udp_receiver();
    {
        std::lock_guard<std::mutex> lock(g_db_mtx);
        if (g_db) {
            mysql_close(g_db);
            g_db = nullptr;
        }
    }
    g_app = nullptr;
    g_voice_loop.store(nullptr);
}
