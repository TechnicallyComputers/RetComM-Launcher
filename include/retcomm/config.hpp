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

struct NetplayConfig {
    // Default recomp-net-server WebSocket URL (title netplay.lobby_url overrides).
    std::string lobby_url;
    std::string display_name; // Player name for create/join
    bool prefer_ice = false;  // Default transport hint when hosting
};

// Built-in default when config.netplay.lobby_url is empty.
inline constexpr const char* kDefaultNetplayLobbyUrl =
    "ws://netplay.retcomm.net:8765";

// User config (~/.config/retcomm/config.json). RomM/ES-style library layout:
//   <library_root>/<platform_folder>/...
struct AppConfig {
    fs::path library_root;
    fs::path bios_root;  // RomM/ES-DE style BIOS tree (flat + per-system folders)
    fs::path saves_root; // Shared native saves (SRAM / memcard) library tree
    // catalog platform slug -> one or more folder names under library_root
    // e.g. "psx" -> ["ps", "ps1"]
    std::map<std::string, std::vector<std::string>> platform_folders;
    std::vector<std::string> exclude_dirs; // basename matches, e.g. "torrents"
    // When true, sibling / library-folder covers may override remote boxart.
    // Default false: always use RomM or Libretro (per romm.sync_boxart).
    bool prefer_local_boxart = false;
    // Hide catalog titles with no local ROM and no cached RomM match.
    // Installed / partial installs stay visible.
    bool filter_unsupported_titles = false;
    CatalogConfig catalog;
    RommConfig romm;
    NetplayConfig netplay;

    // Title override when set, else cfg.netplay.lobby_url, else built-in default.
    std::string resolve_netplay_lobby_url(const std::string& title_lobby_url = {}) const;

    // Resolve folder names for a catalog platform (defaults + overrides).
    std::vector<std::string> folders_for_platform(const std::string& platform) const;

    // library_root/<folder> for each folder of `platform` that exists (or all if
    // create_missing is unused — we only return existing dirs).
    std::vector<fs::path> platform_roots(const std::string& platform) const;

    // bios_root + bios_root/<platform folders> that exist.
    std::vector<fs::path> bios_roots_for_platform(const std::string& platform) const;

    // Preferred per-platform folder under saves_root (created when create=true).
    // Empty when saves_root is unset.
    fs::path saves_dir_for_platform(const std::string& platform, bool create = false) const;
};

// Built-in RomM / EmulationStation-style defaults (overridable in config).
std::map<std::string, std::vector<std::string>> default_platform_folders();
std::vector<std::string> default_exclude_dirs();

// Suggested first-run layout under ~/Emulation/{roms,bios,saves}.
struct SuggestedLibraryRoots {
    fs::path library_root;
    fs::path bios_root;
    fs::path saves_root;
};
SuggestedLibraryRoots suggested_emulation_roots();

// Prefer an existing folder among `folders`; otherwise create folders.front().
fs::path ensure_platform_dir(const fs::path& root, const std::vector<std::string>& folders);

// Create library/bios/saves platform subfolders for every configured mapping
// (same ensure_platform_dir rule). Skips empty roots.
bool ensure_configured_platform_dirs(const AppConfig& cfg, std::string* error = nullptr);

AppConfig load_app_config(const fs::path& config_path);
// Merge defaults under any loaded overrides.
AppConfig normalize_config(AppConfig cfg);
// Write config.json (creates parent dirs). Returns false on I/O failure.
bool save_app_config(const fs::path& config_path, const AppConfig& cfg, std::string* error = nullptr);

} // namespace retcomm
