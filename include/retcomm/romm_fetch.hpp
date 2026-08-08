#pragma once

#include "retcomm/catalog.hpp"
#include "retcomm/config.hpp"
#include "retcomm/paths.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>

namespace retcomm {

namespace fs = std::filesystem;

using RommProgressFn = std::function<void(const std::string& status)>;

struct RommFetchResult {
    bool ok = false;
    std::string message;
    fs::path saved_path;     // preferred path (.cue when present, else primary dump)
    std::string matched_by;  // sha1 | crc32 | size | filename
    std::string remote_name; // remote fs_name / primary file_name
    std::uint64_t bytes = 0;
    int files_saved = 0; // multi-file disc sets download every RomM file
};

// Search RomM for a ROM whose hashes/size/name match the title's rom_identity,
// download it into library_root/<platform folder>/, and return the local path.
RommFetchResult fetch_rom_from_romm(const AppConfig& cfg, const Title& title,
                                    RommProgressFn on_progress = {});

// Search RomM firmware for a file matching bios_identity and download into
// bios_root/<platform folder>/ (creates dirs as needed).
RommFetchResult fetch_bios_from_romm(const AppConfig& cfg, const Title& title,
                                     RommProgressFn on_progress = {});

// ---- RomM availability (no download) ------------------------------------

struct RommRomMatch {
    bool available = false;
    int rom_id = 0;
    int file_id = 0;
    std::string file_name;
    std::string matched_by;
    std::string message;
};

// Probe RomM for a catalog identity match (same rules as fetch, without download).
RommRomMatch match_rom_on_romm(const AppConfig& cfg, const Title& title,
                               RommProgressFn on_progress = {});

struct RommRomIndexEntry {
    bool available = false;
    int rom_id = 0;
    int file_id = 0;
    std::string file_name;
    std::string matched_by;
    std::string checked_at; // ISO UTC
};

struct RommRomIndex {
    int schema_version = 1;
    std::unordered_map<std::string, RommRomIndexEntry> by_title; // title id → entry
};

RommRomIndex load_romm_rom_index(const fs::path& path);
bool save_romm_rom_index(const fs::path& path, const RommRomIndex& index,
                         std::string* error = nullptr);

struct RommRomScanResult {
    bool ok = false;
    std::string message;
    int matched = 0;
    int missing = 0;
    int skipped = 0; // no rom_identity
    int errors = 0;
};

// Probe every catalog title with rom_identity and rewrite romm-rom-index.json.
RommRomScanResult scan_romm_rom_index(const Paths& paths, const AppConfig& cfg,
                                      const Catalog& catalog, RommProgressFn on_progress = {});

} // namespace retcomm
