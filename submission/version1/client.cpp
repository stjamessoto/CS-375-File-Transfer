// Version 1 client: downloads one file from the sequential server and checks
// its SHA-256 checksum.
//
// Usage: ./client [-H host] [-p port] [-o output_dir] [-v] <filename>
// The file is saved as <output_dir>/received_<filename> (default output_dir: downloads).

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

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [-H host] [-p port] [-o output_dir] [-v] <filename>\n"
              << "  -H host        server address (default 127.0.0.1)\n"
              << "  -p port        server port (default " << PORT << ")\n"
              << "  -o output_dir  where to save the file (default: downloads)\n"
              << "  -v             verbose: also print the SHA-256 checksums\n";
}

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    uint16_t port = PORT;
    std::string out_dir = "downloads";
    bool verbose = false;

    int c;
    while ((c = getopt(argc, argv, "H:p:o:vh")) != -1) {
        switch (c) {
            case 'H': host = optarg; break;
            case 'p': port = static_cast<uint16_t>(std::atoi(optarg)); break;
            case 'o': out_dir = optarg; break;
            case 'v': verbose = true; break;
            default: usage(argv[0]); return c == 'h' ? 0 : 2;
        }
    }
    if (optind >= argc) {
        usage(argv[0]);
        return 2;
    }
    std::string filename = argv[optind];
    if (filename.empty() || filename.size() > MAX_FILENAME_LEN) {
        std::cerr << "Error: filename must be 1-" << MAX_FILENAME_LEN << " characters\n";
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    const std::string tag = "[client " + std::to_string(getpid()) + "] ";

    auto t_start = Clock::now();
    int sock = connect_to_server(host, port);
    if (sock < 0) return 1;

    // Request: [uint32 filename_length][filename]
    auto t_request = Clock::now();
    if (!send_u32(sock, static_cast<uint32_t>(filename.size())) ||
        send_all(sock, filename.data(), filename.size()) < 0) {
        std::cerr << tag << "Error sending request: " << io_error_reason() << "\n";
        close(sock);
        return 1;
    }

    uint8_t status;
    if (!recv_u8(sock, status)) {
        std::cerr << tag << "Error: no reply from server (" << io_error_reason() << ")\n";
        close(sock);
        return 1;
    }
    // Time between sending the request and the first reply byte. With the
    // sequential server this is mostly time spent waiting behind other clients.
    double wait_ms = ms_since(t_request);

    if (status != STATUS_OK) {
        std::cerr << tag << "FAILED " << filename << ": " << status_to_string(status) << "\n";
        close(sock);
        return 1;
    }

    uint32_t file_size;
    if (!recv_u32(sock, file_size)) {
        std::cerr << tag << "Error reading file size: " << io_error_reason() << "\n";
        close(sock);
        return 1;
    }

    mkdir(out_dir.c_str(), 0755);  // fine if it already exists
    std::string final_path = out_dir + "/received_" + base_name(filename);
    // Write to a private temp file and rename at the end, so clients that
    // download the same file at the same time never write into one file.
    std::string tmp_path = final_path + ".part." + std::to_string(getpid());
    std::ofstream output(tmp_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << tag << "Error: cannot create " << tmp_path << "\n";
        close(sock);
        return 1;
    }

    auto t_transfer = Clock::now();
    Sha256 sha;
    char buffer[BUFFER_SIZE];
    uint64_t received = 0;
    while (received < file_size) {
        // Never read past the file data, or we would swallow the checksum.
        size_t want = static_cast<size_t>(std::min<uint64_t>(BUFFER_SIZE, file_size - received));
        ssize_t bytes = recv(sock, buffer, want, 0);
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes <= 0) {
            if (bytes == 0) errno = 0;
            std::cerr << tag << "Error: transfer interrupted after " << received << " of " << file_size
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
        std::cerr << tag << "Error reading checksum: " << io_error_reason() << "\n";
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
        std::cerr << tag << "FAILED " << filename << ": "
                  << (output ? "SHA-256 mismatch, file corrupted" : "error writing to disk") << "\n"
                  << "  server: " << server_hex << "\n  local : " << local_hex << "\n";
        std::remove(tmp_path.c_str());
        return 1;
    }
    if (std::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        std::perror("rename");
        return 1;
    }

    double total_ms = ms_since(t_start);
    std::cout << tag << "OK " << filename << " -> " << final_path << " | " << format_bytes(received)
              << " | wait " << format_ms(wait_ms) << " | transfer " << format_ms(transfer_ms) << " | total "
              << format_ms(total_ms) << " | " << format_rate(received, transfer_ms) << " | SHA-256 verified"
              << std::endl;
    if (verbose) {
        std::cout << "  server SHA-256: " << server_hex << "\n  local  SHA-256: " << local_hex << std::endl;
    }
    return 0;
}
