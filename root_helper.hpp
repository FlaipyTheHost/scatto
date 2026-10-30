#pragma once

#include <functional>
#include <string>
#include <vector>

#include "snapper_cli.hpp"

// Manages the one privileged helper process for the app's whole lifetime.
// The GUI itself never runs as root -- only this small subprocess does,
// started once via `pkexec` (which is what shows the "authenticate as
// administrator" prompt), so the interface keeps the user's own theme,
// fonts and settings throughout.
namespace root_helper {

// Looks for `snapper-manager-helper` next to the running executable, then
// launches it as root via `pkexec` and waits (blocking -- this is meant to
// be called once, at startup, before any window is shown) for it to report
// that it is actually running as root.
//
// Returns true on success. Returns false if pkexec could not be found, the
// helper binary could not be located, or the user cancelled/failed
// authentication -- `error` is set to a message suitable for showing to the
// person before the app quits.
bool start(std::string& error);

// Runs `snapper <args...>` as root through the already-running helper.
// Never blocks the caller; `callback` fires on the main loop once the
// helper responds (or once the channel is known to be broken).
void run_async(std::vector<std::string> args, SnapperCallback callback);

// Shuts the helper process down. Safe to call even if start() was never
// called or failed.
void stop();

}  // namespace root_helper
