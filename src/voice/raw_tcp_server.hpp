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

namespace uWS {

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
class WebSocket {
public:
    enum SendStatus {
        SUCCESS,
        BACKPRESSURE,
        DROPPED
    };

    WebSocket(SOCKET s, std::string ip) : socket_(s), remote_ip_(std::move(ip)) {}
    ~WebSocket() { close_socket(); }

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
struct WebSocketBehavior {
    unsigned int maxPayloadLength = 0;
    unsigned int maxBackpressure = 0;
    bool closeOnBackpressureLimit = false;
    std::function<void(WebSocket<false, true, UserData>*)> open;
    std::function<void(WebSocket<false, true, UserData>*, std::string_view, OpCode)> message;
    std::function<void(WebSocket<false, true, UserData>*, int, std::string_view)> close;
};

class App {
public:
    App();
    ~App();

    template <typename UserData>
    App& ws(const char*, WebSocketBehavior<UserData> behavior) {
        using Ws = WebSocket<false, true, UserData>;
        max_payload_ = behavior.maxPayloadLength ? behavior.maxPayloadLength : max_payload_;

        open_cb_ = [open = std::move(behavior.open)](void* ws) {
            if (open) open(static_cast<Ws*>(ws));
        };
        message_cb_ = [message = std::move(behavior.message)](void* ws, std::string_view data, OpCode op) {
            if (message) message(static_cast<Ws*>(ws), data, op);
        };
        close_cb_ = [close = std::move(behavior.close)](void* ws, int code, std::string_view reason) {
            if (close) close(static_cast<Ws*>(ws), code, reason);
        };
        delete_cb_ = [](void* ws) { delete static_cast<Ws*>(ws); };
        socket_cb_ = [](void* ws) { return static_cast<Ws*>(ws)->socket(); };
        close_ws_cb_ = [](void* ws) { static_cast<Ws*>(ws)->close_socket(); };
        make_ws_cb_ = [](SOCKET s, std::string ip) -> void* { return new Ws(s, std::move(ip)); };
        return *this;
    }

    App& listen(const char* ip, int port, std::function<void(void*)> cb);
    void run();
    void close();

private:
    void accept_loop();
    void client_loop(void* ws);

    SOCKET listen_socket_ = INVALID_SOCKET;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::vector<std::thread> client_threads_;
    std::vector<void*> active_ws_;
    std::mutex client_mtx_;

    uint32_t max_payload_ = 1024 * 1024;
    std::function<void*(SOCKET, std::string)> make_ws_cb_;
    std::function<SOCKET(void*)> socket_cb_;
    std::function<void(void*)> open_cb_;
    std::function<void(void*, std::string_view, OpCode)> message_cb_;
    std::function<void(void*, int, std::string_view)> close_cb_;
    std::function<void(void*)> delete_cb_;
    std::function<void(void*)> close_ws_cb_;
};

template <bool A, bool B, typename UserData>
typename WebSocket<A, B, UserData>::SendStatus WebSocket<A, B, UserData>::send(std::string_view data, OpCode op) {
    const uint8_t type = (op == OpCode::TEXT) ? 1 : 2;
    return send_frame(type, reinterpret_cast<const uint8_t*>(data.data()),
                      static_cast<uint32_t>(data.size())) ? SUCCESS : DROPPED;
}

template <bool A, bool B, typename UserData>
void WebSocket<A, B, UserData>::end(int, std::string_view) {
    if (socket_ != INVALID_SOCKET)
        send_frame(3, nullptr, 0);
    close_socket();
}

template <bool A, bool B, typename UserData>
void WebSocket<A, B, UserData>::close_socket() {
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
bool WebSocket<A, B, UserData>::send_frame(uint8_t type, const uint8_t* data, uint32_t len) {
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

} // namespace uWS
