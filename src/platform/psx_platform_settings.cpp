#include "retcomm/psx_platform_settings.hpp"
#include "retcomm/psx_input_profiles.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace retcomm {
namespace {

bool write_text_file(const fs::path& path, const std::string& body, std::string* error) {
    std::error_code ec;
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot write " + path.string();
        return false;
    }
    out << body;
    if (!body.empty() && body.back() != '\n') out << '\n';
    return static_cast<bool>(out);
}

std::string read_text_file(const fs::path& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string trim_copy(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' ||
                          s.back() == '\t'))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

bool ieq(const std::string& a, const char* b) {
    if (!b) return false;
    if (a.size() != std::strlen(b)) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

std::string unquote(std::string v) {
    v = trim_copy(std::move(v));
    if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') ||
                          (v.front() == '\'' && v.back() == '\'')))
        return v.substr(1, v.size() - 2);
    return v;
}

// Upsert key = value inside [table]. Creates the table at EOF when missing.
void upsert_toml_key(std::string& body, const std::string& table, const std::string& key,
                     const std::string& value_literal) {
    const std::string header = "[" + table + "]";
    std::istringstream in(body);
    std::ostringstream out;
    std::string line;
    bool in_table = false;
    bool wrote = false;
    bool saw_table = false;

    while (std::getline(in, line)) {
        std::string trimmed = line;
        if (!trimmed.empty() && trimmed.back() == '\r') trimmed.pop_back();
        const bool is_table = !trimmed.empty() && trimmed.front() == '[';

        if (in_table && is_table) {
            if (!wrote) {
                out << key << " = " << value_literal << "\n";
                wrote = true;
            }
            in_table = false;
        }

        if (!in_table && trimmed == header) {
            in_table = true;
            saw_table = true;
            out << line << "\n";
            continue;
        }

        if (in_table) {
            const auto eq = trimmed.find('=');
            if (eq != std::string::npos) {
                std::string k = trim_copy(trimmed.substr(0, eq));
                if (k == key) {
                    out << key << " = " << value_literal << "\n";
                    wrote = true;
                    continue;
                }
            }
        }
        out << line << "\n";
    }

    if (in_table && !wrote) {
        out << key << " = " << value_literal << "\n";
        wrote = true;
    }
    if (!saw_table) {
        std::string b = out.str();
        while (!b.empty() && (b.back() == '\n' || b.back() == '\r' || b.back() == ' '))
            b.pop_back();
        if (!b.empty()) b.push_back('\n');
        b += "\n" + header + "\n" + key + " = " + value_literal + "\n";
        body = std::move(b);
        return;
    }
    body = out.str();
}

// Drop every matching key under [table] (used to strip legacy pN_analog).
void remove_toml_key(std::string& body, const std::string& table, const std::string& key) {
    const std::string header = "[" + table + "]";
    std::istringstream in(body);
    std::ostringstream out;
    std::string line;
    bool in_table = false;

    while (std::getline(in, line)) {
        std::string trimmed = line;
        if (!trimmed.empty() && trimmed.back() == '\r') trimmed.pop_back();
        const bool is_table = !trimmed.empty() && trimmed.front() == '[';

        if (in_table && is_table) in_table = false;

        if (!in_table && trimmed == header) {
            in_table = true;
            out << line << "\n";
            continue;
        }

        if (in_table) {
            const auto eq = trimmed.find('=');
            if (eq != std::string::npos) {
                std::string k = trim_copy(trimmed.substr(0, eq));
                if (k == key) continue; // drop
            }
        }
        out << line << "\n";
    }
    body = out.str();
}

void upsert_ini_key(std::string& body, const std::string& section, const std::string& key,
                    const std::string& value) {
    const std::string header = "[" + section + "]";
    std::istringstream in(body);
    std::ostringstream out;
    std::string line;
    bool in_sec = false;
    bool wrote = false;
    bool saw = false;

    while (std::getline(in, line)) {
        std::string trimmed = trim_copy(line);
        const bool is_sec = !trimmed.empty() && trimmed.front() == '[' && trimmed.back() == ']';

        if (in_sec && is_sec) {
            if (!wrote) {
                out << key << "=" << value << "\n";
                wrote = true;
            }
            in_sec = false;
        }
        if (!in_sec && ieq(trimmed, header.c_str())) {
            in_sec = true;
            saw = true;
            out << line << "\n";
            continue;
        }
        if (in_sec) {
            const auto eq = trimmed.find('=');
            if (eq != std::string::npos) {
                std::string k = trim_copy(trimmed.substr(0, eq));
                if (ieq(k, key.c_str())) {
                    out << key << "=" << value << "\n";
                    wrote = true;
                    continue;
                }
            }
        }
        out << line << "\n";
    }
    if (in_sec && !wrote) {
        out << key << "=" << value << "\n";
        wrote = true;
    }
    if (!saw) {
        std::string b = out.str();
        while (!b.empty() && (b.back() == '\n' || b.back() == '\r')) b.pop_back();
        if (!b.empty()) b.push_back('\n');
        b += "\n" + header + "\n" + key + "=" + value + "\n";
        body = std::move(b);
        return;
    }
    body = out.str();
}

bool parse_bool(const std::string& v, bool* out) {
    if (ieq(v, "true") || ieq(v, "1") || ieq(v, "yes") || ieq(v, "on")) {
        *out = true;
        return true;
    }
    if (ieq(v, "false") || ieq(v, "0") || ieq(v, "no") || ieq(v, "off")) {
        *out = false;
        return true;
    }
    return false;
}

void parse_toml_settings(const std::string& body, PsxPlatformSettings& s) {
    std::istringstream in(body);
    std::string line;
    std::string table;
    // pN_mode wins over legacy pN_analog when both appear (any order).
    bool mode_from_string[PsxPlatformSettings::kMaxPlayers]{};
    while (std::getline(in, line)) {
        std::string t = trim_copy(line);
        if (t.empty() || t[0] == '#') continue;
        if (t.front() == '[') {
            if (t.back() == ']') table = t.substr(1, t.size() - 2);
            continue;
        }
        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim_copy(t.substr(0, eq));
        std::string val = unquote(t.substr(eq + 1));

        if (table == "video") {
            if (key == "window_width") {
                try {
                    s.window_width = std::stoi(val);
                } catch (...) {
                }
            } else if (key == "renderer") {
                if (ieq(val, "software")) s.renderer = 0;
                else if (ieq(val, "vulkan")) s.renderer = 2;
                else s.renderer = 1;
            } else if (key == "supersampling") {
                try {
                    s.supersampling = std::clamp(std::stoi(val), 1, 4);
                } catch (...) {
                }
            } else if (key == "fullscreen") {
                try {
                    s.fullscreen = std::clamp(std::stoi(val), 0, 2);
                } catch (...) {
                }
            } else if (key == "aspect_ratio") {
                if (val == "16:9") s.view_mode = 1;
                else if (val == "21:9") s.view_mode = 2;
                else s.view_mode = 0;
            } else if (key == "adaptive_view") {
                bool b = false;
                if (parse_bool(val, &b) && b) s.view_mode = 3;
            } else if (key == "texture_filtering") {
                s.texture_filter_bilinear = ieq(val, "bilinear");
            } else if (key == "antialiasing") {
                parse_bool(val, &s.antialiasing);
            } else if (key == "crt_filter") {
                if (ieq(val, "crt")) s.screen_kind = 1;
                else if (ieq(val, "composite")) s.screen_kind = 2;
                else if (ieq(val, "trinitron")) s.screen_kind = 3;
                else s.screen_kind = 0;
            } else if (key == "frame_interpolation") {
                parse_bool(val, &s.frame_interpolation);
            } else if (key == "frame_interpolation_fps") {
                try {
                    s.frame_interpolation_fps = std::stoi(val);
                } catch (...) {
                }
            } else if (key == "perspective_texturing") {
                parse_bool(val, &s.perspective_texturing);
            } else if (key == "auto_skip_fmv") {
                parse_bool(val, &s.auto_skip_fmv);
            } else if (key == "low_latency_input") {
                parse_bool(val, &s.low_latency_input);
            } else if (key == "vsync") {
                if (ieq(val, "immediate")) s.vsync = 0;
                else if (ieq(val, "adaptive")) s.vsync = -1;
                else s.vsync = 1;
            } else if (key == "rewind_depth") {
                try {
                    int d = std::stoi(val);
                    static const int opts[4] = {50, 100, 150, 200};
                    int best = opts[0];
                    int best_d = d > best ? d - best : best - d;
                    for (int i = 1; i < 4; ++i) {
                        int dd = d > opts[i] ? d - opts[i] : opts[i] - d;
                        if (dd < best_d) {
                            best_d = dd;
                            best = opts[i];
                        }
                    }
                    s.rewind_depth = best;
                } catch (...) {
                }
            } else if (key == "rewind_interval") {
                try {
                    int d = std::stoi(val);
                    static const int opts[5] = {1, 4, 8, 12, 15};
                    int best = opts[0];
                    int best_d = d > best ? d - best : best - d;
                    for (int i = 1; i < 5; ++i) {
                        int dd = d > opts[i] ? d - opts[i] : opts[i] - d;
                        if (dd < best_d) {
                            best_d = dd;
                            best = opts[i];
                        }
                    }
                    s.rewind_interval = best;
                } catch (...) {
                }
            }
        } else if (table == "audio") {
            if (key == "spu_hq") parse_bool(val, &s.spu_hq);
        } else if (table == "controller") {
            if (key == "multitap") parse_bool(val, &s.multitap_enabled);
            else if (key == "multitap_analog") parse_bool(val, &s.multitap_analog);
            else {
                // pN_device / pN_mode / pN_deadzone (N = 1..kMaxPlayers).
                // Shortest key is "p1_mode" (7); do not require length 9 or modes never load.
                if (key.size() >= 7 && key[0] == 'p' && std::isdigit(static_cast<unsigned char>(key[1]))) {
                    size_t i = 1;
                    int slot1 = 0;
                    while (i < key.size() && std::isdigit(static_cast<unsigned char>(key[i]))) {
                        slot1 = slot1 * 10 + (key[i] - '0');
                        ++i;
                    }
                    if (slot1 >= 1 && slot1 <= PsxPlatformSettings::kMaxPlayers && i < key.size() &&
                        key[i] == '_') {
                        const int slot = slot1 - 1;
                        const std::string field = key.substr(i + 1);
                        if (field == "device") {
                            s.set_player_from_device_string(slot, val);
                        } else if (field == "mode") {
                            if (ieq(val, "digital")) s.player_mode[static_cast<size_t>(slot)] = 2;
                            else s.player_mode[static_cast<size_t>(slot)] = 1; // analog (+ legacy hybrid)
                            mode_from_string[static_cast<size_t>(slot)] = true;
                        } else if (field == "deadzone") {
                            try {
                                s.player_deadzone[static_cast<size_t>(slot)] =
                                    std::clamp(std::stoi(val), 0, 32767);
                            } catch (...) {
                            }
                        } else if (field == "analog") {
                            // Legacy boolean: true→analog, false→digital.
                            // Ignored when pN_mode already set for this seat.
                            if (mode_from_string[static_cast<size_t>(slot)]) continue;
                            bool b = true;
                            if (parse_bool(val, &b))
                                s.player_mode[static_cast<size_t>(slot)] = b ? 1 : 2;
                        }
                    }
                }
            }
        }
    }
}

void parse_ini_hotkeys(const std::string& body, PsxPlatformSettings& s) {
    std::istringstream in(body);
    std::string line;
    bool in_keymap = false;
    while (std::getline(in, line)) {
        std::string t = trim_copy(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        if (t.front() == '[') {
            in_keymap = ieq(t, "[KeyMap]");
            continue;
        }
        if (!in_keymap) continue;
        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim_copy(t.substr(0, eq));
        const std::string val = trim_copy(t.substr(eq + 1));
        for (int i = 0; i < PsxPlatformSettings::kHotkeyCount; ++i) {
            if (ieq(key, PsxPlatformSettings::hotkey_ini_key(i))) {
                s.hotkeys[static_cast<size_t>(i)] = val;
                break;
            }
        }
    }
}

std::string bool_lit(bool v) { return v ? "true" : "false"; }

std::string renderer_lit(int r) {
    if (r == 0) return "\"software\"";
    if (r == 2) return "\"vulkan\"";
    return "\"opengl\"";
}

std::string screen_lit(int k) {
    switch (k) {
    case 1:
        return "\"crt\"";
    case 2:
        return "\"composite\"";
    case 3:
        return "\"trinitron\"";
    default:
        return "\"raw\"";
    }
}

std::string vsync_lit(int v) {
    if (v == 0) return "\"immediate\"";
    if (v < 0) return "\"adaptive\"";
    return "\"on\"";
}

void write_toml_body(std::string& body, const PsxPlatformSettings& s,
                     bool apply_multitap_analog) {
    upsert_toml_key(body, "video", "window_width", std::to_string(s.window_width));
    upsert_toml_key(body, "video", "renderer", renderer_lit(s.renderer));
    upsert_toml_key(body, "video", "supersampling", std::to_string(s.supersampling));
    upsert_toml_key(body, "video", "fullscreen", std::to_string(s.fullscreen));
    if (s.view_mode == 3) {
        upsert_toml_key(body, "video", "adaptive_view", "true");
        upsert_toml_key(body, "video", "aspect_ratio", "\"4:3\"");
    } else {
        upsert_toml_key(body, "video", "adaptive_view", "false");
        const char* ar = "4:3";
        if (s.view_mode == 1) ar = "16:9";
        else if (s.view_mode == 2) ar = "21:9";
        upsert_toml_key(body, "video", "aspect_ratio", std::string("\"") + ar + "\"");
    }
    upsert_toml_key(body, "video", "texture_filtering",
                    s.texture_filter_bilinear ? "\"bilinear\"" : "\"nearest\"");
    upsert_toml_key(body, "video", "antialiasing", bool_lit(s.antialiasing));
    upsert_toml_key(body, "video", "crt_filter", screen_lit(s.screen_kind));
    upsert_toml_key(body, "video", "frame_interpolation", bool_lit(s.frame_interpolation));
    upsert_toml_key(body, "video", "frame_interpolation_fps",
                    std::to_string(s.frame_interpolation_fps));
    upsert_toml_key(body, "video", "perspective_texturing", bool_lit(s.perspective_texturing));
    upsert_toml_key(body, "video", "auto_skip_fmv", bool_lit(s.auto_skip_fmv));
    upsert_toml_key(body, "video", "low_latency_input", bool_lit(s.low_latency_input));
    upsert_toml_key(body, "video", "vsync", vsync_lit(s.vsync));
    {
        int d = s.rewind_depth;
        if (d != 50 && d != 100 && d != 150 && d != 200) d = 50;
        upsert_toml_key(body, "video", "rewind_depth", std::to_string(d));
        int iv = s.rewind_interval;
        if (iv != 1 && iv != 4 && iv != 8 && iv != 12 && iv != 15) iv = 15;
        upsert_toml_key(body, "video", "rewind_interval", std::to_string(iv));
    }

    upsert_toml_key(body, "audio", "spu_hq", bool_lit(s.spu_hq));
    upsert_toml_key(body, "controller", "multitap", bool_lit(s.multitap_enabled));
    // Digital + lock_mode titles reject DualShock; never push the multitap analog hack.
    if (apply_multitap_analog)
        upsert_toml_key(body, "controller", "multitap_analog", bool_lit(s.multitap_analog));

    for (int i = 0; i < PsxPlatformSettings::kMaxPlayers; ++i) {
        const std::string n = std::to_string(i + 1);
        const std::string dev = s.player_device_string(i);
        // Drop legacy boolean so it cannot override pN_mode on the next load.
        remove_toml_key(body, "controller", "p" + n + "_analog");
        upsert_toml_key(body, "controller", "p" + n + "_device",
                        std::string("\"") + dev + "\"");
        const char* mode = (s.player_mode[static_cast<size_t>(i)] == 2) ? "digital" : "analog";
        upsert_toml_key(body, "controller", "p" + n + "_mode",
                        std::string("\"") + mode + "\"");
        upsert_toml_key(body, "controller", "p" + n + "_deadzone",
                        std::to_string(std::clamp(s.player_deadzone[static_cast<size_t>(i)], 0, 32767)));
    }
}

// game.toml [controller] default_mode = "digital" + lock_mode = true → pad is fixed digital.
bool game_toml_locks_digital_pad(const std::string& body) {
    std::istringstream in(body);
    std::string line;
    bool in_controller = false;
    bool digital = false;
    bool lock = false;
    while (std::getline(in, line)) {
        std::string trimmed = trim_copy(std::move(line));
        if (trimmed.empty() || trimmed.front() == '#') continue;
        if (trimmed.front() == '[') {
            in_controller = (trimmed == "[controller]");
            continue;
        }
        if (!in_controller) continue;
        const auto eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim_copy(trimmed.substr(0, eq));
        const std::string val = unquote(trimmed.substr(eq + 1));
        if (key == "default_mode" && ieq(val, "digital")) digital = true;
        if (key == "lock_mode" && ieq(val, "true")) lock = true;
    }
    return digital && lock;
}

fs::path resolve_install_root_from_game_cwd(const fs::path& game_cwd) {
    if (game_cwd.empty()) return {};
    // Typical launch/install cwd: apps/<title>/releases/<tag>
    if (game_cwd.has_parent_path() && game_cwd.parent_path().filename() == "releases")
        return game_cwd.parent_path().parent_path();
    return game_cwd;
}

bool title_locks_digital_pad(const fs::path& game_cwd) {
    std::vector<fs::path> candidates;
    candidates.push_back(game_cwd / "game.toml");
    const fs::path install_root = resolve_install_root_from_game_cwd(game_cwd);
    if (!install_root.empty()) {
        candidates.push_back(install_root / "src" / "current" / "game.toml");
        candidates.push_back(install_root / "game.toml");
    }
    std::error_code ec;
    for (const auto& p : candidates) {
        if (!fs::is_regular_file(p, ec)) continue;
        if (game_toml_locks_digital_pad(read_text_file(p))) return true;
    }
    return false;
}

void write_ini_body(std::string& body, const PsxPlatformSettings& s) {
    for (int i = 0; i < PsxPlatformSettings::kHotkeyCount; ++i) {
        const std::string& v = s.hotkeys[static_cast<size_t>(i)];
        upsert_ini_key(body, "KeyMap", PsxPlatformSettings::hotkey_ini_key(i),
                       v.empty() ? PsxPlatformSettings::hotkey_default(i) : v);
    }
}

} // namespace

const char* PsxPlatformSettings::hotkey_ini_key(int i) {
    static const char* k[] = {"Fullscreen",  "Reset",        "Pause",       "Turbo",
                              "VolumeUp",    "VolumeDown",   "DisplayPerf", "ToggleRenderer",
                              "Rewind"};
    if (i < 0 || i >= kHotkeyCount) return "";
    return k[i];
}

const char* PsxPlatformSettings::hotkey_label(int i) {
    static const char* k[] = {"Fullscreen",  "Reset",          "Pause",
                              "Fast-forward", "Volume up",     "Volume down",
                              "Display perf", "Toggle renderer", "Rewind"};
    if (i < 0 || i >= kHotkeyCount) return "";
    return k[i];
}

const char* PsxPlatformSettings::hotkey_default(int i) {
    static const char* k[] = {"Alt+Return", "Ctrl+R", "Shift+P", "Tab",
                              "Keypad +",   "Keypad -", "F",     "R",
                              "F8"};
    if (i < 0 || i >= kHotkeyCount) return "";
    return k[i];
}

void PsxPlatformSettings::apply_hotkey_defaults_if_empty() {
    for (int i = 0; i < kHotkeyCount; ++i) {
        if (hotkeys[static_cast<size_t>(i)].empty())
            hotkeys[static_cast<size_t>(i)] = hotkey_default(i);
    }
}

void PsxPlatformSettings::reset_system_to_defaults() {
    const auto keep_src = player_src;
    const auto keep_guid = player_guid;
    const auto keep_mode = player_mode;
    const auto keep_dz = player_deadzone;

    *this = PsxPlatformSettings{};
    player_src = keep_src;
    player_guid = keep_guid;
    player_mode = keep_mode;
    player_deadzone = keep_dz;

    for (int i = 0; i < kHotkeyCount; ++i)
        hotkeys[static_cast<size_t>(i)] = hotkey_default(i);
    apply_controller_defaults_if_unset();
}

void PsxPlatformSettings::apply_controller_defaults_if_unset() {
    // First load leaves arrays zeroed: treat that as P1 keyboard, rest none.
    bool any = false;
    for (int i = 0; i < kMaxPlayers; ++i) {
        if (player_src[static_cast<size_t>(i)] != 0 || !player_guid[static_cast<size_t>(i)].empty() ||
            player_mode[static_cast<size_t>(i)] != 0 || player_deadzone[static_cast<size_t>(i)] != 0) {
            any = true;
            break;
        }
    }
    if (!any) {
        player_src[0] = 1; // keyboard
        for (int i = 1; i < kMaxPlayers; ++i) player_src[static_cast<size_t>(i)] = 0;
    }
    for (int i = 0; i < kMaxPlayers; ++i) {
        player_src[static_cast<size_t>(i)] = std::clamp(player_src[static_cast<size_t>(i)], 0, 2);
        // Keyboard seats stay digital; everything else defaults to DualShock/analog
        // (sticks are assumed on gamepads). Explicit digital for a pad is kept.
        if (player_src[static_cast<size_t>(i)] == 1)
            player_mode[static_cast<size_t>(i)] = 2;
        else if (player_mode[static_cast<size_t>(i)] != 1 &&
                 player_mode[static_cast<size_t>(i)] != 2)
            player_mode[static_cast<size_t>(i)] = 1; // analog
        if (player_deadzone[static_cast<size_t>(i)] < 0 || player_deadzone[static_cast<size_t>(i)] > 32767)
            player_deadzone[static_cast<size_t>(i)] = 3277;
        if (player_deadzone[static_cast<size_t>(i)] == 0 && player_src[static_cast<size_t>(i)] != 0)
            player_deadzone[static_cast<size_t>(i)] = 3277;
        if (player_src[static_cast<size_t>(i)] != 2) player_guid[static_cast<size_t>(i)].clear();
    }
}

std::string PsxPlatformSettings::player_device_string(int slot) const {
    if (slot < 0 || slot >= kMaxPlayers) return "none";
    const int src = player_src[static_cast<size_t>(slot)];
    if (src == 0) return "none";
    if (src == 1) return "keyboard";
    if (!player_guid[static_cast<size_t>(slot)].empty()) return player_guid[static_cast<size_t>(slot)];
    return "gamepad";
}

void PsxPlatformSettings::set_player_from_device_string(int slot, const std::string& device) {
    if (slot < 0 || slot >= kMaxPlayers) return;
    auto lower_eq = [](const std::string& a, const char* b) {
        if (!b || a.size() != std::strlen(b)) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        }
        return true;
    };
    std::string d = device;
    while (!d.empty() && (d.back() == '\r' || d.back() == '\n' || d.back() == ' ' || d.back() == '\t'))
        d.pop_back();
    size_t lead = 0;
    while (lead < d.size() && (d[lead] == ' ' || d[lead] == '\t')) ++lead;
    d = d.substr(lead);
    if (d.empty() || lower_eq(d, "none")) {
        player_src[static_cast<size_t>(slot)] = 0;
        player_guid[static_cast<size_t>(slot)].clear();
        return;
    }
    if (lower_eq(d, "keyboard") || lower_eq(d, "auto")) {
        player_src[static_cast<size_t>(slot)] = 1;
        player_guid[static_cast<size_t>(slot)].clear();
        return;
    }
    player_src[static_cast<size_t>(slot)] = 2;
    if (lower_eq(d, "gamepad") || lower_eq(d, "controller"))
        player_guid[static_cast<size_t>(slot)].clear();
    else
        player_guid[static_cast<size_t>(slot)] = d;
}

fs::path psx_platform_settings_dir(const Paths& paths) {
    return paths.data_dir / "platform" / "psx";
}

fs::path psx_platform_settings_toml_path(const Paths& paths) {
    return psx_platform_settings_dir(paths) / "settings.toml";
}

fs::path psx_platform_settings_ini_path(const Paths& paths) {
    return psx_platform_settings_dir(paths) / "config.ini";
}

PsxPlatformSettings load_psx_platform_settings(const Paths& paths) {
    PsxPlatformSettings s;
    // Do not seed controller defaults before parse: that left P1 as keyboard/digital,
    // and skipped pN_mode keys would keep digital after a gamepad device loaded.
    s.apply_hotkey_defaults_if_empty();
    const fs::path toml = psx_platform_settings_toml_path(paths);
    const fs::path ini = psx_platform_settings_ini_path(paths);
    std::error_code ec;
    if (fs::is_regular_file(toml, ec)) parse_toml_settings(read_text_file(toml), s);
    if (fs::is_regular_file(ini, ec)) parse_ini_hotkeys(read_text_file(ini), s);
    s.apply_hotkey_defaults_if_empty();
    s.apply_controller_defaults_if_unset();
    return s;
}

bool save_psx_platform_settings(const Paths& paths, const PsxPlatformSettings& s_in,
                                std::string* error) {
    PsxPlatformSettings s = s_in;
    s.apply_hotkey_defaults_if_empty();
    s.apply_controller_defaults_if_unset();

    std::string toml = read_text_file(psx_platform_settings_toml_path(paths));
    if (toml.empty()) {
        toml = "# RetComM global PlayStation settings — applied to titles on "
               "install/update/launch.\n";
    }
    write_toml_body(toml, s, /*apply_multitap_analog=*/true);
    if (!write_text_file(psx_platform_settings_toml_path(paths), toml, error)) return false;

    std::string ini = read_text_file(psx_platform_settings_ini_path(paths));
    if (ini.empty()) ini = "; RetComM global PlayStation hotkeys ([KeyMap]).\n";
    write_ini_body(ini, s);
    return write_text_file(psx_platform_settings_ini_path(paths), ini, error);
}

ApplyPsxPlatformResult apply_psx_platform_defaults(const Paths& paths, const AppState& state,
                                                   const Title& title, const fs::path& game_cwd) {
    ApplyPsxPlatformResult r;
    if (!is_psx_platform(title.platform)) {
        r.skipped = true;
        r.message = "not a PlayStation title";
        return r;
    }
    if (title_excludes_platform_config(state, title.id)) {
        r.skipped = true;
        r.message = "excluded from platform config: " + title.id;
        return r;
    }
    if (game_cwd.empty()) {
        r.ok = false;
        r.message = "empty game cwd";
        return r;
    }

    std::error_code ec;
    const fs::path global_toml = psx_platform_settings_toml_path(paths);
    const fs::path global_ini = psx_platform_settings_ini_path(paths);
    if (!fs::is_regular_file(global_toml, ec) && !fs::is_regular_file(global_ini, ec)) {
        r.skipped = true;
        r.message = "no global PlayStation settings yet";
        return r;
    }

    const PsxPlatformSettings s = load_psx_platform_settings(paths);
    const bool apply_multitap_analog = !title_locks_digital_pad(game_cwd);
    const fs::path dest_toml = game_cwd / "settings.toml";
    const fs::path dest_ini = game_cwd / "config.ini";

    std::string toml = read_text_file(dest_toml);
    write_toml_body(toml, s, apply_multitap_analog);
    std::string err;
    if (!write_text_file(dest_toml, toml, &err)) {
        r.ok = false;
        r.message = err;
        return r;
    }

    std::string ini = read_text_file(dest_ini);
    write_ini_body(ini, s);
    if (!write_text_file(dest_ini, ini, &err)) {
        r.ok = false;
        r.message = err;
        return r;
    }

    std::string input_err;
    apply_psx_input_files(paths, game_cwd, &input_err);

    r.message = "applied PlayStation platform settings → " + game_cwd.string();
    return r;
}

} // namespace retcomm
