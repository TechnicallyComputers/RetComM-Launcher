#pragma once

#include "retcomm/app_state.hpp"
#include "retcomm/catalog.hpp"
#include "retcomm/paths.hpp"

#include <array>
#include <filesystem>
#include <string>

namespace retcomm {

namespace fs = std::filesystem;

inline bool is_psx_platform(const std::string& platform) {
    return platform == "psx" || platform == "ps1" || platform == "ps";
}

// Global RetComM PlayStation prefs (Display / Audio / Input / Hotkeys).
// Persisted under data_dir/platform/psx/{settings.toml,config.ini}.
struct PsxPlatformSettings {
    // --- Display ([video]) ---
    int window_width = 1280;       // 960 / 1280 / 1600 / 1920
    int renderer = 1;              // 0 software, 1 opengl, 2 vulkan
    int supersampling = 1;         // 1..4
    int fullscreen = 0;            // 0 off, 1 borderless, 2 exclusive
    // 0 = 4:3 native, 1 = 16:9, 2 = 21:9, 3 = adaptive
    int view_mode = 0;
    bool texture_filter_bilinear = false;
    bool antialiasing = false;
    int screen_kind = 0; // 0 raw, 1 crt, 2 composite, 3 trinitron
    bool frame_interpolation = false;
    int frame_interpolation_fps = 0; // 0 = display rate, else e.g. 120
    bool perspective_texturing = false;
    bool auto_skip_fmv = false;
    bool low_latency_input = false;
    // -1 adaptive, 0 immediate, 1 on
    int vsync = 1;

    // --- Audio ([audio]) ---
    bool spu_hq = true;

    // --- Input ([controller]) ---
    bool multitap_enabled = false;
    bool multitap_analog = false;

    // Per-seat devices (recomp-ui / settings.toml pN_device / pN_mode / pN_deadzone).
    // src: 0 none, 1 keyboard, 2 gamepad (GUID in player_guid when src==2).
    // mode: 1 analog (DualShock), 2 digital — matches psxrecomp PadMode (hybrid not persisted).
    // deadzone: 0..32767 (settings.toml units; UI shows percent).
    static constexpr int kMaxPlayers = 8;
    std::array<int, kMaxPlayers> player_src{};
    std::array<std::string, kMaxPlayers> player_guid{};
    std::array<int, kMaxPlayers> player_mode{};
    std::array<int, kMaxPlayers> player_deadzone{};

    // --- Hotkeys (config.ini [KeyMap]) ---
    // Keys match recomp-ui / psxrecomp: Fullscreen, Reset, Pause, Turbo,
    // VolumeUp, VolumeDown, DisplayPerf, ToggleRenderer.
    static constexpr int kHotkeyCount = 8;
    std::array<std::string, kHotkeyCount> hotkeys{};

    static const char* hotkey_ini_key(int i);
    static const char* hotkey_label(int i);
    static const char* hotkey_default(int i);

    void apply_hotkey_defaults_if_empty();
    void apply_controller_defaults_if_unset();

    // Device string written to settings.toml (none / keyboard / GUID / gamepad).
    std::string player_device_string(int slot) const;
    void set_player_from_device_string(int slot, const std::string& device);
};

fs::path psx_platform_settings_dir(const Paths& paths);
fs::path psx_platform_settings_toml_path(const Paths& paths);
fs::path psx_platform_settings_ini_path(const Paths& paths);

PsxPlatformSettings load_psx_platform_settings(const Paths& paths);
bool save_psx_platform_settings(const Paths& paths, const PsxPlatformSettings& s,
                                std::string* error = nullptr);

// Merge global prefs into a title install cwd (settings.toml + config.ini).
// No-op when not PSX, excluded in AppState, or global files are absent.
struct ApplyPsxPlatformResult {
    bool ok = true;
    bool skipped = false;
    std::string message;
};

ApplyPsxPlatformResult apply_psx_platform_defaults(const Paths& paths, const AppState& state,
                                                   const Title& title, const fs::path& game_cwd);

} // namespace retcomm
