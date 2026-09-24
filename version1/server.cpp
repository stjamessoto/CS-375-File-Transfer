// Version 1: single-threaded (sequential) TCP file server.
//
// One client is served at a time. While a file is being sent, every other
// client waits in the kernel's listen() backlog; that is the blocking
// behavior this version exists to show.
//
// Protocol (all integers in network byte order):
//   Client -> Server : [uint32 filename_length][filename]
//   Server -> Client : [uint8 status]
//                      if status == OK: [uint32 file_size][file_data][32-byte SHA-256]
//
// Usage: ./server [-p port] [-d directory] [-D delay_ms]

#include <sys/stat.h>

#include <climits>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "../common/net_utils.h"
#include "../common/protocol.h"
#include "../common/sha256.h"

static volatile sig_atomic_t g_running = 1;
static void on_signal(int) { g_running = 0; }

struct Options {
    uint16_t port = PORT;
    std::string root = ".";
    int delay_ms = 0;
};

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [-p port] [-d directory] [-D delay_ms]\n"
              << "  -p port       TCP port to listen on (default " << PORT << ")\n"
              << "  -d directory  folder whose files are served (default: current folder)\n"
              << "  -D delay_ms   simulated extra work per request, makes blocking easy to see (default 0)\n";
}

// Handles one request. Returns true if the whole file was delivered.
static bool send_file(int client_socket, const Options& opt, const std::string& peer) {
    uint32_t name_len;
    if (!recv_u32(client_socket, name_len)) {
        log_msg(peer, " | error reading filename length: ", io_error_reason());
        return false;
    }
    // The starter code copied name_len bytes into char[256] without checking,
    // so a large value overflowed the buffer.
    if (name_len == 0 || name_len > MAX_FILENAME_LEN) {
        log_msg(peer, " | rejected: invalid filename length ", name_len);
        send_u8(client_socket, STATUS_BAD_REQUEST);
        return false;
    }

    std::string filename(name_len, '\0');
    if (recv_all(client_socket, &filename[0], name_len) != static_cast<ssize_t>(name_len)) {
        log_msg(peer, " | error reading filename: ", io_error_reason());
        return false;
    }
    log_msg(peer, " | file requested: \"", filename, "\"");

    if (!is_safe_filename(filename)) {
        log_msg(peer, " | rejected: unsafe filename \"", filename, "\"");
        send_u8(client_socket, STATUS_BAD_REQUEST);
        return false;
    }

    if (opt.delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(opt.delay_ms));

    std::string path = opt.root + "/" + filename;
    struct stat st {};
    std::ifstream file(path, std::ios::binary);
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || !file) {
        log_msg(peer, " | \"", filename, "\" not found");
        send_u8(client_socket, STATUS_NOT_FOUND);
        return false;
    }

    uint64_t file_size = static_cast<uint64_t>(st.st_size);
    if (file_size > UINT32_MAX) {
        // The Version 1 wire format has a 32-bit size field (see Challenge 1).
        log_msg(peer, " | rejected: ", format_bytes(file_size), " does not fit in a uint32_t size field");
        send_u8(client_socket, STATUS_TOO_LARGE);
        return false;
    }

    if (!send_u8(client_socket, STATUS_OK) || !send_u32(client_socket, static_cast<uint32_t>(file_size))) {
        log_msg(peer, " | error sending header: ", io_error_reason());
        return false;
    }
    log_msg(peer, " | sending \"", filename, "\", file size: ", format_bytes(file_size), " (", file_size, " bytes)");

    auto start = Clock::now();
    Sha256 sha;
    char buffer[BUFFER_SIZE];
    uint64_t sent = 0;
    // The starter loop used `while (!file.eof())`, which runs one extra time
    // after the last read. Counting bytes against the known size avoids that.
    while (sent < file_size) {
        size_t want = static_cast<size_t>(std::min<uint64_t>(BUFFER_SIZE, file_size - sent));
        file.read(buffer, want);
        std::streamsize got = file.gcount();
        if (got <= 0) {
            log_msg(peer, " | error reading \"", filename, "\" from disk after ", sent, " bytes");
            return false;
        }
        sha.update(buffer, static_cast<size_t>(got));
        if (send_all(client_socket, buffer, static_cast<size_t>(got)) < 0) {
            log_msg(peer, " | send failed after ", sent, " bytes: ", io_error_reason());
            return false;
        }
        sent += static_cast<uint64_t>(got);
    }

    uint8_t digest[Sha256::DIGEST_SIZE];
    sha.finish(digest);
    if (send_all(client_socket, digest, sizeof(digest)) < 0) {
        log_msg(peer, " | error sending checksum: ", io_error_reason());
        return false;
    }

    double ms = ms_since(start);
    log_msg(peer, " | sent ", format_bytes(sent), " in ", format_ms(ms), " (", format_rate(sent, ms),
            ") sha256=", Sha256::to_hex(digest));
    return true;
}

int main(int argc, char* argv[]) {
    Options opt;
    int c;
    while ((c = getopt(argc, argv, "p:d:D:h")) != -1) {
        switch (c) {
            case 'p': opt.port = static_cast<uint16_t>(std::atoi(optarg)); break;
            case 'd': opt.root = optarg; break;
            case 'D': opt.delay_ms = std::atoi(optarg); break;
            default: usage(argv[0]); return c == 'h' ? 0 : 2;
        }
    }
    if (opt.port == 0) {
        std::cerr << "Error: invalid port\n";
        return 2;
    }

    struct stat st {};
    if (stat(opt.root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        std::cerr << "Error: '" << opt.root << "' is not a directory\n";
        return 1;
    }

    // Ctrl+C stops the accept loop cleanly. No SA_RESTART, so accept() returns EINTR.
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
    address.sin_port = htons(opt.port);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        perror("bind");
        std::cerr << "Is another server already running on port " << opt.port << "?\n";
        close(server_fd);
        return 1;
    }
    if (listen(server_fd, 5) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    char real_root[PATH_MAX];
    if (!realpath(opt.root.c_str(), real_root)) std::snprintf(real_root, sizeof(real_root), "%s", opt.root.c_str());

    log_msg("Version 1 (sequential) server listening on port ", opt.port);
    log_msg("Serving files from: ", real_root);
    if (opt.delay_ms > 0) log_msg("Simulated delay per request: ", opt.delay_ms, " ms");
    log_msg("Press Ctrl+C to stop.");

    int served = 0, failed = 0;
    double total_ms = 0;

    while (g_running) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_socket = accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_socket < 0) {
            if (errno == EINTR) continue;  // Ctrl+C, loop condition handles it
            perror("accept");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        std::string peer = addr_to_string(client_addr);
        log_msg(peer, " | client connected (client IP: ", peer.substr(0, peer.find(':')), ")");

        auto t0 = Clock::now();
        bool ok = send_file(client_socket, opt, peer);
        close(client_socket);
        double ms = ms_since(t0);

        ok ? ++served : ++failed;
        total_ms += ms;
        log_msg(peer, " | connection closed, handled in ", format_ms(ms));
    }

    close(server_fd);
    int total = served + failed;
    log_msg("Shutting down. Requests: ", total, " (", served, " succeeded, ", failed, " failed)",
            total ? ", average handling time " + format_ms(total_ms / total) : std::string());
    return 0;
}
