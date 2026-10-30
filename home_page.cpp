#include "home_page.hpp"

#include <cstdio>
#include <ctime>

#include "fs_info.hpp"
#include "i18n.hpp"
#include "snapper_cli.hpp"

namespace {

constexpr const char* kMountPoint = "/";

std::string percent_of(std::uint64_t part, std::uint64_t total) {
    const double value = total ? 100.0 * static_cast<double>(part) / static_cast<double>(total) : 0.0;
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, _("%.1f%% of size"), value);
    return buffer;
}

// Turns the parsed `list-configs` table into our own small struct.
std::vector<SnapperConfig> to_configs(const std::vector<CsvRow>& rows) {
    std::vector<SnapperConfig> configs;
    configs.reserve(rows.size());
    for (const CsvRow& row : rows) {
        auto name = row.find("config");
        auto subvolume = row.find("subvolume");
        if (name == row.end() || subvolume == row.end() || name->second.empty()) continue;
        configs.push_back({name->second, subvolume->second});
    }
    return configs;
}

// Red dot under the days GtkCalendar has marked (it gives them the
// "highlight" style class). Scoped to our own class so other calendars,
// if any, are untouched.
void install_calendar_css() {
    static bool installed = false;
    if (installed) return;
    installed = true;

    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(
        provider,
        "calendar.snapshot-days { border-radius: 12px; padding: 6px; }"
        "calendar.snapshot-days > grid > label.day-number { border-radius: 8px; }"
        "calendar.snapshot-days > grid > label.day-number.highlight {"
        "  background-image: radial-gradient(circle at 50% 88%, @error_color 0, @error_color 2.5px,"
        "                                    transparent 3px);"
        "  font-weight: bold;"
        "}");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

int day_key(int year, int month, int day) {
    return year * 10000 + month * 100 + day;
}

}  // namespace

HomePage::HomePage() {
    page_ = adw_preferences_page_new();
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page_), build_filesystem_group());
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page_), build_subvolumes_group());

    // The widget owns this object.
    g_object_set_data_full(G_OBJECT(page_), "home-page", this,
                           [](gpointer p) { delete static_cast<HomePage*>(p); });
}

HomePage::~HomePage() {
    *alive_ = false;
}

AdwPreferencesGroup* HomePage::build_filesystem_group() {
    fs_group_ = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(fs_group_, _("Filesystem"));

    chart_ = new RingChart();  // owned by its widget
    GtkWidget* chart = chart_->widget();
    gtk_widget_set_halign(chart, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(chart, 6);
    gtk_widget_set_margin_bottom(chart, 6);

    GtkWidget* list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");  // stock libadwaita style

    size_ = add_value_row(list, _("Size"), gtk_image_new_from_icon_name("drive-harddisk-symbolic"));
    allocated_ = add_value_row(list, _("Allocated"), color_dot_new(Tone::AccentSoft));
    used_ = add_value_row(list, _("Used"), color_dot_new(Tone::Accent));
    free_ = add_value_row(list, _("Free"), color_dot_new(Tone::Neutral));

    // Chart on the left, calendar on the right.
    GtkWidget* top = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(top), GTK_SELECTION_NONE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(top), 2);  // never wrap: the window has a minimum width
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(top), 2);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(top), 18);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(top), 12);
    gtk_flow_box_append(GTK_FLOW_BOX(top), chart);
    gtk_flow_box_append(GTK_FLOW_BOX(top), build_calendar());
    for (GtkWidget* child = gtk_widget_get_first_child(top); child; child = gtk_widget_get_next_sibling(child))
        gtk_widget_set_focusable(child, FALSE);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_box_append(GTK_BOX(box), top);
    gtk_box_append(GTK_BOX(box), list);
    adw_preferences_group_add(fs_group_, box);

    return fs_group_;
}

GtkWidget* HomePage::build_calendar() {
    install_calendar_css();

    calendar_ = gtk_calendar_new();
    gtk_widget_add_css_class(calendar_, "snapshot-days");
    gtk_widget_set_halign(calendar_, GTK_ALIGN_CENTER);

    auto on_month_changed = +[](GObject*, GParamSpec*, gpointer data) {
        static_cast<HomePage*>(data)->update_calendar_marks();
    };
    g_signal_connect(calendar_, "notify::month", G_CALLBACK(on_month_changed), this);
    g_signal_connect(calendar_, "notify::year", G_CALLBACK(on_month_changed), this);
    g_signal_connect(calendar_, "day-selected",
                     G_CALLBACK(+[](GtkCalendar*, gpointer data) {
                         static_cast<HomePage*>(data)->update_calendar_detail();
                     }),
                     this);

    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card, "card");  // stock libadwaita style
    gtk_widget_set_halign(card, GTK_ALIGN_CENTER);  // hug the calendar instead of stretching with the window
    gtk_widget_set_overflow(card, GTK_OVERFLOW_HIDDEN);  // clip children to the rounded corners
    gtk_box_append(GTK_BOX(card), calendar_);

    GtkWidget* detail = gtk_label_new(nullptr);
    gtk_widget_add_css_class(detail, "caption");
    gtk_label_set_wrap(GTK_LABEL(detail), true);
    gtk_label_set_justify(GTK_LABEL(detail), GTK_JUSTIFY_CENTER);
    gtk_label_set_max_width_chars(GTK_LABEL(detail), 32);
    calendar_detail_ = GTK_LABEL(detail);

    GtkWidget* column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_valign(column, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(column, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(column), card);
    gtk_box_append(GTK_BOX(column), detail);

    update_calendar_detail();
    return column;
}

// Reads the snapshot dates of every profile, then refreshes the calendar.
void HomePage::load_snapshot_days(const std::vector<SnapperConfig>& configs) {
    struct Pending {
        int remaining;
        std::map<int, std::map<std::string, std::vector<std::string>>> days;
    };
    auto pending = std::make_shared<Pending>();
    pending->remaining = static_cast<int>(configs.size());
    if (configs.empty()) {
        snapshot_days_.clear();
        update_calendar_marks();
        return;
    }

    for (const SnapperConfig& config : configs) {
        run_snapper_async({"-c", config.name, "--csvout", "--iso", "list", "--columns", "number,date"}, alive_,
                          [this, pending, name = config.name](SnapperResult r) {
                              if (r.ok && r.successful) {
                                  for (const CsvRow& row : parse_csv_table(r.std_out)) {
                                      auto number = row.find("number");
                                      auto date = row.find("date");
                                      if (number == row.end() || date == row.end() || number->second == "0") continue;

                                      std::tm tm{};
                                      if (!strptime(date->second.c_str(), "%Y-%m-%d %H:%M:%S", &tm)) continue;
                                      pending->days[day_key(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday)][name]
                                          .push_back(number->second);
                                  }
                              }
                              if (--pending->remaining > 0) return;

                              snapshot_days_ = std::move(pending->days);
                              update_calendar_marks();
                              update_calendar_detail();
                          });
    }
}

void HomePage::update_calendar_marks() {
    GtkCalendar* calendar = GTK_CALENDAR(calendar_);
    gtk_calendar_clear_marks(calendar);

    GDateTime* shown = gtk_calendar_get_date(calendar);
    const int first = day_key(g_date_time_get_year(shown), g_date_time_get_month(shown), 1);
    g_date_time_unref(shown);

    for (auto it = snapshot_days_.lower_bound(first); it != snapshot_days_.end() && it->first < first + 100; ++it)
        gtk_calendar_mark_day(calendar, static_cast<guint>(it->first - first + 1));
}

// One line under the calendar describing the selected day.
void HomePage::update_calendar_detail() {
    GDateTime* selected = gtk_calendar_get_date(GTK_CALENDAR(calendar_));
    const int key = day_key(g_date_time_get_year(selected), g_date_time_get_month(selected),
                            g_date_time_get_day_of_month(selected));
    g_date_time_unref(selected);

    auto it = snapshot_days_.find(key);
    if (it == snapshot_days_.end()) {
        gtk_label_set_text(calendar_detail_, _("No snapshots on this day"));
        return;
    }

    std::string text;
    for (const auto& [profile, numbers] : it->second) {
        if (!text.empty()) text += "\n";
        text += profile + ": ";
        for (std::size_t i = 0; i < numbers.size(); ++i) text += (i ? ", #" : "#") + numbers[i];
    }
    gtk_label_set_text(calendar_detail_, text.c_str());
}

AdwPreferencesGroup* HomePage::build_subvolumes_group() {
    subvol_group_ = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(subvol_group_, _("Subvolumes"));
    adw_preferences_group_set_description(subvol_group_, _("Isolated storage areas for your snapshots"));
    return subvol_group_;
}

HomePage::ValueRow HomePage::add_value_row(GtkWidget* list, const char* title, GtkWidget* prefix) {
    GtkWidget* row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), prefix);

    GtkWidget* value = gtk_label_new("—");
    gtk_widget_add_css_class(value, "numeric");
    gtk_widget_set_valign(value, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), value);

    gtk_list_box_append(GTK_LIST_BOX(list), row);
    return {ADW_ACTION_ROW(row), GTK_LABEL(value)};
}

void HomePage::refresh() {
    refresh_usage();
    refresh_subvolumes();
}

//usage

void HomePage::refresh_usage() {
    const FsUsage u = read_fs_usage(kMountPoint);

    if (!u.ok) {
        adw_preferences_group_set_description(fs_group_, _("Could not read filesystem information"));
        chart_->set_rings({});
        chart_->set_center_text("—", _("Size"));
        for (const ValueRow* r : {&size_, &allocated_, &used_, &free_}) {
            gtk_label_set_text(r->value, "—");
            adw_action_row_set_subtitle(r->row, "");
        }
        return;
    }

    const double used = static_cast<double>(u.used);
    const double free = static_cast<double>(u.free);
    const double allocated = static_cast<double>(u.allocated);
    const double unallocated = static_cast<double>(u.size - u.allocated);

    // Outer ring: used and free.  Inner ring: allocated and unallocated.
    auto tip = [&u](const char* name, std::uint64_t bytes) {
        return std::string(name) + "\n" + format_bytes(bytes) + " · " + percent_of(bytes, u.size);
    };
    chart_->set_rings({
        {{used, Tone::Accent, tip(_("Used"), u.used)}, {free, Tone::Neutral, tip(_("Free"), u.free)}},
        {{allocated, Tone::AccentSoft, tip(_("Allocated"), u.allocated)},
         {unallocated, Tone::Neutral, tip(_("Unallocated"), u.size - u.allocated)}},
    });
    chart_->set_center_text(format_bytes(u.size), _("Size"));

    adw_preferences_group_set_description(
        fs_group_, u.is_btrfs ? _("Storage Usage Summary for /")
                              : _("Unsupported filesystem: partition is not Btrfs"));

    gtk_label_set_text(size_.value, format_bytes(u.size).c_str());
    gtk_label_set_text(allocated_.value, format_bytes(u.allocated).c_str());
    gtk_label_set_text(used_.value, format_bytes(u.used).c_str());
    gtk_label_set_text(free_.value, format_bytes(u.free).c_str());

    adw_action_row_set_subtitle(allocated_.row, percent_of(u.allocated, u.size).c_str());
    adw_action_row_set_subtitle(used_.row, percent_of(u.used, u.size).c_str());
    adw_action_row_set_subtitle(free_.row, percent_of(u.free, u.size).c_str());
}

// subvolumes

void HomePage::refresh_subvolumes() {
    // runs in the background so the window never freezes while snapperd starts
    run_snapper_async({"--csvout", "list-configs"}, alive_, [this](SnapperResult r) {
        if (!r.ok || !r.successful) {
            show_subvolumes_message(_("Could not list Snapper configurations"), r.error_message());
        } else {
            const std::vector<SnapperConfig> configs = to_configs(parse_csv_table(r.std_out));
            show_subvolumes(configs);
            load_snapshot_days(configs);
        }
    });
}

void HomePage::clear_subvolume_rows() {
    for (GtkWidget* row : subvol_rows_) adw_preferences_group_remove(subvol_group_, row);
    subvol_rows_.clear();
}

void HomePage::show_subvolumes(const std::vector<SnapperConfig>& configs) {
    if (configs.empty()) {
        show_subvolumes_message(_("No Snapper configurations found"),
                                _("Create one with: sudo snapper -c root create-config /"));
        return;
    }

    clear_subvolume_rows();

    for (const SnapperConfig& config : configs) {
        GtkWidget* row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), escape_markup(config.subvolume).c_str());
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), escape_markup(format_string(_("Profile: %s"), config.name.c_str())).c_str());
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("folder-symbolic"));

        // Basic health check: the snapshots directory must exist.
        const std::string base = config.subvolume == "/" ? "" : config.subvolume;
        const bool healthy = g_file_test((base + "/.snapshots").c_str(), G_FILE_TEST_IS_DIR);

        const char* label = healthy ? _("OK") : _("Snapshots directory (.snapshots) not found");
        GtkWidget* status =
            gtk_image_new_from_icon_name(healthy ? "emblem-ok-symbolic" : "dialog-warning-symbolic");
        gtk_widget_add_css_class(status, healthy ? "success" : "warning");  // stock styles
        gtk_widget_set_tooltip_text(status, label);
        gtk_accessible_update_property(GTK_ACCESSIBLE(status), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), status);

        adw_preferences_group_add(subvol_group_, row);
        subvol_rows_.push_back(row);
    }
}

void HomePage::show_subvolumes_message(const std::string& title, const std::string& detail) {
    clear_subvolume_rows();

    GtkWidget* row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), escape_markup(title).c_str());
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), escape_markup(detail).c_str());
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("dialog-information-symbolic"));

    adw_preferences_group_add(subvol_group_, row);
    subvol_rows_.push_back(row);
}
