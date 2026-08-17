#pragma once

// texture_packs.hpp — per-title HD texture pack store.
//
// Packs are Beetle PSX HW format: a flat directory of <texhash>-<palhash>.png,
// where the hashes are CRC32s of the VRAM upload rect and the CLUT row. psxrecomp
// reads that format directly (see psxrecomp runtime/include/tex_pack.h), so packs
// authored for RetroArch work unmodified.
//
// The store lives under the launcher's data dir, deliberately NOT inside the
// install's releases/<tag>/ directory: that tree is replaced wholesale on update
// and only saves and a fixed list of config files survive it. Layout:
//
//     <data_dir>/texturepacks/<title_id>/<pack_id>/
//         pack.json        metadata (synthesised on import when absent)
//         *.png            the replacement textures, flat
//         coverage.json    written by the game at exit; how much got used
//
// Exactly one pack per title is active at a time; the selection lives in
// AppState::active_texture_pack and is handed to the game through settings.toml.

#include "retcomm/paths.hpp"

#include <string>
#include <vector>

namespace retcomm {

struct TexturePack {
    std::string id;           // directory name, and the AppState selection key
    std::string name;         // display name (falls back to id)
    std::string author;
    std::string version;
    std::string description;
    std::string source_url;
    fs::path    dir;

    int         texture_count = 0;   // files matching <hex>-<hex>.png
    long long   bytes = 0;

    // From coverage.json, present once the title has been played with this pack.
    bool        has_coverage = false;
    int         coverage_entries = 0;   // entries the game indexed
    int         coverage_matched = 0;   // entries a draw actually asked for
    long long   coverage_seconds = 0;   // length of that session
};

// <data_dir>/texturepacks/<title_id>. Not created by this call.
fs::path texture_packs_dir(const Paths& paths, const std::string& title_id);

// Every pack installed for a title, sorted by display name. Missing directory
// yields an empty list rather than an error — no packs is the normal state.
std::vector<TexturePack> scan_texture_packs(const Paths& paths,
                                            const std::string& title_id);

// Import a pack from a .zip or a directory. Handles the common shapes packs
// ship in: a bare folder of PNGs, a single wrapper directory, and the
// "<name>-texture-replacements" wrapper that Beetle-format packs use. Writes a
// synthesised pack.json when the source carries none. On success `out_pack_id`
// receives the installed id.
bool install_texture_pack(const Paths& paths, const std::string& title_id,
                          const fs::path& source, std::string* out_pack_id,
                          std::string* error);

bool remove_texture_pack(const Paths& paths, const std::string& title_id,
                         const std::string& pack_id, std::string* error);

// True when `name` is <hex>-<hex>.png — the only files that count as textures.
bool is_texture_pack_file(const std::string& name);

} // namespace retcomm
