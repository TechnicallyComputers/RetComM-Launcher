#include "retcomm/psx_input_profiles.hpp"
#include "retcomm/psx_platform_settings.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>

namespace retcomm {
namespace {

constexpr int kSrcCap = 48;
constexpr int kGuidCap = 40;
constexpr int kNameCap = 64;
constexpr int kMaxKbPlayers = PsxPlatformSettings::kMaxPlayers;

static const char* kLabels[kPsxPadButtonCount] = {
    "Up",         "Down",          "Left",          "Right",
    "Triangle",   "Circle",        "Cross",         "Square",
    "L1",         "L2",            "R1",            "R2",
    "L3",         "R3",            "Start",         "Select",
    "L-Stick Up", "L-Stick Down",  "L-Stick Left",  "L-Stick Right",
    "R-Stick Up", "R-Stick Down",  "R-Stick Left",  "R-Stick Right",
};

static const char* kIniKeys[kPsxPadButtonCount] = {
    "up",      "down",     "left",     "right",    "triangle", "circle",   "cross",
    "square",  "l1",       "l2",       "r1",       "r2",       "l3",       "r3",
    "start",   "select",   "ls_up",    "ls_down",  "ls_left",  "ls_right", "rs_up",
    "rs_down", "rs_left",  "rs_right",
};

static const char* kPadDefaults[kPsxPadButtonCount] = {
    "dpup",          "dpdown",         "dpleft",          "dpright",
    "y",             "b",              "a",               "x",
    "leftshoulder",  "lefttrigger",    "rightshoulder",   "righttrigger",
    "leftstick",     "rightstick",     "start",           "back",
    "lefty-",        "lefty+",         "leftx-",          "leftx+",
    "righty-",       "righty+",        "rightx-",         "rightx+",
};

static const int kMapAllOrder[kPsxPadButtonCount] = {
    0, 1, 2, 3, 16, 17, 18, 19, 6, 5, 7, 4, 20, 21, 22, 23, 8, 9, 10, 11, 14, 15, 12, 13,
};

// SDL_Scancode values (stable across SDL2/3 for these keys).
static const int kKbDefaults[kPsxPadButtonCount] = {
    82, 81, 80, 79, // UP DOWN LEFT RIGHT
    4,  22, 27, 29, // A S X Z
    20, 8,  26, 21, // Q E W R
    23, 28, 40, 229, // T Y RETURN RSHIFT
    82, 81, 80, 79, // LS dirs = arrows
    0,  0,  0,  0,  // RS unbound (UNKNOWN)
};

// SDL_GamepadButton / Axis string names (match SDL_GetGamepadStringFor*).
static const char* kGamepadButtonStr[] = {
    "south", "east", "west", "north", "back", "guide", "start", "leftstick", "rightstick",
    "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc1",
    "right_paddle1", "left_paddle1", "right_paddle2", "left_paddle2", "touchpad", "misc2",
    "misc3", "misc4", "misc5", "misc6",
};
static const char* kGamepadAxisStr[] = {
    "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger",
};

struct GuidMap {
    char guid[kGuidCap]{};
    char name[kNameCap]{};
    int name_custom = 0;
    int deadzone_pct = kPsxPadDefaultDeadzonePct;
    char src[kPsxPadButtonCount][kSrcCap]{};
    int used = 0;
};

char g_global[kPsxPadButtonCount][kSrcCap]{};
GuidMap g_maps[kPsxPadMaxKnown]{};
char g_pad_path[1024]{};
int g_pad_init = 0;

int g_kb[kMaxKbPlayers][kPsxPadButtonCount]{};
char g_kb_path[1024]{};
int g_kb_init = 0;

void copy_str(char* d, size_t cap, const char* s) {
    if (!d || !cap) return;
    if (!s) {
        d[0] = 0;
        return;
    }
    size_t n = std::strlen(s);
    if (n >= cap) n = cap - 1;
    std::memcpy(d, s, n);
    d[n] = 0;
}

void tolower_inplace(char* s) {
    for (; *s; ++s) *s = static_cast<char>(std::tolower(static_cast<unsigned char>(*s)));
}

int clamp_dz(int pct) { return std::clamp(pct, 0, 100); }

void seed_defaults(char dest[][kSrcCap]) {
    for (int b = 0; b < kPsxPadButtonCount; ++b) copy_str(dest[b], kSrcCap, kPadDefaults[b]);
}

int key_index(const char* key) {
    for (int b = 0; b < kPsxPadButtonCount; ++b)
        if (std::strcmp(key, kIniKeys[b]) == 0) return b;
    return -1;
}

GuidMap* find_map(const char* guid) {
    if (!guid || !guid[0]) return nullptr;
    for (int i = 0; i < kPsxPadMaxKnown; ++i)
        if (g_maps[i].used && std::strcmp(g_maps[i].guid, guid) == 0) return &g_maps[i];
    return nullptr;
}

GuidMap* alloc_map(const char* guid) {
    GuidMap* m = find_map(guid);
    if (m) return m;
    for (int i = 0; i < kPsxPadMaxKnown; ++i) {
        if (g_maps[i].used) continue;
        std::memset(&g_maps[i], 0, sizeof(g_maps[i]));
        g_maps[i].used = 1;
        g_maps[i].deadzone_pct = kPsxPadDefaultDeadzonePct;
        copy_str(g_maps[i].guid, sizeof(g_maps[i].guid), guid);
        seed_defaults(g_maps[i].src);
        for (int b = 0; b < kPsxPadButtonCount; ++b)
            if (g_global[b][0]) copy_str(g_maps[i].src[b], kSrcCap, g_global[b]);
        return &g_maps[i];
    }
    return nullptr;
}

void parse_mapping_line(char dest[][kSrcCap], const char* key, const char* val) {
    char kbuf[32];
    copy_str(kbuf, sizeof(kbuf), key);
    tolower_inplace(kbuf);
    const int b = key_index(kbuf);
    if (b < 0) return;
    char vbuf[kSrcCap];
    copy_str(vbuf, sizeof(vbuf), val);
    if (char* comma = std::strchr(vbuf, ',')) *comma = '\0';
    char* s = vbuf;
    while (*s && std::isspace(static_cast<unsigned char>(*s))) ++s;
    size_t n = std::strlen(s);
    while (n > 0 && std::isspace(static_cast<unsigned char>(s[n - 1]))) s[--n] = '\0';
    tolower_inplace(s);
    if (std::strcmp(s, "none") == 0 || std::strcmp(s, "disabled") == 0) s[0] = '\0';
    copy_str(dest[b], kSrcCap, s);
}

void load_pad_ini(const char* path) {
    seed_defaults(g_global);
    std::memset(g_maps, 0, sizeof(g_maps));
    std::ifstream in(path);
    if (!in) return;
    std::string section;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                                 std::isspace(static_cast<unsigned char>(line.back()))))
            line.pop_back();
        size_t i = 0;
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i >= line.size() || line[i] == '#' || line[i] == ';') continue;
        if (line[i] == '[') {
            auto end = line.find(']', i);
            section = (end == std::string::npos) ? line.substr(i + 1) : line.substr(i + 1, end - i - 1);
            for (char& c : section) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (section.rfind("mapping.", 0) == 0 && section.size() > 8)
                alloc_map(section.c_str() + 8);
            continue;
        }
        const auto eq = line.find('=', i);
        if (eq == std::string::npos) continue;
        std::string key = line.substr(i, eq - i);
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
        std::string val = line.substr(eq + 1);
        while (!val.empty() && std::isspace(static_cast<unsigned char>(val.front()))) val.erase(val.begin());
        if (section == "mapping") {
            parse_mapping_line(g_global, key.c_str(), val.c_str());
        } else if (section.rfind("mapping.", 0) == 0) {
            GuidMap* m = find_map(section.c_str() + 8);
            if (!m) continue;
            char kbuf[32];
            copy_str(kbuf, sizeof(kbuf), key.c_str());
            tolower_inplace(kbuf);
            if (std::strcmp(kbuf, "deadzone") == 0) m->deadzone_pct = clamp_dz(std::atoi(val.c_str()));
            else if (std::strcmp(kbuf, "name_custom") == 0) m->name_custom = (std::atoi(val.c_str()) != 0);
            else if (std::strcmp(kbuf, "name") == 0) {
                if (!m->name[0]) copy_str(m->name, sizeof(m->name), val.c_str());
            } else
                parse_mapping_line(m->src, key.c_str(), val.c_str());
        } else if (section == "gamepads") {
            char gbuf[kGuidCap];
            copy_str(gbuf, sizeof(gbuf), key.c_str());
            tolower_inplace(gbuf);
            GuidMap* m = alloc_map(gbuf);
            if (m) copy_str(m->name, sizeof(m->name), val.c_str());
        }
    }
}

bool section_is_ours(const std::string& sec) {
    return sec == "mapping" || sec.rfind("mapping.", 0) == 0 || sec == "gamepads";
}

void write_pad_ini(const char* path) {
    std::string preserved;
    {
        std::ifstream in(path);
        if (in) {
            std::string line;
            bool skip = false;
            while (std::getline(in, line)) {
                std::string t = line;
                while (!t.empty() && t.back() == '\r') t.pop_back();
                size_t i = 0;
                while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
                if (i < t.size() && t[i] == '[') {
                    auto end = t.find(']', i);
                    std::string sec =
                        (end == std::string::npos) ? t.substr(i + 1) : t.substr(i + 1, end - i - 1);
                    for (char& c : sec) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    skip = section_is_ours(sec);
                }
                if (skip) continue;
                preserved += t;
                preserved.push_back('\n');
            }
        }
    }

    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out) return;
    if (!preserved.empty()) {
        out << preserved;
        if (preserved.back() != '\n') out << '\n';
    } else {
        out << "; PSXRecomp input mapping. Per-device overrides: [mapping.<guid>].\n"
               "; [gamepads] lists previously used pads (guid = display name).\n\n"
               "[controller]\n"
               "enabled = true\n"
               "device = 0\n"
               "deadzone = 3277\n\n";
    }

    bool any_named = false;
    for (int i = 0; i < kPsxPadMaxKnown; ++i)
        if (g_maps[i].used && g_maps[i].name[0]) {
            any_named = true;
            break;
        }
    if (any_named) {
        out << "[gamepads]\n";
        for (int i = 0; i < kPsxPadMaxKnown; ++i) {
            if (!g_maps[i].used || !g_maps[i].name[0]) continue;
            out << g_maps[i].guid << " = " << g_maps[i].name << "\n";
        }
        out << "\n";
    }

    out << "[mapping]\n";
    for (int b = 0; b < kPsxPadButtonCount; ++b) {
        const char* v = g_global[b][0] ? g_global[b] : "none";
        out << kIniKeys[b] << " = " << v << "\n";
    }
    out << "\n";

    for (int i = 0; i < kPsxPadMaxKnown; ++i) {
        if (!g_maps[i].used) continue;
        out << "[mapping." << g_maps[i].guid << "]\n";
        out << "deadzone = " << clamp_dz(g_maps[i].deadzone_pct) << "\n";
        out << "name_custom = " << (g_maps[i].name_custom ? 1 : 0) << "\n";
        for (int b = 0; b < kPsxPadButtonCount; ++b) {
            const char* v = g_maps[i].src[b][0] ? g_maps[i].src[b] : "none";
            out << kIniKeys[b] << " = " << v << "\n";
        }
        out << "\n";
    }
}

void ensure_pad(const char* path) {
    if (g_pad_init && g_pad_path[0] && path && std::strcmp(g_pad_path, path) == 0) return;
    copy_str(g_pad_path, sizeof(g_pad_path), path ? path : "input.ini");
    load_pad_ini(g_pad_path);
    std::ifstream test(g_pad_path);
    if (!test) write_pad_ini(g_pad_path);
    g_pad_init = 1;
}

void source_from_bind(int kind, int code, int axis_dir, char* out, size_t cap) {
    out[0] = 0;
    if (kind == 1) {
        const char* n = nullptr;
        if (code >= 0 && code < static_cast<int>(sizeof(kGamepadButtonStr) / sizeof(kGamepadButtonStr[0])))
            n = kGamepadButtonStr[code];
        // Prefer Xbox-legacy aliases the runtime/input.ini historically used.
        if (n) {
            if (std::strcmp(n, "south") == 0) n = "a";
            else if (std::strcmp(n, "east") == 0) n = "b";
            else if (std::strcmp(n, "west") == 0) n = "x";
            else if (std::strcmp(n, "north") == 0) n = "y";
        }
        copy_str(out, cap, (n && n[0]) ? n : "");
    } else if (kind == 2) {
        const char* n = "axis";
        if (code >= 0 && code < static_cast<int>(sizeof(kGamepadAxisStr) / sizeof(kGamepadAxisStr[0])))
            n = kGamepadAxisStr[code];
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s%c", n, axis_dir < 0 ? '-' : '+');
        copy_str(out, cap, buf);
    }
}

struct ScName {
    int sc;
    const char* name;
};
// Minimal scancode↔name table for keybinds.ini (SDL names).
static const ScName kScNames[] = {
    {0, "None"},   {4, "A"},     {5, "B"},     {6, "C"},     {7, "D"},     {8, "E"},
    {9, "F"},      {10, "G"},    {11, "H"},    {12, "I"},    {13, "J"},    {14, "K"},
    {15, "L"},     {16, "M"},    {17, "N"},    {18, "O"},    {19, "P"},    {20, "Q"},
    {21, "R"},     {22, "S"},    {23, "T"},    {24, "U"},    {25, "V"},    {26, "W"},
    {27, "X"},     {28, "Y"},    {29, "Z"},    {30, "1"},    {31, "2"},    {32, "3"},
    {33, "4"},     {34, "5"},    {35, "6"},    {36, "7"},    {37, "8"},    {38, "9"},
    {39, "0"},     {40, "Return"}, {41, "Escape"}, {42, "Backspace"}, {43, "Tab"},
    {44, "Space"}, {79, "Right"}, {80, "Left"}, {81, "Down"}, {82, "Up"},
    {224, "Left Ctrl"}, {225, "Left Shift"}, {226, "Left Alt"},
    {228, "Right Ctrl"}, {229, "Right Shift"}, {230, "Right Alt"},
    {99, "Keypad 0"}, {89, "Keypad 1"}, {90, "Keypad 2"}, {91, "Keypad 3"},
    {92, "Keypad 4"}, {93, "Keypad 5"}, {94, "Keypad 6"}, {95, "Keypad 7"},
    {96, "Keypad 8"}, {97, "Keypad 9"}, {87, "Keypad +"}, {86, "Keypad -"},
};

int name_to_sc(const char* name) {
    if (!name || !*name) return 0;
    char buf[32];
    size_t i = 0;
    for (; name[i] && i < sizeof(buf) - 1; ++i)
        buf[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
    buf[i] = 0;
    if (!std::strcmp(buf, "none") || !buf[0]) return 0;
    if (!std::strcmp(buf, "enter") || !std::strcmp(buf, "return")) return 40;
    if (!std::strcmp(buf, "esc") || !std::strcmp(buf, "escape")) return 41;
    if (!std::strcmp(buf, "lshift") || !std::strcmp(buf, "left shift")) return 225;
    if (!std::strcmp(buf, "rshift") || !std::strcmp(buf, "right shift")) return 229;
    if (!std::strcmp(buf, "lctrl") || !std::strcmp(buf, "left ctrl")) return 224;
    if (!std::strcmp(buf, "rctrl") || !std::strcmp(buf, "right ctrl")) return 228;
    if (!std::strcmp(buf, "lalt") || !std::strcmp(buf, "left alt")) return 226;
    if (!std::strcmp(buf, "ralt") || !std::strcmp(buf, "right alt")) return 230;
    if (!std::strcmp(buf, "space")) return 44;
    if (!std::strcmp(buf, "tab")) return 43;
    if (!std::strcmp(buf, "up")) return 82;
    if (!std::strcmp(buf, "down")) return 81;
    if (!std::strcmp(buf, "left")) return 80;
    if (!std::strcmp(buf, "right")) return 79;
    for (const auto& e : kScNames) {
        char en[32];
        size_t j = 0;
        for (; e.name[j] && j < sizeof(en) - 1; ++j)
            en[j] = static_cast<char>(std::tolower(static_cast<unsigned char>(e.name[j])));
        en[j] = 0;
        if (!std::strcmp(buf, en)) return e.sc;
    }
    // Single letter A-Z / 0-9
    if (buf[0] && !buf[1]) {
        if (buf[0] >= 'a' && buf[0] <= 'z') return 4 + (buf[0] - 'a');
        if (buf[0] >= '0' && buf[0] <= '9') return (buf[0] == '0') ? 39 : (30 + (buf[0] - '1'));
    }
    return 0;
}

const char* sc_to_name(int sc) {
    if (sc == 0) return "None";
    for (const auto& e : kScNames)
        if (e.sc == sc) return e.name;
    return "None";
}

void seed_kb_defaults() {
    for (int p = 0; p < kMaxKbPlayers; ++p)
        for (int b = 0; b < kPsxPadButtonCount; ++b) g_kb[p][b] = kKbDefaults[b];
}

void load_kb_ini(const char* path) {
    seed_kb_defaults();
    std::ifstream in(path);
    if (!in) return;
    int player = -1;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        size_t i = 0;
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i >= line.size() || line[i] == '#' || line[i] == ';') continue;
        if (line[i] == '[') {
            player = -1;
            if (line.rfind("[player", i) == i) {
                int n = std::atoi(line.c_str() + i + 7);
                if (n >= 1 && n <= kMaxKbPlayers) player = n - 1;
            }
            continue;
        }
        if (player < 0) continue;
        const auto eq = line.find('=', i);
        if (eq == std::string::npos) continue;
        std::string key = line.substr(i, eq - i);
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string val = line.substr(eq + 1);
        while (!val.empty() && std::isspace(static_cast<unsigned char>(val.front()))) val.erase(val.begin());
        const auto comma = val.find(',');
        if (comma != std::string::npos) val.resize(comma); // primary only
        while (!val.empty() && std::isspace(static_cast<unsigned char>(val.back()))) val.pop_back();
        const int b = key_index(key.c_str());
        if (b >= 0) g_kb[player][b] = name_to_sc(val.c_str());
    }
}

void write_kb_ini(const char* path) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out) return;
    out << "# PSXRecomp Keyboard Keybinds (keyboard -> DualShock).\n"
           "# Written by RetComM (psx_keybinds.c-compatible format).\n\n";
    for (int p = 0; p < kMaxKbPlayers; ++p) {
        out << "[player" << (p + 1) << "]\n";
        for (int b = 0; b < kPsxPadButtonCount; ++b)
            out << kIniKeys[b] << " = " << sc_to_name(g_kb[p][b]) << "\n";
        out << "\n";
    }
}

void ensure_kb(const char* path) {
    if (g_kb_init && g_kb_path[0] && path && std::strcmp(g_kb_path, path) == 0) return;
    copy_str(g_kb_path, sizeof(g_kb_path), path ? path : "keybinds.ini");
    load_kb_ini(g_kb_path);
    std::ifstream test(g_kb_path);
    if (!test) write_kb_ini(g_kb_path);
    g_kb_init = 1;
}

bool copy_file_overwrite(const fs::path& from, const fs::path& to, std::string* error) {
    std::error_code ec;
    if (!fs::is_regular_file(from, ec)) {
        if (error) *error = "missing " + from.string();
        return false;
    }
    if (!to.parent_path().empty()) fs::create_directories(to.parent_path(), ec);
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error) *error = ec.message();
        return false;
    }
    return true;
}

} // namespace

const char* psx_pad_button_label(int b) {
    if (b < 0 || b >= kPsxPadButtonCount) return "";
    return kLabels[b];
}

const char* psx_pad_button_ini_key(int b) {
    if (b < 0 || b >= kPsxPadButtonCount) return "";
    return kIniKeys[b];
}

const int* psx_pad_map_all_order() { return kMapAllOrder; }

fs::path psx_platform_input_ini_path(const Paths& paths) {
    return psx_platform_settings_dir(paths) / "input.ini";
}

fs::path psx_platform_keybinds_ini_path(const Paths& paths) {
    return psx_platform_settings_dir(paths) / "keybinds.ini";
}

void psx_pad_binds_init(const Paths& paths) {
    g_pad_init = 0;
    ensure_pad(psx_platform_input_ini_path(paths).string().c_str());
}

void psx_pad_binds_remember(const Paths& paths, const std::string& guid, const std::string& name,
                            int deadzone_pct) {
    ensure_pad(psx_platform_input_ini_path(paths).string().c_str());
    if (guid.empty()) return;
    GuidMap* m = alloc_map(guid.c_str());
    if (!m) return;
    if (!m->name_custom && !name.empty() && name != "Gamepad")
        copy_str(m->name, sizeof(m->name), name.c_str());
    else if (!m->name[0] && !name.empty() && name != "Gamepad")
        copy_str(m->name, sizeof(m->name), name.c_str());
    if (deadzone_pct >= 0) m->deadzone_pct = clamp_dz(deadzone_pct);
    write_pad_ini(g_pad_path);
}

void psx_pad_binds_rename(const Paths& paths, const std::string& guid, const std::string& name) {
    ensure_pad(psx_platform_input_ini_path(paths).string().c_str());
    if (guid.empty() || name.empty()) return;
    GuidMap* m = alloc_map(guid.c_str());
    if (!m) return;
    copy_str(m->name, sizeof(m->name), name.c_str());
    m->name_custom = 1;
    write_pad_ini(g_pad_path);
}

void psx_pad_binds_delete(const Paths& paths, const std::string& guid) {
    ensure_pad(psx_platform_input_ini_path(paths).string().c_str());
    GuidMap* m = find_map(guid.c_str());
    if (!m) return;
    std::memset(m, 0, sizeof(*m));
    write_pad_ini(g_pad_path);
}

void psx_pad_binds_reset(const Paths& paths, const std::string& guid) {
    ensure_pad(psx_platform_input_ini_path(paths).string().c_str());
    GuidMap* m = alloc_map(guid.c_str());
    if (!m) return;
    seed_defaults(m->src);
    write_pad_ini(g_pad_path);
}

void psx_pad_binds_save_profile(const Paths& paths, const std::string& guid,
                                const std::string& name, bool name_custom, int deadzone_pct) {
    ensure_pad(psx_platform_input_ini_path(paths).string().c_str());
    if (guid.empty()) return;
    GuidMap* m = alloc_map(guid.c_str());
    if (!m) return;
    if (!name.empty()) copy_str(m->name, sizeof(m->name), name.c_str());
    m->name_custom = name_custom ? 1 : 0;
    m->deadzone_pct = clamp_dz(deadzone_pct >= 0 ? deadzone_pct : kPsxPadDefaultDeadzonePct);
    write_pad_ini(g_pad_path);
}

void psx_pad_binds_label(const Paths& paths, const std::string& guid, int b, char* out, int cap) {
    ensure_pad(psx_platform_input_ini_path(paths).string().c_str());
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (b < 0 || b >= kPsxPadButtonCount) {
        copy_str(out, static_cast<size_t>(cap), "(unbound)");
        return;
    }
    const char* src = nullptr;
    if (!guid.empty()) {
        GuidMap* m = find_map(guid.c_str());
        if (m) src = m->src[b];
    }
    if (!src || !src[0]) src = g_global[b];
    if (!src || !src[0]) src = kPadDefaults[b];
    if (!src || !src[0]) copy_str(out, static_cast<size_t>(cap), "(unbound)");
    else copy_str(out, static_cast<size_t>(cap), src);
}

void psx_pad_binds_set(const Paths& paths, const std::string& guid, int b, int kind, int code,
                       int axis_dir) {
    ensure_pad(psx_platform_input_ini_path(paths).string().c_str());
    if (guid.empty() || b < 0 || b >= kPsxPadButtonCount) return;
    GuidMap* m = alloc_map(guid.c_str());
    if (!m) return;
    source_from_bind(kind, code, axis_dir, m->src[b], kSrcCap);
    write_pad_ini(g_pad_path);
}

int psx_pad_binds_known_count(const Paths& paths) {
    ensure_pad(psx_platform_input_ini_path(paths).string().c_str());
    int n = 0;
    for (int i = 0; i < kPsxPadMaxKnown; ++i)
        if (g_maps[i].used) ++n;
    return n;
}

bool psx_pad_binds_known_at(const Paths& paths, int index, char* guid, int guid_cap, char* name,
                            int name_cap) {
    ensure_pad(psx_platform_input_ini_path(paths).string().c_str());
    if (index < 0) return false;
    int n = 0;
    for (int i = 0; i < kPsxPadMaxKnown; ++i) {
        if (!g_maps[i].used) continue;
        if (n == index) {
            if (guid && guid_cap > 0) copy_str(guid, static_cast<size_t>(guid_cap), g_maps[i].guid);
            if (name && name_cap > 0) {
                if (g_maps[i].name[0] && std::strcmp(g_maps[i].name, "Gamepad") != 0)
                    copy_str(name, static_cast<size_t>(name_cap), g_maps[i].name);
                else
                    copy_str(name, static_cast<size_t>(name_cap), "Controller");
            }
            return true;
        }
        ++n;
    }
    return false;
}

void psx_pad_binds_name(const Paths& paths, const std::string& guid, char* out, int cap) {
    ensure_pad(psx_platform_input_ini_path(paths).string().c_str());
    if (!out || cap <= 0) return;
    out[0] = 0;
    GuidMap* m = find_map(guid.c_str());
    if (m && m->name[0]) copy_str(out, static_cast<size_t>(cap), m->name);
}

bool psx_pad_binds_name_is_custom(const Paths& paths, const std::string& guid) {
    ensure_pad(psx_platform_input_ini_path(paths).string().c_str());
    GuidMap* m = find_map(guid.c_str());
    return m && m->name_custom;
}

int psx_pad_binds_deadzone(const Paths& paths, const std::string& guid) {
    ensure_pad(psx_platform_input_ini_path(paths).string().c_str());
    GuidMap* m = find_map(guid.c_str());
    if (!m) return kPsxPadDefaultDeadzonePct;
    return clamp_dz(m->deadzone_pct);
}

void psx_pad_binds_set_deadzone(const Paths& paths, const std::string& guid, int deadzone_pct) {
    ensure_pad(psx_platform_input_ini_path(paths).string().c_str());
    if (guid.empty()) return;
    GuidMap* m = alloc_map(guid.c_str());
    if (!m) return;
    m->deadzone_pct = clamp_dz(deadzone_pct);
    write_pad_ini(g_pad_path);
}

void psx_keybinds_init(const Paths& paths) {
    g_kb_init = 0;
    ensure_kb(psx_platform_keybinds_ini_path(paths).string().c_str());
}

void psx_keybinds_label(const Paths& paths, int player, int b, char* out, int cap) {
    ensure_kb(psx_platform_keybinds_ini_path(paths).string().c_str());
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (player < 0 || player >= kMaxKbPlayers || b < 0 || b >= kPsxPadButtonCount) {
        copy_str(out, static_cast<size_t>(cap), "None");
        return;
    }
    copy_str(out, static_cast<size_t>(cap), sc_to_name(g_kb[player][b]));
}

void psx_keybinds_set_scancode(const Paths& paths, int player, int b, int scancode) {
    ensure_kb(psx_platform_keybinds_ini_path(paths).string().c_str());
    if (player < 0 || player >= kMaxKbPlayers || b < 0 || b >= kPsxPadButtonCount) return;
    g_kb[player][b] = scancode;
    write_kb_ini(g_kb_path);
}

void psx_keybinds_reset_player(const Paths& paths, int player) {
    ensure_kb(psx_platform_keybinds_ini_path(paths).string().c_str());
    if (player < 0 || player >= kMaxKbPlayers) return;
    for (int b = 0; b < kPsxPadButtonCount; ++b) g_kb[player][b] = kKbDefaults[b];
    write_kb_ini(g_kb_path);
}

bool apply_psx_input_files(const Paths& paths, const fs::path& game_cwd, std::string* error) {
    if (game_cwd.empty()) {
        if (error) *error = "empty game cwd";
        return false;
    }
    psx_pad_binds_init(paths);
    psx_keybinds_init(paths);
    std::string err;
    const bool a = copy_file_overwrite(psx_platform_input_ini_path(paths), game_cwd / "input.ini", &err);
    if (!a && error) *error = err;
    const bool b =
        copy_file_overwrite(psx_platform_keybinds_ini_path(paths), game_cwd / "keybinds.ini", &err);
    if (!b && error && error->empty()) *error = err;
    return a || b; // ok if at least one applied after seed
}

} // namespace retcomm
