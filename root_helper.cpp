#include "root_helper.hpp"

#include "i18n.hpp"

#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <unistd.h>

#include <cstring>
#include <thread>

#include "ipc_protocol.hpp"

namespace root_helper {

namespace {

GSubprocess* g_process = nullptr;
int g_write_fd = -1;
int g_read_fd = -1;
GAsyncQueue* g_queue = nullptr;  // holds heap-allocated PendingRequest*, or nullptr to stop
std::thread g_worker;

struct PendingRequest {
    std::vector<std::string> args;
    SnapperCallback callback;
};

struct DeliverContext {
    SnapperCallback callback;
    SnapperResult result;
};

gboolean deliver_on_main_thread(gpointer data) {
    std::unique_ptr<DeliverContext> ctx(static_cast<DeliverContext*>(data));
    ctx->callback(std::move(ctx->result));
    return G_SOURCE_REMOVE;
}

void deliver(SnapperCallback callback, SnapperResult result) {
    auto* ctx = new DeliverContext{std::move(callback), std::move(result)};
    g_idle_add(deliver_on_main_thread, ctx);
}

// Runs on the dedicated worker thread: blocking pipe I/O only happens here,
// so the GTK main loop is never blocked by it.
void worker_loop() {
    for (;;) {
        auto* pending = static_cast<PendingRequest*>(g_async_queue_pop(g_queue));
        if (!pending) return;  // stop() was called
        std::unique_ptr<PendingRequest> owned(pending);

        ipc::Request request{owned->args};
        ipc::Response response;
        SnapperResult result;

        if (!ipc::write_request(g_write_fd, request) || !ipc::read_response(g_read_fd, response)) {
            result.ok = false;
            result.spawn_error = _("Lost connection to the privileged helper process");
        } else {
            result.ok = true;
            result.successful = response.success;
            result.std_out = std::move(response.std_out);
            result.std_err = trim(std::move(response.std_err));
        }

        deliver(std::move(owned->callback), std::move(result));
    }
}

std::string helper_binary_path() {
    char exe_path[4096];
    const ssize_t len = ::readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len <= 0) return {};
    exe_path[len] = '\0';

    std::string path = exe_path;
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return {};
    return path.substr(0, slash + 1) + "snapper-manager-helper";
}

// Blocking: reads exactly as many bytes as the handshake string, or fails on
// short read/EOF/error. Only ever called once, before the main loop starts.
bool read_handshake(int fd) {
    const std::string expected = ipc::kReadyHandshake;
    std::string got;
    got.resize(expected.size());

    std::size_t total = 0;
    while (total < expected.size()) {
        const ssize_t n = ::read(fd, got.data() + total, expected.size() - total);
        if (n <= 0) return false;
        total += static_cast<std::size_t>(n);
    }
    return got == expected;
}

// Drains whatever is left on `fd` (bounded), for a short diagnostic message
// after the handshake failed.
std::string drain_diagnostic(int fd) {
    std::string out;
    char buffer[512];
    for (int i = 0; i < 8; ++i) {  // a few KiB is plenty for an error message
        const ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n <= 0) break;
        out.append(buffer, static_cast<std::size_t>(n));
    }
    return trim(std::move(out));
}

}  // namespace

bool start(std::string& error) {
    const std::string helper_path = helper_binary_path();
    if (helper_path.empty() || !g_file_test(helper_path.c_str(), G_FILE_TEST_IS_EXECUTABLE)) {
        error = _("Could not find the snapper-manager-helper program next to the main executable.");
        return false;
    }

    const gchar* argv[] = {"pkexec", helper_path.c_str(), nullptr};
    GError* spawn_error = nullptr;
    GSubprocess* process = g_subprocess_newv(
        argv,
        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                      G_SUBPROCESS_FLAGS_STDERR_PIPE),
        &spawn_error);
    if (!process) {
        error = spawn_error ? spawn_error->message : _("Could not run pkexec.");
        g_clear_error(&spawn_error);
        return false;
    }

    GOutputStream* stdin_stream = g_subprocess_get_stdin_pipe(process);
    GInputStream* stdout_stream = g_subprocess_get_stdout_pipe(process);
    const int write_fd = g_unix_output_stream_get_fd(G_UNIX_OUTPUT_STREAM(stdin_stream));
    const int read_fd = g_unix_input_stream_get_fd(G_UNIX_INPUT_STREAM(stdout_stream));

    if (!read_handshake(read_fd)) {
        // Either pkexec refused (wrong password, dismissed dialog, not
        // authorized) or the helper itself failed to start as root.
        GInputStream* stderr_stream = g_subprocess_get_stderr_pipe(process);
        const int stderr_fd = g_unix_input_stream_get_fd(G_UNIX_INPUT_STREAM(stderr_stream));
        std::string detail = drain_diagnostic(stderr_fd);

        error = _("Administrator authentication was not completed");
        if (!detail.empty()) error += ": " + detail;
        error += ".";

        g_subprocess_force_exit(process);
        g_object_unref(process);
        return false;
    }

    g_process = process;
    g_write_fd = write_fd;
    g_read_fd = read_fd;
    g_queue = g_async_queue_new();
    g_worker = std::thread(worker_loop);
    return true;
}

void run_async(std::vector<std::string> args, SnapperCallback callback) {
    if (!g_queue) {
        SnapperResult result;
        result.ok = false;
        result.spawn_error = _("The privileged helper is not running.");
        deliver(std::move(callback), std::move(result));
        return;
    }

    g_async_queue_push(g_queue, new PendingRequest{std::move(args), std::move(callback)});
}

void stop() {
    if (!g_queue) return;

    g_async_queue_push(g_queue, nullptr);  // wakes the worker thread up to exit
    if (g_worker.joinable()) g_worker.join();

    g_async_queue_unref(g_queue);
    g_queue = nullptr;

    if (g_process) {
        g_subprocess_force_exit(g_process);
        g_object_unref(g_process);
        g_process = nullptr;
    }
    g_write_fd = g_read_fd = -1;
}

}  // namespace root_helper
