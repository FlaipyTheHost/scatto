#include "fs_info.hpp"

#include <glib.h>
#include <linux/magic.h>
#include <sys/vfs.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

// Btrfs identifies its sysfs directory by filesystem UUID, so ask findmnt for it.
std::string find_fs_uuid(const std::string& path) {
    gchar* argv[] = {const_cast<gchar*>("findmnt"), const_cast<gchar*>("-no"),
                     const_cast<gchar*>("UUID"),    const_cast<gchar*>("-T"),
                     const_cast<gchar*>(path.c_str()), nullptr};

    gchar* out = nullptr;
    GError* error = nullptr;
    const gboolean ok = g_spawn_sync(
        nullptr, argv, nullptr,
        static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL),
        nullptr, nullptr, &out, nullptr, nullptr, &error);

    if (!ok) {
        g_clear_error(&error);
        return {};
    }

    std::string uuid = out ? out : "";
    g_free(out);

    // Keep only the first line, without whitespace.
    uuid = uuid.substr(0, uuid.find('\n'));
    uuid.erase(std::remove_if(uuid.begin(), uuid.end(),
                              [](unsigned char c) { return std::isspace(c); }),
               uuid.end());
    return uuid;
}

bool read_u64(const fs::path& file, std::uint64_t& value) {
    std::ifstream in(file);
    return static_cast<bool>(in >> value);
}

// Sums the raw on-disk allocation of every space type: data, metadata, system.
// These files are world-readable, so no root is needed.
bool read_btrfs_allocation(const std::string& uuid, std::uint64_t& allocated,
                           std::uint64_t& used) {
    const fs::path base = fs::path("/sys/fs/btrfs") / uuid / "allocation";

    std::error_code ec;
    if (!fs::is_directory(base, ec)) return false;

    bool found = false;
    for (const auto& entry : fs::directory_iterator(base, ec)) {
        std::error_code entry_ec;
        if (!entry.is_directory(entry_ec)) continue;

        std::uint64_t total = 0, in_use = 0;
        if (read_u64(entry.path() / "disk_total", total) &&
            read_u64(entry.path() / "disk_used", in_use)) {
            allocated += total;
            used += in_use;
            found = true;
        }
    }
    return found;
}

}  // namespace

FsUsage read_fs_usage(const std::string& path) {
    FsUsage u;

    struct statfs st {};
    if (statfs(path.c_str(), &st) != 0) return u;

    const auto block = static_cast<std::uint64_t>(st.f_bsize);
    u.size = static_cast<std::uint64_t>(st.f_blocks) * block;
    u.free = static_cast<std::uint64_t>(st.f_bavail) * block;
    u.used = u.size > u.free ? u.size - u.free : 0;
    u.allocated = u.used;
    u.ok = true;

    if (static_cast<std::uint32_t>(st.f_type) == BTRFS_SUPER_MAGIC) {
        u.is_btrfs = true;

        const std::string uuid = find_fs_uuid(path);
        std::uint64_t allocated = 0, used = 0;
        if (!uuid.empty() && read_btrfs_allocation(uuid, allocated, used)) {
            u.allocated = allocated;
            u.used = used;
        }
    }

    // Keep the numbers consistent: used <= allocated <= size.
    u.used = std::min(u.used, u.size);
    u.allocated = std::min(std::max(u.allocated, u.used), u.size);
    u.free = u.size - u.used;
    return u;
}

std::string format_bytes(std::uint64_t bytes) {
    gchar* text = g_format_size_full(bytes, G_FORMAT_SIZE_IEC_UNITS);
    std::string result = text;
    g_free(text);
    return result;
}
