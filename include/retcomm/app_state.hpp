#pragma once

#include "retcomm/paths.hpp"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace retcomm {

namespace fs = std::filesystem;

// Sentinel for AppState::preferred_bios — use MIT OpenBIOS (no retail dump).
inline constexpr const char* kOpenBiosChoice = "__openbios__";

// Hub-wide prefs in data_dir/state.json (preferred save slot per title, etc.).
struct AppState {
    int schema_version = 1;
    // title_id → managed save id, e.g. "saves/Pokemon - Emerald Version (U).sav"
    // Cart: the battery file. Disc: memcard slot 1 (card1).
    std::unordered_map<std::string, std::string> preferred_save;
    // Disc / dual-memcard titles: memcard slot 2 (card2). Empty = blank card2.mcd.
    std::unordered_map<std::string, std::string> preferred_save_card2;
    // title_id → absolute BIOS dump path, or kOpenBiosChoice for OpenBIOS.
    std::unordered_map<std::string, std::string> preferred_bios;
    // Multi-disc titles: title_id → absolute path of the disc Play should boot.
    // Mirrored into the install's settings.toml [disc] path, which is what the
    // runtime actually reads; this map is the launcher-side memory of it.
    std::unordered_map<std::string, std::string> preferred_disc;
    // Titles that skip global platform Configure merge on install/update/launch.
    std::unordered_set<std::string> exclude_platform_config;
    // title_id → active HD texture pack id (a directory name under
    // texturepacks_dir_for(title_id)). Absent/empty = no pack, which is the
    // default. Exactly one pack is active per title.
    std::unordered_map<std::string, std::string> active_texture_pack;
};

AppState load_app_state(const fs::path& path);
bool save_app_state(const fs::path& path, const AppState& state, std::string* error = nullptr);

std::string preferred_save_for(const AppState& state, const std::string& title_id);
void set_preferred_save(AppState& state, const std::string& title_id, const std::string& save_id);

std::string preferred_save_card2_for(const AppState& state, const std::string& title_id);
void set_preferred_save_card2(AppState& state, const std::string& title_id,
                              const std::string& save_id);

std::string preferred_bios_for(const AppState& state, const std::string& title_id);
void set_preferred_bios(AppState& state, const std::string& title_id,
                        const std::string& bios_choice);

std::string preferred_disc_for(const AppState& state, const std::string& title_id);
void set_preferred_disc(AppState& state, const std::string& title_id,
                        const std::string& disc_path);

bool title_excludes_platform_config(const AppState& state, const std::string& title_id);

// Active HD texture pack id for a title, or "" when none is selected.
std::string active_texture_pack_for(const AppState& state, const std::string& title_id);
// Passing an empty pack_id clears the selection (the game falls back to its
// native textures), which is also how removing the active pack is handled.
void set_active_texture_pack(AppState& state, const std::string& title_id,
                             const std::string& pack_id);
void set_title_excludes_platform_config(AppState& state, const std::string& title_id,
                                        bool exclude);

} // namespace retcomm
