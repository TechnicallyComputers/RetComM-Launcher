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
// Raw subprocess stdout/stderr lines (generate / cmake configure / cmake build).
using BuildOutputFn = std::function<void(const std::string& line)>;

struct BuildOptions {
    bool force = false; // re-fetch source even if marker tag matches
    // When true, always re-run generate even if codegen-cache / prior src matches.
    // Also wipes the per-title cmake build/ dir (clean rebuild).
    bool force_generate = false;
    fs::path rom_path;  // required verified ROM (library preferred_rom)
    // Retail BIOS dump for psxrecomp generate (--bios). Empty + use_openbios → OpenBIOS.
    // When bios_path is set, generate still regenerates OpenBIOS as well (CLI).
    fs::path bios_path;
    bool use_openbios = false;
    // Pass --force-bios so SCPH1001 + OpenBIOS C are regenerated (not skipped).
    bool force_bios = false;
    // Optional overrides (also accepted via RETCOMM_TOOLCHAIN_DIR / RETCOMM_SDK_DIR /
    // RETCOMM_SOURCE_DIR / RETCOMM_ENGINES_DIR).
    fs::path toolchain_dir;
    fs::path sdk_dir;
    fs::path source_dir;
    fs::path engines_dir; // shared engine cache root (default: data_dir/engines)
    // Hub row / ReleaseTagCache hint — used when GitHub /releases/latest is 403.
    std::string hint_latest_tag;
    // Override Paths::apps_dir for this build (multi-root installs).
    fs::path apps_dir;
    BuildProgressFn on_progress;
    // When set, CLI lines are streamed live and failure messages omit the full dump
    // (already delivered via this callback).
    BuildOutputFn on_output;
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

// Fetch release/zipball source into apps/<install>/src/current/ (or override).
// Package updates overlay in place and preserve cmake build/, local generated/,
// and disc work dirs (bpe/motk/disc) for incremental ninja.
// Vendored engine/UI trees (psxrecomp/, recomp-ui/, …) are harvested into
// data_dir/engines/<name>/<pin>/ and replaced with symlink/junction so titles
// that pin the same commit share one copy. Override cache root with
// RETCOMM_ENGINES_DIR / engines_dir. Skipped for RETCOMM_SOURCE_DIR overrides.
// hint_latest_tag: same offline fallback as zip install when the live API fails.
PackEnsureResult ensure_source_tree(const Paths& paths, const Title& title,
                                    const fs::path& override_dir = {}, bool force = false,
                                    BuildProgressFn on_progress = {},
                                    const std::string& hint_latest_tag = {},
                                    const fs::path& engines_dir = {});

// Local generate + cmake + stage into releases/build-<ref>/ + install.json.
InstallResult build_title(const Paths& paths, const Title& title, const BuildOptions& opts = {});

// Setup-host / one-zip titles (build.enabled): release zip is SOURCE → generate+cmake.
// Dual-mode titles whose release zip already contains the catalog launch binary
// (e.g. Tomba) take the prebuilt extract path. prefer_prebuilt forces zip extract.
InstallResult install_title_auto(const Paths& paths, const Title& title,
                                 const InstallOptions& install_opts = {},
                                 const BuildOptions& build_opts = {});

// Setup-host titles: overlay latest release zip onto src/current and cmake-build
// incrementally; codegen-cache skips regenerate when ROM/BIOS/emitters match.
// Dual-mode playable zips update via prebuilt extract. Generate & Rebuild sets
// force_generate.
InstallResult update_title_auto(const Paths& paths, const Title& title,
                                const InstallOptions& install_opts = {},
                                const BuildOptions& build_opts = {});

} // namespace retcomm
