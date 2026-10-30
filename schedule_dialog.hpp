#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <string>

// Opens the "New Schedule" dialog for `config` / `subvolume`: a single page
// where the user picks a frequency (Snapper's built-in Hourly ... Yearly, or
// Custom, backed by a systemd --user timer) and then sees only its settings. `on_done` fires once the user confirms
// and the underlying command finishes (never on Cancel), with whether it
// succeeded and a message suitable for a toast.
void show_schedule_dialog(GtkRoot* parent, std::string config, std::string subvolume,
                         std::shared_ptr<bool> alive,
                         std::function<void(bool success, std::string message)> on_done);
