#pragma once

#include "retcomm/catalog.hpp"
#include "retcomm/config.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace retcomm {

namespace fs = std::filesystem;

struct BiosFile {
    std::string path;
    std::string platform; // catalog platform slug when known
    std::string filename;
    std::uint64_t size = 0;
    std::int64_t mtime_sec = 0;
    std::string crc32;
    std::string md5;
    std::string sha1;
    std::string sha256;
    std::string title_id;   // empty if unmatched
    std::string matched_by; // crc32 | md5 | sha1 | sha256 | filename | size
};

struct BiosTitleBind {
    std::string title_id;
    std::string preferred_path;
    std::vector<std::string> paths;
};

struct BiosIndex {
    int schema_version = 1;
    std::string bios_root;
    std::vector<BiosFile> files;
    std::vector<BiosTitleBind> titles;

    std::unordered_map<std::string, size_t> by_path;

    void rebuild_path_map();
    const BiosFile* find_path(const std::string& path) const;
    BiosFile* find_path_mut(const std::string& path);
    bool is_fresh(const BiosFile& f, std::uint64_t size, std::int64_t mtime_sec) const;
    fs::path preferred_bios(const std::string& title_id) const;
    const BiosTitleBind* find_title(const std::string& title_id) const;
};

struct BiosScanProgress {
    std::string phase; // walk | cache | hash | match
    std::string platform;
    std::size_t current = 0;
    std::size_t total = 0;
    fs::path path;
};

using BiosScanProgressFn = std::function<void(const BiosScanProgress&)>;

struct BiosScanOptions {
    BiosScanProgressFn on_progress;
    BiosIndex* index = nullptr; // for cache hits (ignored when full_rescan)
    // Ignore bios-index cache and re-hash every candidate.
    bool full_rescan = false;
};

struct BiosScanResult {
    std::vector<fs::path> scanned_roots;
    std::vector<BiosFile> files;
    std::vector<BiosTitleBind> matches;
    std::size_t hashed_files = 0;
    std::size_t cache_hits = 0;
    std::size_t skipped_hash = 0;
    std::vector<std::string> errors;
};

BiosIndex load_bios_index(const fs::path& path);
bool save_bios_index(const fs::path& path, const BiosIndex& index);

BiosScanResult scan_bios_library(const Catalog& catalog, const AppConfig& cfg,
                                 const BiosScanOptions& opts = {});
BiosScanResult scan_bios_roots(const Catalog& catalog, const AppConfig& cfg,
                               const std::vector<fs::path>& roots,
                               const BiosScanOptions& opts = {});

void merge_bios_scan_into_index(BiosIndex& index, const Catalog& catalog,
                                const BiosScanResult& scan, const fs::path& bios_root);

// Rebuild title↔BIOS binds from already-hashed index files against the current
// catalog. No filesystem walk / re-hash — use after Refresh Catalog when new
// titles appear that share dumps already in the index.
// Returns the number of titles with at least one matching BIOS dump.
std::size_t rematch_bios_titles(BiosIndex& index, const Catalog& catalog);

} // namespace retcomm
