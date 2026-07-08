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
void request_server_stop();

// ── Crash logger (Windows) ────────────────────────────────────────────────
// On an unhandled exception (e.g. the login-ซ้อน / duplicate-session crash) the
// process just dies and serv.bat restarts it — no stack, impossible to fix
// blindly. This writes a symbolized backtrace to voice_crash.txt so we can see
// the exact function + line that faulted. Needs voice-server.pdb next to the exe
// (Release builds generate one by default).
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#pragma comment(lib, "dbghelp.lib")

static LONG WINAPI voice_crash_handler(EXCEPTION_POINTERS* ep) {
    FILE* f = fopen("voice_crash.txt", "a");
    if (!f) return EXCEPTION_EXECUTE_HANDLER;

    fprintf(f, "\n[CRASH] code=%08lX addr=%p tid=%lu\n",
            ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress,
            GetCurrentThreadId());
    if (ep->ExceptionRecord->ExceptionCode == 0xC0000005 &&
        ep->ExceptionRecord->NumberParameters >= 2)
        fprintf(f, "  access-violation %s addr=%p\n",
                ep->ExceptionRecord->ExceptionInformation[0] ? "WRITE" : "READ",
                (void*)ep->ExceptionRecord->ExceptionInformation[1]);

    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(proc, nullptr, TRUE);

    CONTEXT* ctx = ep->ContextRecord;
    STACKFRAME64 sf{};
    DWORD machine;
#if defined(_M_X64)
    machine = IMAGE_FILE_MACHINE_AMD64;
    sf.AddrPC.Offset = ctx->Rip; sf.AddrFrame.Offset = ctx->Rbp; sf.AddrStack.Offset = ctx->Rsp;
#else
    machine = IMAGE_FILE_MACHINE_I386;
    sf.AddrPC.Offset = ctx->Eip; sf.AddrFrame.Offset = ctx->Ebp; sf.AddrStack.Offset = ctx->Esp;
#endif
    sf.AddrPC.Mode = sf.AddrFrame.Mode = sf.AddrStack.Mode = AddrModeFlat;

    char symbuf[sizeof(SYMBOL_INFO) + 256] = {};
    SYMBOL_INFO* sym = (SYMBOL_INFO*)symbuf;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;

    for (int i = 0; i < 40 &&
         StackWalk64(machine, proc, GetCurrentThread(), &sf, ctx,
                     nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr); ++i) {
        DWORD64 addr = sf.AddrPC.Offset;
        if (addr == 0) break;
        DWORD64 disp = 0;
        DWORD   ldisp = 0;
        IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof(line);
        if (SymFromAddr(proc, addr, &disp, sym)) {
            if (SymGetLineFromAddr64(proc, addr, &ldisp, &line))
                fprintf(f, "  #%02d %s + 0x%llx   (%s:%lu)\n",
                        i, sym->Name, (unsigned long long)disp, line.FileName, line.LineNumber);
            else
                fprintf(f, "  #%02d %s + 0x%llx\n", i, sym->Name, (unsigned long long)disp);
        } else {
            fprintf(f, "  #%02d 0x%llx\n", i, (unsigned long long)addr);
        }
    }
    fclose(f);
    return EXCEPTION_EXECUTE_HANDLER;   // let it terminate (serv.bat restarts)
}
#endif

static void signal_handler(int sig) {
#ifdef SIGUSR1
    if (sig == SIGUSR1) {
        g_reload_requested.store(true);   // handled safely in server thread
        return;
    }
#endif
    g_shutdown_requested.store(true);
}


int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(voice_crash_handler);   // symbolized backtrace → voice_crash.txt
#endif
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

    std::thread shutdown_monitor([]() {
        while (!g_shutdown_requested.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::cout << "\n[Voice] Shutting down...\n";
        request_server_stop();
    });

    // Run raw TCP server (blocks)
    run_server();

    g_shutdown_requested.store(true);
    if (shutdown_monitor.joinable())
        shutdown_monitor.join();

    return 0;
}
