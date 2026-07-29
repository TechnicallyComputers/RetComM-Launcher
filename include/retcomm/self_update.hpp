#pragma once

#include "retcomm/paths.hpp"

#include <filesystem>
#include <string>

namespace retcomm {

namespace fs = std::filesystem;

// Compile-time version string (CMake RETCOMM_VERSION), e.g. "0.1.0".
std::string retcomm_app_version();

// GitHub owner/repo used for launcher self-update.
std::string retcomm_github_slug();

// Tag recorded from the last successful self-update (launcher.json), else
// retcomm_app_version().
std::string retcomm_installed_tag(const Paths& paths);

struct SelfUpdateOptions {
    bool force = false;           // re-apply even when tags match
    bool allow_prerelease = true; // launcher is early; prefer newest non-draft
};

struct SelfUpdateResult {
    bool ok = false;
    bool skipped = false;            // already on latest
    bool restart_scheduled = false;  // apply script launched; caller should exit
    std::string current_tag;
    std::string latest_tag;
    std::string asset_name;
    std::string message;
};

// Check TechnicallyComputers/RetComM-Launcher (or RETCOMM_GITHUB_SLUG) for a
// newer release, download the host-OS asset, and schedule an in-place replace of
// the running binaries after this process exits.
SelfUpdateResult self_update_retcomm(const Paths& paths, const SelfUpdateOptions& opts = {});

} // namespace retcomm
