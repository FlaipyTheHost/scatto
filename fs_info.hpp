#pragma once

#include <cstdint>
#include <string>

// Space information for one mounted filesystem.
struct FsUsage {
    bool ok = false;        // false if the data could not be read
    bool is_btrfs = false;  // allocation details are only available on Btrfs

    std::uint64_t size = 0;       // total size of the filesystem
    std::uint64_t allocated = 0;  // space reserved in chunks (Btrfs); == used elsewhere
    std::uint64_t used = 0;       // space actually in use
    std::uint64_t free = 0;       // size - used
};

// Reads size / allocated / used / free for the filesystem mounted at `path`.
// Works without root privileges (statfs + /sys/fs/btrfs).
FsUsage read_fs_usage(const std::string& path);

// "465.8 GiB"
std::string format_bytes(std::uint64_t bytes);
