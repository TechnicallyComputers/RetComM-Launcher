#pragma once

#include "retcomm/catalog.hpp"
#include "retcomm/install.hpp"
#include "retcomm/paths.hpp"

#include <filesystem>
#include <functional>
#include <string>

namespace retcomm {

namespace fs = std::filesystem;

// Progress updates for Hub / CLI (phase label + optional 0..1 fraction).
using BuildProgressFn = std::function<void(const std::string& message, float fraction)>;

struct BuildOptions {
    bool force = false; // re-fetch source / rebuild even if pins match
    // When true, always re-run generate even if codegen-cache / prior src matches.
    bool force_generate = false;
    fs::path rom_path;  // required verified ROM (library preferred_rom)
    // Retail BIOS dump for psxrecomp generate (--bios). Empty + use_openbios → OpenBIOS.
    fs::path bios_path;
    bool use_openbios = false;
    // Optional overrides (also accepted via RETCOMM_TOOLCHAIN_DIR / RETCOMM_SDK_DIR /
    // RETCOMM_SOURCE_DIR).
    fs::path toolchain_dir;
    fs::path sdk_dir;
    fs::path source_dir;
    BuildProgressFn on_progress;
};

struct PackEnsureResult {
    bool ok = false;
    fs::path root;
    std::string tag;
    std::string message;
};

struct ToolchainUpdateInfo {
    bool ok = false;
    bool installed = false;
    bool update_available = false;
    std::string pack_id = "cmake-clang-v1";
    std::string current_version;
    std::string latest_tag;
    std::string message;
};

// Resolve or download a release asset pack into toolchains/ or sdks/.
// When force is true, re-download even if a matching cache entry exists.
PackEnsureResult ensure_pack(const Paths& paths, const TitleBuildPack& pack,
                             bool toolchain /* true → toolchains_dir */,
                             const fs::path& override_dir = {},
                             BuildProgressFn on_progress = {}, bool force = false);

// Compare shared-cache cmake-clang-v1 (or pack_id) to GitHub /releases/latest.
ToolchainUpdateInfo check_toolchain_update(
    const Paths& paths, const std::string& pack_id = "cmake-clang-v1",
    const std::string& github = "TechnicallyComputers/retcomm-toolchains");

// Ensure the latest toolchain pack is installed (skips download when already current),
// then refresh PATH/latest.
PackEnsureResult update_toolchain_to_latest(
    const Paths& paths, BuildProgressFn on_progress = {},
    const std::string& pack_id = "cmake-clang-v1",
    const std::string& github = "TechnicallyComputers/retcomm-toolchains");

// Fetch GitHub source zipball into apps/<install>/src/<ref>/ (or override).
PackEnsureResult ensure_source_tree(const Paths& paths, const Title& title,
                                    const fs::path& override_dir = {}, bool force = false,
                                    BuildProgressFn on_progress = {});

// Local generate + cmake + stage into releases/build-<ref>/ + install.json.
InstallResult build_title(const Paths& paths, const Title& title, const BuildOptions& opts = {});

// Prefer build_title when the catalog recipe is enabled; otherwise zip install.
InstallResult install_title_auto(const Paths& paths, const Title& title,
                                 const InstallOptions& install_opts = {},
                                 const BuildOptions& build_opts = {});

// Prefer rebuild when installed via method=build or catalog build is enabled.
InstallResult update_title_auto(const Paths& paths, const Title& title,
                                const InstallOptions& install_opts = {},
                                const BuildOptions& build_opts = {});

} // namespace retcomm
