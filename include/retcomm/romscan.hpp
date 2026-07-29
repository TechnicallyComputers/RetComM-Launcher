#pragma once

#include "retcomm/catalog.hpp"
#include "retcomm/config.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace retcomm {

namespace fs = std::filesystem;

struct LibraryIndex; // defined in library_index.hpp

struct RomFile {
    fs::path path;
    std::string platform;  // catalog platform that owned this scan root
    std::string ext;       // lowercase including dot
    std::string crc32;     // lowercase hex, may be empty if skipped
    std::string md5;       // computed only when a title needs md5
    std::string sha1;      // computed only when a title needs sha1
    std::string sha256;    // computed only when a title needs sha256
    std::uint64_t size = 0;
    std::int64_t mtime_sec = 0;
    bool from_cache = false;
};

struct TitleMatch {
    const Title* title = nullptr;
    fs::path rom_path;          // preferred path for launch
    std::string matched_by;     // "crc32" | "md5" | "sha1" | "sha256"
    std::vector<fs::path> all_paths;
};

struct ScanResult {
    std::vector<RomFile> files;
    std::vector<TitleMatch> matches;
    std::vector<std::string> errors;
    std::vector<fs::path> scanned_roots;
    size_t hashed_files = 0;
    size_t skipped_hash = 0;
    size_t cache_hits = 0;
};

struct ScanProgress {
    std::string phase;     // "walk" | "hash" | "match" | "cache"
    std::string platform;
    fs::path path;
    size_t current = 0;
    size_t total = 0;
};

using ScanProgressFn = std::function<void(const ScanProgress&)>;

struct ScanOptions {
    bool compute_sha1_when_needed = true;
    bool compute_sha256_when_needed = true;
    bool compute_md5_when_needed = true;
    bool compute_crc_when_needed = true;
    // Ignore library-index cache and recompute digests for every candidate.
    bool full_rescan = false;
    // If set, only these platforms are scanned (catalog slugs). Empty = all in catalog.
    std::vector<std::string> platforms;
    ScanProgressFn on_progress;
    // Optional persistent index for incremental hashing (ignored when full_rescan).
    const LibraryIndex* index = nullptr;
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
