#include "snaps_page.hpp"

#include <algorithm>
#include <cstdlib>
#include <cerrno>
#include <ctime>
#include <unistd.h>

#include "i18n.hpp"
#include "snapper_cli.hpp"

namespace {

// A GSimpleAction whose "activate" runs an arbitrary std::function. Used to
// wire up each snapshot row's menu without a maze of hand-written GObject
// subclasses.
void add_action(GSimpleActionGroup* group, const char* name, std::function<void()> fn) {
    GSimpleAction* action = g_simple_action_new(name, nullptr);
    auto* fn_ptr = new std::function<void()>(std::move(fn));

    g_signal_connect_data(
        action, "activate",
        G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer data) {
            (*static_cast<std::function<void()>*>(data))();
        }),
        fn_ptr, [](gpointer data, GClosure*) { delete static_cast<std::function<void()>*>(data); },
        static_cast<GConnectFlags>(0));

    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
    g_object_unref(action);
}

std::string format_datetime(const std::string& iso) {
    if (iso.empty()) return "—";

    std::tm tm{};
    if (!strptime(iso.c_str(), "%Y-%m-%d %H:%M:%S", &tm)) return iso;

    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%d/%m/%Y %H:%M", &tm);
    return buffer;
}

const char* icon_for_type(const std::string& type) {
    if (type == "pre") return "media-seek-backward-symbolic";
    if (type == "post") return "media-seek-forward-symbolic";
    if (type == "timeline") return "alarm-symbolic";
    return "camera-photo-symbolic";  // "single", and anything unrecognised
}

std::string display_title(const std::string& description, const std::string& number) {
    return description.empty() ? format_string(_("Snapshot #%s"), number.c_str()) : description;
}

}  // namespace

SnapsPage::SnapsPage(AdwToastOverlay* toast_overlay) : toast_overlay_(toast_overlay) {
    page_ = adw_preferences_page_new();

    // The widget owns this object.
    g_object_set_data_full(G_OBJECT(page_), "snaps-page", this,
                           [](gpointer p) { delete static_cast<SnapsPage*>(p); });
}

SnapsPage::~SnapsPage() {
    *alive_ = false;
}

void SnapsPage::toast(const std::string& message) {
    AdwToast* t = adw_toast_new(message.c_str());
    adw_toast_set_timeout(t, 4);
    adw_toast_overlay_add_toast(toast_overlay_, t);
}

void SnapsPage::refresh() {
    run_snapper_async({"--csvout", "list-configs"}, alive_, [this](SnapperResult r) {
        if (!r.ok || !r.successful) {
            show_message(_("Could not list profiles"), r.error_message());
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

void SnapsPage::clear_volumes() {
    for (const std::shared_ptr<Volume>& v : volumes_) adw_preferences_page_remove(
        ADW_PREFERENCES_PAGE(page_), ADW_PREFERENCES_GROUP(v->group));
    volumes_.clear();
}

void SnapsPage::show_message(const std::string& title, const std::string& detail) {
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

void SnapsPage::show_configs(std::vector<std::pair<std::string, std::string>> configs) {
    if (configs.empty()) {
        show_message(_("No profiles found"),
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
            SnapsPage* page;
            std::shared_ptr<Volume> volume;
        };
        // Note: the lambdas are assigned to a plain variable *before* being
        // handed to G_CALLBACK, since that macro mis-parses any un-parenthesised
        // comma in its argument (e.g. inside a template type) as an extra arg.
        auto on_clicked = +[](GtkButton*, gpointer data) {
            auto* ctx = static_cast<AddButtonContext*>(data);
            ctx->page->show_create_dialog(ctx->volume);
        };
        auto on_destroy = +[](gpointer data, GClosure*) { delete static_cast<AddButtonContext*>(data); };

        GtkWidget* add_button = gtk_button_new_from_icon_name("list-add-symbolic");
        gtk_widget_add_css_class(add_button, "flat");  // stock style
        gtk_widget_set_valign(add_button, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(add_button, _("New snapshot"));
        g_signal_connect_data(add_button, "clicked", G_CALLBACK(on_clicked),
                              new AddButtonContext{this, volume}, on_destroy,
                              static_cast<GConnectFlags>(0));
        adw_preferences_group_set_header_suffix(volume->group, add_button);

        adw_preferences_page_add(ADW_PREFERENCES_PAGE(page_), volume->group);
        volumes_.push_back(volume);

        refresh_snapshots(volume);
    }
}

// ----------------------------------------------------------- snapshots ---

void SnapsPage::refresh_snapshots(const std::shared_ptr<Volume>& volume) {
    run_snapper_async(
        {"-c", volume->config, "--csvout", "--iso", "list", "--columns",
         "number,date,description,type,active"},
        alive_, [this, volume](SnapperResult r) {
            if (!r.ok || !r.successful) {
                show_volume_message(volume, r.error_message().empty()
                                               ? _("Could not list snapshots")
                                               : r.error_message());
                return;
            }

            std::vector<Snapshot> snapshots;
            for (const CsvRow& row : parse_csv_table(r.std_out)) {
                auto number = row.find("number");
                if (number == row.end() || number->second.empty()) continue;
                if (number->second == "0") continue;
                Snapshot s;
                s.number = number->second;
                if (auto it = row.find("date"); it != row.end()) s.date = it->second;
                if (auto it = row.find("description"); it != row.end()) s.description = it->second;
                if (auto it = row.find("type"); it != row.end()) s.type = it->second;
                if (auto it = row.find("active"); it != row.end())
                    s.active = (it->second == "yes" || it->second == "true");
                snapshots.push_back(std::move(s));
            }

            // Most recent first order by
            std::sort(snapshots.begin(), snapshots.end(), [](const Snapshot& a, const Snapshot& b) {
                return std::atol(a.number.c_str()) > std::atol(b.number.c_str());
            });

            show_snapshots(volume, std::move(snapshots));
        });
}

void SnapsPage::show_volume_message(const std::shared_ptr<Volume>& volume, const std::string& text) {
    for (GtkWidget* row : volume->rows) adw_preferences_group_remove(volume->group, row);
    volume->rows.clear();

    GtkWidget* row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), escape_markup(text).c_str());
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("dialog-information-symbolic"));
    adw_preferences_group_add(volume->group, row);
    volume->rows.push_back(row);
}

void SnapsPage::show_snapshots(const std::shared_ptr<Volume>& volume, std::vector<Snapshot> snapshots) {
    for (GtkWidget* row : volume->rows) adw_preferences_group_remove(volume->group, row);
    volume->rows.clear();

    if (snapshots.empty()) {
        show_volume_message(volume, _("No snapshots yet"));
        return;
    }

    for (const Snapshot& snap : snapshots) {
        GtkWidget* row = build_snapshot_row(volume, snap);
        adw_preferences_group_add(volume->group, row);
        volume->rows.push_back(row);
    }
}

GtkWidget* SnapsPage::build_snapshot_row(const std::shared_ptr<Volume>& volume, const Snapshot& snap) {
    GtkWidget* row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                  escape_markup(display_title(snap.description, snap.number)).c_str());
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(row),
        escape_markup("#" + snap.number + " - " + format_datetime(snap.date)).c_str());
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name(icon_for_type(snap.type)));

    if (snap.active) {
        GtkWidget* current = gtk_image_new_from_icon_name("emblem-default-symbolic");
        gtk_widget_set_tooltip_text(current, _("Current default snapshot"));
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), current);
    }

    // Per-row actions (Browse / Restore / Delete), reached through a menu button.
    GSimpleActionGroup* actions = g_simple_action_group_new();
    add_action(actions, "browse", [this, volume, snap] { browse(volume, snap); });
    add_action(actions, "restore", [this, volume, snap] { confirm_restore(volume, snap); });
    add_action(actions, "delete", [this, volume, snap] { confirm_delete(volume, snap); });
    gtk_widget_insert_action_group(row, "row", G_ACTION_GROUP(actions));
    g_object_unref(actions);

    GMenu* menu = g_menu_new();
    g_menu_append(menu, _("Browse Files"), "row.browse");
    g_menu_append(menu, _("Restore Snapshot"), "row.restore");
    GMenu* danger = g_menu_new();
    g_menu_append(danger, _("Delete Snapshot"), "row.delete");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(danger));
    g_object_unref(danger);

    GtkWidget* menu_button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "view-more-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), G_MENU_MODEL(menu));
    gtk_widget_add_css_class(menu_button, "flat");  // stock style
    gtk_widget_set_valign(menu_button, GTK_ALIGN_CENTER);
    g_object_unref(menu);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), menu_button);

    return row;
}

// --------------------------------------------------------------- create ---

void SnapsPage::show_create_dialog(const std::shared_ptr<Volume>& volume) {
    const std::string body = format_string(_("Creating a new snapshot for %s."), volume->subvolume.c_str());

    AdwDialog* dialog = adw_alert_dialog_new(_("New Snapshot"), nullptr);
    adw_alert_dialog_set_body(ADW_ALERT_DIALOG(dialog), body.c_str());

    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), _("Description, e.g. \"Before update\""));
    gtk_entry_set_activates_default(GTK_ENTRY(entry), true);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), entry);
    adw_dialog_set_focus(ADW_DIALOG(dialog), entry);  // so typing works right away

    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", _("Cancel"));
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "create", _("Create"));
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "create", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "create");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");

    struct Context {
        SnapsPage* page;
        std::shared_ptr<Volume> volume;
        GtkWidget* entry;
    };
    auto* ctx = new Context{this, volume, entry};

    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(dialog), GTK_WIDGET(gtk_widget_get_root(page_)), nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<Context> ctx(static_cast<Context*>(data));
            const char* response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);

            if (g_strcmp0(response, "create") != 0) return;
            if (!*ctx->page->alive_) return;

            std::string description = trim(gtk_editable_get_text(GTK_EDITABLE(ctx->entry)));
            std::vector<std::string> args = {"-c", ctx->volume->config, "create", "--type", "single"};
            if (!description.empty()) {
                args.push_back("--description");
                args.push_back(description);
            }

            ctx->page->run_action(std::move(args), "", _("Snapshot created"), ctx->volume);
        },
        ctx);
}

// -------------------------------------------------------- restore/delete ---

void SnapsPage::confirm_restore(const std::shared_ptr<Volume>& volume, const Snapshot& snap) {
    const std::string title = format_string(_("Restore %s?"), display_title(snap.description, snap.number).c_str());
    const std::string body = format_string(
        _("This overwrites the current content of %s with the content of snapshot #%s. This cannot be undone."),
        volume->subvolume.c_str(), snap.number.c_str());

    AdwDialog* dialog = adw_alert_dialog_new(title.c_str(), body.c_str());
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", _("Cancel"));
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "restore", _("Restore"));
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "restore", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");

    struct Context {
        SnapsPage* page;
        std::shared_ptr<Volume> volume;
        std::string number;
    };
    auto* ctx = new Context{this, volume, snap.number};

    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(dialog), GTK_WIDGET(gtk_widget_get_root(page_)), nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<Context> ctx(static_cast<Context*>(data));
            const char* response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
            if (g_strcmp0(response, "restore") != 0 || !*ctx->page->alive_) return;

            ctx->page->run_action({"-c", ctx->volume->config, "undochange", ctx->number + "..0"}, "",
                                  format_string(_("Snapshot #%s restored"), ctx->number.c_str()), ctx->volume);
        },
        ctx);
}

void SnapsPage::confirm_delete(const std::shared_ptr<Volume>& volume, const Snapshot& snap) {
    const std::string title = format_string(_("Delete %s?"), display_title(snap.description, snap.number).c_str());
    const std::string body = format_string(_("Snapshot #%s will be permanently deleted."), snap.number.c_str());

    AdwDialog* dialog = adw_alert_dialog_new(title.c_str(), body.c_str());
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", _("Cancel"));
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "delete", _("Delete"));
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "delete", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");

    struct Context {
        SnapsPage* page;
        std::shared_ptr<Volume> volume;
        std::string number;
    };
    auto* ctx = new Context{this, volume, snap.number};

    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(dialog), GTK_WIDGET(gtk_widget_get_root(page_)), nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<Context> ctx(static_cast<Context*>(data));
            const char* response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
            if (g_strcmp0(response, "delete") != 0 || !*ctx->page->alive_) return;

            ctx->page->run_action({"-c", ctx->volume->config, "delete", ctx->number}, "",
                                  format_string(_("Snapshot #%s deleted"), ctx->number.c_str()), ctx->volume);
        },
        ctx);
}

void SnapsPage::run_action(std::vector<std::string> args, std::string /*busy_toast*/,
                           std::string success_toast, std::shared_ptr<Volume> volume_to_refresh) {
    run_snapper_async(std::move(args), alive_,
                      [this, success_toast, volume_to_refresh](SnapperResult r) {
                          if (!r.ok || !r.successful) {
                              toast(r.error_message().empty() ? _("Snapper command failed")
                                                               : r.error_message());
                              return;
                          }
                          toast(success_toast);
                          if (volume_to_refresh) refresh_snapshots(volume_to_refresh);
                      });
}

// ----------------------------------------------------------------- browse ---

void SnapsPage::browse(const std::shared_ptr<Volume>& volume, const Snapshot& snap) {
    std::string base = volume->subvolume;
    if (!base.empty() && base.back() == '/') base.pop_back();
    const std::string path = base + "/.snapshots/" + snap.number + "/snapshot";

    // Snapper creates <subvolume>/.snapshots as root-only (0750), so a regular
    // user's file manager just fails with "permission denied". Detect that and
    // offer to enable Snapper's own ALLOW_USERS/SYNC_ACL mechanism.
    if (access(path.c_str(), R_OK | X_OK) != 0) {
        if (errno == EACCES) offer_browse_access(volume, snap);
        else toast(format_string(_("Snapshot folder not found: %s"), path.c_str()));
        return;
    }

    open_in_file_manager(path);
}

void SnapsPage::open_in_file_manager(const std::string& path) {
    GError* error = nullptr;
    gchar* uri = g_filename_to_uri(path.c_str(), nullptr, &error);
    if (!uri) {
        toast(_("Could not build a path for that snapshot"));
        g_clear_error(&error);
        return;
    }

    const gboolean ok = g_app_info_launch_default_for_uri(uri, nullptr, &error);
    g_free(uri);

    if (!ok) {
        toast(_("Could not open a file manager for that snapshot"));
        g_clear_error(&error);
    }
}

void SnapsPage::offer_browse_access(const std::shared_ptr<Volume>& volume, const Snapshot& snap) {
    const std::string user = g_get_user_name();
    const std::string body = format_string(
        _("Snapshots of %s can only be read by root. Allow \"%s\" to browse them? "
          "This adds the user to ALLOW_USERS and enables SYNC_ACL in the Snapper configuration \"%s\"."),
        volume->subvolume.c_str(), user.c_str(), volume->config.c_str());

    AdwDialog* dialog = adw_alert_dialog_new(_("Allow Access to Snapshots?"), body.c_str());
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", _("Cancel"));
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "allow", _("Allow Access"));
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "allow", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "allow");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");

    struct Context {
        SnapsPage* page;
        std::shared_ptr<Volume> volume;
        Snapshot snap;
    };
    auto* ctx = new Context{this, volume, snap};

    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(dialog), GTK_WIDGET(gtk_widget_get_root(page_)), nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<Context> ctx(static_cast<Context*>(data));
            const char* response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
            if (g_strcmp0(response, "allow") != 0 || !*ctx->page->alive_) return;
            ctx->page->grant_browse_access(ctx->volume, ctx->snap);
        },
        ctx);
}

void SnapsPage::grant_browse_access(const std::shared_ptr<Volume>& volume, const Snapshot& snap) {
    // Keep users that are already allowed: read ALLOW_USERS first, then append.
    run_snapper_async(
        {"-c", volume->config, "--csvout", "get-config", "--columns", "key,value"}, alive_,
        [this, volume, snap](SnapperResult r) {
            if (!r.ok || !r.successful) {
                toast(_("Could not read the Snapper configuration"));
                return;
            }

            std::string allowed;
            for (const CsvRow& row : parse_csv_table(r.std_out)) {
                auto key = row.find("key");
                auto value = row.find("value");
                if (key != row.end() && value != row.end() && key->second == "ALLOW_USERS") allowed = value->second;
            }

            // ALLOW_USERS is a space-separated list.
            const std::string user = g_get_user_name();
            bool present = false;
            for (std::size_t pos = 0; pos <= allowed.size();) {
                std::size_t end = allowed.find(' ', pos);
                if (end == std::string::npos) end = allowed.size();
                if (allowed.compare(pos, end - pos, user) == 0 && end - pos == user.size()) present = true;
                pos = end + 1;
            }
            if (!present) allowed += (allowed.empty() ? "" : " ") + user;

            run_snapper_async({"-c", volume->config, "set-config", "ALLOW_USERS=" + allowed, "SYNC_ACL=yes"},
                              alive_, [this, volume, snap](SnapperResult r2) {
                                  if (!r2.ok || !r2.successful) {
                                      toast(r2.error_message().empty() ? _("Could not grant access")
                                                                        : r2.error_message());
                                      return;
                                  }
                                  browse(volume, snap);
                              });
        });
}
