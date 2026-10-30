#include "schedule_page.hpp"

#include <array>
#include <cstdlib>
#include <map>

#include "i18n.hpp"
#include "schedule_dialog.hpp"
#include "snapper_cli.hpp"
#include "systemd_timer.hpp"
#include "timeline_periods.hpp"

namespace {

}  // namespace

SchedulePage::SchedulePage(AdwToastOverlay* toast_overlay) : toast_overlay_(toast_overlay) {
    page_ = adw_preferences_page_new();
    g_object_set_data_full(G_OBJECT(page_), "schedule-page", this,
                           [](gpointer p) { delete static_cast<SchedulePage*>(p); });
}

SchedulePage::~SchedulePage() {
    *alive_ = false;
}

void SchedulePage::toast(const std::string& message) {
    AdwToast* t = adw_toast_new(message.c_str());
    adw_toast_set_timeout(t, 4);
    adw_toast_overlay_add_toast(toast_overlay_, t);
}

void SchedulePage::refresh() {
    run_snapper_async({"--csvout", "list-configs"}, alive_, [this](SnapperResult r) {
        if (!r.ok || !r.successful) {
            show_message(_("Could not list Snapper configurations"), r.error_message());
            return;
        }

        std::vector<std::pair<std::string, std::string>> configs;
        for (const CsvRow& row : parse_csv_table(r.std_out)) {
            auto name = row.find("config");
            auto subvolume = row.find("subvolume");
            if (name == row.end() || subvolume == row.end() || name->second.empty()) continue;
            configs.emplace_back(name->second, subvolume->second);
        }
        show_configs(std::move(configs));
    });
}

void SchedulePage::clear_volumes() {
    for (const std::shared_ptr<Volume>& v : volumes_)
        adw_preferences_page_remove(ADW_PREFERENCES_PAGE(page_), ADW_PREFERENCES_GROUP(v->group));
    volumes_.clear();
}

void SchedulePage::show_message(const std::string& title, const std::string& detail) {
    clear_volumes();

    AdwPreferencesGroup* group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    GtkWidget* row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), escape_markup(title).c_str());
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), escape_markup(detail).c_str());
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("dialog-information-symbolic"));
    adw_preferences_group_add(group, row);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page_), group);

    auto placeholder = std::make_shared<Volume>();
    placeholder->group = group;
    volumes_.push_back(placeholder);
}

void SchedulePage::show_configs(std::vector<std::pair<std::string, std::string>> configs) {
    if (configs.empty()) {
        show_message(_("No Snapper configurations found"),
                     _("Create one with: sudo snapper -c root create-config /"));
        return;
    }

    clear_volumes();

    for (auto& [name, subvolume] : configs) {
        auto volume = std::make_shared<Volume>();
        volume->config = name;
        volume->subvolume = subvolume;

        volume->group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(volume->group, subvolume.c_str());
        adw_preferences_group_set_description(volume->group, format_string(_("Profile: %s"), name.c_str()).c_str());

        struct AddButtonContext {
            SchedulePage* page;
            std::shared_ptr<Volume> volume;
        };
        auto on_clicked = +[](GtkButton*, gpointer data) {
            auto* ctx = static_cast<AddButtonContext*>(data);
            ctx->page->open_create_dialog(ctx->volume);
        };
        auto on_destroy = +[](gpointer data, GClosure*) { delete static_cast<AddButtonContext*>(data); };

        GtkWidget* add_button = gtk_button_new_from_icon_name("list-add-symbolic");
        gtk_widget_add_css_class(add_button, "flat");  // stock style
        gtk_widget_set_valign(add_button, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(add_button, _("New schedule"));
        g_signal_connect_data(add_button, "clicked", G_CALLBACK(on_clicked),
                              new AddButtonContext{this, volume}, on_destroy,
                              static_cast<GConnectFlags>(0));
        adw_preferences_group_set_header_suffix(volume->group, add_button);

        adw_preferences_page_add(ADW_PREFERENCES_PAGE(page_), volume->group);
        volumes_.push_back(volume);

        refresh_rules(volume);
    }
}

// ------------------------------------------------------------------ rules ---

void SchedulePage::refresh_rules(const std::shared_ptr<Volume>& volume) {
    run_snapper_async(
        {"-c", volume->config, "--csvout", "get-config", "--columns", "key,value"}, alive_,
        [this, volume](SnapperResult r) {
            if (!r.ok || !r.successful) {
                show_rules(volume, {});
                toast(format_string(_("Could not read schedule for %s: %s"), volume->subvolume.c_str(), r.error_message().c_str()));
                return;
            }

            std::map<std::string, std::string> settings;
            for (const CsvRow& row : parse_csv_table(r.std_out)) {
                auto key = row.find("key");
                auto value = row.find("value");
                if (key != row.end() && value != row.end()) settings[key->second] = value->second;
            }

            const bool timeline_on = settings["TIMELINE_CREATE"] == "yes";

            std::vector<GtkWidget*> rows;
            if (timeline_on) {
                for (const TimelinePeriod& period : kTimelinePeriods) {
                    const int count = std::atoi(settings[period.config_key].c_str());
                    if (count > 0) rows.push_back(build_timeline_row(volume, period.config_key, period.label, count));
                }
            }
            for (const systemd_timer::CustomSchedule& s : systemd_timer::list_schedules(volume->config))
                rows.push_back(build_custom_row(volume, s.unit, s.description));

            show_rules(volume, std::move(rows));
        });
}

void SchedulePage::show_rules(const std::shared_ptr<Volume>& volume, std::vector<GtkWidget*> rows) {
    for (GtkWidget* row : volume->rows) adw_preferences_group_remove(volume->group, row);
    volume->rows.clear();

    if (rows.empty()) {
        GtkWidget* row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), _("No active schedule"));
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("dialog-information-symbolic"));
        adw_preferences_group_add(volume->group, row);
        volume->rows.push_back(row);
        return;
    }

    for (GtkWidget* row : rows) {
        adw_preferences_group_add(volume->group, row);
        volume->rows.push_back(row);
    }
}

GtkWidget* SchedulePage::build_timeline_row(const std::shared_ptr<Volume>& volume, const std::string& period,
                                           const std::string& label, int count) {
    GtkWidget* row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), _(label.c_str()));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), timeline_keep_sentence(period, count).c_str());
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("alarm-symbolic"));

    struct Context {
        SchedulePage* page;
        std::shared_ptr<Volume> volume;
        std::string period;
    };
    auto on_clicked = +[](GtkButton*, gpointer data) {
        auto* ctx = static_cast<Context*>(data);
        SchedulePage* page = ctx->page;
        std::shared_ptr<Volume> volume = ctx->volume;
        run_snapper_async({"-c", volume->config, "set-config", ctx->period + "=0"}, page->alive_,
                          [page, volume](SnapperResult r) {
                              if (!r.ok || !r.successful) {
                                  page->toast(format_string(_("Could not remove schedule: %s"), r.error_message().c_str()));
                                  return;
                              }
                              page->toast(_("Schedule removed"));
                              page->refresh_rules(volume);
                          });
    };
    // The button's object data (set below) owns this context and frees it
    // when the button itself is destroyed.
    auto* keep_alive = new Context{this, volume, period};
    GtkWidget* remove_button = gtk_button_new_from_icon_name("user-trash-symbolic");
    gtk_widget_add_css_class(remove_button, "flat");
    gtk_widget_set_valign(remove_button, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(remove_button, _("Remove this schedule"));
    g_signal_connect(remove_button, "clicked", G_CALLBACK(on_clicked), keep_alive);
    // If the row (and button) is destroyed without ever being clicked, avoid leaking.
    g_object_set_data_full(G_OBJECT(remove_button), "context", keep_alive,
                           [](gpointer p) { delete static_cast<Context*>(p); });
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove_button);

    return row;
}

GtkWidget* SchedulePage::build_custom_row(const std::shared_ptr<Volume>& volume, const std::string& unit,
                                         const std::string& description) {
    GtkWidget* row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), _("Custom"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), escape_markup(description).c_str());
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("x-office-calendar-symbolic"));

    struct Context {
        SchedulePage* page;
        std::shared_ptr<Volume> volume;
        std::string unit;
    };
    auto on_clicked = +[](GtkButton*, gpointer data) {
        auto* ctx = static_cast<Context*>(data);
        SchedulePage* page = ctx->page;
        std::shared_ptr<Volume> volume = ctx->volume;
        systemd_timer::delete_schedule(ctx->unit, page->alive_, [page, volume](SnapperResult r) {
            if (!r.ok || !r.successful) {
                page->toast(format_string(_("Could not remove schedule: %s"), r.error_message().c_str()));
                return;
            }
            page->toast(_("Schedule removed"));
            page->refresh_rules(volume);
        });
    };
    auto* keep_alive = new Context{this, volume, unit};
    GtkWidget* remove_button = gtk_button_new_from_icon_name("user-trash-symbolic");
    gtk_widget_add_css_class(remove_button, "flat");
    gtk_widget_set_valign(remove_button, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(remove_button, _("Remove this schedule"));
    g_signal_connect(remove_button, "clicked", G_CALLBACK(on_clicked), keep_alive);
    g_object_set_data_full(G_OBJECT(remove_button), "context", keep_alive,
                           [](gpointer p) { delete static_cast<Context*>(p); });
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove_button);

    return row;
}

// ----------------------------------------------------------- create dialog ---

void SchedulePage::open_create_dialog(const std::shared_ptr<Volume>& volume) {
    show_schedule_dialog(gtk_widget_get_root(page_), volume->config, volume->subvolume, alive_,
                        [this, volume](bool success, std::string message) {
                            toast(message);
                            if (success) refresh_rules(volume);
                        });
}
