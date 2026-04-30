#include "config.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

Config             g_config;
std::string        g_conf_path;
std::atomic<bool>  g_reload_requested{false};
std::atomic<bool>  g_shutdown_requested{false};

static std::string trim(const std::string& s) {
    auto a = s.find_first_not_of(" \t\r\n");
    auto b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

static std::string base_dir_of(const std::string& path) {
    auto slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? std::string{} : path.substr(0, slash + 1);
}

static std::string join_path(const std::string& base_dir, const std::string& rel) {
    if (rel.empty()) return base_dir;
    if (rel.size() > 1 && rel[1] == ':') return rel;
    if (rel[0] == '/' || rel[0] == '\\') return rel;
    if (rel.rfind("conf/", 0) == 0 || rel.rfind("conf\\", 0) == 0) return rel;
    return base_dir + rel;
}

static std::vector<int> parse_int_list(const std::string& s) {
    std::vector<int> out;
    auto start = s.find('[');
    auto end   = s.find(']');
    if (start == std::string::npos || end == std::string::npos) return out;
    std::string inner = s.substr(start + 1, end - start - 1);
    std::stringstream ss(inner);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        tok = trim(tok);
        if (!tok.empty()) {
            try { out.push_back(std::stoi(tok)); } catch (...) {}
        }
    }
    return out;
}

static void load_voice_db_from_conf(Config& cfg, const std::string& path) {
    cfg.blocked_maps.clear();
    cfg.whisper_bypass_groups.clear();
    cfg.voice_db_valid = false;

    std::string dir = path;
    auto slash = dir.find_last_of("/\\");
    dir = (slash != std::string::npos) ? dir.substr(0, slash + 1) : "";
    std::string yml_path = dir + "../db/voice_blocked_maps.yml";

    std::ifstream yf(yml_path);
    if (!yf.is_open()) {
        std::cerr << "[Config] Cannot open " << yml_path << "\n";
        return;
    }

    enum class Section { None, Header, BlockedMaps, WhisperBypass };
    Section section = Section::None;
    BlockedMapRule cur;
    bool has_cur = false;
    std::string header_type;
    int header_version = 0;
    bool header_seen = false;

    auto commit_rule = [&]() {
        if (has_cur && !cur.map_name.empty())
            cfg.blocked_maps.push_back(cur);
        cur = BlockedMapRule{};
        has_cur = false;
    };

    std::string line;
    while (std::getline(yf, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        std::string tline = trim(line);
        if (tline.empty()) continue;

        if (line[0] != ' ' && line[0] != '\t' && line[0] != '-') {
            commit_rule();
            if (tline == "Header:")                 { section = Section::Header; header_seen = true; }
            else if (tline == "blocked_maps:")      section = Section::BlockedMaps;
            else if (tline == "whisper_bypass:")    section = Section::WhisperBypass;
            else                                    section = Section::None;
            continue;
        }

        if (section == Section::Header) {
            auto item_sep = tline.find(':');
            if (item_sep == std::string::npos) continue;
            std::string k = trim(tline.substr(0, item_sep));
            std::string v = trim(tline.substr(item_sep + 1));
            if (k == "Type") {
                header_type = v;
            } else if (k == "Version") {
                try { header_version = std::stoi(v); } catch (...) { header_version = 0; }
            }
            continue;
        }

        if (section == Section::BlockedMaps) {
            if (tline.rfind("- ", 0) == 0) {
                commit_rule();
                std::string rest = trim(tline.substr(2));

                if (rest.rfind("map:", 0) == 0) {
                    cur = BlockedMapRule{};
                    cur.map_name = trim(rest.substr(4));
                    has_cur = true;
                } else if (!rest.empty()) {
                    cfg.blocked_maps.push_back(BlockedMapRule{rest, -1, -1, {}, {}});
                    has_cur = false;
                }
                continue;
            }

            if (has_cur) {
                auto item_sep = tline.find(':');
                if (item_sep == std::string::npos) continue;
                std::string k = trim(tline.substr(0, item_sep));
                std::string v = trim(tline.substr(item_sep + 1));

                if      (k == "min_level") { try { cur.min_level = std::stoi(v); } catch (...) {} }
                else if (k == "max_level") { try { cur.max_level = std::stoi(v); } catch (...) {} }
                else if (k == "jobs")      { for (int id : parse_int_list(v)) cur.jobs.insert(id); }
                else if (k == "group_ids") { for (int id : parse_int_list(v)) cur.group_ids.insert(id); }
            }
        }

        if (section == Section::WhisperBypass) {
            auto item_sep = tline.find(':');
            if (item_sep == std::string::npos) continue;
            std::string k = trim(tline.substr(0, item_sep));
            std::string v = trim(tline.substr(item_sep + 1));
            if (k == "group_ids") {
                for (int id : parse_int_list(v))
                    cfg.whisper_bypass_groups.insert(id);
            }
        }
    }
    commit_rule();

    if (!header_seen || header_type != "VOICE_BLOCKED_MAPS" || header_version != 1) {
        cfg.blocked_maps.clear();
        cfg.whisper_bypass_groups.clear();
        std::cerr << "[Config] Invalid " << yml_path
                  << " Header (expected Type=VOICE_BLOCKED_MAPS Version=1)\n";
        return;
    }

    cfg.voice_db_valid = true;
    std::cout << "[Config] Blocked maps loaded: " << cfg.blocked_maps.size()
              << " rules, bypass groups: " << cfg.whisper_bypass_groups.size() << "\n";
}

Config Config::load(const std::string& path) {
    Config cfg;

    std::unordered_set<std::string> visited_paths;
    std::vector<std::string> loaded_files;

    auto load_conf_file = [&](auto&& self, const std::string& file_path) -> void {
        if (!visited_paths.insert(file_path).second)
            return;

        std::ifstream f(file_path);
        if (!f.is_open()) {
            std::cerr << "[Config] Cannot open " << file_path << " - skipping\n";
            return;
        }

        loaded_files.push_back(file_path);
        const std::string base_dir = base_dir_of(file_path);

        std::string line;
        while (std::getline(f, line)) {
            auto cmt = line.find("//");
            if (cmt != std::string::npos) line = line.substr(0, cmt);
            auto sep = line.find(':');
            if (sep == std::string::npos) continue;
            std::string key = trim(line.substr(0, sep));
            std::string val = trim(line.substr(sep + 1));
            if (key.empty() || val.empty()) continue;

            if (key == "import") {
                self(self, join_path(base_dir, val));
                continue;
            }

            try {
                if      (key == "bind_ip" || key == "voice_ip")    cfg.ip              = val;
                else if (key == "voice_port")                       cfg.port            = std::stoi(val);
                else if (key == "voice_api_port")                   cfg.api_port        = std::stoi(val);
                else if (key == "proximity_full_range")             cfg.full_range      = std::stof(val);
                else if (key == "proximity_max_range")              cfg.max_range       = std::stof(val);
                else if (key == "proximity_update_ms")              cfg.prox_update_ms  = std::stoi(val);
                else if (key == "whisper_timeout")                  cfg.whisper_timeout = std::stoi(val);
                else if (key == "log_level")                        cfg.log_level       = std::stoi(val);
            } catch (...) {
            }
        }
    };

    load_conf_file(load_conf_file, path);
    if (loaded_files.empty()) {
        std::cerr << "[Config] No config files loaded - using defaults\n";
    } else {
        for (const auto& file : loaded_files)
            std::cout << "[Config] Loaded from " << file << "\n";
    }

    load_voice_db_from_conf(cfg, path);
    return cfg;
}

Config Config::load_voice_db(const std::string& conf_path) {
    Config cfg;
    load_voice_db_from_conf(cfg, conf_path);
    return cfg;
}
