#pragma once

#include "retcomm/catalog.hpp"
#include "retcomm/paths.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace retcomm {

namespace fs = std::filesystem;

// Persisted under apps/<install_dir_name>/install.json
struct InstallRecord {
    int schema_version = 1;
    std::string title_id;
    std::string github;
    std::string tag;
    std::string asset_name;
    std::string binary; // path relative to current/
    std::string host_os;   // machine that ran install (linux|windows|macos)
    std::string target_os; // asset OS selected (usually host; "windows" for Wine)
    std::string runtime;   // "native" | "wine"
    std::string installed_at;
    std::string release_url;
};

struct InstallPlan {
    const Title* title = nullptr;
    fs::path install_root; // apps/<install_dir_name>
    fs::path current_link; // install_root/current
    fs::path binary_path;  // resolved launch binary under current/
    bool installed = false;
    std::optional<InstallRecord> record;
    std::string installed_tag;
    std::string latest_tag; // filled when an update check ran
    bool update_available = false;
    std::string message;
};

struct InstallOptions {
    bool force = false;           // reinstall even if same tag
    bool check_latest = true;     // query GitHub for latest release
    bool allow_prerelease = false;
    // On Linux/macOS: download the Windows asset and record runtime=wine.
    bool use_wine = false;
};

struct InstallResult {
    bool ok = false;
    bool skipped = false; // already up to date
    InstallPlan plan;
    std::string message;
};

InstallRecord load_install_record(const fs::path& install_root);
bool save_install_record(const fs::path& install_root, const InstallRecord& rec);

InstallPlan inspect_install(const Paths& paths, const Title& title);

// Plan + optional remote latest tag (when check_latest).
InstallPlan plan_install(const Paths& paths, const Title& title,
                         const InstallOptions& opts = {});

// Download latest (or current) host-OS asset, unwrap nested archives, link current/.
InstallResult install_title(const Paths& paths, const Title& title,
                            const InstallOptions& opts = {});

// Same as install when an update exists (or force).
InstallResult update_title(const Paths& paths, const Title& title,
                           const InstallOptions& opts = {});

struct UninstallOptions {
    bool keep_saves = true; // preserve memcards / SRAM / savestates
    bool dry_run = false;
};

struct UninstallResult {
    bool ok = false;
    bool skipped = false;
    InstallPlan plan;
    std::vector<std::string> preserved_paths; // relative paths kept under preserved/
    std::string message;
};

// Remove apps/<install_dir_name> build tree. With keep_saves, stash matching
// save/savestate files under install_root/preserved/ for the next install.
UninstallResult uninstall_title(const Paths& paths, const Title& title,
                                const UninstallOptions& opts = {});

// Fetch latest release tag for owner/repo (empty on failure).
std::string fetch_latest_release_tag(const std::string& github_slug, std::string* error = nullptr,
                                     bool allow_prerelease = false);

// Wine helpers for Linux / macOS fallback installs.
bool host_supports_wine();
std::string resolve_wine_binary(std::string* error = nullptr);

} // namespace retcomm
