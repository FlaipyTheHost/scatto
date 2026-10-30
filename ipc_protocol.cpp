#include "ipc_protocol.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace ipc {

namespace {

bool write_all(int fd, const char* data, std::size_t len) {
    std::size_t written = 0;
    while (written < len) {
        const ssize_t n = ::write(fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        written += static_cast<std::size_t>(n);
    }
    return true;
}

bool write_line(int fd, long value) {
    char buffer[32];
    const int len = std::snprintf(buffer, sizeof buffer, "%ld\n", value);
    return len > 0 && write_all(fd, buffer, static_cast<std::size_t>(len));
}

bool write_string(int fd, const std::string& s) {
    return write_line(fd, static_cast<long>(s.size())) && write_all(fd, s.data(), s.size());
}

// Reads one '\n'-terminated line (without the newline) into `out`. A byte at
// a time is plenty fast for the tiny control lines this protocol uses.
bool read_line(int fd, std::string& out) {
    out.clear();
    char c;
    for (;;) {
        const ssize_t n = ::read(fd, &c, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;  // EOF
        if (c == '\n') return true;
        out += c;
        if (out.size() > 64) return false;  // a length/count line is never this long
    }
}

bool read_exact(int fd, std::string& out, std::size_t len) {
    out.clear();
    out.resize(len);
    std::size_t got = 0;
    while (got < len) {
        const ssize_t n = ::read(fd, out.data() + got, len - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;  // EOF
        got += static_cast<std::size_t>(n);
    }
    return true;
}

bool read_length(int fd, std::size_t& out) {
    std::string line;
    if (!read_line(fd, line)) return false;
    char* end = nullptr;
    const long value = std::strtol(line.c_str(), &end, 10);
    if (end == line.c_str() || value < 0) return false;
    out = static_cast<std::size_t>(value);
    return true;
}

}  // namespace

bool write_request(int fd, const Request& request) {
    if (!write_line(fd, static_cast<long>(request.args.size()))) return false;
    for (const std::string& arg : request.args)
        if (!write_string(fd, arg)) return false;
    return true;
}

bool read_request(int fd, Request& request) {
    std::size_t argc = 0;
    if (!read_length(fd, argc)) return false;
    if (argc > 256) return false;  // sanity limit

    request.args.clear();
    request.args.reserve(argc);
    for (std::size_t i = 0; i < argc; ++i) {
        std::size_t len = 0;
        if (!read_length(fd, len) || len > (1u << 20)) return false;  // 1 MiB/arg sanity limit
        std::string value;
        if (!read_exact(fd, value, len)) return false;
        request.args.push_back(std::move(value));
    }
    return true;
}

bool write_response(int fd, const Response& response) {
    return write_line(fd, response.success ? 0 : 1) && write_string(fd, response.std_out) &&
          write_string(fd, response.std_err);
}

bool read_response(int fd, Response& response) {
    std::size_t code = 0;
    if (!read_length(fd, code)) return false;
    response.success = (code == 0);

    std::size_t out_len = 0, err_len = 0;
    if (!read_length(fd, out_len) || out_len > (64u << 20)) return false;
    if (!read_exact(fd, response.std_out, out_len)) return false;
    if (!read_length(fd, err_len) || err_len > (64u << 20)) return false;
    if (!read_exact(fd, response.std_err, err_len)) return false;
    return true;
}

}  // namespace ipc
