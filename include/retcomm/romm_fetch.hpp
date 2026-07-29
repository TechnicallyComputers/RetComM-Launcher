#pragma once

#include "retcomm/catalog.hpp"
#include "retcomm/config.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace retcomm {

namespace fs = std::filesystem;

using RommProgressFn = std::function<void(const std::string& status)>;

struct RommFetchResult {
    bool ok = false;
    std::string message;
    fs::path saved_path;
    std::string matched_by;  // sha1 | crc32 | size | filename
    std::string remote_name; // remote fs_name / file_name
    std::uint64_t bytes = 0;
};

// Search RomM for a ROM whose hashes/size/name match the title's rom_identity,
// download it into library_root/<platform folder>/, and return the local path.
RommFetchResult fetch_rom_from_romm(const AppConfig& cfg, const Title& title,
                                    RommProgressFn on_progress = {});

// Search RomM firmware for a file matching bios_identity and download into
// bios_root/<platform folder>/ (creates dirs as needed).
RommFetchResult fetch_bios_from_romm(const AppConfig& cfg, const Title& title,
                                     RommProgressFn on_progress = {});

} // namespace retcomm
