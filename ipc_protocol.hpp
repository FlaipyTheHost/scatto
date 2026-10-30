#pragma once

// Minimal, binary-safe framing protocol used on the pipe between the
// unprivileged GUI process and the privileged helper process (started once,
// as root, via `pkexec`, and kept running for the lifetime of the app).
//
// The GUI never runs as root; only this tiny helper does, and it will only
// ever exec `/usr/bin/snapper` with the arguments it is given -- nothing
// else. This keeps the privileged surface as small as practical.
//
// Wire format, all as plain ASCII/UTF-8 lengths followed by raw bytes so
// arbitrary content (e.g. a snapshot description with any character) is
// never ambiguous:
//
//   Request  (GUI -> helper, one per line-prefixed length):
//     "<argc>\n"
//     for each argument: "<byte length>\n" <raw bytes>
//
//   Response (helper -> GUI):
//     "<0 or 1, 0 = success>\n"
//     "<stdout byte length>\n" <raw bytes>
//     "<stderr byte length>\n" <raw bytes>
//
// Both sides use blocking reads/writes on a raw file descriptor (the GUI
// does so from a dedicated background thread so the main loop is never
// blocked); a read returning 0 (EOF) or a short/failed write means the
// other end is gone and the caller should treat the channel as dead.

#include <string>
#include <vector>

namespace ipc {

// Sent by the helper immediately after it starts, before reading any
// requests, so the GUI can tell a successful `pkexec` authentication (which
// leads to the helper actually running) apart from a cancelled/failed one
// (where the process exits, or never runs, without ever writing this).
inline constexpr char kReadyHandshake[] = "SNAPPER_MANAGER_HELPER_READY\n";

struct Request {
    std::vector<std::string> args;  // arguments to pass to /usr/bin/snapper
};

struct Response {
    bool success = false;
    std::string std_out;
    std::string std_err;
};

// Returns false on any write error (broken pipe, etc).
bool write_request(int fd, const Request& request);
// Returns false on EOF or a malformed frame.
bool read_request(int fd, Request& request);

bool write_response(int fd, const Response& response);
bool read_response(int fd, Response& response);

}  // namespace ipc
