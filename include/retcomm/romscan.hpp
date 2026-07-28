#pragma once

#include "retcomm/catalog.hpp"
#include "retcomm/config.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace retcomm {

namespace fs = std::filesystem;

struct RomFile {
    fs::path path;
    std::string platform;  // catalog platform that owned this scan root
    std::string ext;       // lowercase including dot
    std::string crc32;     // lowercase hex, may be empty if skipped
    std::string sha1;      // computed only when a title needs sha1
};

struct TitleMatch {
    const Title* title = nullptr;
    fs::path rom_path;
    std::string matched_by; // "crc32" | "sha1" | "sha256"
};

struct ScanResult {
    std::vector<RomFile> files;
    std::vector<TitleMatch> matches;
    std::vector<std::string> errors;
    std::vector<fs::path> scanned_roots;
    size_t hashed_files = 0;
    size_t skipped_hash = 0;
};

struct ScanProgress {
    std::string phase;     // "walk" | "hash" | "match"
    std::string platform;
    fs::path path;
    size_t current = 0;
    size_t total = 0;
};

using ScanProgressFn = std::function<void(const ScanProgress&)>;

struct ScanOptions {
    bool compute_sha1_when_needed = true;
    bool compute_crc_when_needed = true;
    // If set, only these platforms are scanned (catalog slugs). Empty = all in catalog.
    std::vector<std::string> platforms;
    ScanProgressFn on_progress;
};

// Platform-scoped scan under config.library_root (RomM layout).
ScanResult scan_rom_library(const Catalog& catalog, const AppConfig& config,
                            const ScanOptions& opts = {});

// Explicit roots: each path is treated as a platform folder for `platform_hint`
// if non-empty; otherwise the folder basename is matched back to a catalog
// platform via config.platform_folders. Used for `--rom-dir snes`.
ScanResult scan_rom_roots(const Catalog& catalog, const AppConfig& config,
                          const std::vector<fs::path>& roots,
                          const ScanOptions& opts = {});

} // namespace retcomm
