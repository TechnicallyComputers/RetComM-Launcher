#pragma once

#include "retcomm/catalog.hpp"
#include "retcomm/config.hpp"
#include "retcomm/paths.hpp"
#include "retcomm/romm_fetch.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace retcomm {

namespace fs = std::filesystem;

struct RommSaveSyncResult {
    bool ok = false;
    std::string message;
    int rom_id = 0;
    int uploaded = 0;
    int downloaded = 0;
    int skipped = 0;
    int conflicts = 0; // remote kept / local kept after newer-wins
};

// Bidirectional sync of native game saves (SRAM / memcard — not emulator savestates)
// between the installed recomp tree and RomM /api/saves for the matched ROM.
// Newer file wins (local mtime vs remote updated_at); identical MD5 skips.
RommSaveSyncResult sync_saves_with_romm(const Paths& paths, const AppConfig& cfg,
                                        const Title& title, RommProgressFn on_progress = {});

// Bidirectional sync of savestates with RomM /api/states.
// Same newer-wins rules. Recomp and emulator savestates are often incompatible —
// callers should warn the user in the UI.
RommSaveSyncResult sync_states_with_romm(const Paths& paths, const AppConfig& cfg,
                                         const Title& title, RommProgressFn on_progress = {});

// One managed native save under an install's saves/ tree (RomM sync target).
// `id` is release-relative posix (e.g. "saves/Foo.sav"); `label` is filename only.
struct ManagedSave {
    std::string id;
    std::string label;
    fs::path host_path;
};

// Enumerate native battery / memcard files for the title (install saves/ only).
// Empty when the title is not installed or has no matching files yet.
std::vector<ManagedSave> list_managed_saves(const Paths& paths, const Title& title);

// Resolve a managed save id (or bare filename) to a host path under the install.
// Empty when missing / not installed.
fs::path resolve_managed_save(const Paths& paths, const Title& title, const std::string& save_id);

struct RecompSaveBindResult {
    bool ok = false;
    std::string message;
    fs::path saves_dir;
    fs::path card1;
    fs::path card2;
    fs::path active_save; // preferred / activated cart save when set
};

// Point the installed recomp at the shared saves/ folder used by RomM sync:
// - ensures <install>/current/saves exists
// - upserts settings.toml [memcard] dir/card1/card2 (psxrecomp)
// - for cart SRAM, copies a lone synced battery file onto saves/save.srm|save.sav
//   when those defaults are missing (skipped when preferred_save is set)
// - preferred_save: managed id or host path; disc → card1; cart → default-slot
//   symlink/copy + active_save for --save-path
// use_wine: write Wine guest paths (Z:/…) into settings.toml when applicable.
RecompSaveBindResult bind_recomp_save_paths(const Paths& paths, const Title& title,
                                            bool use_wine = false,
                                            const fs::path& preferred_save = {});

} // namespace retcomm
