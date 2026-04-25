#include "config.hpp"
#include "room_manager.hpp"
#include "whisper_manager.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

// Forward declare from server.cpp
void run_server();

static std::atomic<bool> g_running{true};

static void signal_handler(int sig) {
#ifdef SIGUSR1
    if (sig == SIGUSR1) {
        g_reload_requested.store(true);   // handled safely in uWS thread
        return;
    }
#endif
    g_running = false;
    std::cout << "\n[Voice] Shutting down...\n";
    std::exit(0);
}


int main(int argc, char* argv[]) {
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);
#ifdef SIGUSR1
    std::signal(SIGUSR1, signal_handler);   // kill -USR1 <pid>  →  reload config
#endif

    // Load config
    g_conf_path = "conf/voice_athena.conf";
    if (argc > 1) g_conf_path = argv[1];
    g_config = Config::load(g_conf_path);

    std::cout << R"(
 __   __    _           ___
 \ \ / /__ (_) ___ ___ / __| ___ _ ___ _____ _ _
  \ V / _ \| |/ __/ -_)__ \/ -_) '_\ V / -_) '_|
   \_/\___/|_|\___\___|___/\___|_|  \_/\___|_|
  Voice Server v1.0  |  port )" << g_config.port << R"(
  Developed by TiTaNos  |  https://sitecraft.in.th
  Discord: https://discord.com/invite/aTkZw9ZrQ9
)" << "\n";

    // Run WebSocket server (blocks)
    run_server();

    return 0;
}
