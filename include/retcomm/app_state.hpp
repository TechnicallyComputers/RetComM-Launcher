#pragma once

#include "retcomm/paths.hpp"

#include <filesystem>
#include <string>
#include <unordered_map>

namespace retcomm {

namespace fs = std::filesystem;

// Hub-wide prefs in data_dir/state.json (preferred save slot per title, etc.).
struct AppState {
    int schema_version = 1;
    // title_id → release-relative save id, e.g. "saves/Pokemon - Emerald Version (U).sav"
    std::unordered_map<std::string, std::string> preferred_save;
};

AppState load_app_state(const fs::path& path);
bool save_app_state(const fs::path& path, const AppState& state, std::string* error = nullptr);

std::string preferred_save_for(const AppState& state, const std::string& title_id);
void set_preferred_save(AppState& state, const std::string& title_id, const std::string& save_id);

} // namespace retcomm
