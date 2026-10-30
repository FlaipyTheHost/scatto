#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "snapper_cli.hpp"

// A "custom" schedule is implemented as a systemd --user timer + oneshot
// service pair, living in ~/.config/systemd/user/. Snapper itself has no
// concept of "every Tuesday and Thursday at 3am" -- only its built-in
// timeline (hourly/daily/weekly/monthly/yearly retention counts). Custom
// schedules just call `snapper -c <config> create` on their own timer, same
// as a user would from a terminal, so they need no extra privileges beyond
// whatever lets that user run `snapper create` for the config already.
namespace systemd_timer {

struct CustomSchedule {
    std::string unit;         // "snapper-manager-<config>-<id>", without extension
    std::string config;
    std::string description;  // human-readable sentence, e.g. "... every Tuesday and Thursday at 3:00 AM"
    std::string on_calendar;  // raw systemd OnCalendar= expression
};

// 0 = Monday ... 6 = Sunday (matches ISO 8601 / std::tm's tm_wday, shifted).
constexpr int kDaysPerWeek = 7;
extern const char* const kWeekdayAbbrev[kDaysPerWeek];   // "Mon".."Sun"
extern const char* const kWeekdayFull[kDaysPerWeek];     // "Monday".."Sunday"
extern const char* const kWeekdaySystemd[kDaysPerWeek];  // systemd calendar names

// Builds the `OnCalendar=` value for the given weekdays (indices into the
// arrays above) and time of day. `weekdays` must not be empty.
std::string build_on_calendar(const std::vector<int>& weekdays, int hour, int minute);

// Builds the human-readable sentence shown in the dialog's preview and later
// reused as the unit files' Description=.
std::string describe_custom_schedule(const std::string& subvolume, const std::vector<int>& weekdays,
                                     int hour, int minute);

// Writes the unit files and runs `systemctl --user daemon-reload` +
// `enable --now`. Calls back with the final SnapperResult (of the enable
// step, which is the one most likely to fail).
void create_schedule(const std::string& config, const std::string& subvolume,
                     const std::vector<int>& weekdays, int hour, int minute,
                     std::shared_ptr<bool> alive, SnapperCallback callback);

// Disables the timer and removes its unit files.
void delete_schedule(const std::string& unit, std::shared_ptr<bool> alive, SnapperCallback callback);

// Synchronously scans ~/.config/systemd/user/ for schedules previously
// created by this app for `config`. Cheap local file reads, no subprocess.
std::vector<CustomSchedule> list_schedules(const std::string& config);

}  // namespace systemd_timer
