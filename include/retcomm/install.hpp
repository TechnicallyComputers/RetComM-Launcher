#pragma once

#include "retcomm/catalog.hpp"
#include "retcomm/config.hpp"
#include "retcomm/http.hpp"
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
    // "zip" (prebuilt release) or "build" (local generate + cmake). Empty → zip.
    std::string method;
    std::string source_ref;     // build.source.ref when method=build
    std::string sdk_tag;        // snesrecomp tools pack tag
    std::string toolchain_tag;  // toolchain pack tag
    // Local build BIOS backend: "openbios" | "retail". Empty on zip installs /
    // legacy records (hub may infer from codegen stamp / generated C).
    std::string bios_source;
};

struct InstallPlan {
    const Title* title = nullptr;
    fs::path install_root; // apps/<install_dir_name>
    fs::path current_link; // install_root/current
    fs::path binary_path;  // resolved launch binary under current/
    bool installed = false;
    // True when apps/<install_dir_name> exists with leftover install/build artifacts
    // but no launch binary (failed/partial install — hub offers Open Folder / cleanup).
    // False when the root is absent, or only holds preserved/ from keep-saves uninstall.
    bool install_dir_present = false;
    // True when uninstall kept saves/config under install_root/preserved/.
    bool has_preserved_state = false;
    std::string expected_binary; // catalog launch name looked for when missing
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
    // When the title supports local build, force the prebuilt zip path instead.
    bool prefer_prebuilt = false;
    // Optional latest tag from a prior check (hub row / ReleaseTagCache). Used when
    // the live GitHub API is rate-limited so Update can still apply a prefetched zip.
    std::string hint_latest_tag;
    // Override Paths::apps_dir for this operation (multi-root installs).
    fs::path apps_dir;
};

struct InstallResult {
    bool ok = false;
    bool skipped = false; // already up to date
    InstallPlan plan;
    std::string message;
};

InstallRecord load_install_record(const fs::path& install_root);
bool save_install_record(const fs::path& install_root, const InstallRecord& rec);

// Tag to compare against GitHub latest release. Build installs pin install.json
// tag as "build-<ref>"; prefer source_ref (or strip the build- prefix).
std::string install_release_compare_tag(const InstallRecord& rec);
std::string install_release_compare_tag(const InstallPlan& plan);

// True when a local build was generated with OpenBIOS only (no retail SCPH
// backend). Uses install.json bios_source, else codegen stamp / generated C.
bool install_built_with_openbios(const InstallPlan& plan);

InstallPlan inspect_install(const Paths& paths, const Title& title);
// Same as inspect_install but with an explicit apps/ root.
InstallPlan inspect_install(const Paths& paths, const Title& title, const fs::path& apps_dir);
// Prefer an installed (or leftover) tree across configured + builtin apps roots.
InstallPlan inspect_install_any(const Paths& paths, const AppConfig& cfg, const Title& title);

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

struct MoveInstallResult {
    bool ok = false;
    bool skipped = false; // already at dest_apps_dir
    fs::path from_root;
    fs::path to_root;
    std::string message;
};

// Move apps/<install_dir_name> (full install, partial leftovers, or preserved/)
// from its current root into dest_apps_dir/<install_dir_name>. Uses rename when
// possible; falls back to recursive copy + delete across devices.
MoveInstallResult move_title_install(const Paths& paths, const AppConfig& cfg,
                                     const Title& title, const fs::path& dest_apps_dir);

// Path-based uninstall (orphans / renamed install dirs). When save_globs is
// empty, uses the default saves/states/config patterns.
UninstallResult uninstall_install_root(const Paths& paths, const fs::path& install_root,
                                       const UninstallOptions& opts = {},
                                       const std::string& display_id = {},
                                       const std::vector<std::string>& save_globs = {});

// Local apps/<dir> with no matching catalog title.install_dir_name.
struct OrphanInstall {
    fs::path install_root;
    std::string dir_name;
    std::string title_id; // from install.json when present, else dir_name
    std::string tag;
    bool has_install_record = false;
    bool has_preserved_only = false;
};

std::vector<OrphanInstall> list_orphan_installs(const Paths& paths, const Catalog& catalog);

struct OrphanCleanupOptions {
    bool keep_saves = true;
    bool dry_run = false;
    // Also drop library/BIOS/RomM/app_state/boxart keys for ids not in catalog.
    bool prune_indexes = true;
};

struct OrphanCleanupResult {
    bool ok = false;
    int removed = 0;
    int failed = 0;
    int pruned_ids = 0;
    std::vector<std::string> messages;
    std::string message;
};

// Uninstall orphan install dirs and prune stale per-title index/state entries.
OrphanCleanupResult cleanup_removed_catalog_titles(const Paths& paths, const Catalog& catalog,
                                                   const OrphanCleanupOptions& opts = {});

// Stash saves + user config from existing release trees into
// apps/<install>/preserved/ before a wipe/rebuild/update.
// Restores into a newly staged release_dir (skip_existing).
void stash_user_state_for_update(const Paths& paths, const Title& title,
                                 std::string* note = nullptr);
void restore_user_state(const fs::path& install_root, const fs::path& release_dir,
                        std::string* note = nullptr);

// apps/<title>/current → releases/<tag> (empty if unset / missing).
fs::path resolve_current_release_dir(const fs::path& install_root);
// After a successful install/update: delete releases/<other-tag> dirs, keep only keep_tag.
// Returns number of directories removed.
size_t prune_old_release_dirs(const fs::path& install_root, const std::string& keep_tag,
                              std::string* note = nullptr);

struct OldReleaseCleanupResult {
    bool ok = true;
    int titles_scanned = 0;
    int titles_cleaned = 0;
    int dirs_removed = 0;
    std::vector<std::string> messages;
    std::string message;
};

// For each catalog install: stash/restore user state into the current release,
// then delete sibling releases/<old-tag>/ folders left behind by updates.
OldReleaseCleanupResult cleanup_old_release_dirs(const Paths& paths, const Catalog& catalog);

// Fetch latest release tag for owner/repo (empty on failure).
std::string fetch_latest_release_tag(const std::string& github_slug, std::string* error = nullptr,
                                     bool allow_prerelease = false);

// Extract zip/tar/7z into dest (creates dest). Used by install + self-update.
bool extract_archive_to(const fs::path& archive, const fs::path& dest, std::string* error = nullptr);

// Durable release zip cache: data_dir/cache/releases/<owner_repo>/<tag>/<asset>.
fs::path release_download_cache_path(const Paths& paths, const std::string& github_slug,
                                    const std::string& tag, const std::string& asset_name);

struct PrefetchResult {
    bool ok = false;
    bool skipped = false; // already cached, or already on latest tag
    fs::path cached_path;
    std::string message;
};

// Resolve the release asset and download into the cache (resume / skip if complete).
// Does not extract or swap current/. Used to warm the cache before Update.
PrefetchResult prefetch_title_release(const Paths& paths, const Title& title,
                                      const InstallOptions& opts = {});

// Same durable-cache resolution as install/prefetch (live GitHub, else hint/tag
// cache + on-disk zip). Used by local generate+cmake source staging.
struct ResolvedReleaseZip {
    bool ok = false;
    bool from_cache = false;
    fs::path zip_path;
    std::string tag;
    std::string asset_name;
    std::string message;
};
ResolvedReleaseZip resolve_title_release_zip(const Paths& paths, const Title& title,
                                             const InstallOptions& opts = {},
                                             HttpProgressFn on_progress = {});

// Wine helpers for Linux / macOS fallback installs.
bool host_supports_wine();
std::string resolve_wine_binary(std::string* error = nullptr);

} // namespace retcomm
