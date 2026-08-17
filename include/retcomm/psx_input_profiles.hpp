#pragma once

// Global PlayStation input profiles for RetComM — mirrors recomp-ui:
//   platform/psx/input.ini     — GUID-keyed gamepad maps ([gamepads], [mapping.<guid>])
//   platform/psx/keybinds.ini  — per-player keyboard maps ([playerN])

#include "retcomm/paths.hpp"

#include <filesystem>
#include <string>

namespace retcomm {

namespace fs = std::filesystem;

inline constexpr int kPsxPadButtonCount = 24;
inline constexpr int kPsxPadMaxKnown = 16;
inline constexpr int kPsxPadDefaultDeadzonePct = 10;

// Labels / INI keys / Map-All order — same vocabulary as recomp-ui psx_profile.h.
const char* psx_pad_button_label(int b);
const char* psx_pad_button_ini_key(int b);
// Column-major Map All order (24 indices into the button list).
const int* psx_pad_map_all_order();

fs::path psx_platform_input_ini_path(const Paths& paths);
fs::path psx_platform_keybinds_ini_path(const Paths& paths);

void psx_pad_binds_init(const Paths& paths);

// Silent GUID index (creates [gamepads] + [mapping.<guid>] when missing).
void psx_pad_binds_remember(const Paths& paths, const std::string& guid, const std::string& name,
                            int deadzone_pct = -1);
void psx_pad_binds_rename(const Paths& paths, const std::string& guid, const std::string& name);
void psx_pad_binds_delete(const Paths& paths, const std::string& guid);
void psx_pad_binds_reset(const Paths& paths, const std::string& guid);
void psx_pad_binds_save_profile(const Paths& paths, const std::string& guid,
                                const std::string& name, bool name_custom, int deadzone_pct);

void psx_pad_binds_label(const Paths& paths, const std::string& guid, int b, char* out, int cap);
// kind: 0 none, 1 button, 2 axis (axis_dir: -1/+1).
void psx_pad_binds_set(const Paths& paths, const std::string& guid, int b, int kind, int code,
                       int axis_dir);
// Direct source string (SDL gamepad button/axis name, optional +/-). Prefer this when
// the hub already resolved a name via SDL_GetGamepadStringFor*.
void psx_pad_binds_set_source(const Paths& paths, const std::string& guid, int b,
                              const char* source);

int psx_pad_binds_known_count(const Paths& paths);
bool psx_pad_binds_known_at(const Paths& paths, int index, char* guid, int guid_cap, char* name,
                            int name_cap);
void psx_pad_binds_name(const Paths& paths, const std::string& guid, char* out, int cap);
bool psx_pad_binds_name_is_custom(const Paths& paths, const std::string& guid);
int psx_pad_binds_deadzone(const Paths& paths, const std::string& guid);
void psx_pad_binds_set_deadzone(const Paths& paths, const std::string& guid, int deadzone_pct);

void psx_keybinds_init(const Paths& paths);
void psx_keybinds_label(const Paths& paths, int player /*0-based*/, int b, char* out, int cap);
int psx_keybinds_get_scancode(const Paths& paths, int player, int b);
void psx_keybinds_set_scancode(const Paths& paths, int player, int b, int scancode);
void psx_keybinds_reset_player(const Paths& paths, int player);

// Copy global input.ini + keybinds.ini into a game install cwd (best-effort).
bool apply_psx_input_files(const Paths& paths, const fs::path& game_cwd, std::string* error = nullptr);

} // namespace retcomm
