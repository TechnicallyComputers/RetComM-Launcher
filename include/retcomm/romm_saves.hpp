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
// with RomM /api/saves. Promotes install→library first when saves_root is set,
// then syncs the library only. Otherwise uses install saves/. Newer wins.
RommSaveSyncResult sync_saves_with_romm(const Paths& paths, const AppConfig& cfg,
                                        const Title& title, RommProgressFn on_progress = {});

// Bidirectional sync of savestates with RomM /api/states.
// Same newer-wins rules. Recomp and emulator savestates are often incompatible —
// callers should warn the user in the UI. Always uses the install tree.
RommSaveSyncResult sync_states_with_romm(const Paths& paths, const AppConfig& cfg,
                                         const Title& title, RommProgressFn on_progress = {});

// One managed native save (RomM sync / hub picker target).
// `id` is "saves/<file>" (stable for state.json); `label` is filename only.
struct ManagedSave {
    std::string id;
    std::string label;
    fs::path host_path;
};

struct CanonicalSaveResult {
    bool ok = false;
    bool created = false; // minted a new empty file
    int promoted = 0;     // install/preserved → library copies
    std::string message;
    ManagedSave save; // primary (cart battery or memcard 1)
};

// Enumerate native battery / memcard files for the title (library saves_root when
// set, else install saves/). Empty when nothing is available yet.
std::vector<ManagedSave> list_managed_saves(const Paths& paths, const AppConfig& cfg,
                                            const Title& title);

// Resolve a managed save id (or bare filename) to a host path.
fs::path resolve_managed_save(const Paths& paths, const AppConfig& cfg, const Title& title,
                              const std::string& save_id);

// Copy install/preserved native saves into saves_root/<platform>/ when configured.
// Renames generic slot files (save.*, card1.*) to a ROM/title stem. Returns count.
int promote_install_saves_to_library(const Paths& paths, const AppConfig& cfg,
                                     const Title& title, const fs::path& rom_hint = {});

// Ensure a canonical library (or install) save exists: promote → reuse → mint.
// Persists preferred_save in state.json when minting or when none was set.
// rom_hint: preferred ROM path used for ES-DE-friendly naming.
CanonicalSaveResult ensure_canonical_save(const Paths& paths, const AppConfig& cfg,
                                          const Title& title, const fs::path& rom_hint = {},
                                          bool mint_if_missing = true);

// Mint a new empty managed save (unique stem), set it preferred, return it.
CanonicalSaveResult create_managed_save(const Paths& paths, const AppConfig& cfg,
                                        const Title& title, const fs::path& rom_hint = {});

struct RecompSaveBindResult {
    bool ok = false;
    std::string message;
    fs::path saves_dir;
    fs::path card1;
    fs::path card2;
    fs::path active_save; // preferred / activated cart save when set
};

// Point the installed recomp at the shared saves folder (library or install):
// - ensures the library/install saves dir exists
// - upserts settings.toml [memcard] dir/card1/card2 (psxrecomp)
// - for cart SRAM, links install saves/save.* → preferred library file when needed
// - preferred_save: managed id or host path; cart → --save-path; disc → card1
// - preferred_save_card2: disc memcard slot 2 path
// - card2_blank: force empty card2.mcd (overrides auto-pick when path empty)
// use_wine: write Wine guest paths (Z:/…) into settings.toml when applicable.
RecompSaveBindResult bind_recomp_save_paths(const Paths& paths, const AppConfig& cfg,
                                            const Title& title, bool use_wine = false,
                                            const fs::path& preferred_save = {},
                                            const fs::path& preferred_save_card2 = {},
                                            bool card2_blank = false);

// True for disc platforms or titles with memcard_glob (dual-slot UI).
bool title_uses_memcards(const Title& title);

// state.json sentinel for an explicit blank memcard 2 selection.
inline constexpr const char* kBlankMemcardId = "__blank__";

} // namespace retcomm
