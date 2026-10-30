#include <adwaita.h>

#include <clocale>
#include <string>

#include "home_page.hpp"
#include "i18n.hpp"
#include "root_helper.hpp"
#include "schedule_page.hpp"
#include "snaps_page.hpp"

namespace {

constexpr const char* kAppId = "org.gnome.scatto";
constexpr const char* kAppName = "Scatto";
constexpr const char* kAppVersion = "0.1.0";
constexpr const char* kRepoUrl = "https://github.com/FlaipyTheHost/scatto";

struct Pages {
    HomePage* home;
    SnapsPage* snaps;
    SchedulePage* schedule;
    GtkWidget* window;
};

void on_refresh(GSimpleAction*, GVariant*, gpointer user_data) {
    auto* pages = static_cast<Pages*>(user_data);
    pages->home->refresh();
    pages->snaps->refresh();
    pages->schedule->refresh();
}

void on_about(GSimpleAction*, GVariant*, gpointer user_data) {
    auto* pages = static_cast<Pages*>(user_data);

    AdwDialog* about = adw_about_dialog_new();
    adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(about), kAppName);
    adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(about), kAppId);
    adw_about_dialog_set_version(ADW_ABOUT_DIALOG(about), kAppVersion);
    adw_about_dialog_set_comments(ADW_ABOUT_DIALOG(about), _("Manage Btrfs snapshots and schedules with Snapper"));
    adw_about_dialog_set_developer_name(ADW_ABOUT_DIALOG(about), "FlaipyTheHost");
    adw_about_dialog_set_license_type(ADW_ABOUT_DIALOG(about), GTK_LICENSE_GPL_3_0);
    adw_about_dialog_set_translator_credits(ADW_ABOUT_DIALOG(about), _("translator-credits"));
    adw_about_dialog_set_website(ADW_ABOUT_DIALOG(about), kRepoUrl);
    adw_about_dialog_set_issue_url(ADW_ABOUT_DIALOG(about), (std::string(kRepoUrl) + "/issues").c_str());
    adw_dialog_present(about, pages->window);
}

GtkWindow* create_window(AdwApplication* app) {
    //Toasts
    GtkWidget* toast_overlay = adw_toast_overlay_new();

    // Tabs

    auto* home = new HomePage();                                        // owned by its widget
    auto* snaps = new SnapsPage(ADW_TOAST_OVERLAY(toast_overlay));       // owned by its widget
    auto* schedule = new SchedulePage(ADW_TOAST_OVERLAY(toast_overlay)); // owned by its widget

    GtkWidget* stack = adw_view_stack_new();
    adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(stack), home->widget(), "home", _("Home"),
                                        "go-home-symbolic");
    adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(stack), snaps->widget(), "snaps", _("Snaps"),
                                        "camera-photo-symbolic");
    adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(stack), schedule->widget(), "schedule", _("Schedule"),
                                        "alarm-symbolic");
    // Future tabs go here, e.g. "configs", ...

    adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(toast_overlay), stack);

    // Header bAR
    GtkWidget* switcher = adw_view_switcher_new();
    adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);
    adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_STACK(stack));

    GtkWidget* refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh, _("Refresh (Ctrl+R)"));
    gtk_actionable_set_action_name(GTK_ACTIONABLE(refresh), "win.refresh");

    // Primary menu: "About" goes last, per the GNOME HIG.
    GMenu* menu = g_menu_new();
    g_menu_append(menu, format_string(_("About %s"), kAppName).c_str(), "win.about");
    GtkWidget* menu_button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), G_MENU_MODEL(menu));
    gtk_widget_set_tooltip_text(menu_button, _("Main Menu"));
    g_object_unref(menu);

    GtkWidget* header = adw_header_bar_new();
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), switcher);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), refresh);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu_button);

    GtkWidget* toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), toast_overlay);

    // Window
    GtkWidget* window = adw_application_window_new(GTK_APPLICATION(app));
    gtk_window_set_title(GTK_WINDOW(window), "Scatto - Snapper Manager");
    gtk_window_set_default_size(GTK_WINDOW(window), 720, 820);
    gtk_widget_set_size_request(GTK_WIDGET(window), 680, 660);  // minimum size
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), toolbar);

    // Actions

    auto* pages = new Pages{home, snaps, schedule, window};
    g_object_set_data_full(G_OBJECT(window), "pages", pages, [](gpointer p) { delete static_cast<Pages*>(p); });

    static const GActionEntry entries[] = {
        {"refresh", on_refresh, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"about", on_about, nullptr, nullptr, nullptr, {0, 0, 0}},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(window), entries, G_N_ELEMENTS(entries), pages);

    const char* const accels[] = {"<Control>r", "F5", nullptr};
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.refresh", accels);

    home->refresh();
    snaps->refresh();
    schedule->refresh();
    return GTK_WINDOW(window);
}

void on_activate(GApplication* app, gpointer) {
    GtkWindow* window = gtk_application_get_active_window(GTK_APPLICATION(app));
    if (!window) window = create_window(ADW_APPLICATION(app));
    gtk_window_present(window);
}

// Shown *before* the app's main loop starts, so it needs its own short-lived
// one. There is no parent window yet -- this either happens right at
// startup, or not at all.
void show_startup_error_and_wait(const std::string& message) {
    gtk_init();

    GtkAlertDialog* dialog = gtk_alert_dialog_new(
        _("Snapper Manager needs administrator privileges to manage Snapper, and could not get them."));
    gtk_alert_dialog_set_detail(dialog, message.c_str());

    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    gtk_alert_dialog_choose(
        dialog, nullptr, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, nullptr);
            g_main_loop_quit(static_cast<GMainLoop*>(data));
        },
        loop);
    g_main_loop_run(loop);

    g_main_loop_unref(loop);
    g_object_unref(dialog);
}

// Translations: standard gettext. Installed builds read <prefix>/share/locale;
// a build tree finds the .mo files that meson generates in ./po next to the
// executable; SCATTO_LOCALEDIR overrides both (handy for testing).
void setup_i18n() {
    setlocale(LC_ALL, "");

    std::string dir = LOCALEDIR;
    if (const char* override_dir = g_getenv("SCATTO_LOCALEDIR")) {
        dir = override_dir;
    } else if (gchar* exe = g_file_read_link("/proc/self/exe", nullptr)) {
        gchar* exe_dir = g_path_get_dirname(exe);
        gchar* build_dir = g_build_filename(exe_dir, "po", nullptr);
        if (g_file_test(build_dir, G_FILE_TEST_IS_DIR)) dir = build_dir;
        g_free(build_dir);
        g_free(exe_dir);
        g_free(exe);
    }

    bindtextdomain(GETTEXT_PACKAGE, dir.c_str());
    bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
    textdomain(GETTEXT_PACKAGE);
}

}  // namespace

int main(int argc, char** argv) {
    setup_i18n();
    // The GUI runs as the current user throughout (so it keeps the user's
    // own theme, fonts and settings); only this one small helper process
    // runs as root, authenticated once via pkexec. If that authentication
    // is refused or cancelled, the app does not open at all -- same as
    // GParted's "Executing as root" prompt.
    std::string error;
    if (!root_helper::start(error)) {
        show_startup_error_and_wait(error);
        return 1;
    }

    AdwApplication* app = adw_application_new(kAppId, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);

    const int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    root_helper::stop();
    return status;
}
