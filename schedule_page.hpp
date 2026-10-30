#pragma once

#include <adwaita.h>

#include <memory>
#include <string>
#include <vector>

// The "Schedule" tab: one group per Snapper-managed volume, listing its
// active scheduling rules -- both Snapper's built-in timeline (hourly ...
// yearly retention counts) and this app's own custom systemd-timer-based
// schedules -- with a "+" button to create a new rule.
class SchedulePage {
public:
    explicit SchedulePage(AdwToastOverlay* toast_overlay);
    SchedulePage(const SchedulePage&) = delete;
    SchedulePage& operator=(const SchedulePage&) = delete;
    ~SchedulePage();

    GtkWidget* widget() const { return page_; }

    void refresh();

private:
    struct Volume {
        std::string config;
        std::string subvolume;
        AdwPreferencesGroup* group = nullptr;
        std::vector<GtkWidget*> rows;
    };

    void show_message(const std::string& title, const std::string& detail);
    void clear_volumes();
    void show_configs(std::vector<std::pair<std::string, std::string>> configs);

    void refresh_rules(const std::shared_ptr<Volume>& volume);
    void show_rules(const std::shared_ptr<Volume>& volume, std::vector<GtkWidget*> rows);
    GtkWidget* build_timeline_row(const std::shared_ptr<Volume>& volume, const std::string& period,
                                 const std::string& label, int count);
    GtkWidget* build_custom_row(const std::shared_ptr<Volume>& volume, const std::string& unit,
                               const std::string& description);

    void open_create_dialog(const std::shared_ptr<Volume>& volume);
    void toast(const std::string& message);

    GtkWidget* page_ = nullptr;  // AdwPreferencesPage
    AdwToastOverlay* toast_overlay_ = nullptr;
    std::vector<std::shared_ptr<Volume>> volumes_;

    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
