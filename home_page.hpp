#pragma once

#include <adwaita.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ring_chart.hpp"

struct SnapperConfig {
    std::string name;       // e.g. "root"
    std::string subvolume;  // e.g. "/"
};

class HomePage {
public:
    HomePage();
    HomePage(const HomePage&) = delete;
    HomePage& operator=(const HomePage&) = delete;
    ~HomePage();

    GtkWidget* widget() const { return page_; }

    // Re-reads the filesystem usage and the Snapper configs.
    void refresh();

private:
    struct ValueRow {
        AdwActionRow* row = nullptr;
        GtkLabel* value = nullptr;
    };

    AdwPreferencesGroup* build_filesystem_group();
    AdwPreferencesGroup* build_subvolumes_group();
    ValueRow add_value_row(GtkWidget* list, const char* title, GtkWidget* prefix);

    void refresh_usage();
    void refresh_subvolumes();

    // Snapshot calendar: marks every day on which any profile has a snapshot.
    GtkWidget* build_calendar();
    void load_snapshot_days(const std::vector<SnapperConfig>& configs);
    void update_calendar_marks();
    void update_calendar_detail();

    void show_subvolumes(const std::vector<SnapperConfig>& configs);
    void show_subvolumes_message(const std::string& title, const std::string& detail);
    void clear_subvolume_rows();

    GtkWidget* page_ = nullptr;
    AdwPreferencesGroup* fs_group_ = nullptr;
    AdwPreferencesGroup* subvol_group_ = nullptr;
    RingChart* chart_ = nullptr;
    GtkWidget* calendar_ = nullptr;
    GtkLabel* calendar_detail_ = nullptr;

    // y*10000 + m*100 + d  ->  profile name  ->  snapshot numbers taken that day
    std::map<int, std::map<std::string, std::vector<std::string>>> snapshot_days_;

    ValueRow size_, allocated_, used_, free_;
    std::vector<GtkWidget*> subvol_rows_;

    // Lets an async callback know the page is gone.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
