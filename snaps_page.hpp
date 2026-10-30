#pragma once

#include <adwaita.h>

#include <memory>
#include <string>
#include <vector>

class SnapsPage {
public:
    explicit SnapsPage(AdwToastOverlay* toast_overlay);
    SnapsPage(const SnapsPage&) = delete;
    SnapsPage& operator=(const SnapsPage&) = delete;
    ~SnapsPage();

    GtkWidget* widget() const { return page_; }

    // Re-reads the list of configs and, for each, its snapshots.
    void refresh();

private:
    struct Snapshot {
        std::string number;
        std::string date;         // raw ISO date from snapper, may be empty
        std::string description;
        std::string type;         // single, pre, post, timeline
        bool active = false;      // this is the config's default/active snapshot
    };

    struct Volume {
        std::string config;
        std::string subvolume;
        AdwPreferencesGroup* group = nullptr;
        std::vector<GtkWidget*> rows;
    };

    void show_configs(std::vector<std::pair<std::string, std::string>> configs);
    void show_message(const std::string& title, const std::string& detail);
    void clear_volumes();

    void refresh_snapshots(const std::shared_ptr<Volume>& volume);
    void show_snapshots(const std::shared_ptr<Volume>& volume, std::vector<Snapshot> snapshots);
    void show_volume_message(const std::shared_ptr<Volume>& volume, const std::string& text);
    GtkWidget* build_snapshot_row(const std::shared_ptr<Volume>& volume, const Snapshot& snap);

    void show_create_dialog(const std::shared_ptr<Volume>& volume);
    void confirm_restore(const std::shared_ptr<Volume>& volume, const Snapshot& snap);
    void confirm_delete(const std::shared_ptr<Volume>& volume, const Snapshot& snap);
    void browse(const std::shared_ptr<Volume>& volume, const Snapshot& snap);
    void open_in_file_manager(const std::string& path);
    void offer_browse_access(const std::shared_ptr<Volume>& volume, const Snapshot& snap);
    void grant_browse_access(const std::shared_ptr<Volume>& volume, const Snapshot& snap);

    void run_action(std::vector<std::string> args, std::string busy_toast,
                    std::string success_toast, std::shared_ptr<Volume> volume_to_refresh);
    void toast(const std::string& message);

    GtkWidget* page_ = nullptr;  // AdwPreferencesPage
    AdwToastOverlay* toast_overlay_ = nullptr;
    std::vector<std::shared_ptr<Volume>> volumes_;

    // Lets async callbacks know the page is gone.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
