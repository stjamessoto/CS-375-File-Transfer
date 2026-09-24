// Networking, logging and timing helpers shared by both versions.
#ifndef NET_UTILS_H
#define NET_UTILS_H

#include <arpa/inet.h>
#include <netdb.h>
#include <endian.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

// ---------------------------------------------------------------------------
// Challenge 2 — partial send()/recv()
//
// send() and recv() are allowed to move fewer bytes than requested (full socket
// buffers, TCP segmenting, signals). These wrappers loop until the whole buffer
// has been transferred.
// ---------------------------------------------------------------------------

// Returns `length` on success, -1 on error (errno is set).
inline ssize_t send_all(int sock, const void* buffer, size_t length) {
    const char* p = static_cast<const char*>(buffer);
    size_t total = 0;
    while (total < length) {
        // MSG_NOSIGNAL: a closed peer gives EPIPE instead of killing us with SIGPIPE.
        ssize_t n = send(sock, p + total, length - total, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(total);
}

// Returns `length` on success, a smaller count if the peer closed the
// connection early (errno is set to 0), or -1 on error (errno is set).
inline ssize_t recv_all(int sock, void* buffer, size_t length) {
    char* p = static_cast<char*>(buffer);
    size_t total = 0;
    while (total < length) {
        ssize_t n = recv(sock, p + total, length - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            errno = 0;
            break;
        }
        total += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(total);
}

// Fixed-width integers in network byte order (big-endian).
inline bool send_u8(int sock, uint8_t v) { return send_all(sock, &v, 1) == 1; }
inline bool send_u32(int sock, uint32_t v) {
    uint32_t n = htonl(v);
    return send_all(sock, &n, sizeof(n)) == sizeof(n);
}
inline bool send_u64(int sock, uint64_t v) {
    uint64_t n = htobe64(v);
    return send_all(sock, &n, sizeof(n)) == sizeof(n);
}
inline bool recv_u8(int sock, uint8_t& v) { return recv_all(sock, &v, 1) == 1; }
inline bool recv_u32(int sock, uint32_t& v) {
    uint32_t n;
    if (recv_all(sock, &n, sizeof(n)) != sizeof(n)) return false;
    v = ntohl(n);
    return true;
}
inline bool recv_u64(int sock, uint64_t& v) {
    uint64_t n;
    if (recv_all(sock, &n, sizeof(n)) != sizeof(n)) return false;
    v = be64toh(n);
    return true;
}

// Human-readable reason for the last failed send/recv (reads errno).
inline std::string io_error_reason() {
    if (errno == 0) return "connection closed by peer";
    if (errno == EAGAIN || errno == EWOULDBLOCK) return "idle timeout (no data from peer)";
    return std::strerror(errno);
}

// ---------------------------------------------------------------------------
// Addresses
// ---------------------------------------------------------------------------
inline std::string addr_to_string(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

inline std::string peer_address(int sock) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getpeername(sock, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return "unknown";
    return addr_to_string(addr);
}

// Client side: resolve `host` (IPv4 address or hostname) and connect.
// Returns a connected socket, or -1 after printing why it failed.
inline int connect_to_server(const std::string& host, uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (rc != 0) {
        std::cerr << "Error: cannot resolve host '" << host << "': " << gai_strerror(rc) << "\n";
        return -1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "Error: socket(): " << std::strerror(errno) << "\n";
        freeaddrinfo(res);
        return -1;
    }
    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        std::cerr << "Error: could not connect to " << host << ":" << port << " ("
                  << std::strerror(errno) << "). Is the server running?\n";
        close(sock);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return sock;
}

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
using Clock = std::chrono::steady_clock;

inline double ms_between(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}
inline double ms_since(Clock::time_point t) { return ms_between(t, Clock::now()); }

// ---------------------------------------------------------------------------
// Formatting + thread-safe logging
// ---------------------------------------------------------------------------
inline std::string format_bytes(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    std::ostringstream os;
    if (u == 0)
        os << bytes << " B";
    else
        os << std::fixed << std::setprecision(2) << v << " " << units[u];
    return os.str();
}

inline std::string format_ms(double ms) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(1) << ms << " ms";
    return os.str();
}

inline std::string format_rate(uint64_t bytes, double ms) {
    std::ostringstream os;
    double mbps = ms > 0 ? (bytes / (1024.0 * 1024.0)) / (ms / 1000.0) : 0.0;
    os << std::fixed << std::setprecision(1) << mbps << " MB/s";
    return os.str();
}

inline std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream os;
    os << std::put_time(&tm, "%H:%M:%S") << "." << std::setfill('0') << std::setw(3) << ms;
    return os.str();
}

inline std::mutex& log_mutex() {
    static std::mutex m;
    return m;
}

// log_msg("a", 1, "b") -> "[12:34:56.789] a1b". Safe to call from many threads.
template <typename... Args>
void log_msg(Args&&... args) {
    std::ostringstream os;
    (os << ... << args);
    std::lock_guard<std::mutex> lock(log_mutex());
    std::cout << "[" << timestamp() << "] " << os.str() << std::endl;
}

#endif
