#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace retcomm {

namespace fs = std::filesystem;

// A ROM or disc image lives in the player's emulation library and is never
// duplicated into an app install folder. psxrecomp's prepare_disc still needs
// the tracks while it extracts the boot EXE and builds a working cue, and its
// default answer is to copy the whole Redump set into
// apps/<title>/src/current/disc — 700 MB per PSX title, 40+ GB across a full
// catalog (issue #8). So the launcher seeds that work tree with links to the
// library set before generate runs: prepare_disc then finds every destination
// already resolving to its source and copies nothing. Where the host refuses
// links, the copy still happens and prune_copied_disc_media reclaims it after
// the build — the library stays the one source of truth either way.

// [prepare_disc] of a title's game.toml.
struct DiscWorkTreeSpec {
    fs::path dir;          // <src_root>/<out_dir>
    std::string cue_name;  // working cue prepare_disc writes into dir
    std::string bin_name;  // data track name prepare_disc expects in dir
    std::string boot_exe;  // extracted boot EXE (kept; it is not a disc image)
};

// Parse [prepare_disc] from <src_root>/<config_rel> ("game.toml" when empty).
// False when the file is unreadable or carries no [prepare_disc] section — i.e.
// the title is not a disc title and has no work tree to police.
bool read_disc_work_tree_spec(const fs::path& src_root, const std::string& config_rel,
                              DiscWorkTreeSpec* out);

// FILE "…" entries of a .cue, resolved against the sheet's own directory.
std::vector<fs::path> cue_track_files(const fs::path& cue_path);

struct DiscStageResult {
    bool ok = false;    // work tree now points at the library; nothing left to copy
    int linked = 0;
    std::string message;
};

// Link `library_cue`'s tracks into the work tree under the names prepare_disc
// expects. ok=false is never fatal: it just means the build falls back to the
// copy, which prune_copied_disc_media removes afterwards.
DiscStageResult stage_disc_tracks_from_library(const fs::path& src_root,
                                               const std::string& config_rel,
                                               const fs::path& library_cue);

struct DiscPruneResult {
    std::size_t removed = 0;
    std::uint64_t bytes_freed = 0;
    std::vector<std::string> messages;
};

// Remove disc images that a work tree owns outright — a regular file with one
// hard link, i.e. bytes that exist nowhere else but here. Symlinks and files
// that share their inode with the library cost nothing and are kept, as is the
// extracted boot EXE that lets the next generate skip prepare_disc entirely.
DiscPruneResult prune_copied_disc_media(const fs::path& src_root,
                                        const std::string& config_rel = {});

// True for the extensions the launcher refuses to duplicate into an install
// folder (disc images and cartridge dumps alike).
bool is_rom_or_disc_media_ext(const std::string& lower_ext);

} // namespace retcomm
