// snapper-manager-helper -- the only part of Snapper Manager that runs as
// root. It is started once, via `pkexec`, by the (unprivileged) main GUI
// process, and kept running for the whole session. It speaks the tiny
// protocol in ipc_protocol.hpp over stdin/stdout: for each request, it runs
// `/usr/bin/snapper <args...>` -- and *only* that program, regardless of
// what the request contains -- and reports back the exit status, stdout and
// stderr. It never interprets, shells out, or execs anything else.
#include <gio/gio.h>
#include <unistd.h>

#include <cstdio>

#include "ipc_protocol.hpp"

namespace {

constexpr const char* kSnapperPath = "/usr/bin/snapper";

ipc::Response run_snapper(const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(kSnapperPath);
    for (const std::string& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    ipc::Response response;

    GError* error = nullptr;
    GSubprocess* process = g_subprocess_newv(
        argv.data(),
        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE),
        &error);
    if (!process) {
        response.success = false;
        response.std_err = error ? error->message : "Could not start /usr/bin/snapper";
        g_clear_error(&error);
        return response;
    }

    gchar* out = nullptr;
    gchar* err = nullptr;
    const gboolean communicated =
        g_subprocess_communicate_utf8(process, nullptr, nullptr, &out, &err, &error);

    if (!communicated) {
        response.success = false;
        response.std_err = error ? error->message : "Could not communicate with /usr/bin/snapper";
    } else {
        response.success = g_subprocess_get_successful(process);
        response.std_out = out ? out : "";
        response.std_err = err ? err : "";
    }

    g_free(out);
    g_free(err);
    g_clear_error(&error);
    g_object_unref(process);
    return response;
}

}  // namespace

int main() {
    if (geteuid() != 0) {
        std::fprintf(stderr, "snapper-manager-helper: refusing to run without root privileges\n");
        return 1;
    }

    // Tell the GUI we are alive *before* blocking on the first request, so
    // it can tell a successful pkexec authentication from a cancelled one.
    if (::write(STDOUT_FILENO, ipc::kReadyHandshake, __builtin_strlen(ipc::kReadyHandshake)) < 0)
        return 1;

    for (;;) {
        ipc::Request request;
        if (!ipc::read_request(STDIN_FILENO, request)) break;  // parent went away

        const ipc::Response response = run_snapper(request.args);
        if (!ipc::write_response(STDOUT_FILENO, response)) break;
    }

    return 0;
}
