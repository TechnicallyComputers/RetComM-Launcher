#pragma once

#include "retcomm/data_root.hpp"
#include "retcomm/paths.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace retcomm {

namespace fs = std::filesystem;

// What to do with the data already sitting in the current location.
enum class RootMigrationMode : int {
    Move = 0,     // relocate config+data to the new root
    UseExisting,  // adopt whatever is already at the new root; leave the old tree alone
    StartFresh,   // new root starts empty; old tree left on disk untouched
};

// What a migration would do, computed before the user commits.
struct RootMigrationPlan {
    fs::path from_config;
    fs::path from_data;
    fs::path to_root;
    fs::path to_config;
    fs::path to_data;
    std::uintmax_t existing_bytes = 0; // size of from_config + from_data
    bool from_exists = false;
    bool target_has_data = false;      // <root>/data already holds something
    bool target_writable = false;
    bool same_as_current = false;
    bool to_default = false;           // new_root was empty → back to the OS default
    std::string blocker;               // non-empty → migration must not run
    std::string warning;               // advisory only
};

// An empty `new_root` plans a move back to the OS default location.
RootMigrationPlan plan_root_migration(const Paths& current, const fs::path& new_root);

struct RootMigrationResult {
    bool ok = false;
    std::string message;
    std::vector<std::string> notes; // non-fatal observations worth logging
};

// Perform the migration and persist the root pointer. The pointer is written
// last, so a failure anywhere leaves the app still pointing at the intact old
// tree. Callers should relaunch on success (schedule_retcomm_relaunch).
RootMigrationResult migrate_data_root(const Paths& current, const fs::path& new_root,
                                      RootMigrationMode mode, const fs::path& exe_dir,
                                      bool prefer_exe_marker,
                                      const std::function<void(const std::string&)>& log = {});

// Recursive byte total; symlinks are counted as their own (tiny) size, never
// followed. Returns 0 when the path is missing.
std::uintmax_t directory_size_bytes(const fs::path& root);

// Rewrite directory links whose targets point into `old_data` so they point at
// the matching location under `new_data`. Covers toolchains/<id>/latest and the
// per-title shared-engine links; returns how many were retargeted.
// Exposed for testing and for repair after a manual folder move.
int retarget_data_dir_links(const fs::path& new_data, const fs::path& old_data,
                            std::vector<std::string>* notes = nullptr);

} // namespace retcomm
