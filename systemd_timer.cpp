#include "systemd_timer.hpp"

#include <glib.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "i18n.hpp"

namespace systemd_timer {

// Marked for translation here, translated where they are shown: _() for the
// full names, C_("weekday abbreviation", ...) for the short ones.
const char* const kWeekdayAbbrev[kDaysPerWeek] = {
    NC_("weekday abbreviation", "Mon"), NC_("weekday abbreviation", "Tue"), NC_("weekday abbreviation", "Wed"),
    NC_("weekday abbreviation", "Thu"), NC_("weekday abbreviation", "Fri"), NC_("weekday abbreviation", "Sat"),
    NC_("weekday abbreviation", "Sun")};
const char* const kWeekdayFull[kDaysPerWeek] = {N_("Monday"), N_("Tuesday"),  N_("Wednesday"), N_("Thursday"),
                                                N_("Friday"), N_("Saturday"), N_("Sunday")};
const char* const kWeekdaySystemd[kDaysPerWeek] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

namespace {

std::string sanitize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    return out.empty() ? "config" : out;
}

std::string user_unit_dir() {
    return std::string(g_get_user_config_dir()) + "/systemd/user";
}

std::string unit_path(const std::string& unit, const char* extension) {
    return user_unit_dir() + "/" + unit + extension;
}

// The strftime pattern is translatable so each language picks its own clock
// ("3:00 AM" in English, "03:00" in Portuguese).
std::string format_time(int hour, int minute) {
    std::tm tm{};
    tm.tm_hour = hour;
    tm.tm_min = minute;

    char buffer[32];
    std::strftime(buffer, sizeof buffer, C_("time format", "%-I:%M %p"), &tm);
    return buffer;
}

std::string join_weekday_names(const std::vector<int>& weekdays, const char* const names[]) {
    std::vector<int> sorted = weekdays;
    std::sort(sorted.begin(), sorted.end());

    std::string result;
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        if (i > 0) result += (i + 1 == sorted.size()) ? _(" and ") : ", ";
        result += _(names[sorted[i]]);
    }
    return result;
}

}  // namespace

std::string build_on_calendar(const std::vector<int>& weekdays, int hour, int minute) {
    std::vector<int> sorted = weekdays;
    std::sort(sorted.begin(), sorted.end());

    std::string csv_days;
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        if (i > 0) csv_days += ",";
        csv_days += kWeekdaySystemd[sorted[i]];
    }

    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "%02d:%02d:00", hour, minute);
    return csv_days + " *-*-* " + buffer;
}

std::string describe_custom_schedule(const std::string& subvolume, const std::vector<int>& weekdays,
                                     int hour, int minute) {
    const std::string when = weekdays.size() == kDaysPerWeek
                                 ? std::string(_("every day"))
                                 : format_string(_("every %s"), join_weekday_names(weekdays, kWeekdayFull).c_str());
    return format_string(_("Snapshots of %s will be created %s at %s."), subvolume.c_str(), when.c_str(),
                         format_time(hour, minute).c_str());
}

void create_schedule(const std::string& config, const std::string& subvolume,
                     const std::vector<int>& weekdays, int hour, int minute,
                     std::shared_ptr<bool> alive, SnapperCallback callback) {
    if (weekdays.empty()) {
        SnapperResult r;
        r.ok = true;
        r.successful = false;
        r.std_err = _("Select at least one day of the week");
        callback(std::move(r));
        return;
    }

    GError* error = nullptr;
    if (g_mkdir_with_parents(user_unit_dir().c_str(), 0755) != 0) {
        SnapperResult r;
        r.ok = false;
        r.spawn_error = _("Could not create ~/.config/systemd/user");
        callback(std::move(r));
        return;
    }

    gchar* uuid = g_uuid_string_random();
    const std::string unit = "snapper-manager-" + sanitize(config) + "-" + uuid;
    g_free(uuid);

    const std::string description = describe_custom_schedule(subvolume, weekdays, hour, minute);
    const std::string on_calendar = build_on_calendar(weekdays, hour, minute);

    const std::string service_contents =
        "[Unit]\n"
        "Description=" +
        description +
        "\n\n"
        "[Service]\n"
        "Type=oneshot\n"
        "ExecStart=/usr/bin/snapper -c " +
        config + " create --type single --description \"Scheduled snapshot\"\n";

    const std::string timer_contents =
        "[Unit]\n"
        "Description=" +
        description +
        "\n\n"
        "[Timer]\n"
        "OnCalendar=" +
        on_calendar +
        "\n"
        "Persistent=true\n\n"
        "[Install]\n"
        "WantedBy=timers.target\n";

    if (!g_file_set_contents(unit_path(unit, ".service").c_str(), service_contents.c_str(), -1, &error) ||
        !g_file_set_contents(unit_path(unit, ".timer").c_str(), timer_contents.c_str(), -1, &error)) {
        SnapperResult r;
        r.ok = false;
        r.spawn_error = error ? error->message : _("Could not write systemd unit files");
        g_clear_error(&error);
        callback(std::move(r));
        return;
    }

    const std::string timer_unit = unit + ".timer";
    run_command_async(
        {"systemctl", "--user", "daemon-reload"}, alive,
        [alive, timer_unit, callback = std::move(callback)](SnapperResult reload) mutable {
            if (!reload.ok || !reload.successful) {
                callback(std::move(reload));
                return;
            }
            run_command_async({"systemctl", "--user", "enable", "--now", timer_unit}, alive,
                              std::move(callback));
        });
}

void delete_schedule(const std::string& unit, std::shared_ptr<bool> alive, SnapperCallback callback) {
    const std::string timer_unit = unit + ".timer";
    const std::string service_path = unit_path(unit, ".service");
    const std::string timer_path = unit_path(unit, ".timer");

    run_command_async(
        {"systemctl", "--user", "disable", "--now", timer_unit}, alive,
        [alive, service_path, timer_path, callback = std::move(callback)](SnapperResult) mutable {
            // Remove the unit files regardless of whether "disable" reported
            // success -- an already-inactive timer still needs cleaning up.
            g_remove(service_path.c_str());
            g_remove(timer_path.c_str());
            run_command_async({"systemctl", "--user", "daemon-reload"}, alive, std::move(callback));
        });
}

std::vector<CustomSchedule> list_schedules(const std::string& config) {
    std::vector<CustomSchedule> schedules;

    const std::string dir_path = user_unit_dir();
    GDir* dir = g_dir_open(dir_path.c_str(), 0, nullptr);
    if (!dir) return schedules;

    const std::string prefix = "snapper-manager-" + sanitize(config) + "-";

    const gchar* name;
    while ((name = g_dir_read_name(dir)) != nullptr) {
        std::string filename = name;
        if (filename.size() < 6 || filename.compare(filename.size() - 6, 6, ".timer") != 0) continue;
        if (filename.compare(0, prefix.size(), prefix) != 0) continue;

        const std::string unit = filename.substr(0, filename.size() - 6);  // strip ".timer"

        GKeyFile* key_file = g_key_file_new();
        gchar* path = g_build_filename(dir_path.c_str(), filename.c_str(), nullptr);
        const gboolean loaded = g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, nullptr);
        g_free(path);

        CustomSchedule s;
        s.unit = unit;
        s.config = config;
        if (loaded) {
            gchar* desc = g_key_file_get_string(key_file, "Unit", "Description", nullptr);
            gchar* cal = g_key_file_get_string(key_file, "Timer", "OnCalendar", nullptr);
            s.description = desc ? desc : _("Custom schedule");
            s.on_calendar = cal ? cal : "";
            g_free(desc);
            g_free(cal);
        } else {
            s.description = _("Custom schedule");
        }
        g_key_file_free(key_file);

        schedules.push_back(std::move(s));
    }
    g_dir_close(dir);

    std::sort(schedules.begin(), schedules.end(),
             [](const CustomSchedule& a, const CustomSchedule& b) { return a.unit < b.unit; });
    return schedules;
}

}  // namespace systemd_timer
