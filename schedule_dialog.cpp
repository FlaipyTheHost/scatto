#include "schedule_dialog.hpp"

#include <adwaita.h>

#include <array>
#include <cstdio>
#include <vector>

#include "i18n.hpp"
#include "snapper_cli.hpp"
#include "systemd_timer.hpp"
#include "timeline_periods.hpp"

namespace {

constexpr std::size_t kCustomIndex = kTimelinePeriods.size();

// All the state the dialog's widgets and callbacks need, kept alive for the
// dialog's lifetime via GObject data on the dialog itself.
struct DialogState {
    std::string config;
    std::string subvolume;
    std::shared_ptr<bool> alive;
    std::function<void(bool, std::string)> on_done;

    AdwDialog* dialog = nullptr;

    // Index into kTimelinePeriods, or kCustomIndex for the weekday/time schedule.
    std::size_t selected = 0;

    AdwPreferencesGroup* retention_group = nullptr;
    AdwSpinRow* retention_spin = nullptr;
    AdwPreferencesGroup* days_group = nullptr;
    AdwPreferencesGroup* time_group = nullptr;

    std::array<GtkCheckButton*, systemd_timer::kDaysPerWeek> weekday_check{};
    GtkSpinButton* hour_spin = nullptr;
    GtkSpinButton* minute_spin = nullptr;
    GtkLabel* preview = nullptr;

    GtkWidget* create_button = nullptr;
    GtkWidget* cancel_button = nullptr;
};

// A GtkLabel inside a "card"-styled box: a read-only, native-looking preview
// area (the closest stock-Adwaita equivalent of a read-only rich text box
// for a single sentence).
GtkWidget* preview_card_new(GtkLabel** out_label) {
    GtkWidget* label = gtk_label_new(nullptr);
    gtk_label_set_wrap(GTK_LABEL(label), true);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_margin_top(label, 12);
    gtk_widget_set_margin_bottom(label, 12);
    gtk_widget_set_margin_start(label, 12);
    gtk_widget_set_margin_end(label, 12);

    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card, "card");  // stock libadwaita style
    gtk_box_append(GTK_BOX(card), label);

    *out_label = GTK_LABEL(label);
    return card;
}

std::vector<int> selected_weekdays(DialogState* state) {
    std::vector<int> days;
    for (int i = 0; i < systemd_timer::kDaysPerWeek; ++i)
        if (gtk_check_button_get_active(state->weekday_check[i])) days.push_back(i);
    return days;
}

void update_preview(DialogState* state) {
    std::string sentence;
    if (state->selected == kCustomIndex) {
        const std::vector<int> days = selected_weekdays(state);
        const int hour = gtk_spin_button_get_value_as_int(state->hour_spin);
        const int minute = gtk_spin_button_get_value_as_int(state->minute_spin);
        sentence = days.empty() ? _("Select at least one day of the week.")
                                : systemd_timer::describe_custom_schedule(state->subvolume, days, hour, minute);
    } else {
        const int count = static_cast<int>(adw_spin_row_get_value(state->retention_spin));
        sentence = timeline_keep_sentence(kTimelinePeriods[state->selected].config_key, count);
    }
    gtk_label_set_text(state->preview, sentence.c_str());
}

// Shows only the settings that belong to the chosen frequency.
void select_frequency(DialogState* state, std::size_t index) {
    state->selected = index;
    const bool custom = index == kCustomIndex;
    gtk_widget_set_visible(GTK_WIDGET(state->retention_group), !custom);
    gtk_widget_set_visible(GTK_WIDGET(state->days_group), custom);
    gtk_widget_set_visible(GTK_WIDGET(state->time_group), custom);
    if (!custom) adw_spin_row_set_value(state->retention_spin, kTimelinePeriods[index].default_count);
    update_preview(state);
}

GtkWidget* build_frequency_group(DialogState* state) {
    AdwPreferencesGroup* group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, _("How often?"));
    adw_preferences_group_set_description(group, _("Choose when new snapshots are taken."));

    static const char* const kSubtitles[] = {
        N_("A snapshot every hour"),
        N_("A snapshot every day"),
        N_("A snapshot every week"),
        N_("A snapshot every month"),
        N_("A snapshot every year"),
        N_("Pick the weekdays and the time of day"),
    };
    static const char* const kIcons[] = {
        "alarm-symbolic",             // Hourly
        "weather-clear-symbolic",     // Daily
        "media-playlist-repeat-symbolic",  // Weekly
        "x-office-calendar-symbolic",      // Monthly
        "starred-symbolic",                // Yearly
        "emblem-system-symbolic",          // Custom
    };

    GtkWidget* first = nullptr;
    for (std::size_t i = 0; i <= kCustomIndex; ++i) {
        const char* title = i == kCustomIndex ? _("Custom") : _(kTimelinePeriods[i].label);

        GtkWidget* row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), _(kSubtitles[i]));
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name(kIcons[i]));

        GtkWidget* radio = gtk_check_button_new();
        gtk_widget_set_valign(radio, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(radio, "selection-mode");  // stock round radio style
        if (first) gtk_check_button_set_group(GTK_CHECK_BUTTON(radio), GTK_CHECK_BUTTON(first));
        else first = radio;
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), radio);
        adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), radio);

        struct Choice {
            DialogState* state;
            std::size_t index;
        };
        g_signal_connect_data(
            radio, "toggled",
            G_CALLBACK(+[](GtkCheckButton* b, gpointer data) {
                auto* c = static_cast<Choice*>(data);
                if (gtk_check_button_get_active(b)) select_frequency(c->state, c->index);
            }),
            new Choice{state, i}, +[](gpointer d, GClosure*) { delete static_cast<Choice*>(d); },
            static_cast<GConnectFlags>(0));

        adw_preferences_group_add(group, row);
    }
    gtk_check_button_set_active(GTK_CHECK_BUTTON(first), TRUE);
    return GTK_WIDGET(group);
}

GtkWidget* build_retention_group(DialogState* state) {
    AdwPreferencesGroup* group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, _("Retention"));
    adw_preferences_group_set_description(
        group, _("Requires Snapper's timeline timer (snapper-timeline.timer) to be enabled to actually run."));

    GtkWidget* spin_row = adw_spin_row_new_with_range(1, 100, 1);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(spin_row), _("Snapshots to keep"));
    adw_spin_row_set_value(ADW_SPIN_ROW(spin_row), kTimelinePeriods[0].default_count);
    adw_preferences_group_add(group, spin_row);
    g_signal_connect(spin_row, "notify::value",
                     G_CALLBACK(+[](GObject*, GParamSpec*, gpointer d) { update_preview(static_cast<DialogState*>(d)); }),
                     state);

    state->retention_group = group;
    state->retention_spin = ADW_SPIN_ROW(spin_row);
    return GTK_WIDGET(group);
}

GtkWidget* build_days_group(DialogState* state) {
    AdwPreferencesGroup* group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, _("Days"));

    GtkWidget* days_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(days_box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(days_box, 6);
    gtk_widget_set_margin_bottom(days_box, 6);

    auto on_toggled = +[](GtkCheckButton*, gpointer data) { update_preview(static_cast<DialogState*>(data)); };
    for (int i = 0; i < systemd_timer::kDaysPerWeek; ++i) {
        GtkWidget* check = gtk_check_button_new_with_label(g_dpgettext2(nullptr, "weekday abbreviation", systemd_timer::kWeekdayAbbrev[i]));
        gtk_widget_set_tooltip_text(check, _(systemd_timer::kWeekdayFull[i]));
        g_signal_connect(check, "toggled", G_CALLBACK(on_toggled), state);
        gtk_box_append(GTK_BOX(days_box), check);
        state->weekday_check[i] = GTK_CHECK_BUTTON(check);
    }
    adw_preferences_group_add(group, days_box);

    state->days_group = group;
    return GTK_WIDGET(group);
}

GtkWidget* build_time_group(DialogState* state) {
    AdwPreferencesGroup* group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, _("Time"));

    GtkWidget* time_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(time_row), _("Time of day"));
    adw_action_row_add_prefix(ADW_ACTION_ROW(time_row), gtk_image_new_from_icon_name("alarm-symbolic"));

    GtkWidget* time_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_valign(time_box, GTK_ALIGN_CENTER);

    GtkWidget* hour = gtk_spin_button_new_with_range(0, 23, 1);
    gtk_spin_button_set_wrap(GTK_SPIN_BUTTON(hour), true);
    gtk_editable_set_width_chars(GTK_EDITABLE(hour), 2);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(hour), 3);

    GtkWidget* minute = gtk_spin_button_new_with_range(0, 59, 5);
    gtk_spin_button_set_wrap(GTK_SPIN_BUTTON(minute), true);
    gtk_editable_set_width_chars(GTK_EDITABLE(minute), 2);

    // Always show two digits ("03" instead of "3"), like a clock.
    auto on_format = +[](GtkSpinButton* spin, gpointer) -> gboolean {
        char buffer[8];
        std::snprintf(buffer, sizeof buffer, "%02d", gtk_spin_button_get_value_as_int(spin));
        gtk_editable_set_text(GTK_EDITABLE(spin), buffer);
        return TRUE;
    };
    g_signal_connect(hour, "output", G_CALLBACK(on_format), nullptr);
    g_signal_connect(minute, "output", G_CALLBACK(on_format), nullptr);

    gtk_box_append(GTK_BOX(time_box), hour);
    gtk_box_append(GTK_BOX(time_box), gtk_label_new(":"));
    gtk_box_append(GTK_BOX(time_box), minute);
    adw_action_row_add_suffix(ADW_ACTION_ROW(time_row), time_box);
    adw_preferences_group_add(group, time_row);

    state->hour_spin = GTK_SPIN_BUTTON(hour);
    state->minute_spin = GTK_SPIN_BUTTON(minute);
    auto on_changed = +[](GtkSpinButton*, gpointer data) { update_preview(static_cast<DialogState*>(data)); };
    g_signal_connect(hour, "value-changed", G_CALLBACK(on_changed), state);
    g_signal_connect(minute, "value-changed", G_CALLBACK(on_changed), state);

    state->time_group = group;
    return GTK_WIDGET(group);
}

GtkWidget* build_summary_group(DialogState* state) {
    AdwPreferencesGroup* group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, _("Summary"));
    GtkWidget* card = preview_card_new(&state->preview);
    adw_preferences_group_add(group, card);
    return GTK_WIDGET(group);
}

void finish(DialogState* state, SnapperResult r) {
    const bool success = r.ok && r.successful;
    std::string message;
    if (success) {
        message = _("Schedule created");
    } else {
        message = r.error_message();
        if (message.empty()) message = _("Could not create schedule");
    }

    adw_dialog_close(state->dialog);
    state->on_done(success, message);
}

void on_create_clicked(GtkButton*, gpointer data) {
    auto* state = static_cast<DialogState*>(data);

    if (state->selected == kCustomIndex) {
        const std::vector<int> days = selected_weekdays(state);
        if (days.empty()) {
            update_preview(state);  // shows the "select a day" hint
            return;
        }
        const int hour = gtk_spin_button_get_value_as_int(state->hour_spin);
        const int minute = gtk_spin_button_get_value_as_int(state->minute_spin);
        systemd_timer::create_schedule(state->config, state->subvolume, days, hour, minute, state->alive,
                                       [state](SnapperResult r) { finish(state, std::move(r)); });
        return;
    }

    const int count = static_cast<int>(adw_spin_row_get_value(state->retention_spin));
    run_snapper_async({"-c", state->config, "set-config", "TIMELINE_CREATE=yes",
                       std::string(kTimelinePeriods[state->selected].config_key) + "=" + std::to_string(count)},
                      state->alive, [state](SnapperResult r) { finish(state, std::move(r)); });
}

}  // namespace

void show_schedule_dialog(GtkRoot* parent, std::string config, std::string subvolume,
                         std::shared_ptr<bool> alive,
                         std::function<void(bool success, std::string message)> on_done) {
    auto* state = new DialogState{std::move(config), std::move(subvolume), std::move(alive), std::move(on_done)};

    AdwDialog* dialog = ADW_DIALOG(adw_dialog_new());
    adw_dialog_set_content_width(dialog, 480);
    adw_dialog_set_content_height(dialog, 680);
    state->dialog = dialog;

    AdwPreferencesPage* page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(build_frequency_group(state)));
    adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(build_retention_group(state)));
    adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(build_days_group(state)));
    adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(build_time_group(state)));
    adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(build_summary_group(state)));
    GtkWidget* content = GTK_WIDGET(page);
    select_frequency(state, 0);

    GtkWidget* cancel_button = gtk_button_new_with_label(_("Cancel"));
    GtkWidget* create_button = gtk_button_new_with_label(_("Create"));
    gtk_widget_add_css_class(create_button, "suggested-action");  // stock style
    state->cancel_button = cancel_button;
    state->create_button = create_button;

    GtkWidget* header = adw_header_bar_new();
    adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), false);
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), false);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel_button);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), create_button);

    GtkWidget* toolbar_view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view), content);

    adw_dialog_set_child(dialog, toolbar_view);
    adw_dialog_set_title(dialog, format_string(_("New schedule for %s"), state->subvolume.c_str()).c_str());

    g_signal_connect(cancel_button, "clicked",
                     G_CALLBACK(+[](GtkButton*, gpointer data) { adw_dialog_close(ADW_DIALOG(data)); }), dialog);
    g_signal_connect(create_button, "clicked", G_CALLBACK(on_create_clicked), state);

    // The dialog owns the state and frees it when closed.
    g_object_set_data_full(G_OBJECT(dialog), "schedule-dialog-state", state,
                           [](gpointer p) { delete static_cast<DialogState*>(p); });

    adw_dialog_present(dialog, GTK_WIDGET(parent));
}
