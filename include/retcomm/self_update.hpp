#pragma once

#include "retcomm/paths.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace retcomm {

namespace fs = std::filesystem;

// Compile-time version string (CMake RETCOMM_VERSION), e.g. "0.1.0".
// This is the source of truth for the running binary.
std::string retcomm_app_version();

// GitHub owner/repo used for launcher self-update.
std::string retcomm_github_slug();

// Display / compare version for the running process (= retcomm_app_version()).
// launcher.json is metadata only and is not used for the UI version string.
std::string retcomm_installed_tag(const Paths& paths);

// First-class install channels that support Menu → Update RetComM.
enum class RetcommInstallChannel {
    Unsupported = 0, // dev build, loose binary, unknown layout
    LinuxAppImage,
    MacosApp,
    WindowsInstaller,
    WindowsPortable, // still updatable; Windows primary channel is Installer
};

struct RetcommInstallInfo {
    RetcommInstallChannel channel = RetcommInstallChannel::Unsupported;
    bool self_update_supported = false;
    std::string channel_id; // appimage | macos-app | windows-installer | windows-portable | dev
    std::string hint;       // why update is disabled / how to install
    fs::path path;          // AppImage file, .app bundle, or install directory
};

// Detect how this process was launched (AppImage / macOS .app / Windows installer|portable).
RetcommInstallInfo retcomm_install_info();

struct SelfUpdateOptions {
    bool force = false;           // re-apply even when tags match
    bool allow_prerelease = true; // launcher is early; prefer newest non-draft
};

struct SelfUpdateResult {
    bool ok = false;
    bool skipped = false;           // already on latest
    bool restart_scheduled = false; // apply script launched; caller should exit
    std::string current_tag;
    std::string latest_tag;
    std::string asset_name;
    std::string message;
};

// Check TechnicallyComputers/RetComM-Launcher (or RETCOMM_GITHUB_SLUG) for a
// newer release, download the host-OS asset for this install channel, and
// schedule an in-place replace after this process exits.
//
// Supported: Linux AppImage, macOS .app (via DMG), Windows setup.exe / portable.exe.
// Unsupported layouts (dev binaries, loose copies) fail with a clear hint.
SelfUpdateResult self_update_retcomm(const Paths& paths, const SelfUpdateOptions& opts = {});

// Query-only: compare the running launcher version to the latest GitHub release
// (no download). Unsupported install channels report update_available=false.
struct SelfUpdateCheckInfo {
    bool ok = false;
    bool update_available = false;
    bool supported = false;
    std::string current_tag;
    std::string latest_tag;
    std::string message;
};
SelfUpdateCheckInfo check_retcomm_update(const Paths& paths,
                                         const SelfUpdateOptions& opts = {});

// Relaunch the current hub/AppImage after this process exits (hard-reset, etc.).
bool schedule_retcomm_relaunch(std::string* error = nullptr);

// Full removal. Covers every RetComM tree this build can reach: the active
// config/data roots (custom root included), the OS-default AppData /
// .local/share pair an earlier root move may have left behind, both data-root
// markers, and the app itself when the install channel supports it.
//
// plan_retcomm_uninstall() only describes the work, so the confirm dialog can
// name the exact paths; nothing is touched until schedule_retcomm_uninstall()
// writes a detached script that waits for this process to exit, deletes
// everything, and - unlike the update scripts - does not relaunch.
struct RetcommUninstallPlan {
    std::vector<fs::path> data_paths; // roots + marker files, deleted recursively
    fs::path app_path;                // exe / .app bundle / install dir
    fs::path uninstaller;             // Windows: unins000.exe when the installer left one
    bool removes_app = false;         // false for dev / loose builds: data only
    std::string channel_id;           // retcomm_install_info().channel_id
    std::string app_note;             // what happens to the binary, for the UI
};

RetcommUninstallPlan plan_retcomm_uninstall(const Paths& paths, const fs::path& exe_dir);
bool schedule_retcomm_uninstall(const RetcommUninstallPlan& plan, std::string* error = nullptr);

} // namespace retcomm
