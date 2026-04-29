#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  define SOCKET int
#  define INVALID_SOCKET (-1)
#  define SOCKET_ERROR (-1)
#  define closesocket(s) ::close(s)
#endif

namespace VoiceTcp {

enum class OpCode {
    TEXT,
    BINARY
};

class Loop {
public:
    static Loop* get();

    void defer(std::function<void()> fn);
    bool run_one(uint32_t timeout_ms);
    void wake();

private:
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
};

template <bool, bool, typename UserData>
class Connection {
public:
    enum SendStatus {
        SUCCESS,
        BACKPRESSURE,
        DROPPED
    };

    Connection(SOCKET s, std::string ip) : socket_(s), remote_ip_(std::move(ip)) {}
    ~Connection() { close_socket(); }

    UserData* getUserData() { return &user_data_; }
    std::string_view getRemoteAddressAsText() const { return remote_ip_; }
    unsigned int getBufferedAmount() const { return 0; }

    SendStatus send(std::string_view data, OpCode op);
    void end(int code, std::string_view reason);
    void close_socket();

    SOCKET socket() const { return socket_; }
    bool is_closed() const { return closed_.load(); }

private:
    bool send_frame(uint8_t type, const uint8_t* data, uint32_t len);

    SOCKET socket_ = INVALID_SOCKET;
    std::string remote_ip_;
    UserData user_data_{};
    std::atomic<bool> closed_{false};
    std::mutex send_mtx_;
};

template <typename UserData>
struct ConnectionBehavior {
    unsigned int maxPayloadLength = 0;
    unsigned int maxBackpressure = 0;
    bool closeOnBackpressureLimit = false;
    std::function<void(Connection<false, true, UserData>*)> open;
    std::function<void(Connection<false, true, UserData>*, std::string_view, OpCode)> message;
    std::function<void(Connection<false, true, UserData>*, int, std::string_view)> close;
};

class App {
public:
    App();
    ~App();

    template <typename UserData>
    App& connection(const char*, ConnectionBehavior<UserData> behavior) {
        using Conn = Connection<false, true, UserData>;
        max_payload_ = behavior.maxPayloadLength ? behavior.maxPayloadLength : max_payload_;

        open_cb_ = [open = std::move(behavior.open)](void* conn) {
            if (open) open(static_cast<Conn*>(conn));
        };
        message_cb_ = [message = std::move(behavior.message)](void* conn, std::string_view data, OpCode op) {
            if (message) message(static_cast<Conn*>(conn), data, op);
        };
        close_cb_ = [close = std::move(behavior.close)](void* conn, int code, std::string_view reason) {
            if (close) close(static_cast<Conn*>(conn), code, reason);
        };
        delete_cb_ = [](void* conn) { delete static_cast<Conn*>(conn); };
        socket_cb_ = [](void* conn) { return static_cast<Conn*>(conn)->socket(); };
        close_conn_cb_ = [](void* conn) { static_cast<Conn*>(conn)->close_socket(); };
        make_conn_cb_ = [](SOCKET s, std::string ip) -> void* { return new Conn(s, std::move(ip)); };
        return *this;
    }

    App& listen(const char* ip, int port, std::function<void(void*)> cb);
    void run();
    void close();

private:
    void accept_loop();
    void client_loop(void* conn);

    SOCKET listen_socket_ = INVALID_SOCKET;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::vector<std::thread> client_threads_;
    std::vector<void*> active_connections_;
    std::mutex client_mtx_;

    uint32_t max_payload_ = 1024 * 1024;
    std::function<void*(SOCKET, std::string)> make_conn_cb_;
    std::function<SOCKET(void*)> socket_cb_;
    std::function<void(void*)> open_cb_;
    std::function<void(void*, std::string_view, OpCode)> message_cb_;
    std::function<void(void*, int, std::string_view)> close_cb_;
    std::function<void(void*)> delete_cb_;
    std::function<void(void*)> close_conn_cb_;
};

template <bool A, bool B, typename UserData>
typename Connection<A, B, UserData>::SendStatus Connection<A, B, UserData>::send(std::string_view data, OpCode op) {
    const uint8_t type = (op == OpCode::TEXT) ? 1 : 2;
    return send_frame(type, reinterpret_cast<const uint8_t*>(data.data()),
                      static_cast<uint32_t>(data.size())) ? SUCCESS : DROPPED;
}

template <bool A, bool B, typename UserData>
void Connection<A, B, UserData>::end(int, std::string_view) {
    if (socket_ != INVALID_SOCKET)
        send_frame(3, nullptr, 0);
    close_socket();
}

template <bool A, bool B, typename UserData>
void Connection<A, B, UserData>::close_socket() {
    if (closed_.exchange(true))
        return;
    std::lock_guard<std::mutex> lock(send_mtx_);
    SOCKET s = socket_;
    socket_ = INVALID_SOCKET;
    if (s != INVALID_SOCKET) {
#ifdef _WIN32
        shutdown(s, SD_BOTH);
#else
        shutdown(s, SHUT_RDWR);
#endif
        closesocket(s);
    }
}

template <bool A, bool B, typename UserData>
bool Connection<A, B, UserData>::send_frame(uint8_t type, const uint8_t* data, uint32_t len) {
    std::lock_guard<std::mutex> lock(send_mtx_);
    if (closed_.load() || socket_ == INVALID_SOCKET)
        return false;

    uint8_t header[8] = {
        0x56, 0x54,
        0x01,
        type,
        static_cast<uint8_t>((len >> 24) & 0xFF),
        static_cast<uint8_t>((len >> 16) & 0xFF),
        static_cast<uint8_t>((len >> 8) & 0xFF),
        static_cast<uint8_t>(len & 0xFF)
    };

    auto send_all = [this](const uint8_t* p, size_t n) {
        size_t sent = 0;
        while (sent < n) {
            int rc = ::send(socket_, reinterpret_cast<const char*>(p + sent),
                            static_cast<int>(std::min<size_t>(n - sent, 64 * 1024)), 0);
            if (rc <= 0) return false;
            sent += static_cast<size_t>(rc);
        }
        return true;
    };

    return send_all(header, sizeof(header)) && (len == 0 || send_all(data, len));
}

} // namespace VoiceTcp
