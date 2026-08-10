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
    // Titles that skip global platform Configure merge on install/update/launch.
    std::unordered_set<std::string> exclude_platform_config;
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

bool title_excludes_platform_config(const AppState& state, const std::string& title_id);
void set_title_excludes_platform_config(AppState& state, const std::string& title_id,
                                        bool exclude);

} // namespace retcomm
