// Version 2 client: download a file from, or upload a file to, the thread-pool server.
//
// Usage:
//   ./client [-H host] [-p port] [-o output_dir] [-v] <filename>   download (default)
//   ./client [-H host] [-p port] [-v] -u <local_file>              upload
//
// Downloads are saved as <output_dir>/received_<filename> (default output_dir: downloads).
// Uploads are stored by the server in its uploads/ folder.

#include <sys/stat.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "../common/net_utils.h"
#include "../common/protocol.h"
#include "../common/sha256.h"

static std::string g_tag;

static void usage(const char* prog) {
    std::cerr << "Usage:\n"
              << "  " << prog << " [-H host] [-p port] [-o output_dir] [-v] <filename>    download\n"
              << "  " << prog << " [-H host] [-p port] [-v] -u <local_file>               upload\n"
              << "  -H host        server address (default 127.0.0.1)\n"
              << "  -p port        server port (default " << PORT << ")\n"
              << "  -o output_dir  where downloads are saved (default: downloads)\n"
              << "  -u             upload <local_file> instead of downloading\n"
              << "  -v             verbose: also print the SHA-256 checksums\n";
}

// Connects and waits for the server's admission byte (OK or SERVER_BUSY).
static int connect_and_admit(const std::string& host, uint16_t port) {
    int sock = connect_to_server(host, port);
    if (sock < 0) return -1;
    uint8_t admission;
    if (!recv_u8(sock, admission)) {
        std::cerr << g_tag << "Error: server closed the connection (" << io_error_reason() << ")\n";
        close(sock);
        return -1;
    }
    if (admission != STATUS_OK) {
        std::cerr << g_tag << "REFUSED: " << status_to_string(admission) << "\n";
        close(sock);
        return -1;
    }
    return sock;
}

static bool send_request(int sock, uint8_t command, const std::string& name) {
    return send_u8(sock, command) && send_u32(sock, static_cast<uint32_t>(name.size())) &&
           send_all(sock, name.data(), name.size()) >= 0;
}

static int download(const std::string& host, uint16_t port, const std::string& filename, const std::string& out_dir,
                    bool verbose) {
    auto t_start = Clock::now();
    int sock = connect_and_admit(host, port);
    if (sock < 0) return 1;

    auto t_request = Clock::now();
    if (!send_request(sock, CMD_DOWNLOAD, filename)) {
        std::cerr << g_tag << "Error sending request: " << io_error_reason() << "\n";
        close(sock);
        return 1;
    }

    uint8_t status;
    if (!recv_u8(sock, status)) {
        std::cerr << g_tag << "Error: no reply from server (" << io_error_reason() << ")\n";
        close(sock);
        return 1;
    }
    double wait_ms = ms_since(t_request);  // mostly time spent in the server's queue
    if (status != STATUS_OK) {
        std::cerr << g_tag << "FAILED " << filename << ": " << status_to_string(status) << "\n";
        close(sock);
        return 1;
    }

    uint64_t file_size;
    if (!recv_u64(sock, file_size)) {
        std::cerr << g_tag << "Error reading file size: " << io_error_reason() << "\n";
        close(sock);
        return 1;
    }

    mkdir(out_dir.c_str(), 0755);
    std::string final_path = out_dir + "/received_" + base_name(filename);
    std::string tmp_path = final_path + ".part." + std::to_string(getpid());
    std::ofstream output(tmp_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << g_tag << "Error: cannot create " << tmp_path << "\n";
        close(sock);
        return 1;
    }

    auto t_transfer = Clock::now();
    Sha256 sha;
    char buffer[BUFFER_SIZE];
    uint64_t received = 0;
    while (received < file_size) {
        size_t want = static_cast<size_t>(std::min<uint64_t>(BUFFER_SIZE, file_size - received));
        ssize_t bytes = recv(sock, buffer, want, 0);
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes <= 0) {
            if (bytes == 0) errno = 0;
            std::cerr << g_tag << "Error: transfer interrupted after " << received << " of " << file_size
                      << " bytes (" << io_error_reason() << ")\n";
            output.close();
            std::remove(tmp_path.c_str());
            close(sock);
            return 1;
        }
        sha.update(buffer, static_cast<size_t>(bytes));
        output.write(buffer, bytes);
        received += static_cast<uint64_t>(bytes);
    }

    uint8_t server_digest[Sha256::DIGEST_SIZE];
    if (recv_all(sock, server_digest, sizeof(server_digest)) != sizeof(server_digest)) {
        std::cerr << g_tag << "Error reading checksum: " << io_error_reason() << "\n";
        output.close();
        std::remove(tmp_path.c_str());
        close(sock);
        return 1;
    }
    double transfer_ms = ms_since(t_transfer);
    close(sock);

    uint8_t local_digest[Sha256::DIGEST_SIZE];
    sha.finish(local_digest);
    output.close();

    std::string server_hex = Sha256::to_hex(server_digest);
    std::string local_hex = Sha256::to_hex(local_digest);
    if (!output || server_hex != local_hex) {
        std::cerr << g_tag << "FAILED " << filename << ": "
                  << (output ? "SHA-256 mismatch, file corrupted" : "error writing to disk") << "\n"
                  << "  server: " << server_hex << "\n  local : " << local_hex << "\n";
        std::remove(tmp_path.c_str());
        return 1;
    }
    if (std::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        std::perror("rename");
        return 1;
    }

    std::cout << g_tag << "OK download " << filename << " -> " << final_path << " | " << format_bytes(received)
              << " | wait " << format_ms(wait_ms) << " | transfer " << format_ms(transfer_ms) << " | total "
              << format_ms(ms_since(t_start)) << " | " << format_rate(received, transfer_ms) << " | SHA-256 verified"
              << std::endl;
    if (verbose)
        std::cout << "  server SHA-256: " << server_hex << "\n  local  SHA-256: " << local_hex << std::endl;
    return 0;
}

static int upload(const std::string& host, uint16_t port, const std::string& local_path, bool verbose) {
    struct stat st {};
    std::ifstream file(local_path, std::ios::binary);
    if (stat(local_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || !file) {
        std::cerr << g_tag << "Error: cannot open local file '" << local_path << "'\n";
        return 1;
    }
    uint64_t file_size = static_cast<uint64_t>(st.st_size);
    std::string name = base_name(local_path);
    if (!is_safe_filename(name)) {
        std::cerr << g_tag << "Error: '" << name << "' is not an allowed file name\n";
        return 1;
    }

    auto t_start = Clock::now();
    int sock = connect_and_admit(host, port);
    if (sock < 0) return 1;

    auto t_request = Clock::now();
    if (!send_request(sock, CMD_UPLOAD, name) || !send_u64(sock, file_size)) {
        std::cerr << g_tag << "Error sending request: " << io_error_reason() << "\n";
        close(sock);
        return 1;
    }

    uint8_t status;
    if (!recv_u8(sock, status)) {
        std::cerr << g_tag << "Error: no reply from server (" << io_error_reason() << ")\n";
        close(sock);
        return 1;
    }
    double wait_ms = ms_since(t_request);
    if (status != STATUS_OK) {
        std::cerr << g_tag << "FAILED upload " << name << ": " << status_to_string(status) << "\n";
        close(sock);
        return 1;
    }

    auto t_transfer = Clock::now();
    Sha256 sha;
    char buffer[BUFFER_SIZE];
    uint64_t sent = 0;
    while (sent < file_size) {
        size_t want = static_cast<size_t>(std::min<uint64_t>(BUFFER_SIZE, file_size - sent));
        file.read(buffer, want);
        std::streamsize got = file.gcount();
        if (got <= 0) {
            std::cerr << g_tag << "Error reading local file after " << sent << " bytes\n";
            close(sock);
            return 1;
        }
        sha.update(buffer, static_cast<size_t>(got));
        if (send_all(sock, buffer, static_cast<size_t>(got)) < 0) {
            std::cerr << g_tag << "Error: upload interrupted after " << sent << " bytes (" << io_error_reason()
                      << ")\n";
            close(sock);
            return 1;
        }
        sent += static_cast<uint64_t>(got);
    }
    uint8_t digest[Sha256::DIGEST_SIZE];
    sha.finish(digest);
    if (send_all(sock, digest, sizeof(digest)) < 0) {
        std::cerr << g_tag << "Error sending checksum: " << io_error_reason() << "\n";
        close(sock);
        return 1;
    }

    uint8_t final_status;
    if (!recv_u8(sock, final_status)) {
        std::cerr << g_tag << "Error: no confirmation from server (" << io_error_reason() << ")\n";
        close(sock);
        return 1;
    }
    if (final_status != STATUS_OK) {
        std::cerr << g_tag << "FAILED upload " << name << ": " << status_to_string(final_status) << "\n";
        close(sock);
        return 1;
    }
    uint32_t stored_len;
    std::string stored;
    if (recv_u32(sock, stored_len) && stored_len <= MAX_FILENAME_LEN + 16) {
        stored.resize(stored_len);
        if (recv_all(sock, &stored[0], stored_len) != static_cast<ssize_t>(stored_len)) stored = name;
    }
    double transfer_ms = ms_since(t_transfer);
    close(sock);

    std::cout << g_tag << "OK upload " << local_path << " -> server:uploads/" << stored << " | "
              << format_bytes(file_size) << " | wait " << format_ms(wait_ms) << " | transfer "
              << format_ms(transfer_ms) << " | total " << format_ms(ms_since(t_start)) << " | "
              << format_rate(file_size, transfer_ms) << " | SHA-256 verified by server" << std::endl;
    if (verbose) std::cout << "  SHA-256: " << Sha256::to_hex(digest) << std::endl;
    return 0;
}

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    uint16_t port = PORT;
    std::string out_dir = "downloads";
    bool upload_mode = false, verbose = false;

    int c;
    while ((c = getopt(argc, argv, "H:p:o:uvh")) != -1) {
        switch (c) {
            case 'H': host = optarg; break;
            case 'p': port = static_cast<uint16_t>(std::atoi(optarg)); break;
            case 'o': out_dir = optarg; break;
            case 'u': upload_mode = true; break;
            case 'v': verbose = true; break;
            default: usage(argv[0]); return c == 'h' ? 0 : 2;
        }
    }
    if (optind >= argc) {
        usage(argv[0]);
        return 2;
    }
    std::string target = argv[optind];
    if (target.empty() || (!upload_mode && target.size() > MAX_FILENAME_LEN)) {
        std::cerr << "Error: filename must be 1-" << MAX_FILENAME_LEN << " characters\n";
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    g_tag = "[client " + std::to_string(getpid()) + "] ";

    return upload_mode ? upload(host, port, target, verbose) : download(host, port, target, out_dir, verbose);
}
