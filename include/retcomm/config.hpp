#pragma once

#include "retcomm/romm.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace retcomm {

namespace fs = std::filesystem;

struct CatalogConfig {
    std::string url;         // catalog.zip download URL (empty = built-in default)
    std::string github_repo; // owner/repo for display (empty = built-in default)
    bool auto_update = true; // check latest release on startup; download if tag/date changed
};

// User config (~/.config/retcomm/config.json). RomM/ES-style library layout:
//   <library_root>/<platform_folder>/...
struct AppConfig {
    fs::path library_root;
    fs::path bios_root; // RomM/ES-DE style BIOS tree (flat + per-system folders)
    // catalog platform slug -> one or more folder names under library_root
    // e.g. "psx" -> ["ps", "ps1"]
    std::map<std::string, std::vector<std::string>> platform_folders;
    std::vector<std::string> exclude_dirs; // basename matches, e.g. "torrents"
    // When true, sibling / library-folder covers may override remote boxart.
    // Default false: always use RomM or Libretro (per romm.sync_boxart).
    bool prefer_local_boxart = false;
    CatalogConfig catalog;
    RommConfig romm;

    // Resolve folder names for a catalog platform (defaults + overrides).
    std::vector<std::string> folders_for_platform(const std::string& platform) const;

    // library_root/<folder> for each folder of `platform` that exists (or all if
    // create_missing is unused — we only return existing dirs).
    std::vector<fs::path> platform_roots(const std::string& platform) const;

    // bios_root + bios_root/<platform folders> that exist.
    std::vector<fs::path> bios_roots_for_platform(const std::string& platform) const;
};

// Built-in RomM / EmulationStation-style defaults (overridable in config).
std::map<std::string, std::vector<std::string>> default_platform_folders();
std::vector<std::string> default_exclude_dirs();

AppConfig load_app_config(const fs::path& config_path);
// Merge defaults under any loaded overrides.
AppConfig normalize_config(AppConfig cfg);
// Write config.json (creates parent dirs). Returns false on I/O failure.
bool save_app_config(const fs::path& config_path, const AppConfig& cfg, std::string* error = nullptr);

} // namespace retcomm
