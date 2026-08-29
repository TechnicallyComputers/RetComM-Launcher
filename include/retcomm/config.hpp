#pragma once

#include "retcomm/paths.hpp"
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

// One game-install location (holds <install_dir_name>/ trees like default apps/).
struct InstallRootEntry {
    std::string label;
    fs::path path;
};

// User config (~/.config/retcomm/config.json). RomM/ES-style library layout:
//   <library_root>/<platform_folder>/...
struct AppConfig {
    fs::path library_root;
    fs::path bios_root;  // RomM/ES-DE style BIOS tree (flat + per-system folders)
    fs::path saves_root; // Native saves library (SRAM / memcard), per-title under platform
    // Optional extra (or replacement) game install roots. Empty → only the
    // RetComM data_dir/apps default. When 2+ effective roots exist, Install asks.
    std::vector<InstallRootEntry> install_roots;
    // Preferred root for new installs (must match an effective root path).
    fs::path default_install_root;
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
    // On hub launch, run Check Updates (catalog → launcher → games → toolchain).
    bool check_updates_on_startup = true;
    // After a catalog update introduces titles that have no ROM binding yet,
    // re-bind them from cached hashes and then scan the affected platforms, so
    // newly catalogued games appear without a manual Scan Files.
    bool auto_scan_after_catalog_update = true;
    // When Play is pressed, query GitHub for that title and prompt if newer.
    bool check_updates_before_launch = true;
    // After a successful local generate+cmake, delete apps/<title>/src/current/build/
    // to save disk. Off by default so package updates stay incremental.
    bool auto_clean_build_dirs = false;
    // Shared cache housekeeping (toolchains / sdks / engines / release zips /
    // idle cmake build dirs). Runs after successful builds and via Hub / CLI.
    bool auto_gc_caches = true;
    int keep_toolchain_versions = 2;   // newest N per pack id (always keeps `latest`)
    int keep_sdk_versions = 3;
    int keep_orphan_engine_pins = 1;   // newest unreferenced pins to keep per engine name
    int keep_release_zips_per_repo = 1;
    int idle_build_keep_days = 14;     // 0 = never auto-prune idle src/.../build/
    int ccache_max_gb = 5;             // 0 = don't manage CCACHE_DIR / MAXSIZE
    CatalogConfig catalog;
    RommConfig romm;
    NetplayConfig netplay;
    // Optional GitHub PAT for api.github.com when asset lists / downloads need it.
    // Update *checks* prefer github.com redirects and do not require a token.
    // Env GITHUB_TOKEN / GH_TOKEN still overrides when set.
    std::string github_token;

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
    // Title quarantine lives one level deeper: <this>/<title_id>/.
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

// data_dir/apps (always the built-in default install location).
fs::path builtin_apps_dir(const Paths& paths);
// Roots offered for new Install (config list, or single Default when unset).
std::vector<InstallRootEntry> effective_install_roots(const AppConfig& cfg, const Paths& paths);
// Roots to scan for existing installs (effective ∪ builtin apps/).
std::vector<InstallRootEntry> scan_install_roots(const AppConfig& cfg, const Paths& paths);
// Preferred new-install root (default_install_root, else first effective).
fs::path resolve_default_install_root(const AppConfig& cfg, const Paths& paths);
// True when a and b refer to the same directory (equivalent / weakly_canonical).
bool same_install_root_path(const fs::path& a, const fs::path& b);
// Index of apps_dir in roots, or -1 when not found (relaxed path compare).
int find_install_root_index(const std::vector<InstallRootEntry>& roots, const fs::path& apps_dir);
// Copy of paths with apps_dir overridden (empty override → unchanged).
Paths with_apps_dir(Paths paths, const fs::path& apps_dir);

} // namespace retcomm
