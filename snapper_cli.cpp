#include "snapper_cli.hpp"

#include "i18n.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "root_helper.hpp"

std::string trim(std::string s) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::string escape_markup(const std::string& text) {
    gchar* escaped = g_markup_escape_text(text.c_str(), -1);
    std::string result = escaped;
    g_free(escaped);
    return result;
}

std::string SnapperResult::error_message() const {
    if (!ok) return spawn_error.empty() ? _("Could not run snapper") : spawn_error;
    if (!successful) return std_err.empty() ? _("snapper exited with an error") : std_err;
    return {};
}

namespace {

struct Call {
    SnapperCallback callback;
    std::shared_ptr<bool> alive;
};

void on_finished(GObject* source, GAsyncResult* result, gpointer user_data) {
    std::unique_ptr<Call> call(static_cast<Call*>(user_data));
    GSubprocess* process = G_SUBPROCESS(source);

    gchar* out = nullptr;
    gchar* err = nullptr;
    GError* error = nullptr;
    const gboolean spawned_ok = g_subprocess_communicate_utf8_finish(process, result, &out, &err, &error);

    SnapperResult r;
    r.ok = spawned_ok;
    r.successful = spawned_ok && g_subprocess_get_successful(process);
    r.std_out = out ? out : "";
    r.std_err = err ? trim(err) : "";
    r.spawn_error = error ? error->message : "";
    g_free(out);
    g_free(err);
    g_clear_error(&error);

    if (*call->alive) call->callback(std::move(r));
}

// One field of a CSV line, honouring double-quoted fields with "" escapes.
std::vector<std::string> parse_csv_line(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool in_quotes = false;

    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];

        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    field += '"';
                    ++i;
                } else {
                    in_quotes = false;
                }
            } else {
                field += c;
            }
            continue;
        }

        if (c == '"') {
            in_quotes = true;
        } else if (c == ',') {
            fields.push_back(std::move(field));
            field.clear();
        } else {
            field += c;
        }
    }
    fields.push_back(std::move(field));
    return fields;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

}  // namespace

void run_command_async(std::vector<std::string> argv_strings, std::shared_ptr<bool> alive,
                       SnapperCallback callback) {
    std::vector<const char*> argv;
    argv.reserve(argv_strings.size() + 1);
    for (const std::string& a : argv_strings) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    GError* error = nullptr;
    GSubprocess* process = g_subprocess_newv(
        argv.data(),
        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE),
        &error);

    if (!process) {
        SnapperResult r;
        r.ok = false;
        r.spawn_error = error ? error->message : _("Could not run command");
        g_clear_error(&error);
        if (*alive) callback(std::move(r));
        return;
    }

    auto* call = new Call{std::move(callback), std::move(alive)};
    g_subprocess_communicate_utf8_async(process, nullptr, nullptr, on_finished, call);
    g_object_unref(process);  // the async operation keeps its own reference
}

void run_snapper_async(std::vector<std::string> args, std::shared_ptr<bool> alive,
                       SnapperCallback callback) {
    // All `snapper` invocations go through the privileged helper (started
    // once, as root, via pkexec, at app startup) instead of running as the
    // current user -- see root_helper.hpp.
    root_helper::run_async(std::move(args), [alive, callback = std::move(callback)](SnapperResult r) {
        if (*alive) callback(std::move(r));
    });
}

std::vector<CsvRow> parse_csv_table(const std::string& text) {
    std::vector<CsvRow> rows;

    std::istringstream in(text);
    std::string line;
    std::vector<std::string> header;

    while (std::getline(in, line)) {
        line = trim(std::move(line));
        if (line.empty()) continue;

        std::vector<std::string> fields = parse_csv_line(line);
        for (std::string& f : fields) f = trim(std::move(f));

        if (header.empty()) {
            header = std::move(fields);
            for (std::string& h : header) h = lower(std::move(h));
            continue;
        }

        CsvRow row;
        for (std::size_t i = 0; i < header.size(); ++i)
            row[header[i]] = i < fields.size() ? fields[i] : "";
        rows.push_back(std::move(row));
    }
    return rows;
}
