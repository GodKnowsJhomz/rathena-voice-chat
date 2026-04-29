#include "raw_tcp_server.hpp"

#include <algorithm>
#include <chrono>

#ifdef _WIN32
using socklen_t = int;
#endif

namespace VoiceTcp {
namespace {

constexpr uint16_t RAW_MAGIC = 0x5654;
constexpr uint8_t RAW_VERSION = 1;
constexpr uint32_t RAW_MAX_PAYLOAD = 1024 * 1024;

static uint16_t read_u16_be(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

static uint32_t read_u32_be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

static bool recv_all(SOCKET s, uint8_t* data, size_t len) {
    size_t got = 0;
    while (got < len) {
        int rc = ::recv(s, reinterpret_cast<char*>(data + got),
                        static_cast<int>(std::min<size_t>(len - got, 64 * 1024)), 0);
        if (rc <= 0)
            return false;
        got += static_cast<size_t>(rc);
    }
    return true;
}

static std::string sockaddr_to_ip(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, const_cast<in_addr*>(&addr.sin_addr), ip, sizeof(ip));
    return ip;
}

static sockaddr_in make_bind_addr(const char* ip, int port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (!ip || std::strcmp(ip, "0.0.0.0") == 0 || std::strcmp(ip, "*") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        inet_pton(AF_INET, ip, &addr.sin_addr);
    }
    return addr;
}

} // namespace

Loop* Loop::get() {
    static Loop loop;
    return &loop;
}

void Loop::defer(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        queue_.push_back(std::move(fn));
    }
    cv_.notify_one();
}

bool Loop::run_one(uint32_t timeout_ms) {
    std::function<void()> fn;
    {
        std::unique_lock<std::mutex> lock(mtx_);
        if (queue_.empty()) {
            cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return !queue_.empty(); });
        }
        if (queue_.empty())
            return false;
        fn = std::move(queue_.front());
        queue_.pop_front();
    }
    if (fn)
        fn();
    return true;
}

void Loop::wake() {
    cv_.notify_all();
}

App::App() {
#ifdef _WIN32
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

App::~App() {
    close();
#ifdef _WIN32
    WSACleanup();
#endif
}

App& App::listen(const char* ip, int port, std::function<void(void*)> cb) {
    listen_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket_ == INVALID_SOCKET) {
        if (cb) cb(nullptr);
        return *this;
    }

    int yes = 1;
    setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr = make_bind_addr(ip, port);
    if (bind(listen_socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        ::listen(listen_socket_, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
        if (cb) cb(nullptr);
        return *this;
    }

    running_.store(true);
    if (cb) cb(this);
    return *this;
}

void App::run() {
    if (listen_socket_ == INVALID_SOCKET)
        return;

    accept_thread_ = std::thread([this] { accept_loop(); });
    while (running_.load()) {
        Loop::get()->run_one(100);
    }

    if (accept_thread_.joinable())
        accept_thread_.join();

    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(client_mtx_);
        threads.swap(client_threads_);
    }
    for (auto& t : threads) {
        if (t.joinable())
            t.join();
    }
}

void App::close() {
    if (!running_.exchange(false))
        return;

    SOCKET s = listen_socket_;
    listen_socket_ = INVALID_SOCKET;
    if (s != INVALID_SOCKET) {
#ifdef _WIN32
        shutdown(s, SD_BOTH);
#else
        shutdown(s, SHUT_RDWR);
#endif
        closesocket(s);
    }

    std::vector<void*> clients;
    {
        std::lock_guard<std::mutex> lock(client_mtx_);
        clients = active_connections_;
    }
    for (void* conn : clients) {
        if (close_conn_cb_)
            close_conn_cb_(conn);
    }
    Loop::get()->wake();
}

void App::accept_loop() {
    while (running_.load()) {
        sockaddr_in from{};
        socklen_t len = sizeof(from);
        SOCKET client = accept(listen_socket_, reinterpret_cast<sockaddr*>(&from), &len);
        if (client == INVALID_SOCKET) {
            if (!running_.load())
                break;
            continue;
        }

        if (!make_conn_cb_) {
            closesocket(client);
            continue;
        }

        void* conn = make_conn_cb_(client, sockaddr_to_ip(from));
        {
            std::lock_guard<std::mutex> lock(client_mtx_);
            active_connections_.push_back(conn);
            client_threads_.emplace_back([this, conn] { client_loop(conn); });
        }
    }
}

void App::client_loop(void* conn) {
    int close_code = 1006;
    if (open_cb_)
        open_cb_(conn);

    SOCKET s = socket_cb_ ? socket_cb_(conn) : INVALID_SOCKET;
    while (running_.load() && s != INVALID_SOCKET) {
        uint8_t header[8] = {};
        if (!recv_all(s, header, sizeof(header)))
            break;
        if (read_u16_be(header) != RAW_MAGIC || header[2] != RAW_VERSION)
            break;

        uint8_t type = header[3];
        uint32_t len = read_u32_be(header + 4);
        if (len > (std::min)(max_payload_, RAW_MAX_PAYLOAD))
            break;

        std::string payload(len, '\0');
        if (len && !recv_all(s, reinterpret_cast<uint8_t*>(payload.data()), len))
            break;

        if (type == 1 || type == 2) {
            if (message_cb_)
                message_cb_(conn, payload, type == 1 ? OpCode::TEXT : OpCode::BINARY);
        } else if (type == 3) {
            close_code = 1000;
            break;
        } else {
            break;
        }

        s = socket_cb_ ? socket_cb_(conn) : INVALID_SOCKET;
    }

    if (close_cb_)
        close_cb_(conn, close_code, {});
    {
        std::lock_guard<std::mutex> lock(client_mtx_);
        active_connections_.erase(std::remove(active_connections_.begin(), active_connections_.end(), conn), active_connections_.end());
    }
    if (delete_cb_)
        delete_cb_(conn);
}

} // namespace VoiceTcp
