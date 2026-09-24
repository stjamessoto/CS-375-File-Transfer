// Wire-protocol constants shared by both versions.
// See docs/LAB_REPORT.md ("Protocol") for the full message layouts.
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <cstdint>
#include <string>

#define PORT 9000
#define BUFFER_SIZE 4096

constexpr uint32_t MAX_FILENAME_LEN = 255;

// Version 2 request types (first byte the client sends).
constexpr uint8_t CMD_DOWNLOAD = 'D';
constexpr uint8_t CMD_UPLOAD = 'U';

// One-byte status codes sent by the server.
enum Status : uint8_t {
    STATUS_OK = 0,
    STATUS_NOT_FOUND = 1,
    STATUS_BAD_REQUEST = 2,
    STATUS_TOO_LARGE = 3,
    STATUS_SERVER_BUSY = 4,
    STATUS_CHECKSUM_MISMATCH = 5,
    STATUS_IO_ERROR = 6,
};

inline const char* status_to_string(uint8_t status) {
    switch (status) {
        case STATUS_OK: return "OK";
        case STATUS_NOT_FOUND: return "file not found on server";
        case STATUS_BAD_REQUEST: return "bad request (invalid or unsafe filename)";
        case STATUS_TOO_LARGE: return "file exceeds the server's size limit";
        case STATUS_SERVER_BUSY: return "server busy (too many connections), try again later";
        case STATUS_CHECKSUM_MISMATCH: return "SHA-256 checksum mismatch";
        case STATUS_IO_ERROR: return "server I/O error";
        default: return "unknown status";
    }
}

// Only plain file names are accepted: no directories, no "..", no hidden
// files, no control characters. This stops path-traversal requests such as
// "../../etc/passwd".
inline bool is_safe_filename(const std::string& name) {
    if (name.empty() || name.size() > MAX_FILENAME_LEN) return false;
    if (name[0] == '.') return false;  // also rejects "." and ".."
    for (unsigned char c : name) {
        if (c == '/' || c == '\\' || c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

// "dir/sub/report.pdf" -> "report.pdf"
inline std::string base_name(const std::string& path) {
    size_t pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

#endif
