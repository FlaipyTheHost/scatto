#pragma once

#include <array>
#include <string>

#include "i18n.hpp"

// Snapper's five built-in timeline periods, in display order. Each is just a
// retention count (TIMELINE_LIMIT_<...>): Snapper creates a snapshot every
// hour (via snapper-timeline.timer) when TIMELINE_CREATE=yes, then its
// cleanup keeps only the `count` most recent snapshots of each period.
struct TimelinePeriod {
    const char* config_key;  // TIMELINE_LIMIT_<...>
    const char* label;       // "Hourly", shown as a title
    const char* unit_word;   // "hourly", used inside a sentence
    const char* tab_name;    // page/tab identifier, e.g. "hourly"
    int default_count;
};

inline constexpr std::array<TimelinePeriod, 5> kTimelinePeriods = {{
    {"TIMELINE_LIMIT_HOURLY", N_("Hourly"), "hourly", "hourly", 5},
    {"TIMELINE_LIMIT_DAILY", N_("Daily"), "daily", "daily", 7},
    {"TIMELINE_LIMIT_WEEKLY", N_("Weekly"), "weekly", "weekly", 4},
    {"TIMELINE_LIMIT_MONTHLY", N_("Monthly"), "monthly", "monthly", 6},
    {"TIMELINE_LIMIT_YEARLY", N_("Yearly"), "yearly", "yearly", 2},
}};

// "Keeps the 5 most recent hourly snapshots." -- one whole sentence per period
// (with its own plural forms) rather than a unit word spliced into a template,
// since word order and agreement differ between languages.
inline std::string timeline_keep_sentence(const std::string& config_key, int count) {
    if (config_key == "TIMELINE_LIMIT_HOURLY")
        return format_string(ngettext("Keeps the %d most recent hourly snapshot.",
                                      "Keeps the %d most recent hourly snapshots.", count), count);
    if (config_key == "TIMELINE_LIMIT_DAILY")
        return format_string(ngettext("Keeps the %d most recent daily snapshot.",
                                      "Keeps the %d most recent daily snapshots.", count), count);
    if (config_key == "TIMELINE_LIMIT_WEEKLY")
        return format_string(ngettext("Keeps the %d most recent weekly snapshot.",
                                      "Keeps the %d most recent weekly snapshots.", count), count);
    if (config_key == "TIMELINE_LIMIT_MONTHLY")
        return format_string(ngettext("Keeps the %d most recent monthly snapshot.",
                                      "Keeps the %d most recent monthly snapshots.", count), count);
    return format_string(ngettext("Keeps the %d most recent yearly snapshot.",
                                  "Keeps the %d most recent yearly snapshots.", count), count);
}
