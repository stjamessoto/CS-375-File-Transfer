// Version 2: thread-pool TCP file server.
//
// The main thread accepts connections and pushes each client socket into the
// pool's queue. A fixed number of worker threads pop sockets and handle the
// transfers in parallel.
//
// Challenges built in:
//   1. 64-bit file sizes (uint64_t on the wire)
//   2. send_all()/recv_all() for partial send()/recv()   (common/net_utils.h)
//   3. Pool metrics: active workers, queue length, average service time
//   4. File upload: files are stored in <dir>/uploads/, checksum-verified, and never overwritten
//   5. Resource limits: maximum file size, maximum concurrent connections, idle timeout
//
// Protocol (all integers in network byte order):
//   Server -> Client : [uint8 admission]  OK, or SERVER_BUSY followed by close
//   Client -> Server : [uint8 command 'D'|'U'][uint32 filename_length][filename]
//   Download ('D'):
//     Server -> Client : [uint8 status]  if OK: [uint64 file_size][file_data][32-byte SHA-256]
//   Upload ('U'):
//     Client -> Server : [uint64 file_size]
//     Server -> Client : [uint8 status]  (OK = go ahead, TOO_LARGE, ...)
//     Client -> Server : [file_data][32-byte SHA-256]
//     Server -> Client : [uint8 status]  if OK: [uint32 name_length][stored_name]
//
// Usage: ./server [-p port] [-d dir] [-t threads] [-m max_file_mb] [-c max_connections]
//                 [-i idle_timeout_s] [-D delay_ms]

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <atomic>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "../common/net_utils.h"
#include "../common/protocol.h"
#include "../common/sha256.h"
#include "threadpool.h"

#define THREADS 4

struct Options {
    uint16_t port = PORT;
    std::string root = ".";
    size_t threads = THREADS;
    uint64_t max_file_bytes = 1024ULL * 1024 * 1024;  // 1 GiB
    int max_connections = 64;                          // queued + being served
    int idle_timeout_s = 30;
    int delay_ms = 0;
};

static Options g_opt;
static std::string g_upload_dir;
static volatile sig_atomic_t g_running = 1;
static std::atomic<int> g_connections{0};  // admitted clients that have not finished yet

static void on_signal(int) { g_running = 0; }

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "  -p port     TCP port to listen on (default " << PORT << ")\n"
              << "  -d dir      folder whose files are served; uploads go to dir/uploads (default: .)\n"
              << "  -t threads  worker threads in the pool (default " << THREADS << ")\n"
              << "  -m MB       maximum file size for downloads and uploads (default 1024)\n"
              << "  -c N        maximum concurrent connections, queued + active (default 64)\n"
              << "  -i seconds  idle timeout: drop clients that send nothing for this long (default 30)\n"
              << "  -D ms       simulated extra work per request, for demos (default 0)\n";
}

// ---------------------------------------------------------------------------
// Download
// ---------------------------------------------------------------------------
static void handle_download(int sock, const std::string& peer, const std::string& filename) {
    std::string path = g_opt.root + "/" + filename;
    struct stat st {};
    std::ifstream file(path, std::ios::binary);
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || !file) {
        log_msg(peer, " | \"", filename, "\" not found");
        send_u8(sock, STATUS_NOT_FOUND);
        return;
    }

    uint64_t file_size = static_cast<uint64_t>(st.st_size);
    if (file_size > g_opt.max_file_bytes) {
        log_msg(peer, " | rejected: \"", filename, "\" is ", format_bytes(file_size), ", limit is ",
                format_bytes(g_opt.max_file_bytes));
        send_u8(sock, STATUS_TOO_LARGE);
        return;
    }

    if (!send_u8(sock, STATUS_OK) || !send_u64(sock, file_size)) {
        log_msg(peer, " | error sending header: ", io_error_reason());
        return;
    }
    log_msg(peer, " | sending \"", filename, "\", file size: ", format_bytes(file_size), " (", file_size, " bytes)");

    auto start = Clock::now();
    Sha256 sha;
    char buffer[BUFFER_SIZE];
    uint64_t sent = 0;
    while (sent < file_size) {
        size_t want = static_cast<size_t>(std::min<uint64_t>(BUFFER_SIZE, file_size - sent));
        file.read(buffer, want);
        std::streamsize got = file.gcount();
        if (got <= 0) {
            log_msg(peer, " | error reading \"", filename, "\" from disk after ", sent, " bytes");
            return;
        }
        sha.update(buffer, static_cast<size_t>(got));
        if (send_all(sock, buffer, static_cast<size_t>(got)) < 0) {
            log_msg(peer, " | send failed after ", sent, " bytes: ", io_error_reason());
            return;
        }
        sent += static_cast<uint64_t>(got);
    }

    uint8_t digest[Sha256::DIGEST_SIZE];
    sha.finish(digest);
    if (send_all(sock, digest, sizeof(digest)) < 0) {
        log_msg(peer, " | error sending checksum: ", io_error_reason());
        return;
    }
    double ms = ms_since(start);
    log_msg(peer, " | sent ", format_bytes(sent), " in ", format_ms(ms), " (", format_rate(sent, ms),
            ") sha256=", Sha256::to_hex(digest));
}

// ---------------------------------------------------------------------------
// Upload (Challenge 4)
// ---------------------------------------------------------------------------

// Deletes the temp file unless the upload succeeded.
struct TempFile {
    std::string path;
    int fd = -1;
    bool keep = false;
    ~TempFile() {
        if (fd >= 0) close(fd);
        if (!keep && !path.empty()) unlink(path.c_str());
    }
};

static bool write_all(int fd, const char* data, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

// Moves a finished temp file into uploads/ under a name nobody else has taken.
// "a.txt" becomes "a_1.txt", "a_2.txt", ... if needed. O_CREAT|O_EXCL claims
// the name atomically, so two uploads of the same name at the same moment
// cannot overwrite each other.
static std::string store_unique(const std::string& tmp_path, const std::string& filename) {
    size_t dot = filename.find_last_of('.');
    std::string stem = (dot == std::string::npos) ? filename : filename.substr(0, dot);
    std::string ext = (dot == std::string::npos) ? "" : filename.substr(dot);

    for (int i = 0; i < 10000; ++i) {
        std::string candidate = (i == 0) ? filename : stem + "_" + std::to_string(i) + ext;
        std::string path = g_upload_dir + "/" + candidate;
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            return "";
        }
        close(fd);
        if (rename(tmp_path.c_str(), path.c_str()) != 0) {  // atomically replaces the placeholder
            unlink(path.c_str());
            return "";
        }
        return candidate;
    }
    return "";
}

static void handle_upload(int sock, const std::string& peer, const std::string& filename) {
    uint64_t file_size;
    if (!recv_u64(sock, file_size)) {
        log_msg(peer, " | error reading upload size: ", io_error_reason());
        return;
    }
    log_msg(peer, " | wants to upload \"", filename, "\", file size: ", format_bytes(file_size));

    if (file_size > g_opt.max_file_bytes) {
        log_msg(peer, " | rejected upload: ", format_bytes(file_size), " exceeds limit of ",
                format_bytes(g_opt.max_file_bytes));
        send_u8(sock, STATUS_TOO_LARGE);
        return;
    }

    TempFile tmp;
    std::string templ = g_upload_dir + "/.upload-XXXXXX";
    tmp.fd = mkstemp(&templ[0]);  // unique temp file per upload
    if (tmp.fd < 0) {
        log_msg(peer, " | cannot create temp file: ", std::strerror(errno));
        send_u8(sock, STATUS_IO_ERROR);
        return;
    }
    tmp.path = templ;

    if (!send_u8(sock, STATUS_OK)) {
        log_msg(peer, " | error sending go-ahead: ", io_error_reason());
        return;
    }

    auto start = Clock::now();
    Sha256 sha;
    char buffer[BUFFER_SIZE];
    uint64_t received = 0;
    while (received < file_size) {
        size_t want = static_cast<size_t>(std::min<uint64_t>(BUFFER_SIZE, file_size - received));
        ssize_t n = recv(sock, buffer, want, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (n == 0) errno = 0;
            log_msg(peer, " | upload aborted after ", received, " of ", file_size, " bytes: ", io_error_reason());
            return;
        }
        sha.update(buffer, static_cast<size_t>(n));
        if (!write_all(tmp.fd, buffer, static_cast<size_t>(n))) {
            log_msg(peer, " | disk write failed: ", std::strerror(errno));
            send_u8(sock, STATUS_IO_ERROR);
            return;
        }
        received += static_cast<uint64_t>(n);
    }

    uint8_t client_digest[Sha256::DIGEST_SIZE];
    if (recv_all(sock, client_digest, sizeof(client_digest)) != sizeof(client_digest)) {
        log_msg(peer, " | error reading upload checksum: ", io_error_reason());
        return;
    }
    uint8_t local_digest[Sha256::DIGEST_SIZE];
    sha.finish(local_digest);

    if (Sha256::to_hex(client_digest) != Sha256::to_hex(local_digest)) {
        log_msg(peer, " | upload of \"", filename, "\" rejected: SHA-256 mismatch");
        send_u8(sock, STATUS_CHECKSUM_MISMATCH);
        return;
    }

    // Flush to disk before the file becomes visible under its real name.
    if (fsync(tmp.fd) != 0 || close(tmp.fd) != 0) {
        tmp.fd = -1;
        log_msg(peer, " | fsync/close failed: ", std::strerror(errno));
        send_u8(sock, STATUS_IO_ERROR);
        return;
    }
    tmp.fd = -1;

    std::string stored = store_unique(tmp.path, filename);
    if (stored.empty()) {
        log_msg(peer, " | could not store \"", filename, "\": ", std::strerror(errno));
        send_u8(sock, STATUS_IO_ERROR);
        return;
    }
    tmp.keep = true;  // renamed, nothing left to delete

    double ms = ms_since(start);
    log_msg(peer, " | stored upload as uploads/", stored, " (", format_bytes(received), " in ", format_ms(ms), ", ",
            format_rate(received, ms), ") sha256=", Sha256::to_hex(local_digest));

    if (!send_u8(sock, STATUS_OK) || !send_u32(sock, static_cast<uint32_t>(stored.size())) ||
        send_all(sock, stored.data(), stored.size()) < 0) {
        log_msg(peer, " | error sending upload confirmation: ", io_error_reason());
    }
}

// ---------------------------------------------------------------------------
// Per-client entry point (runs on a worker thread)
// ---------------------------------------------------------------------------
static void handle_client(int sock, int worker_id) {
    struct ConnectionGuard {
        ~ConnectionGuard() { --g_connections; }
    } guard;

    std::string peer = peer_address(sock) + " (worker " + std::to_string(worker_id) + ")";

    uint8_t command;
    uint32_t name_len;
    if (!recv_u8(sock, command) || !recv_u32(sock, name_len)) {
        log_msg(peer, " | error reading request: ", io_error_reason());
        return;
    }
    if (name_len == 0 || name_len > MAX_FILENAME_LEN) {
        log_msg(peer, " | rejected: invalid filename length ", name_len);
        send_u8(sock, STATUS_BAD_REQUEST);
        return;
    }
    std::string filename(name_len, '\0');
    if (recv_all(sock, &filename[0], name_len) != static_cast<ssize_t>(name_len)) {
        log_msg(peer, " | error reading filename: ", io_error_reason());
        return;
    }

    const char* action = command == CMD_DOWNLOAD ? "download" : command == CMD_UPLOAD ? "upload" : "unknown";
    log_msg(peer, " | request: ", action, " \"", filename, "\"");

    if (!is_safe_filename(filename)) {
        log_msg(peer, " | rejected: unsafe filename \"", filename, "\"");
        send_u8(sock, STATUS_BAD_REQUEST);
        return;
    }

    if (g_opt.delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(g_opt.delay_ms));

    if (command == CMD_DOWNLOAD) {
        handle_download(sock, peer, filename);
    } else if (command == CMD_UPLOAD) {
        handle_upload(sock, peer, filename);
    } else {
        log_msg(peer, " | rejected: unknown command byte ", static_cast<int>(command));
        send_u8(sock, STATUS_BAD_REQUEST);
    }
}

// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    int c;
    while ((c = getopt(argc, argv, "p:d:t:m:c:i:D:h")) != -1) {
        switch (c) {
            case 'p': g_opt.port = static_cast<uint16_t>(std::atoi(optarg)); break;
            case 'd': g_opt.root = optarg; break;
            case 't': g_opt.threads = static_cast<size_t>(std::atoi(optarg)); break;
            case 'm': g_opt.max_file_bytes = std::strtoull(optarg, nullptr, 10) * 1024ULL * 1024ULL; break;
            case 'c': g_opt.max_connections = std::atoi(optarg); break;
            case 'i': g_opt.idle_timeout_s = std::atoi(optarg); break;
            case 'D': g_opt.delay_ms = std::atoi(optarg); break;
            default: usage(argv[0]); return c == 'h' ? 0 : 2;
        }
    }
    if (g_opt.port == 0 || g_opt.threads == 0 || g_opt.max_connections <= 0 || g_opt.idle_timeout_s <= 0) {
        std::cerr << "Error: port, threads, max connections and idle timeout must be positive\n";
        return 2;
    }

    struct stat st {};
    if (stat(g_opt.root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        std::cerr << "Error: '" << g_opt.root << "' is not a directory\n";
        return 1;
    }
    g_upload_dir = g_opt.root + "/uploads";
    if (mkdir(g_upload_dir.c_str(), 0755) != 0 && errno != EEXIST) {
        std::cerr << "Error: cannot create " << g_upload_dir << ": " << std::strerror(errno) << "\n";
        return 1;
    }

    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }
    int yes = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) perror("setsockopt(SO_REUSEADDR)");

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(g_opt.port);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        perror("bind");
        std::cerr << "Is another server already running on port " << g_opt.port << "?\n";
        close(server_fd);
        return 1;
    }
    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    char real_root[PATH_MAX];
    if (!realpath(g_opt.root.c_str(), real_root)) std::snprintf(real_root, sizeof(real_root), "%s", g_opt.root.c_str());

    ThreadPool pool(g_opt.threads, handle_client);

    log_msg("Version 2 (thread pool) server listening on port ", g_opt.port, " with ", g_opt.threads, " worker threads");
    log_msg("Serving files from: ", real_root, "  (uploads -> ", real_root, "/uploads)");
    log_msg("Limits: max file ", format_bytes(g_opt.max_file_bytes), ", max ", g_opt.max_connections,
            " connections, idle timeout ", g_opt.idle_timeout_s, " s");
    if (g_opt.delay_ms > 0) log_msg("Simulated delay per request: ", g_opt.delay_ms, " ms");
    log_msg("Press Ctrl+C to stop.");

    uint64_t rejected = 0;
    while (g_running) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_socket = accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_socket < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        std::string peer = addr_to_string(client_addr);

        // Challenge 5: limit concurrent connections. The client waits for this
        // admission byte before sending anything, so refusing here is clean.
        if (g_connections.load() >= g_opt.max_connections) {
            send_u8(client_socket, STATUS_SERVER_BUSY);
            close(client_socket);
            ++rejected;
            log_msg(peer, " | REJECTED: server busy (", g_connections.load(), "/", g_opt.max_connections,
                    " connections in use)");
            continue;
        }

        // Challenge 5: idle timeout. A recv()/send() that makes no progress for
        // this long fails with EAGAIN, and the worker drops the client.
        timeval tv{};
        tv.tv_sec = g_opt.idle_timeout_s;
        setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        ++g_connections;
        if (!send_u8(client_socket, STATUS_OK)) {
            --g_connections;
            close(client_socket);
            continue;
        }
        log_msg(peer, " | client connected (client IP: ", peer.substr(0, peer.find(':')), ") | connections: ",
                g_connections.load(), "/", g_opt.max_connections);
        pool.enqueue(client_socket);
    }

    close(server_fd);
    log_msg("Shutting down: finishing ", pool.queue_length() + pool.active_workers(), " queued/active clients...");
    pool.shutdown();
    log_msg("Summary: ", pool.completed(), " requests served, ", rejected, " rejected as busy | avg service time ",
            format_ms(pool.average_service_ms()), " | avg queue wait ", format_ms(pool.average_wait_ms()),
            " | peak queue length ", pool.peak_queue_length());
    return 0;
}
