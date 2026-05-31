#pragma once

#include <algorithm>
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
#  include <netinet/tcp.h>   // TCP_NODELAY
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

    Connection(SOCKET s, std::string ip) : socket_(s), remote_ip_(std::move(ip)) {
        send_thread_ = std::thread([this] { send_loop(); });
    }
    ~Connection() { close_socket(); }

    UserData* getUserData() { return &user_data_; }
    std::string_view getRemoteAddressAsText() const { return remote_ip_; }
    unsigned int getBufferedAmount() const {
        const size_t bytes = queued_bytes_.load();
        return bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<unsigned int>(bytes);
    }

    SendStatus send(std::string_view data, OpCode op);
    void end(int code, std::string_view reason);
    void close_socket();

    SOCKET socket() const { return socket_.load(); }
    bool is_closed() const { return closed_.load(); }

private:
    SendStatus send_frame(uint8_t type, const uint8_t* data, uint32_t len);
    void send_loop();
    bool send_all(const char* data, size_t len);

    std::atomic<SOCKET> socket_{INVALID_SOCKET};
    std::string remote_ip_;
    UserData user_data_{};
    std::atomic<bool> closed_{false};
    std::thread send_thread_;
    std::mutex queue_mtx_;
    std::condition_variable queue_cv_;
    std::deque<std::string> send_queue_;
    std::atomic<size_t> queued_bytes_{0};
    static constexpr size_t MAX_QUEUED_BYTES = 1024 * 1024;
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
    // Per-connection worker threads are DETACHED, not stored: storing them in a
    // vector that was only joined at shutdown leaked one std::thread object per
    // connection for the whole server lifetime (handle/memory growth under
    // normal connect/disconnect churn). We track a live count instead so
    // shutdown can wait for them to drain.
    std::atomic<int> live_client_threads_{0};
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
                      static_cast<uint32_t>(data.size()));
}

template <bool A, bool B, typename UserData>
void Connection<A, B, UserData>::end(int, std::string_view) {
    if (socket_.load() != INVALID_SOCKET)
        send_frame(3, nullptr, 0);
    close_socket();
}

template <bool A, bool B, typename UserData>
void Connection<A, B, UserData>::close_socket() {
    const bool was_open = !closed_.exchange(true);
    if (was_open) {
        SOCKET s = socket_.exchange(INVALID_SOCKET);
        if (s != INVALID_SOCKET) {
#ifdef _WIN32
            shutdown(s, SD_BOTH);
#else
            shutdown(s, SHUT_RDWR);
#endif
            closesocket(s);
        }
    }
    queue_cv_.notify_all();
    if (send_thread_.joinable() && send_thread_.get_id() != std::this_thread::get_id())
        send_thread_.join();
}

template <bool A, bool B, typename UserData>
typename Connection<A, B, UserData>::SendStatus Connection<A, B, UserData>::send_frame(uint8_t type, const uint8_t* data, uint32_t len) {
    if (closed_.load() || socket_.load() == INVALID_SOCKET)
        return DROPPED;

    std::string frame;
    frame.resize(8 + len);
    auto* h = reinterpret_cast<uint8_t*>(&frame[0]);
    h[0] = 0x56;
    h[1] = 0x54;
    h[2] = 0x01;
    h[3] = type;
    h[4] = static_cast<uint8_t>((len >> 24) & 0xFF);
    h[5] = static_cast<uint8_t>((len >> 16) & 0xFF);
    h[6] = static_cast<uint8_t>((len >> 8) & 0xFF);
    h[7] = static_cast<uint8_t>(len & 0xFF);
    if (len && data)
        std::memcpy(frame.data() + 8, data, len);

    const size_t frame_size = frame.size();
    std::lock_guard<std::mutex> lock(queue_mtx_);
    if (closed_.load() || socket_.load() == INVALID_SOCKET)
        return DROPPED;
    if (queued_bytes_.load() + frame_size > MAX_QUEUED_BYTES)
        return BACKPRESSURE;
    send_queue_.push_back(std::move(frame));
    queued_bytes_.fetch_add(frame_size);
    queue_cv_.notify_one();
    return SUCCESS;
}

template <bool A, bool B, typename UserData>
void Connection<A, B, UserData>::send_loop() {
    while (true) {
        std::string frame;
        {
            std::unique_lock<std::mutex> lock(queue_mtx_);
            queue_cv_.wait(lock, [this] { return closed_.load() || !send_queue_.empty(); });
            if (send_queue_.empty()) {
                if (closed_.load())
                    break;
                continue;
            }
            frame = std::move(send_queue_.front());
            send_queue_.pop_front();
            queued_bytes_.fetch_sub(frame.size());
        }

        if (!send_all(frame.data(), frame.size())) {
            SOCKET s = socket_.exchange(INVALID_SOCKET);
            if (s != INVALID_SOCKET) {
#ifdef _WIN32
                shutdown(s, SD_BOTH);
#else
                shutdown(s, SHUT_RDWR);
#endif
                closesocket(s);
            }
            closed_.store(true);
            queue_cv_.notify_all();
            break;
        }
    }
}

template <bool A, bool B, typename UserData>
bool Connection<A, B, UserData>::send_all(const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        SOCKET s = socket_.load();
        if (closed_.load() || s == INVALID_SOCKET)
            return false;
        int rc = ::send(s, data + sent,
                        static_cast<int>(std::min<size_t>(len - sent, 64 * 1024)), 0);
        if (rc <= 0)
            return false;
        sent += static_cast<size_t>(rc);
    }
    return true;
}

} // namespace VoiceTcp
