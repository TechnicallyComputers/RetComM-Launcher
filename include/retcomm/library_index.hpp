#pragma once

#include "retcomm/catalog.hpp"
#include "retcomm/romscan.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace retcomm {

namespace fs = std::filesystem;

// One indexed ROM/disc file under the user's library.
struct LibraryFile {
    std::string path;
    std::string platform;
    std::string ext;
    std::uint64_t size = 0;
    std::int64_t mtime_sec = 0;
    std::string crc32;
    std::string md5;
    std::string sha1;
    std::string sha256;
    std::string title_id;   // empty if unmatched
    std::string matched_by; // crc32 | md5 | sha1 | sha256 | …
};

// Preferred launch binding for a catalog title.
struct LibraryTitleBind {
    std::string title_id;
    std::string preferred_path;
    std::vector<std::string> paths;
};

struct LibraryIndex {
    int schema_version = 1;
    std::string library_root;
    std::vector<LibraryFile> files;
    std::vector<LibraryTitleBind> titles;

    // Fast lookup by absolute path string.
    std::unordered_map<std::string, size_t> by_path;

    void rebuild_path_map();
    const LibraryFile* find_path(const std::string& path) const;
    LibraryFile* find_path_mut(const std::string& path);

    // True when cached entry is still valid for this on-disk file.
    bool is_fresh(const LibraryFile& f, std::uint64_t size, std::int64_t mtime_sec) const;

    // Preferred ROM path for a title (empty if none).
    fs::path preferred_rom(const std::string& title_id) const;
    const LibraryTitleBind* find_title(const std::string& title_id) const;
};

LibraryIndex load_library_index(const fs::path& path);
bool save_library_index(const fs::path& path, const LibraryIndex& index);

// Merge a scan into the index: upsert seen files, drop vanished ones under
// scanned roots, refresh title bindings / preferred paths.
void merge_scan_into_index(LibraryIndex& index, const Catalog& catalog,
                           const ScanResult& scan, const fs::path& library_root);

// Extension preference for launch staging (lower = better).
int rom_path_rank(const std::string& ext);

// Same-stem sibling .cue, else a sheet whose FILE "…" basename matches the dump.
fs::path companion_cue_for_disc_dump(const fs::path& dump_path);

// Count TRACK lines in a .cue sheet (0 if unreadable / not a cue).
int count_cue_tracks(const fs::path& cue_path);

// After a digest hit: enforce rom_identity.track_counts / require_cue.
// Resolves companion .cue for .bin/.iso dumps. True when policy is empty or met.
bool rom_identity_toc_ok(const RomIdentity& id, const fs::path& matched_path);

std::int64_t file_mtime_sec(const fs::path& path);

} // namespace retcomm
