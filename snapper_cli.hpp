#pragma once

#include <gio/gio.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Result of running a `snapper` command.
struct SnapperResult {
    bool ok = false;             // false if the process could not even be spawned
    bool successful = false;     // true if it ran and exited with status 0
    std::string std_out;
    std::string std_err;         // trimmed
    std::string spawn_error;     // set only when `ok` is false

    // A short, human-readable reason, picking whichever message applies.
    std::string error_message() const;
};

using SnapperCallback = std::function<void(SnapperResult)>;

// Runs `<argv[0]> <argv[1...]>` in the background (never blocks the UI thread).
// `alive` is checked when the process finishes; if it is false by then (the
// owning widget was destroyed), the callback is skipped.
void run_command_async(std::vector<std::string> argv, std::shared_ptr<bool> alive,
                       SnapperCallback callback);

// Convenience wrapper: run_command_async with "snapper" prepended.
void run_snapper_async(std::vector<std::string> args, std::shared_ptr<bool> alive,
                       SnapperCallback callback);

// One row of a `--csvout` table, keyed by (lower-cased) column name.
using CsvRow = std::map<std::string, std::string>;

// Parses a full `--csvout` table: first line is the header, one row per
// following line. Handles RFC4180-style quoting, since Snapper quotes any
// field (e.g. a snapshot description) that contains the separator or a quote.
std::vector<CsvRow> parse_csv_table(const std::string& text);

std::string trim(std::string s);

// Escapes text for use in AdwActionRow titles/subtitles, which take Pango markup.
std::string escape_markup(const std::string& text);
