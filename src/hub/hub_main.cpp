#include "hub/hub_boxart.hpp"
#include "hub/hub_model.hpp"
#include "hub/hub_theme.hpp"

#include "retcomm/catalog_sync.hpp"
#include "retcomm/config.hpp"
#include "retcomm/paths.hpp"
#include "retcomm/romm_saves.hpp"
#include "retcomm/self_update.hpp"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_opengl.h>

#include <set>
#include <unordered_set>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Resolve hub assets (fonts, platform icons). Prefer packaged locations, then
// walk up from the app base path for a source-tree assets/<kind> (local builds).
fs::path find_hub_asset_file(const char* kind, const char* filename) {
    std::error_code ec;
    auto try_file = [&](const fs::path& p) -> fs::path {
        if (p.empty()) return {};
        const fs::path c = fs::weakly_canonical(p, ec);
        const fs::path& use = (!ec && !c.empty()) ? c : p;
        if (fs::is_regular_file(use, ec)) return use;
        return {};
    };

    std::vector<fs::path> dirs;
    // AppImage runtime sets APPDIR to the mounted squashfs root.
    if (const char* appdir = std::getenv("APPDIR")) {
        const fs::path ad(appdir);
        dirs.push_back(ad / "usr" / "share" / "retcomm" / kind);
        dirs.push_back(ad / "usr" / "bin" / kind);
        dirs.push_back(ad / kind);
    }
    // SDL3: cached string — do not free. Often …/usr/bin/ inside an AppImage.
    if (const char* base = SDL_GetBasePath()) {
        fs::path b(base);
        dirs.push_back(b);
        dirs.push_back(b / kind);
        dirs.push_back(b / ".." / "share" / "retcomm" / kind);
        dirs.push_back(b / ".." / "Resources" / kind);
        dirs.push_back(b / ".." / ".." / "Resources" / kind);
        // Local cmake: build/ → ../assets/<kind>
        fs::path walk = b;
        for (int i = 0; i < 6 && !walk.empty(); ++i) {
            dirs.push_back(walk / "assets" / kind);
            dirs.push_back(walk / kind);
            dirs.push_back(walk / "share" / "retcomm" / kind);
            walk = walk.parent_path();
        }
    }

    for (const auto& dir : dirs) {
        if (auto hit = try_file(dir / filename); !hit.empty()) return hit;
    }
    return {};
}

fs::path find_hub_font_file(const char* filename) {
    return find_hub_asset_file("fonts", filename);
}

// Catalog platform slug → human-readable label for library cards.
const char* platform_display_name(const std::string& slug) {
    if (slug.empty()) return "Library";
    if (slug == "psx" || slug == "ps1" || slug == "ps") return "PlayStation";
    if (slug == "snes") return "Super Nintendo";
    if (slug == "gba") return "Game Boy Advance";
    if (slug == "n64") return "Nintendo 64";
    if (slug == "genesis" || slug == "md" || slug == "megadrive") return "Genesis / Mega Drive";
    if (slug == "gb" || slug == "dmg") return "Game Boy";
    if (slug == "gbc") return "Game Boy Color";
    if (slug == "nds") return "Nintendo DS";
    if (slug == "psp") return "PlayStation Portable";
    return slug.c_str();
}

// Resolve assets/platforms/<slug>.png (falls back to all.png).
fs::path platform_icon_path(const std::string& slug) {
    std::string key = slug.empty() ? "all" : slug;
    for (char& c : key) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    fs::path hit = find_hub_asset_file("platforms", (key + ".png").c_str());
    if (!hit.empty()) return hit;
    if (key == "ps1" || key == "ps") {
        hit = find_hub_asset_file("platforms", "psx.png");
        if (!hit.empty()) return hit;
    }
    if (key == "md" || key == "megadrive") {
        hit = find_hub_asset_file("platforms", "genesis.png");
        if (!hit.empty()) return hit;
    }
    return find_hub_asset_file("platforms", "all.png");
}

// Load Lato like recomp-ui (18px body, oversample 2). Falls back to ImGui default.
void load_hub_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    const fs::path regular = find_hub_font_file("LatoLatin-Regular.ttf");
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    static const ImWchar kRanges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin-1
        0x2010, 0x2027, // dashes, curly quotes, ellipsis
        0,
    };
    constexpr float kBody = 18.0f;
    bool loaded = false;
    if (!regular.empty()) {
        loaded = io.Fonts->AddFontFromFileTTF(regular.string().c_str(), kBody, &cfg, kRanges) !=
                 nullptr;
        if (loaded) {
            std::fprintf(stderr, "retcomm-hub: loaded UI font %s\n", regular.string().c_str());
        } else {
            std::fprintf(stderr, "retcomm-hub: failed to load UI font %s\n",
                         regular.string().c_str());
        }
    }
    if (!loaded) {
        const char* appdir = std::getenv("APPDIR");
        const char* base = SDL_GetBasePath();
        std::fprintf(stderr,
                     "retcomm-hub: using ImGui default font (LatoLatin-Regular.ttf not found; "
                     "APPDIR=%s SDL_GetBasePath=%s)\n",
                     appdir ? appdir : "(null)", base ? base : "(null)");
        cfg.SizePixels = kBody;
        io.Fonts->AddFontDefault(&cfg);
    }
    io.FontGlobalScale = 1.0f;
}

using retcomm::hub::BoxartCache;
using retcomm::hub::BoxartTexture;
using retcomm::hub::FolderPickTarget;
using retcomm::hub::SetupPath;
using retcomm::hub::HubJob;
using retcomm::hub::HubModel;
using retcomm::hub::Theme;
using retcomm::hub::TitleRow;

void SDLCALL on_folder_dialog(void* userdata, const char* const* filelist, int /*filter*/) {
    auto* hub = static_cast<HubModel*>(userdata);
    if (!hub) return;
    std::lock_guard<std::mutex> lock(hub->folder_pick_mu);
    hub->folder_pick_busy = false;
    if (!filelist) {
        hub->folder_pick_target = FolderPickTarget::None;
        hub->folder_pick_path.clear();
        return; // error
    }
    if (!filelist[0]) {
        hub->folder_pick_target = FolderPickTarget::None;
        hub->folder_pick_path.clear();
        return; // canceled
    }
    hub->folder_pick_path = filelist[0];
}

void begin_folder_pick(HubModel& hub, SDL_Window* window, FolderPickTarget target,
                       const char* current_path) {
    {
        std::lock_guard<std::mutex> lock(hub.folder_pick_mu);
        if (hub.folder_pick_busy) return;
        hub.folder_pick_busy = true;
        hub.folder_pick_target = target;
        hub.folder_pick_path.clear();
    }
    const char* start = nullptr;
    if (current_path && current_path[0] != '\0') start = current_path;
    SDL_ShowOpenFolderDialog(on_folder_dialog, &hub, window, start, false);
}

void SDLCALL on_file_dialog(void* userdata, const char* const* filelist, int /*filter*/) {
    auto* hub = static_cast<HubModel*>(userdata);
    if (!hub) return;
    std::lock_guard<std::mutex> lock(hub->file_pick_mu);
    hub->file_pick_busy = false;
    hub->file_pick_paths.clear();
    if (!filelist || !filelist[0]) {
        hub->file_pick_kind = retcomm::hub::FilePickKind::None;
        hub->file_pick_platform.clear();
        hub->file_pick_title_id.clear();
        return;
    }
    for (const char* const* p = filelist; *p; ++p) hub->file_pick_paths.emplace_back(*p);
}

std::string strip_dot_ext(std::string e) {
    if (!e.empty() && e.front() == '.') e.erase(e.begin());
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

std::string join_ext_pattern(const std::set<std::string>& exts) {
    std::string pat;
    for (const auto& e : exts) {
        if (e.empty() || e == "*") continue;
        if (!pat.empty()) pat.push_back(';');
        pat += e;
    }
    return pat;
}

std::set<std::string> rom_exts_for_platform(const retcomm::Catalog& catalog,
                                            const std::string& platform) {
    // PSX: library expects cue sheets + bin tracks only.
    if (platform == "psx" || platform == "ps1" || platform == "ps") return {"cue", "bin"};
    std::set<std::string> exts;
    for (const auto& t : catalog.titles) {
        if (t.platform != platform) continue;
        for (const auto& e : t.rom_extensions) {
            const std::string s = strip_dot_ext(e);
            if (!s.empty()) exts.insert(s);
        }
    }
    if (!exts.empty()) return exts;
    // Fallbacks aligned with retcomm-catalog platform-defaults.json
    if (platform == "snes") return {"sfc", "smc", "fig", "swc"};
    if (platform == "gba") return {"gba"};
    if (platform == "n64") return {"z64", "n64", "v64"};
    if (platform == "genesis" || platform == "md" || platform == "megadrive")
        return {"bin", "md", "gen", "smd"};
    return {"bin", "rom", "iso", "cue"};
}

std::set<std::string> save_exts_for_platform(const retcomm::Catalog& catalog,
                                             const std::string& platform) {
    if (platform == "psx" || platform == "ps1" || platform == "ps") return {"mcd"};
    std::set<std::string> exts;
    auto add_glob = [&](const std::string& g) {
        const auto dot = g.find_last_of('.');
        if (dot == std::string::npos) return;
        const std::string s = strip_dot_ext(g.substr(dot));
        if (!s.empty()) exts.insert(s);
    };
    for (const auto& t : catalog.titles) {
        if (t.platform != platform) continue;
        for (const auto& g : t.saves_memcard_glob) add_glob(g);
        for (const auto& g : t.saves_sram_glob) add_glob(g);
    }
    if (!exts.empty()) return exts;
    if (platform == "gba") return {"sav"};
    return {"srm", "sav", "mcd", "mcr"};
}

std::set<std::string> bios_exts_for_platform(const std::string& platform) {
    if (platform == "psx" || platform == "ps1" || platform == "ps") return {"bin"};
    return {"bin", "rom", "bios", "img"};
}

void begin_file_pick(HubModel& hub, SDL_Window* window, retcomm::hub::FilePickKind kind,
                     const std::string& platform, const std::string& filter_name,
                     const std::set<std::string>& exts, bool allow_many,
                     const std::string& title_id = {}) {
    const std::string pattern = join_ext_pattern(exts);
    if (pattern.empty()) return;
    {
        std::lock_guard<std::mutex> lock(hub.file_pick_mu);
        if (hub.file_pick_busy) return;
        hub.file_pick_busy = true;
        hub.file_pick_kind = kind;
        hub.file_pick_platform = platform;
        hub.file_pick_title_id = title_id;
        hub.file_pick_paths.clear();
        hub.file_pick_filter_name = filter_name;
        hub.file_pick_filter_pattern = pattern;
    }
    // Pointers must remain valid until the callback runs.
    static SDL_DialogFileFilter filters[1];
    filters[0].name = hub.file_pick_filter_name.c_str();
    filters[0].pattern = hub.file_pick_filter_pattern.c_str();
    SDL_ShowOpenFileDialog(on_file_dialog, &hub, window, filters, 1, nullptr, allow_many);
}

// Path field + native Browse button. Returns true if the text field changed.
bool path_field_with_browse(const char* label, const char* input_id, char* buf, size_t buf_n,
                            HubModel& hub, SDL_Window* window, FolderPickTarget target,
                            const Theme& th) {
    ImGui::TextColored(th.text_muted, "%s", label);
    const float browse_w = 96.f;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float input_w = ImGui::GetContentRegionAvail().x - browse_w - gap;
    bool changed = false;
    if (input_w > 80.f) ImGui::SetNextItemWidth(input_w);
    changed = ImGui::InputText(input_id, buf, buf_n);
    ImGui::SameLine();
    bool busy = false;
    {
        std::lock_guard<std::mutex> lock(hub.folder_pick_mu);
        busy = hub.folder_pick_busy;
    }
    ImGui::BeginDisabled(busy);
    ImGui::PushID(input_id);
    if (ImGui::Button("Browse", ImVec2(browse_w, 0)))
        begin_folder_pick(hub, window, target, buf);
    ImGui::PopID();
    ImGui::EndDisabled();
    return changed;
}

bool title_has_rom_source(const TitleRow& r) { return r.has_rom || r.has_romm; }

// Filter Unsupported Titles: keep rows with a ROM source, or anything already installed.
bool title_passes_library_filter(const TitleRow& r, const HubModel& hub) {
    if (!hub.cfg.filter_unsupported_titles) return true;
    if (title_has_rom_source(r)) return true;
    if (r.installed || r.install_dir_present || r.has_preserved_state) return true;
    return false;
}

const char* chip_label(const TitleRow& r) {
    if (r.update_available) return "UPDATE";
    if (r.installed && r.runtime == "wine") return "WINE";
    if (r.installed) return "INSTALLED";
    if (r.install_dir_present) return "NEEDS SETUP";
    // Keep-saves uninstall: catalog/ROM state, not a broken install.
    if (r.has_rom) return "ROM READY";
    if (r.has_romm) return "ON ROMM";
    if (r.has_preserved_state) return "SAVES KEPT";
    return "CATALOG";
}

ImVec4 chip_color(const TitleRow& r, const Theme& th) {
    if (r.update_available) return th.warn;
    if (r.installed && r.runtime == "wine") return th.good;
    if (r.installed) return th.good;
    if (r.install_dir_present) return th.warn;
    if (r.has_rom) return th.focus;
    if (r.has_romm) return th.accent;
    if (r.has_preserved_state) return th.focus;
    return th.text_muted;
}

// Corner glyph on library boxart (drawn inside the status badge).
enum class TileStatusIcon : int {
    None = 0,
    Catalog,    // installable / catalog-only (no ROM gate)
    NoRom,      // local build needs a ROM; none matched
    RomReady,   // verified ROM on disk — ready to install
    OnRomm,     // RomM match (download) — same download glyph as RomReady
    Installed,  // checkmark
    NeedsSetup, // partial install
    Update,     // small up-chevron (center update overlay still shown)
};

TileStatusIcon tile_status_icon(const TitleRow& r) {
    if (r.update_available) return TileStatusIcon::Update;
    if (r.installed) return TileStatusIcon::Installed;
    if (r.install_dir_present) return TileStatusIcon::NeedsSetup;
    if (r.has_rom) return TileStatusIcon::RomReady;
    if (r.has_romm) return TileStatusIcon::OnRomm;
    // Prebuilt zip (or no local-build ROM requirement) → not blocked.
    const bool needs_rom = r.supports_local_build && !r.can_prebuilt_install;
    if (needs_rom) return TileStatusIcon::NoRom;
    return TileStatusIcon::Catalog;
}

// Glyph ink on the badge fill (dark on bright accents, light on muted).
ImU32 status_glyph_ink(const ImVec4& badge_col, const Theme& th) {
    const float lum = 0.2126f * badge_col.x + 0.7152f * badge_col.y + 0.0722f * badge_col.z;
    if (lum > 0.55f) return ImGui::ColorConvertFloat4ToU32(th.background);
    return IM_COL32(245, 247, 252, 255);
}

void draw_status_badge_glyph(ImDrawList* dl, const ImVec2& c, float rad, TileStatusIcon icon,
                             ImU32 ink) {
    switch (icon) {
    case TileStatusIcon::None:
        break;
    case TileStatusIcon::Installed: {
        // Checkmark.
        const ImVec2 a(c.x - rad * 0.45f, c.y + rad * 0.02f);
        const ImVec2 b(c.x - rad * 0.08f, c.y + rad * 0.38f);
        const ImVec2 d(c.x + rad * 0.48f, c.y - rad * 0.36f);
        dl->AddLine(a, b, ink, 2.1f);
        dl->AddLine(b, d, ink, 2.1f);
        break;
    }
    case TileStatusIcon::NeedsSetup: {
        // Exclamation.
        dl->AddLine(ImVec2(c.x, c.y - rad * 0.42f), ImVec2(c.x, c.y + rad * 0.08f), ink, 2.2f);
        dl->AddCircleFilled(ImVec2(c.x, c.y + rad * 0.38f), rad * 0.14f, ink, 8);
        break;
    }
    case TileStatusIcon::Update: {
        // Up chevron.
        const float h = rad * 0.42f;
        const float w = rad * 0.40f;
        dl->AddTriangleFilled(ImVec2(c.x, c.y - h), ImVec2(c.x - w, c.y + h * 0.35f),
                              ImVec2(c.x + w, c.y + h * 0.35f), ink);
        break;
    }
    case TileStatusIcon::RomReady:
    case TileStatusIcon::OnRomm: {
        // Download: arrow into tray.
        const float h = rad * 0.36f;
        const float w = rad * 0.34f;
        const ImVec2 tip(c.x, c.y + h * 0.55f);
        dl->AddTriangleFilled(tip, ImVec2(c.x - w, c.y - h * 0.15f),
                              ImVec2(c.x + w, c.y - h * 0.15f), ink);
        const float stem_w = w * 0.38f;
        dl->AddRectFilled(ImVec2(c.x - stem_w * 0.5f, c.y - h * 0.70f),
                          ImVec2(c.x + stem_w * 0.5f, c.y - h * 0.05f), ink, 1.2f);
        dl->AddLine(ImVec2(c.x - rad * 0.48f, c.y + rad * 0.55f),
                    ImVec2(c.x + rad * 0.48f, c.y + rad * 0.55f), ink, 1.8f);
        break;
    }
    case TileStatusIcon::NoRom: {
        // Slash-circle (unavailable).
        dl->AddCircle(c, rad * 0.55f, ink, 16, 1.8f);
        dl->AddLine(ImVec2(c.x - rad * 0.38f, c.y + rad * 0.38f),
                    ImVec2(c.x + rad * 0.38f, c.y - rad * 0.38f), ink, 1.8f);
        break;
    }
    case TileStatusIcon::Catalog: {
        // Soft disc / catalog mark.
        dl->AddCircle(c, rad * 0.48f, ink, 16, 1.7f);
        dl->AddCircleFilled(c, rad * 0.14f, ink, 10);
        break;
    }
    }
}

bool accent_button(const char* label, const Theme& th, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, th.accent_button);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th.accent_button_hovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, th.accent_button_active);
    ImGui::PushStyleColor(ImGuiCol_Text, th.accent_text);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}

// Play / success actions — muted green fill; bright th.good stays for status text.
bool good_button(const char* label, const Theme& th, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, th.good_button);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th.good_button_hovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, th.good_button_active);
    ImGui::PushStyleColor(ImGuiCol_Text, th.good_button_text);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}

// Destructive actions — muted red fill (mirrors good_button contrast).
bool danger_button(const char* label, const Theme& /*th*/, const ImVec2& size = ImVec2(0, 0)) {
    const ImVec4 btn(0.561f, 0.165f, 0.200f, 1.f);       // #8F2A33
    const ImVec4 hovered(0.655f, 0.220f, 0.255f, 1.f);   // #A73841
    const ImVec4 active(0.455f, 0.130f, 0.165f, 1.f);    // #74212A
    const ImVec4 text(0.980f, 0.920f, 0.925f, 1.f);      // #FAEBEB
    ImGui::PushStyleColor(ImGuiCol_Button, btn);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
    ImGui::PushStyleColor(ImGuiCol_Text, text);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}

// RomM brand purple (docs.romm.app brand guidelines: #553e98 / #371f69).
// When inside BeginDisabled (e.g. RomM not configured), keep default grey chrome.
bool romm_button(const char* label, const Theme& /*th*/, const ImVec2& size = ImVec2(0, 0)) {
    const bool disabled =
        (GImGui != nullptr) && ((GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0);
    if (disabled) return ImGui::Button(label, size);

    const ImVec4 btn(0.333f, 0.243f, 0.596f, 1.f);         // #553E98
    const ImVec4 hovered(0.420f, 0.322f, 0.690f, 1.f);     // #6B52B0
    const ImVec4 active(0.216f, 0.122f, 0.412f, 1.f);      // #371F69
    const ImVec4 text(0.929f, 0.898f, 0.973f, 1.f);        // #EDE5F8
    ImGui::PushStyleColor(ImGuiCol_Button, btn);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
    ImGui::PushStyleColor(ImGuiCol_Text, text);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}

void draw_marquee(HubModel& hub, const Theme& th, float width) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float h = 72.f;
    dl->AddRectFilledMultiColor(p0, ImVec2(p0.x + width, p0.y + h),
                                ImGui::ColorConvertFloat4ToU32(th.background2),
                                ImGui::ColorConvertFloat4ToU32(th.background2),
                                ImGui::ColorConvertFloat4ToU32(th.background),
                                ImGui::ColorConvertFloat4ToU32(th.background));
    // Neon underline: CRT violet → icon green.
    dl->AddRectFilledMultiColor(ImVec2(p0.x, p0.y + h - 3), ImVec2(p0.x + width, p0.y + h),
                                ImGui::ColorConvertFloat4ToU32(th.accent),
                                ImGui::ColorConvertFloat4ToU32(th.good),
                                ImGui::ColorConvertFloat4ToU32(th.good),
                                ImGui::ColorConvertFloat4ToU32(th.accent));

    ImGui::Dummy(ImVec2(width, h));
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 20.f, p0.y + 14.f));
    ImGui::PushStyleColor(ImGuiCol_Text, th.good);
    ImGui::TextUnformatted("RetComM");
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 20.f, p0.y + 40.f));
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextUnformatted("Retro Compilation Manager");
    ImGui::PopStyleColor();

    // Status chip (left of top-right actions).
    std::string chip = "Ready";
    ImVec4 chip_col = th.good;
    {
        std::string st;
        bool game_upd = false;
        {
            std::lock_guard<std::mutex> lock(hub.mu);
            st = hub.status;
            for (const auto& r : hub.rows) {
                if (r.update_available) {
                    game_upd = true;
                    break;
                }
            }
        }
        if (hub.job_running.load()) {
            chip = st.empty() ? "Working…" : st;
            if (chip.size() > 42) chip = chip.substr(0, 39) + "…";
            chip_col = th.accent;
        } else if (hub.launch_running.load()) {
            chip = "Launching…";
            chip_col = th.accent;
        } else if (hub.launcher_update_prompt_pending.load() ||
                   hub.toolchain_prompt_pending.load() || hub.toolchain_update_available ||
                   game_upd) {
            chip = "Update available";
            chip_col = th.warn;
        } else if (!st.empty() && st != "Ready") {
            // Keep last notable status briefly as chip when idle.
            if (st == "Up to date" || st.find("complete") != std::string::npos) {
                chip = st.size() > 42 ? st.substr(0, 39) + "…" : st;
                chip_col = th.text_muted;
            }
        }
    }
    {
        const ImVec2 chip_sz = ImGui::CalcTextSize(chip.c_str());
        const float chip_pad_x = 10.f;
        const float chip_pad_y = 5.f;
        const float chip_w = chip_sz.x + chip_pad_x * 2.f;
        const float chip_h = chip_sz.y + chip_pad_y * 2.f;
        // Clear of both brand lines (title + subtitle), not just "RetComM".
        const float brand_w =
            std::max(ImGui::CalcTextSize("RetComM").x,
                     ImGui::CalcTextSize("Retro Compilation Manager").x);
        const float chip_x = p0.x + 20.f + brand_w + 28.f;
        const float chip_y = p0.y + (h - chip_h) * 0.5f;
        const ImVec2 c0(chip_x, chip_y);
        const ImVec2 c1(chip_x + chip_w, chip_y + chip_h);
        dl->AddRectFilled(c0, c1, ImGui::ColorConvertFloat4ToU32(th.background2), 6.f);
        dl->AddRect(c0, c1, ImGui::ColorConvertFloat4ToU32(chip_col), 6.f, 0, 1.5f);
        dl->AddText(ImVec2(chip_x + chip_pad_x, chip_y + chip_pad_y),
                    ImGui::ColorConvertFloat4ToU32(chip_col), chip.c_str());
    }

    // Top-right: Add/Scan Files + Check for Updates + Menu, or Back when editing settings.
    constexpr float kMenuH = 36.f;
    constexpr float kBtnGap = 8.f;
    const bool in_settings = hub.show_settings || hub.show_romm_settings;
    const char* btn_label = in_settings ? "Back to Library" : "Menu";
    const char* library_label = "Add/Scan Files";
    const char* updates_label = "Check for Updates";
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.f, 8.f));
    const float menu_w =
        std::max(88.f, ImGui::CalcTextSize(btn_label).x + ImGui::GetStyle().FramePadding.x * 2.f);
    const float library_w =
        std::max(100.f, ImGui::CalcTextSize(library_label).x + ImGui::GetStyle().FramePadding.x * 2.f);
    const float updates_w =
        std::max(120.f, ImGui::CalcTextSize(updates_label).x + ImGui::GetStyle().FramePadding.x * 2.f);
    const float btn_y = p0.y + (h - kMenuH) * 0.5f;
    float btn_x = p0.x + width - 16.f - menu_w;
    if (!in_settings) {
        ImGui::SetCursorScreenPos(ImVec2(btn_x - kBtnGap - updates_w - kBtnGap - library_w, btn_y));
        if (ImGui::Button(library_label, ImVec2(library_w, kMenuH)))
            hub.pending_open_library = true;
        ImGui::SetCursorScreenPos(ImVec2(btn_x - kBtnGap - updates_w, btn_y));
        ImGui::BeginDisabled(hub.job_running.load());
        if (ImGui::Button(updates_label, ImVec2(updates_w, kMenuH)))
            hub.start_job(HubJob::CheckUpdates);
        ImGui::EndDisabled();
    }
    ImGui::SetCursorScreenPos(ImVec2(btn_x, btn_y));
    if (ImGui::Button(btn_label, ImVec2(menu_w, kMenuH))) {
        if (in_settings) {
            hub.show_settings = false;
            hub.show_romm_settings = false;
            hub.settings.dirty = false;
            hub.romm_settings.dirty = false;
        } else {
            hub.pending_open_menu = true;
        }
    }
    ImGui::PopStyleVar();

    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h + 8.f));
}

// Typical Named_Boxarts width/height per platform (placeholder when art missing).
// Real textures always size with their own aspect — never stretched to this.
float platform_boxart_aspect(const std::string& slug) {
    if (slug == "psx" || slug == "ps1" || slug == "ps" || slug == "psp") return 1.0f;
    if (slug == "gba" || slug == "gb" || slug == "gbc" || slug == "dmg") return 0.62f;
    if (slug == "nds") return 0.90f;
    // Cartridge / NA retail boxes tend to be portrait.
    if (slug == "snes" || slug == "n64" || slug == "genesis" || slug == "md" ||
        slug == "megadrive")
        return 0.72f;
    return 0.75f;
}

// Fit image into max box without cropping or stretching (letterbox unused space).
ImVec2 contain_size(float src_w, float src_h, float max_w, float max_h) {
    if (src_w <= 0.f || src_h <= 0.f) return ImVec2(max_w, max_h);
    const float scale = std::min(max_w / src_w, max_h / src_h);
    return ImVec2(src_w * scale, src_h * scale);
}

void draw_ellipsized_centered(const char* text, float max_w, const ImVec4& col) {
    if (!text || !text[0] || max_w <= 4.f) return;
    std::string shown = text;
    ImVec2 sz = ImGui::CalcTextSize(shown.c_str());
    if (sz.x > max_w) {
        const char* ell = "...";
        while (shown.size() > 1 &&
               ImGui::CalcTextSize((shown + ell).c_str()).x > max_w)
            shown.pop_back();
        shown += ell;
        sz = ImGui::CalcTextSize(shown.c_str());
    }
    const float x = ImGui::GetCursorScreenPos().x;
    const float y = ImGui::GetCursorScreenPos().y;
    ImGui::SetCursorScreenPos(ImVec2(x + std::max(0.f, (max_w - sz.x) * 0.5f), y));
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(shown.c_str());
    ImGui::PopStyleColor();
}

// Portrait/platform grid tile: art on top (aspect preserved), labels below.
// Content is clipped inside the frame; colored border is drawn last on top.
// Returns total tile height used.
void draw_art_dim(ImDrawList* dl, const ImVec2& art0, const ImVec2& art1, float radius) {
    dl->AddRectFilled(art0, art1, IM_COL32(8, 10, 18, 155), radius, ImDrawFlags_RoundCornersTop);
}

void draw_busy_spinner(ImDrawList* dl, const ImVec2& art0, const ImVec2& art1, const Theme& th) {
    const ImVec2 c((art0.x + art1.x) * 0.5f, (art0.y + art1.y) * 0.5f);
    const float rad = std::min(art1.x - art0.x, art1.y - art0.y) * 0.16f;
    const float t = static_cast<float>(ImGui::GetTime()) * 4.8f;
    constexpr int kSeg = 11;
    for (int i = 0; i < kSeg; ++i) {
        const float a0 = t + (static_cast<float>(i) / static_cast<float>(kSeg)) * 6.2831853f;
        const float a1 = a0 + 0.5f;
        ImVec4 col = th.accent;
        col.w = 0.22f + 0.78f * (static_cast<float>(i + 1) / static_cast<float>(kSeg));
        dl->PathClear();
        dl->PathArcTo(c, rad, a0, a1, 10);
        dl->PathStroke(ImGui::ColorConvertFloat4ToU32(col), 0, 3.0f);
    }
}

float draw_grid_tile(const ImVec2& tile_min, float tile_w, bool selected, const Theme& th,
                     const BoxartTexture* tex, float art_aspect_wh, const char* title,
                     const char* subtitle, const ImVec4* badge_col, bool busy_spinner = false,
                     bool dim_art = false, bool update_overlay = false,
                     TileStatusIcon status_icon = TileStatusIcon::None) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    constexpr float kArtPad = 8.f;
    constexpr float kLabelGap = 4.f;
    constexpr float kFrameInset = 2.f; // keep art/badge inside the border stroke
    const float line_h = ImGui::GetTextLineHeight();
    const float label_h = line_h + (subtitle && subtitle[0] ? line_h + 2.f : 0.f) + 6.f;

    // Art frame height follows platform/boxart aspect (width fixed by grid column).
    const float art_box_w = tile_w;
    const float art_box_h = art_box_w / std::max(0.35f, art_aspect_wh);
    const float tile_h = art_box_h + kLabelGap + label_h;
    const ImVec2 tile_max(tile_min.x + tile_w, tile_min.y + tile_h);

    const ImU32 bg = ImGui::ColorConvertFloat4ToU32(selected ? th.panel_hovered : th.panel);
    dl->AddRectFilled(tile_min, tile_max, bg, th.radius_sm);

    // Clip all inner content so it cannot paint over the frame edge.
    dl->PushClipRect(ImVec2(tile_min.x + kFrameInset, tile_min.y + kFrameInset),
                     ImVec2(tile_max.x - kFrameInset, tile_max.y - kFrameInset), true);

    const ImVec2 art0(tile_min.x, tile_min.y);
    const ImVec2 art1(tile_min.x + art_box_w, tile_min.y + art_box_h);
    dl->AddRectFilled(art0, art1, ImGui::ColorConvertFloat4ToU32(th.control), th.radius_sm,
                      ImDrawFlags_RoundCornersTop);

    if (tex && tex->gl_id && tex->width > 0 && tex->height > 0) {
        const ImVec2 fit =
            contain_size(static_cast<float>(tex->width), static_cast<float>(tex->height),
                         art_box_w - kArtPad * 2.f, art_box_h - kArtPad * 2.f);
        const float ix = art0.x + (art_box_w - fit.x) * 0.5f;
        const float iy = art0.y + (art_box_h - fit.y) * 0.5f;
        dl->AddImage((ImTextureID)(intptr_t)tex->gl_id, ImVec2(ix, iy),
                     ImVec2(ix + fit.x, iy + fit.y));
    }

    if (dim_art || busy_spinner) draw_art_dim(dl, art0, art1, th.radius_sm);
    if (busy_spinner) draw_busy_spinner(dl, art0, art1, th);

    // Update-available: centered soft disc + up arrow over dimmed cover.
    if (update_overlay && !busy_spinner) {
        const ImVec2 c((art0.x + art1.x) * 0.5f, (art0.y + art1.y) * 0.5f);
        const float s = std::min(art1.x - art0.x, art1.y - art0.y);
        const float rad = s * 0.20f;
        dl->AddCircleFilled(c, rad, IM_COL32(12, 16, 28, 220), 36);
        dl->AddCircle(c, rad, ImGui::ColorConvertFloat4ToU32(th.good), 36, 2.2f);
        const ImU32 arrow = ImGui::ColorConvertFloat4ToU32(th.good);
        const float h = rad * 0.58f;
        const float w = rad * 0.48f;
        const ImVec2 tip(c.x, c.y - h * 0.72f);
        const ImVec2 bl(c.x - w, c.y + h * 0.08f);
        const ImVec2 br(c.x + w, c.y + h * 0.08f);
        dl->AddTriangleFilled(tip, bl, br, arrow);
        const float stem_w = w * 0.42f;
        dl->AddRectFilled(ImVec2(c.x - stem_w * 0.5f, c.y - h * 0.08f),
                          ImVec2(c.x + stem_w * 0.5f, c.y + h * 0.72f), arrow, 2.f);
    }

    if (badge_col) {
        // Slightly larger when carrying a glyph so the mark stays legible.
        const float r = (status_icon != TileStatusIcon::None) ? 10.f : 7.f;
        const ImVec2 c(art1.x - (12.f + (r - 7.f)), art0.y + (12.f + (r - 7.f)));
        dl->AddCircleFilled(c, r, ImGui::ColorConvertFloat4ToU32(*badge_col), 20);
        dl->AddCircle(c, r, ImGui::ColorConvertFloat4ToU32(th.background), 20, 1.5f);
        if (status_icon != TileStatusIcon::None)
            draw_status_badge_glyph(dl, c, r, status_icon, status_glyph_ink(*badge_col, th));
    }

    ImGui::SetCursorScreenPos(ImVec2(tile_min.x + 4.f, tile_min.y + art_box_h + kLabelGap));
    draw_ellipsized_centered(title, tile_w - 8.f, th.text);
    if (subtitle && subtitle[0]) {
        ImGui::SetCursorScreenPos(
            ImVec2(tile_min.x + 4.f, tile_min.y + art_box_h + kLabelGap + line_h + 2.f));
        draw_ellipsized_centered(subtitle, tile_w - 8.f, th.text_muted);
    }

    dl->PopClipRect();

    // Frame stroke last so selection/border always sits above clipped content.
    const float border_t = selected ? 2.5f : 1.5f;
    dl->AddRect(tile_min, tile_max,
                ImGui::ColorConvertFloat4ToU32(selected ? th.accent : th.border), th.radius_sm,
                0, border_t);
    return tile_h;
}

void select_first_visible_title(HubModel& hub) {
    hub.selected = 0;
    for (int i = 0; i < static_cast<int>(hub.rows.size()); ++i) {
        const TitleRow& cand = hub.rows[static_cast<size_t>(i)];
        if (!title_passes_library_filter(cand, hub)) continue;
        if (hub.library_platform.empty() || cand.platform == hub.library_platform) {
            hub.selected = i;
            break;
        }
    }
}

void enter_platform_titles(HubModel& hub, const char* platform_id) {
    if (!platform_id || !platform_id[0]) return;
    hub.library_nav = retcomm::hub::LibraryNav::Titles;
    hub.library_platform = platform_id;
    bool ok = hub.selected >= 0 && hub.selected < static_cast<int>(hub.rows.size());
    if (ok) {
        const TitleRow& sel = hub.rows[static_cast<size_t>(hub.selected)];
        if (!title_passes_library_filter(sel, hub)) ok = false;
        if (ok && sel.platform != hub.library_platform) ok = false;
    }
    if (!ok) select_first_visible_title(hub);
}

void draw_library(HubModel& hub, BoxartCache& boxart, const Theme& th) {
    const bool job_busy = hub.job_running.load();
    const bool install_busy = job_busy && retcomm::hub::hub_job_is_install(hub.job);
    std::string busy_title_id;
    if (install_busy) busy_title_id = hub.job_title_id;
    ImGui::BeginChild("library", ImVec2(0, 0), ImGuiChildFlags_Borders);

    // Header: LIBRARY on platform picker; platform display name on title grid.
    // Fixed row height so the divider matches with/without Back (Back still fits).
    {
        const bool show_back = hub.library_nav == retcomm::hub::LibraryNav::Titles;
        const char* header = "LIBRARY";
        if (show_back && !hub.library_platform.empty())
            header = platform_display_name(hub.library_platform);

        constexpr float kHeaderScale = 1.25f;
        constexpr float kBackPadX = 14.f;
        constexpr float kBackPadY = 6.f;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kBackPadX, kBackPadY));
        const ImVec2 back_txt = ImGui::CalcTextSize("Back");
        const float back_w = back_txt.x + kBackPadX * 2.f;
        const float back_h = back_txt.y + kBackPadY * 2.f;
        ImGui::PopStyleVar();

        ImGui::SetWindowFontScale(kHeaderScale);
        const float title_h = ImGui::GetTextLineHeight();
        ImGui::SetWindowFontScale(1.f);
        // Same band on both screens — tall enough for the padded Back control.
        const float row_h = std::max(title_h, back_h);

        const ImVec2 row0 = ImGui::GetCursorScreenPos();
        const float content_right =
            ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, row_h));

        ImGui::SetWindowFontScale(kHeaderScale);
        const ImVec2 title_sz = ImGui::CalcTextSize(header);
        ImGui::SetCursorScreenPos(
            ImVec2(row0.x, row0.y + (row_h - title_sz.y) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextUnformatted(header);
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.f);

        if (show_back) {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kBackPadX, kBackPadY));
            ImGui::SetCursorScreenPos(
                ImVec2(content_right - back_w, row0.y + (row_h - back_h) * 0.5f));
            if (ImGui::Button("Back", ImVec2(back_w, back_h))) {
                hub.library_nav = retcomm::hub::LibraryNav::Platforms;
                hub.library_platform.clear();
            }
            ImGui::PopStyleVar();
        }

        ImGui::SetCursorScreenPos(ImVec2(row0.x, row0.y + row_h + 4.f));
        ImGui::Separator();
    }

    std::lock_guard<std::mutex> lock(hub.mu);

    constexpr float kGap = 12.f;
    const float avail_w = ImGui::GetContentRegionAvail().x;

    // Wrapping grid: fixed column width; tile height follows boxart aspect.
    auto begin_grid = [&](float prefer_tile_w, int min_cols, int max_cols) -> float {
        int cols = (std::max)(min_cols, static_cast<int>((avail_w + kGap) / (prefer_tile_w + kGap)));
        cols = (std::clamp)(cols, min_cols, max_cols);
        const float tile_w = (avail_w - kGap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
        return (std::max)(72.f, tile_w);
    };

    if (hub.library_nav == retcomm::hub::LibraryNav::Platforms) {
        std::map<std::string, int> counts;
        for (const auto& r : hub.rows) {
            if (!title_passes_library_filter(r, hub)) continue;
            if (!r.platform.empty()) counts[r.platform]++;
        }
        std::vector<std::string> platforms;
        platforms.reserve(counts.size());
        for (const auto& [plat, _] : counts) platforms.push_back(plat);
        std::sort(platforms.begin(), platforms.end());

        struct PlatCard {
            std::string id;
            std::string title;
            std::string subtitle;
        };
        std::vector<PlatCard> cards;
        for (const auto& plat : platforms) {
            const int n = counts[plat];
            cards.push_back({plat, platform_display_name(plat),
                             std::to_string(n) + (n == 1 ? " title" : " titles")});
        }

        // Platform tiles: portrait cards (~ES style).
        const float tile_w = begin_grid(148.f, 2, 8);
        const float start_x = ImGui::GetCursorPosX();
        float x = 0.f;
        float y = ImGui::GetCursorPosY();
        float row_h = 0.f;

        for (const auto& card : cards) {
            if (x > 0.f && x + tile_w > avail_w + 0.5f) {
                x = 0.f;
                y += row_h + kGap;
                row_h = 0.f;
            }
            ImGui::PushID(card.id.c_str());
            ImGui::SetCursorPos(ImVec2(start_x + x, y));
            const ImVec2 tile_min = ImGui::GetCursorScreenPos();

            const fs::path icon = platform_icon_path(card.id);
            const BoxartTexture* tex =
                icon.empty() ? nullptr : boxart.get(std::string("platform:") + card.id, icon);
            // Controller icons are landscape; tile frame is portrait — contain, no stretch.
            constexpr float kPlatAspect = 0.78f;
            const float tile_h =
                draw_grid_tile(tile_min, tile_w, false, th, tex, kPlatAspect, card.title.c_str(),
                               card.subtitle.c_str(), nullptr);

            ImGui::SetCursorScreenPos(tile_min);
            if (ImGui::InvisibleButton("##plat", ImVec2(tile_w, tile_h)))
                enter_platform_titles(hub, card.id.c_str());

            row_h = (std::max)(row_h, tile_h);
            x += tile_w + kGap;
            ImGui::PopID();
        }
        ImGui::SetCursorPos(ImVec2(start_x, y + row_h + kGap));
        ImGui::Dummy(ImVec2(0, 0));
    } else {
        // Title cover grid for one platform (no mixed "All" view).
        const std::string& plat_filter = hub.library_platform;
        const float filter_aspect = platform_boxart_aspect(plat_filter);
        const float prefer_w = (filter_aspect >= 0.95f) ? 140.f : 120.f;
        const float tile_w = begin_grid(prefer_w, 2, 10);

        const float start_x = ImGui::GetCursorPosX();
        float x = 0.f;
        float y = ImGui::GetCursorPosY();
        float row_h = 0.f;

        for (int i = 0; i < static_cast<int>(hub.rows.size()); ++i) {
            const TitleRow& r = hub.rows[static_cast<size_t>(i)];
            if (r.platform != plat_filter) continue;
            if (!title_passes_library_filter(r, hub)) continue;

            const float aspect = filter_aspect;

            if (x > 0.f && x + tile_w > avail_w + 0.5f) {
                x = 0.f;
                y += row_h + kGap;
                row_h = 0.f;
            }

            ImGui::PushID(r.id.c_str());
            ImGui::SetCursorPos(ImVec2(start_x + x, y));
            const ImVec2 tile_min = ImGui::GetCursorScreenPos();
            const bool selected = (hub.selected == i);
            const bool title_busy = install_busy && r.id == busy_title_id;
            const bool needs_update = r.update_available;
            const bool dim_art = !r.installed || needs_update;
            const BoxartTexture* tex =
                r.boxart_path.empty() ? nullptr : boxart.get(r.id, r.boxart_path);
            const ImVec4 badge = chip_color(r, th);
            const TileStatusIcon status = tile_status_icon(r);
            const float tile_h =
                draw_grid_tile(tile_min, tile_w, selected, th, tex, aspect, r.name.c_str(),
                               nullptr, &badge, title_busy, dim_art, needs_update, status);

            ImGui::SetCursorScreenPos(tile_min);
            if (ImGui::InvisibleButton("##row", ImVec2(tile_w, tile_h))) {
                hub.selected = i;
                hub.detail_scroll_top = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip("%s\n%s · %s\n%s", r.name.c_str(),
                                  platform_display_name(r.platform), r.kind.c_str(),
                                  chip_label(r));
            }

            row_h = (std::max)(row_h, tile_h);
            x += tile_w + kGap;
            ImGui::PopID();
        }
        ImGui::SetCursorPos(ImVec2(start_x, y + row_h + kGap));
        ImGui::Dummy(ImVec2(0, 0));
    }
    ImGui::EndChild();
}

void center_modal_next() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
}

// Dismiss the topmost modal when the user clicks the dimmed backdrop (not the
// modal itself). Nested confirms stay sticky until they are topmost. Call just
// before EndPopup() inside BeginPopupModal. Skip first-run setup wizard.
void close_modal_on_outside_click() {
    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
    if (ImGui::GetTopMostPopupModal() != ImGui::GetCurrentWindow()) return;
    const ImVec2 tl = ImGui::GetWindowPos();
    const ImVec2 br(tl.x + ImGui::GetWindowSize().x, tl.y + ImGui::GetWindowSize().y);
    const ImVec2 m = ImGui::GetIO().MousePos;
    if (m.x >= tl.x && m.y >= tl.y && m.x < br.x && m.y < br.y) return;
    ImGui::CloseCurrentPopup();
}

void draw_detail_save_controls(HubModel& hub, const TitleRow& row, const Theme& th, bool busy,
                               SDL_Window* window) {
    struct PickerState {
        std::string title_id;
        int slot = 0;   // 0 = cart/save file, 1 = memcard 1, 2 = memcard 2
        int sel = -2;   // -1 = Empty Card Slot (slot 2 only), >=0 = save index
        bool renaming = false;
        char rename_buf[256]{};
    };
    static PickerState ps;

    auto slot_button_label = [](const TitleRow& r, int index, bool is_card2) -> std::string {
        if (is_card2 && index < 0) return "Empty Card Slot";
        if (index >= 0 && index < static_cast<int>(r.save_labels.size()))
            return r.save_labels[static_cast<size_t>(index)];
        if (!r.save_labels.empty() && !is_card2) return r.save_labels.front();
        return "None";
    };

    auto open_picker = [&](int slot) {
        ps.title_id = row.id;
        ps.slot = slot;
        ps.renaming = false;
        ps.rename_buf[0] = '\0';
        if (slot == 2)
            ps.sel = row.preferred_save_card2_index;
        else if (row.preferred_save_index >= 0)
            ps.sel = row.preferred_save_index;
        else
            ps.sel = row.save_labels.empty() ? -2 : 0;
        ImGui::OpenPopup("###memcard_picker");
    };

    if (row.dual_memcard) {
        ImGui::TextColored(th.text_muted, "Memory card 1 & 2");
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ImGui::GetStyle().ItemSpacing.x, 4.f));
        {
            const std::string lab =
                slot_button_label(row, row.preferred_save_index, false) + "##memcard1_btn";
            if (ImGui::Button(lab.c_str(), ImVec2(-1, 0))) open_picker(1);
        }
        {
            const std::string lab =
                slot_button_label(row, row.preferred_save_card2_index, true) + "##memcard2_btn";
            if (ImGui::Button(lab.c_str(), ImVec2(-1, 0))) open_picker(2);
        }
        ImGui::PopStyleVar();
    } else {
        ImGui::TextColored(th.text_muted, "Save file");
        const std::string lab =
            slot_button_label(row, row.preferred_save_index, false) + "##save_btn";
        if (ImGui::Button(lab.c_str(), ImVec2(-1, 0))) open_picker(0);
    }

    if (!ImGui::IsPopupOpen("###memcard_picker")) return;
    constexpr float kW = 560.f;
    constexpr float kH = 720.f;
    center_modal_next();
    ImGui::SetNextWindowSizeConstraints(ImVec2(kW, kH), ImVec2(kW, kH));
    ImGui::SetNextWindowSize(ImVec2(kW, kH), ImGuiCond_Appearing);

    const char* slot_title = "Save file";
    if (ps.slot == 1) slot_title = "Memory Card 1";
    else if (ps.slot == 2) slot_title = "Memory Card 2";
    char picker_title[96];
    std::snprintf(picker_title, sizeof(picker_title), "%s###memcard_picker", slot_title);
    if (!ImGui::BeginPopupModal(
            picker_title, nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        return;

    const TitleRow* live = nullptr;
    for (const auto& r : hub.rows) {
        if (r.id == ps.title_id) {
            live = &r;
            break;
        }
    }
    if (!live) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    ImGui::TextWrapped("%s", live->name.c_str());
    ImGui::Separator();

    const bool allow_empty = (ps.slot == 2);
    // Negative height: fill remaining space after the footer (list scrolls; window does not).
    // Include ItemSpacing after every widget (Dummy / button rows / Close), plus a safety
    // margin — under-reserving clips Close under NoScrollbar.
    constexpr float kPadTop = 12.f;
    constexpr float kPadClose = 14.f;
    constexpr float kFooterSafety = 12.f;
    const float fh = ImGui::GetFrameHeight();
    const float sp = ImGui::GetStyle().ItemSpacing.y;
    const float footer =
        ps.renaming
            // Dummy + InputText + Confirm + Cancel + Dummy + Close (+ spacing each)
            ? (sp + kPadTop + sp + fh + sp + fh + sp + fh + sp + kPadClose + sp + fh +
               kFooterSafety)
            // Dummy + 2 action rows + Dummy + Close (+ spacing each)
            : (sp + kPadTop + sp + fh + sp + fh + sp + kPadClose + sp + fh + kFooterSafety);
    ImGui::BeginChild("##memcard_list", ImVec2(-FLT_MIN, -footer), ImGuiChildFlags_Borders);

    auto apply_selection = [&](int sel) {
        std::string err;
        bool ok = false;
        if (ps.slot == 2) {
            if (sel < 0)
                ok = hub.set_title_preferred_save_card2(ps.title_id, retcomm::kBlankMemcardId,
                                                        &err);
            else if (sel < static_cast<int>(live->save_ids.size()))
                ok = hub.set_title_preferred_save_card2(ps.title_id, live->save_ids[static_cast<size_t>(sel)],
                                                        &err);
        } else if (sel >= 0 && sel < static_cast<int>(live->save_ids.size())) {
            ok = hub.set_title_preferred_save(ps.title_id, live->save_ids[static_cast<size_t>(sel)],
                                              &err);
        }
        if (!ok && !err.empty()) hub.append_log("Could not save preference: " + err);
        return ok;
    };

    // Theme Header matches panel bg, so Selectable selection is invisible without a push.
    auto selectable_row = [&](const char* label, bool sel) -> bool {
        if (sel) {
            ImGui::PushStyleColor(ImGuiCol_Header, th.accent_button);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, th.accent_button_hovered);
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, th.accent_button_active);
        }
        const bool clicked =
            ImGui::Selectable(label, sel, ImGuiSelectableFlags_AllowDoubleClick);
        if (sel) {
            ImGui::PopStyleColor(3);
            ImGui::SetItemDefaultFocus();
        }
        return clicked;
    };

    if (allow_empty) {
        const bool sel = (ps.sel == -1);
        if (selectable_row("Empty Card Slot", sel)) {
            ps.sel = -1;
            ps.renaming = false;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (apply_selection(-1)) ImGui::CloseCurrentPopup();
            }
        }
    }

    for (size_t i = 0; i < live->save_labels.size(); ++i) {
        const bool sel = (ps.sel == static_cast<int>(i));
        const std::string item = live->save_labels[i] + "##mc" + std::to_string(i);
        if (selectable_row(item.c_str(), sel)) {
            ps.sel = static_cast<int>(i);
            ps.renaming = false;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (apply_selection(ps.sel)) ImGui::CloseCurrentPopup();
            }
        }
    }
    if (live->save_labels.empty() && !allow_empty) {
        ImGui::TextColored(th.text_muted, "No save files yet.");
    }
    ImGui::EndChild();

    const bool has_file = ps.sel >= 0 && ps.sel < static_cast<int>(live->save_ids.size());
    const bool can_select = has_file || (allow_empty && ps.sel == -1);

    if (ps.renaming && has_file) {
        ImGui::Dummy(ImVec2(0, 12));
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##rename_save", ps.rename_buf, sizeof(ps.rename_buf));
        if (ImGui::Button("Confirm rename", ImVec2(-1, 0))) {
            std::string err;
            const std::string id = live->save_ids[static_cast<size_t>(ps.sel)];
            if (hub.rename_title_save(ps.title_id, id, ps.rename_buf, &err)) {
                ps.renaming = false;
                // Re-resolve selection after refresh.
                for (const auto& r : hub.rows) {
                    if (r.id != ps.title_id) continue;
                    live = &r;
                    break;
                }
                if (live) {
                    const std::string want = std::string("saves/") + ps.rename_buf;
                    ps.sel = -2;
                    for (size_t i = 0; i < live->save_ids.size(); ++i) {
                        if (live->save_ids[i] == want ||
                            live->save_labels[i] == ps.rename_buf) {
                            ps.sel = static_cast<int>(i);
                            break;
                        }
                    }
                    if (ps.sel < 0) {
                        // Extension may have been added by rename.
                        const std::string base = ps.rename_buf;
                        for (size_t i = 0; i < live->save_labels.size(); ++i) {
                            if (live->save_labels[i].rfind(base, 0) == 0) {
                                ps.sel = static_cast<int>(i);
                                break;
                            }
                        }
                    }
                }
            } else {
                hub.append_log("Rename failed: " + err);
            }
        }
        if (ImGui::Button("Cancel rename", ImVec2(-1, 0))) ps.renaming = false;
    } else {
        ImGui::Dummy(ImVec2(0, 12));
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float third = (ImGui::GetContentRegionAvail().x - gap * 2.f) / 3.f;
        ImGui::BeginDisabled(!can_select);
        if (good_button("Select", th, ImVec2(third, 0))) {
            if (apply_selection(ps.sel)) ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0, gap);
        ImGui::BeginDisabled(!has_file);
        if (danger_button("Delete", th, ImVec2(third, 0))) {
            std::string err;
            const std::string id = live->save_ids[static_cast<size_t>(ps.sel)];
            if (hub.delete_title_save(ps.title_id, id, &err)) {
                ps.sel = allow_empty ? -1 : -2;
                ps.renaming = false;
            } else {
                hub.append_log("Delete failed: " + err);
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0, gap);
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Create", ImVec2(third, 0))) {
            std::string err;
            if (hub.create_title_save(ps.title_id, &err, ps.slot == 2)) {
                ps.renaming = false;
                for (const auto& r : hub.rows) {
                    if (r.id != ps.title_id) continue;
                    if (ps.slot == 2)
                        ps.sel = r.preferred_save_card2_index;
                    else
                        ps.sel = r.preferred_save_index;
                    break;
                }
            } else {
                hub.append_log("Create failed: " + err);
            }
        }
        ImGui::EndDisabled();

        bool file_busy = false;
        {
            std::lock_guard<std::mutex> lock(hub.file_pick_mu);
            file_busy = hub.file_pick_busy;
        }
        ImGui::BeginDisabled(!has_file);
        if (ImGui::Button("Rename", ImVec2(third, 0))) {
            const std::string& lab = live->save_labels[static_cast<size_t>(ps.sel)];
            std::snprintf(ps.rename_buf, sizeof(ps.rename_buf), "%s", lab.c_str());
            ps.renaming = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0, gap);
        ImGui::BeginDisabled(busy || file_busy || window == nullptr);
        if (ImGui::Button("Import Save File", ImVec2(third, 0))) {
            const auto exts = save_exts_for_platform(hub.catalog, live->platform);
            begin_file_pick(hub, window, retcomm::hub::FilePickKind::ImportSave, live->platform,
                            "Save files", exts, /*allow_many=*/false, ps.title_id);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0, gap);
        ImGui::BeginDisabled(busy || !live->romm_ready || !live->installed);
        if (romm_button("Sync with RomM", th, ImVec2(third, 0))) {
            hub.start_job(HubJob::SyncRommSaves, ps.title_id);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
    }

    ImGui::Dummy(ImVec2(0, 14));
    if (ImGui::Button("Close", ImVec2(-1, 0))) {
        ps.renaming = false;
        ImGui::CloseCurrentPopup();
    }
    close_modal_on_outside_click();
    ImGui::EndPopup();
}

void draw_detail_manage_game_popup(HubModel& hub, const TitleRow& row, const Theme& th, bool busy) {
    if (!ImGui::IsPopupOpen("Manage Game Data###detail_manage_game")) return;
    constexpr float kW = 420.f;
    center_modal_next();
    ImGui::SetNextWindowSizeConstraints(ImVec2(kW, 0.f), ImVec2(kW, FLT_MAX));
    ImGui::SetNextWindowSize(ImVec2(kW, 0.f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Manage Game Data###detail_manage_game", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + kW - 40.f);
    ImGui::TextWrapped("%s", row.name.c_str());
    ImGui::Separator();
    ImGui::BeginDisabled(busy);

    const bool can_open =
        row.installed || row.install_dir_present || row.has_preserved_state;
    if (can_open && ImGui::Button("Open Folder", ImVec2(-1, 0))) {
        fs::path open_dir;
        if (!row.install_root.empty())
            open_dir = retcomm::resolve_current_release_dir(row.install_root);
        if (open_dir.empty() && !row.binary_path.empty()) {
            fs::path walk = fs::path(row.binary_path).parent_path();
            while (!walk.empty() && walk.has_parent_path()) {
                if (walk.parent_path().filename() == "releases") {
                    open_dir = walk;
                    break;
                }
                const fs::path parent = walk.parent_path();
                if (parent == walk) break;
                walk = parent;
            }
            if (open_dir.empty()) open_dir = fs::path(row.binary_path).parent_path();
        }
        if (open_dir.empty()) open_dir = row.install_root;
        std::string err;
        if (!retcomm::open_path_in_file_manager(open_dir, &err))
            hub.append_log("Open Folder failed: " + err);
    }

    const bool can_uninstall =
        row.installed || row.install_dir_present || row.has_preserved_state;
    if (can_uninstall) {
        ImGui::Dummy(ImVec2(0, 6));
        static bool keep_saves = true;
        static std::string keep_saves_for_id;
        if (keep_saves_for_id != row.id) {
            keep_saves_for_id = row.id;
            keep_saves = true;
        }
        ImGui::Checkbox("Keep save data", &keep_saves);
        ImGui::Dummy(ImVec2(0, 4));
        if (ImGui::Button("Uninstall", ImVec2(-1, 0))) {
            hub.start_job(keep_saves ? HubJob::Uninstall : HubJob::UninstallPurge, row.id);
            ImGui::CloseCurrentPopup();
        }
        if (row.supports_local_build &&
            (row.installed || row.install_dir_present || row.install_method == "build")) {
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::BeginDisabled(!row.has_cmake_build_data);
            if (ImGui::Button("Delete Build Data", ImVec2(-1, 0))) {
                hub.start_job(HubJob::DeleteBuildData, row.id);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal |
                                     ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (row.has_cmake_build_data) {
                    ImGui::SetTooltip(
                        "Deletes cmake build intermediates (src/*/build/) to free disk space.\n"
                        "The next app update will need a full rebuild instead of an incremental "
                        "one.\n"
                        "Does not remove the installed game, saves, or ROM library.");
                } else {
                    ImGui::SetTooltip("No cmake build data on disk.");
                }
            }
        }
    } else {
        ImGui::TextColored(th.text_muted, "No install folder or preserved data yet.");
    }

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextColored(th.text_muted, "ROM / disc");
    if (row.has_rom)
        ImGui::TextWrapped("%s", row.rom_path.c_str());
    else if (row.has_romm) {
        ImGui::TextColored(th.accent, "Available on RomM");
        if (!row.romm_file_name.empty()) ImGui::TextWrapped("%s", row.romm_file_name.c_str());
    } else {
        ImGui::TextColored(th.warn, "No library match");
        if (!row.suggested_rom.empty()) {
            ImGui::TextColored(th.text_muted, "Looking for:");
            ImGui::TextWrapped("%s", row.suggested_rom.c_str());
        }
    }

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextColored(th.text_muted, "RomM");
    if (!row.romm_ready) {
        ImGui::TextColored(th.text_muted, "Not configured — Menu → RomM Sync Settings.");
    } else {
        if (row.has_rom_identity) {
            const char* rom_label = row.has_rom ? "Re-download ROM" : "Download ROM";
            if (romm_button(rom_label, th, ImVec2(-1, 0))) {
                hub.start_job(HubJob::FetchRommRom, row.id);
                ImGui::CloseCurrentPopup();
            }
        }
        if (row.needs_bios) {
            const char* bios_label = row.has_bios ? "Re-download BIOS" : "Download BIOS";
            if (romm_button(bios_label, th, ImVec2(-1, 0))) {
                hub.start_job(HubJob::FetchRommBios, row.id);
                ImGui::CloseCurrentPopup();
            }
        }
        if (!row.has_rom_identity && !row.needs_bios) {
            ImGui::TextColored(th.text_muted, "No RomM download actions for this title.");
        }
    }

    ImGui::EndDisabled();
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 8));
    if (ImGui::Button("Close", ImVec2(-1, 0))) ImGui::CloseCurrentPopup();
    close_modal_on_outside_click();
    ImGui::EndPopup();
}

void draw_menu_popup(HubModel& hub, const Theme& th, SDL_Window* /*window*/) {
    if (hub.pending_open_menu) {
        ImGui::OpenPopup("Menu###hub_menu_panel");
        hub.pending_open_menu = false;
    }

    if (!ImGui::IsPopupOpen("Menu###hub_menu_panel")) return;
    constexpr float kMenuW = 420.f;
    center_modal_next();
    // Fixed width — AlwaysAutoResize alone starts wide then shrinks on the next frame.
    ImGui::SetNextWindowSizeConstraints(ImVec2(kMenuW, 0.f), ImVec2(kMenuW, FLT_MAX));
    ImGui::SetNextWindowSize(ImVec2(kMenuW, 0.f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Menu###hub_menu_panel", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::TextColored(th.text_muted, "Settings");
    // Settings stay reachable during Build & Install / library jobs.
    if (ImGui::Button("Library Settings", ImVec2(-1, 0))) {
        hub.open_settings();
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::Button("RomM Sync Settings", ImVec2(-1, 0))) {
        hub.open_romm_settings();
        ImGui::CloseCurrentPopup();
    }

    ImGui::Dummy(ImVec2(0, 10));
    {
        const std::string ver = retcomm::retcomm_app_version();
        const retcomm::RetcommInstallInfo install = retcomm::retcomm_install_info();
        std::string tc_line;
        bool tc_upd = false;
        {
            std::lock_guard<std::mutex> lock(hub.mu);
            tc_upd = hub.toolchain_update_available;
            tc_line = hub.toolchain_status;
            if (tc_line.empty() && !hub.toolchain_current_version.empty())
                tc_line = "Toolchain " + hub.toolchain_current_version;
        }
        ImGui::TextColored(th.text_muted, "Launcher %s", ver.c_str());
        if (install.self_update_supported && !install.channel_id.empty())
            ImGui::TextColored(th.text_muted, "Channel %s", install.channel_id.c_str());
        if (!tc_line.empty())
            ImGui::TextColored(tc_upd ? th.warn : th.text_muted, "%s", tc_line.c_str());
    }
    ImGui::Dummy(ImVec2(0, 10));
    if (ImGui::Button("Close", ImVec2(-1, 0))) ImGui::CloseCurrentPopup();
    close_modal_on_outside_click();
    ImGui::EndPopup();
}

void draw_library_popup(HubModel& hub, const Theme& th, SDL_Window* window) {
    if (hub.pending_open_library) {
        // Prefill from the platform the user is browsing (Titles view).
        if (hub.library_nav == retcomm::hub::LibraryNav::Titles &&
            !hub.library_platform.empty()) {
            hub.library_import_platform = hub.library_platform;
            hub.scans_platform_filter = hub.library_platform;
        }
        ImGui::OpenPopup("Library###library_panel");
        hub.pending_open_library = false;
    }

    // Only set next-window pos/size when this popup will actually begin — otherwise
    // SetNextWindow* leaks onto the following modal in the frame.
    if (!ImGui::IsPopupOpen("Library###library_panel")) return;
    constexpr float kLibW = 420.f;
    center_modal_next();
    ImGui::SetNextWindowSizeConstraints(ImVec2(kLibW, 0.f), ImVec2(kLibW, FLT_MAX));
    ImGui::SetNextWindowSize(ImVec2(kLibW, 0.f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Library###library_panel", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    const bool busy = hub.job_running.load();
    bool file_busy = false;
    {
        std::lock_guard<std::mutex> lock(hub.file_pick_mu);
        file_busy = hub.file_pick_busy;
    }

    std::vector<std::string> plats;
    {
        std::unordered_set<std::string> seen;
        for (const auto& t : hub.catalog.titles) {
            if (t.platform.empty() || !seen.insert(t.platform).second) continue;
            plats.push_back(t.platform);
        }
        std::sort(plats.begin(), plats.end(), [](const std::string& a, const std::string& b) {
            return std::string(platform_display_name(a)) < std::string(platform_display_name(b));
        });
    }
    if (!hub.library_import_platform.empty() &&
        std::find(plats.begin(), plats.end(), hub.library_import_platform) == plats.end())
        hub.library_import_platform.clear();
    if (!hub.scans_platform_filter.empty() &&
        std::find(plats.begin(), plats.end(), hub.scans_platform_filter) == plats.end())
        hub.scans_platform_filter.clear();

    ImGui::TextColored(th.text_muted, "Add files");
    {
        const char* preview = hub.library_import_platform.empty()
                                  ? "Select a Platform"
                                  : platform_display_name(hub.library_import_platform);
        ImGui::BeginDisabled(file_busy || busy);
        if (ImGui::BeginCombo("##library_import_platform", preview)) {
            for (const auto& p : plats) {
                const bool sel = hub.library_import_platform == p;
                if (ImGui::Selectable(platform_display_name(p), sel))
                    hub.library_import_platform = p;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        const bool plat_ok = !hub.library_import_platform.empty();
        ImGui::BeginDisabled(!plat_ok);
        if (ImGui::Button("Import ROM", ImVec2(-1, 0))) {
            const auto exts = rom_exts_for_platform(hub.catalog, hub.library_import_platform);
            begin_file_pick(hub, window, retcomm::hub::FilePickKind::ImportRom,
                            hub.library_import_platform, "ROM files", exts, /*allow_many=*/true);
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::Button("Import save file", ImVec2(-1, 0))) {
            const auto exts = save_exts_for_platform(hub.catalog, hub.library_import_platform);
            begin_file_pick(hub, window, retcomm::hub::FilePickKind::ImportSave,
                            hub.library_import_platform, "Save files", exts, /*allow_many=*/false);
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::Button("Import BIOS", ImVec2(-1, 0))) {
            const auto exts = bios_exts_for_platform(hub.library_import_platform);
            begin_file_pick(hub, window, retcomm::hub::FilePickKind::ImportBios,
                            hub.library_import_platform, "BIOS files", exts, /*allow_many=*/false);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Copies into your library / saves / BIOS folders, then scans new files for that "
            "platform. Multi-track discs: select the .cue and .bin tracks together.");
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, 10));
    ImGui::Separator();
    ImGui::TextColored(th.text_muted, "Advanced");
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "For files you already placed in your folders, or to drop deleted entries.");
    ImGui::PopStyleColor();
    {
        const char* scope_preview = hub.scans_platform_filter.empty()
                                        ? "All Platforms"
                                        : platform_display_name(hub.scans_platform_filter);
        ImGui::BeginDisabled(busy || file_busy);
        if (ImGui::BeginCombo("##library_advanced_scope", scope_preview)) {
            if (ImGui::Selectable("All Platforms", hub.scans_platform_filter.empty()))
                hub.scans_platform_filter.clear();
            for (const auto& p : plats) {
                const bool sel = hub.scans_platform_filter == p;
                if (ImGui::Selectable(platform_display_name(p), sel))
                    hub.scans_platform_filter = p;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button("Scan new files", ImVec2(-1, 0))) {
            hub.pending_scan_missing_rom_id.clear();
            hub.start_job(HubJob::ScanRoms);
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::Button("Clean missing files", ImVec2(-1, 0))) {
            hub.start_job(HubJob::PurgeMissingFiles);
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::Button("Full rebuild index…", ImVec2(-1, 0)))
            ImGui::OpenPopup("Full rebuild###confirm_full_rescan");
        ImGui::EndDisabled();
    }

    ImGui::Dummy(ImVec2(0, 10));
    if (ImGui::Button("Close", ImVec2(-1, 0))) ImGui::CloseCurrentPopup();

    if (ImGui::BeginPopupModal("Full rebuild###confirm_full_rescan", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 360.f);
        if (hub.scans_platform_filter.empty()) {
            ImGui::TextWrapped(
                "Re-hash every ROM and BIOS candidate and rebuild the indexes. On a large "
                "collection this can take a long time.");
        } else {
            ImGui::TextWrapped(
                "Re-hash every ROM and BIOS candidate for %s. Other platforms in the index "
                "are left alone.",
                platform_display_name(hub.scans_platform_filter));
        }
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 8));
        if (accent_button("Rebuild", th, ImVec2(120, 0))) {
            hub.pending_scan_missing_rom_id.clear();
            hub.start_job(HubJob::FullScanRoms);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        close_modal_on_outside_click();
        ImGui::EndPopup();
    }

    close_modal_on_outside_click();
    ImGui::EndPopup();
}

fs::path find_hub_logo_path() {
    // Packaged icon location + source-tree assets/retcomm.png via walk.
    std::error_code ec;
    auto try_file = [&](const fs::path& p) -> fs::path {
        if (p.empty()) return {};
        if (fs::is_regular_file(p, ec)) return p;
        return {};
    };
    if (const char* appdir = std::getenv("APPDIR")) {
        const fs::path ad(appdir);
        if (auto h = try_file(ad / "usr" / "share" / "icons" / "hicolor" / "512x512" / "apps" /
                             "retcomm.png");
            !h.empty())
            return h;
        if (auto h = try_file(ad / "usr" / "share" / "retcomm" / "retcomm.png"); !h.empty())
            return h;
    }
    if (const char* base = SDL_GetBasePath()) {
        fs::path walk(base);
        for (int i = 0; i < 6 && !walk.empty(); ++i) {
            if (auto h = try_file(walk / "assets" / "retcomm.png"); !h.empty()) return h;
            if (auto h = try_file(walk / "retcomm.png"); !h.empty()) return h;
            if (auto h = try_file(walk / "share" / "retcomm" / "retcomm.png"); !h.empty())
                return h;
            if (auto h = try_file(walk / "share" / "icons" / "hicolor" / "512x512" / "apps" /
                                  "retcomm.png");
                !h.empty())
                return h;
            walk = walk.parent_path();
        }
    }
    return {};
}

void draw_welcome_panel(BoxartCache& boxart, const Theme& th) {
    ImGui::BeginChild("detail", ImVec2(0, 0), ImGuiChildFlags_Borders);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float logo_max = std::min(220.f, std::min(avail.x - 24.f, avail.y * 0.45f));

    const fs::path logo = find_hub_logo_path();
    const BoxartTexture* tex =
        logo.empty() ? nullptr : boxart.get("hub:logo", logo);

    float block_h = ImGui::GetTextLineHeight() * 2.5f;
    float logo_w = 0.f, logo_h = 0.f;
    if (tex && tex->gl_id && tex->width > 0 && tex->height > 0) {
        const ImVec2 fit = contain_size(static_cast<float>(tex->width),
                                        static_cast<float>(tex->height), logo_max, logo_max);
        logo_w = fit.x;
        logo_h = fit.y;
        block_h += logo_h + 16.f;
    }

    const float start_y = ImGui::GetCursorPosY() + std::max(0.f, (avail.y - block_h) * 0.35f);
    ImGui::SetCursorPosY(start_y);

    if (tex && tex->gl_id && logo_w > 0.f) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (avail.x - logo_w) * 0.5f));
        ImGui::Image((ImTextureID)(intptr_t)tex->gl_id, ImVec2(logo_w, logo_h));
        ImGui::Dummy(ImVec2(0, 12.f));
    }

    const char* welcome = "Welcome to RetComM";
    const ImVec2 tw = ImGui::CalcTextSize(welcome);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (avail.x - tw.x) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, th.accent);
    ImGui::TextUnformatted(welcome);
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 6.f));
    const char* hint = "Choose a platform, then a title.";
    const ImVec2 hw = ImGui::CalcTextSize(hint);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (avail.x - hw.x) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextUnformatted(hint);
    ImGui::PopStyleColor();
    ImGui::EndChild();
}

void draw_detail(HubModel& hub, BoxartCache& boxart, const Theme& th, SDL_Window* window) {
    const bool job_busy = hub.job_running.load();
    const bool install_op = job_busy && retcomm::hub::hub_job_is_install(hub.job);
    // Any exclusive worker job blocks starting another install/scan/mutate.
    const bool block_title_mutate = job_busy;

    TitleRow row;
    bool show_welcome = false;
    {
        std::lock_guard<std::mutex> lock(hub.mu);
        show_welcome = hub.library_nav == retcomm::hub::LibraryNav::Platforms ||
                       hub.rows.empty() || hub.selected < 0 ||
                       hub.selected >= static_cast<int>(hub.rows.size());
        if (!show_welcome) row = hub.rows[static_cast<size_t>(hub.selected)];
    }
    if (show_welcome) {
        draw_welcome_panel(boxart, th);
        return;
    }

    ImGui::BeginChild("detail", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (hub.detail_scroll_top) {
        ImGui::SetScrollY(0.f);
        hub.detail_scroll_top = false;
    }

    // Header = title name only.
    ImGui::TextWrapped("%s", row.name.c_str());

    ImGui::Dummy(ImVec2(0, 10));

    // Primary actions under the title.
    const float btn_w = (ImGui::GetContentRegionAvail().x - 8.f) * 0.5f;
    if (row.installed) {
        // Play stays available during Build & Install / scans (own worker thread).
        const bool play_blocked = hub.launch_running.load() ||
                                  (hub.job_running.load() && hub.job == HubJob::CheckLaunchUpdate);
        ImGui::BeginDisabled(play_blocked);
        const bool play_clicked =
            row.update_available ? ImGui::Button("Play", ImVec2(btn_w, 0))
                                 : good_button("Play", th, ImVec2(btn_w, 0));
        if (play_clicked) {
            if (hub.cfg.check_updates_before_launch) {
                const auto* t = hub.catalog.find(row.id);
                if (t && !t->release.github.empty())
                    hub.start_job(HubJob::CheckLaunchUpdate, row.id);
                else
                    hub.start_job(HubJob::Launch, row.id);
            } else {
                hub.start_job(HubJob::Launch, row.id);
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0, 8);
        ImGui::BeginDisabled(block_title_mutate);
        if (row.update_available) {
            if (good_button("Update", th, ImVec2(btn_w, 0)))
                hub.start_job(HubJob::Update, row.id);
        } else if (ImGui::Button("Update", ImVec2(btn_w, 0))) {
            hub.start_job(HubJob::Update, row.id);
        }
        ImGui::EndDisabled();
    } else if (row.supports_local_build) {
        // Prefer prebuilt zip when available — ROM only required for build-only titles.
        const bool zip_first = row.can_prebuilt_install;
        if (!row.has_rom && !zip_first) {
            ImGui::PushStyleColor(ImGuiCol_Text, th.warn);
            if (row.has_rom_identity) {
                ImGui::TextWrapped(
                    "No verified .cue / ROM in your library yet. Install will offer a "
                    "quick scan for %s%s.",
                    platform_display_name(row.platform),
                    row.romm_ready ? ", or a RomM download" : "");
            } else {
                ImGui::TextWrapped(
                    "Match a verified .cue / ROM in your library before Install.");
            }
            ImGui::PopStyleColor();
        }
        ImGui::BeginDisabled(block_title_mutate ||
                             (!zip_first && !row.has_rom && !row.has_rom_identity));
        if (good_button(row.install_dir_present ? "Reinstall" : "Install", th, ImVec2(-1, 0)))
            hub.begin_install(row.id);
        ImGui::EndDisabled();
    } else {
        ImGui::BeginDisabled(block_title_mutate);
        if (good_button(row.install_dir_present ? "Reinstall" : "Install", th, ImVec2(-1, 0)))
            hub.begin_install(row.id);
        ImGui::EndDisabled();
    }

    // Slot selection under Play; install lifecycle in Manage Game Data.
    if (row.installed) {
        ImGui::Dummy(ImVec2(0, 8));
        draw_detail_save_controls(hub, row, th, block_title_mutate, window);
    }

    ImGui::Dummy(ImVec2(0, 12));
    ImGui::BeginDisabled(install_op);
    if (ImGui::Button("Manage Game Data", ImVec2(-1, 0)))
        ImGui::OpenPopup("Manage Game Data###detail_manage_game");
    ImGui::EndDisabled();

    draw_detail_manage_game_popup(hub, row, th, block_title_mutate);

    // Seldom-touched: BIOS just above author.
    if (row.needs_bios || row.supports_openbios) {
        ImGui::Dummy(ImVec2(0, 16));
        ImGui::TextColored(th.text_muted, "BIOS");
        int dump_count = 0;
        bool has_openbios_opt = false;
        for (const auto& id : row.bios_choice_ids) {
            if (id == retcomm::kOpenBiosChoice) has_openbios_opt = true;
            else ++dump_count;
        }
        const bool show_bios_combo =
            static_cast<int>(row.bios_choice_ids.size()) > 1; // conflict / choice
        if (show_bios_combo) {
            const char* preview =
                (row.preferred_bios_index >= 0 &&
                 row.preferred_bios_index < static_cast<int>(row.bios_choice_labels.size()))
                    ? row.bios_choice_labels[static_cast<size_t>(row.preferred_bios_index)].c_str()
                    : "(select)";
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##bios_choice", preview)) {
                for (size_t i = 0; i < row.bios_choice_ids.size(); ++i) {
                    const bool selected = static_cast<int>(i) == row.preferred_bios_index;
                    const std::string item =
                        row.bios_choice_labels[i] + "##" + row.bios_choice_ids[i];
                    if (ImGui::Selectable(item.c_str(), selected)) {
                        std::string err;
                        if (!hub.set_title_preferred_bios(row.id, row.bios_choice_ids[i], &err))
                            hub.append_log("BIOS preference failed: " + err);
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (row.bios_choice == retcomm::kOpenBiosChoice) {
                ImGui::TextColored(th.text_muted,
                                   "OpenBIOS regenerates at Install (no dump needed).");
            }
        } else if (dump_count == 1) {
            const char* label =
                (row.preferred_bios_index >= 0 &&
                 row.preferred_bios_index < static_cast<int>(row.bios_choice_labels.size()))
                    ? row.bios_choice_labels[static_cast<size_t>(row.preferred_bios_index)].c_str()
                    : "matched";
            ImGui::TextColored(th.good, "Using %s", label);
        } else if (has_openbios_opt) {
            ImGui::TextColored(th.good, "Using OpenBIOS");
            ImGui::TextColored(th.text_muted, "Regenerates at Install (no dump needed).");
        } else if (row.has_bios) {
            ImGui::TextColored(th.good, "BIOS matched");
        } else {
            ImGui::TextColored(th.warn, "Missing — Import BIOS from Library");
            if (row.supports_openbios)
                ImGui::TextColored(th.text_muted, "Or use OpenBIOS after a catalog refresh.");
        }
    }

    // Footer: author → GitHub → App status → About.
    ImGui::Dummy(ImVec2(0, 12));
    const char* author_label =
        (row.kind == "decomp") ? "Decomp Author" : "Recomp Author";
    ImGui::TextColored(th.text_muted, "%s", author_label);
    if (!row.author.empty())
        ImGui::Text("%s", row.author.c_str());
    else
        ImGui::TextColored(th.text_muted, "(unknown)");
    if (!row.github_url.empty()) {
        if (ImGui::Button("GitHub Source", ImVec2(-1, 0))) {
            std::string err;
            if (!retcomm::open_url_in_browser(row.github_url, &err))
                hub.append_log("Open URL failed: " + err);
        }
    }
    if (!row.author_notes.empty()) {
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::TextColored(th.text_muted, "Author's Notes");
        ImGui::TextWrapped("%s", row.author_notes.c_str());
    }

    ImGui::Dummy(ImVec2(0, 10));
    ImGui::TextColored(th.text_muted, "App");
    if (row.installed) {
        const std::string& shown_tag =
            !row.release_compare_tag.empty() ? row.release_compare_tag : row.installed_tag;
        ImGui::TextColored(th.good, "Installed %s%s",
                           shown_tag.empty() ? "" : shown_tag.c_str(),
                           row.runtime == "wine" ? " (Wine)" : "");
        if (row.update_available)
            ImGui::TextColored(th.warn, "Update available: %s", row.latest_tag.c_str());
    } else if (row.install_dir_present) {
        ImGui::TextColored(th.warn, "Install folder present — launch binary not found");
        if (!row.install_issue.empty()) ImGui::TextWrapped("%s", row.install_issue.c_str());
        if (!row.expected_binary.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
            ImGui::TextWrapped(
                "Looking for executable \"%s\". Use Reinstall to clear the install folder "
                "and start over, or Manage Game Data → Open Folder to fix setup manually.",
                row.expected_binary.c_str());
            ImGui::PopStyleColor();
        }
    } else {
        ImGui::TextColored(th.text_muted, "Not installed");
        if (row.has_preserved_state) {
            ImGui::TextColored(th.focus, "Preserved saves/config ready");
            ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
            ImGui::TextWrapped(
                "Previous uninstall kept user data under preserved/. The next install "
                "will restore it into the new release.");
            ImGui::PopStyleColor();
        }
    }

    if (!row.description.empty()) {
        ImGui::Dummy(ImVec2(0, 10));
        ImGui::TextColored(th.text_muted, "About");
        ImGui::TextWrapped("%s", row.description.c_str());
    }

    ImGui::EndChild();
}

void draw_settings_panel(HubModel& hub, const Theme& th, SDL_Window* window) {
    ImGui::BeginChild("settings", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextUnformatted("LIBRARY SETTINGS");
    ImGui::PopStyleColor();
    ImGui::TextWrapped("Paths and platform folder names written to config.json.");
    ImGui::Separator();

    if (path_field_with_browse("ROM library root", "##library_root", hub.settings.library_root,
                               sizeof(hub.settings.library_root), hub, window,
                               FolderPickTarget::LibraryRoot, th))
        hub.settings.dirty = true;

    ImGui::Dummy(ImVec2(0, 6));
    if (path_field_with_browse("BIOS root", "##bios_root", hub.settings.bios_root,
                               sizeof(hub.settings.bios_root), hub, window,
                               FolderPickTarget::BiosRoot, th))
        hub.settings.dirty = true;

    ImGui::Dummy(ImVec2(0, 6));
    if (path_field_with_browse("Game saves root", "##saves_root", hub.settings.saves_root,
                               sizeof(hub.settings.saves_root), hub, window,
                               FolderPickTarget::SavesRoot, th))
        hub.settings.dirty = true;
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "Native SRAM / memcard files (RomM sync + launch). Per-platform folders under this "
        "root (e.g. …/saves/snes). Leave empty to keep saves inside each install.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextColored(th.text_muted, "Exclude dirs (comma-separated basenames)");
    if (ImGui::InputText("##exclude_dirs", hub.settings.exclude_dirs,
                         sizeof(hub.settings.exclude_dirs)))
        hub.settings.dirty = true;

    ImGui::Dummy(ImVec2(0, 12));
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextUnformatted("GAME INSTALL LOCATIONS");
    ImGui::PopStyleColor();
    ImGui::TextWrapped(
        "Folders that hold installed titles (each contains <game>/releases/…). "
        "Default is RetComM's apps/ folder. Add another root (e.g. an external drive) when "
        "you want Install to ask where to put a new game. Existing installs stay where they are.");
    if (ImGui::BeginTable("install_roots", 4,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("##def", ImGuiTableColumnFlags_WidthFixed, 56.f);
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, 100.f);
        ImGui::TableSetupColumn("path", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##rm", ImGuiTableColumnFlags_WidthFixed, 36.f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < static_cast<int>(hub.settings.install_roots.size()); ++i) {
            auto& row = hub.settings.install_roots[static_cast<size_t>(i)];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::RadioButton("##default_root", &hub.settings.default_install_root_index, i))
                hub.settings.dirty = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Default for new installs");
            ImGui::TableNextColumn();
            if (ImGui::InputText("##label", row.label, sizeof(row.label)))
                hub.settings.dirty = true;
            ImGui::TableNextColumn();
            {
                const float browse_w = 72.f;
                const float gap = ImGui::GetStyle().ItemSpacing.x;
                const float input_w = ImGui::GetContentRegionAvail().x - browse_w - gap;
                if (input_w > 60.f) ImGui::SetNextItemWidth(input_w);
                if (ImGui::InputText("##path", row.path, sizeof(row.path)))
                    hub.settings.dirty = true;
                ImGui::SameLine();
                bool busy = false;
                {
                    std::lock_guard<std::mutex> lock(hub.folder_pick_mu);
                    busy = hub.folder_pick_busy;
                }
                ImGui::BeginDisabled(busy);
                if (ImGui::Button("…", ImVec2(browse_w, 0))) {
                    hub.folder_pick_install_index = i;
                    begin_folder_pick(hub, window, FolderPickTarget::InstallRoot, row.path);
                }
                ImGui::EndDisabled();
            }
            ImGui::TableNextColumn();
            if (ImGui::Button("X")) {
                hub.settings.install_roots.erase(hub.settings.install_roots.begin() + i);
                if (hub.settings.default_install_root_index >=
                    static_cast<int>(hub.settings.install_roots.size())) {
                    hub.settings.default_install_root_index =
                        std::max(0, static_cast<int>(hub.settings.install_roots.size()) - 1);
                } else if (hub.settings.default_install_root_index > i) {
                    --hub.settings.default_install_root_index;
                }
                hub.settings.dirty = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Add Install Location", ImVec2(200, 0)))
        hub.add_install_root_row();

    ImGui::Dummy(ImVec2(0, 12));
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "Add ROMs / BIOS / saves, refresh folders, or clean missing files from the Library "
        "button in the top bar.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 10));
    ImGui::Separator();
    if (ImGui::Checkbox("Always Check For Updates On Startup",
                        &hub.settings.check_updates_on_startup))
        hub.settings.dirty = true;
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "When enabled, RetComM checks for launcher, game, and toolchain updates after the hub "
        "starts. Turn off to skip the startup prompt (Check Updates in the menu still works). "
        "Save to apply.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 8));
    if (ImGui::Checkbox("Always Check For Updates Before Game Launch",
                        &hub.settings.check_updates_before_launch))
        hub.settings.dirty = true;
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "When enabled, Play queries GitHub for that title and asks before launching if a newer "
        "release is available. Save to apply.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 10));
    if (ImGui::Checkbox("Filter Unsupported Titles", &hub.settings.filter_unsupported_titles))
        hub.settings.dirty = true;
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "Hide catalog titles that do not have a ROM available — neither on your local ROM "
        "library path nor (when scanned) on RomM. Installed titles stay visible. Save, then "
        "use Install / RomM sync so remote-only matches appear.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 8));
    {
        const bool busy = hub.job_running.load();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Find Missing Boxart", ImVec2(200, 0)))
            hub.start_job(HubJob::FetchBoxart, {}, false);
        ImGui::SameLine();
        if (ImGui::Button("Resync All Boxart", ImVec2(200, 0)))
            hub.start_job(HubJob::FetchBoxart, {}, true);
        ImGui::EndDisabled();
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Find Missing downloads covers only for titles with no cached art. "
            "Resync All clears the cover cache and re-downloads every catalog title "
            "(Libretro thumbnails, or RomM when Sync Boxart is enabled).");
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, 8));
    {
        const bool busy = hub.job_running.load();
        constexpr float kGap = 8.f;
        const float btn_w = (ImGui::GetContentRegionAvail().x - kGap) * 0.5f;
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Clean Unlisted Installs…", ImVec2(btn_w, 0))) {
            const size_t n = hub.refresh_orphan_installs();
            if (n == 0) {
                hub.set_status("No installs outside the catalog");
                hub.append_log("Orphan scan: none");
            } else {
                hub.orphan_prompt_pending.store(true);
            }
        }
        ImGui::SameLine(0, kGap);
        if (ImGui::Button("Clean Old Update Files", ImVec2(btn_w, 0)))
            hub.start_job(HubJob::CleanupOldReleases);
        ImGui::EndDisabled();
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Unlisted: remove apps/ installs no longer in the catalog. Old update files: "
            "promote saves/config into the current release, then delete leftover "
            "releases/<old-tag>/ folders.");
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, 10));
    if (ImGui::CollapsingHeader("Advanced")) {
        if (ImGui::Checkbox("Auto-clean cmake build directories after install",
                            &hub.settings.auto_clean_build_dirs))
            hub.settings.dirty = true;
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "When enabled, RetComM deletes each title's src/current/build/ after a successful "
            "local build to free disk space. Leave off for faster package updates (incremental "
            "Ninja). Save to apply.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 6));
        {
            const bool busy = hub.job_running.load();
            ImGui::BeginDisabled(busy);
            if (ImGui::Button("Clean All Cmake Build Directories", ImVec2(280, 0)))
                hub.start_job(HubJob::CleanupCmakeBuildDirs);
            ImGui::EndDisabled();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Immediately delete cmake build/ trees under every game install's src/ "
            "(Play binary, saves, and generated C are kept). The next Update or Generate & "
            "Rebuild will reconfigure from scratch.");
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, 10));
    if (ImGui::CollapsingHeader("Advanced folder mapping")) {
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Catalog platform slug → folder name(s) under the library / BIOS / saves roots "
            "(e.g. psx → ps, ps1). Most setups can leave the defaults.");
        ImGui::PopStyleColor();
        ImGui::Separator();

        if (ImGui::BeginTable("platform_folders", 3,
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("platform", ImGuiTableColumnFlags_WidthFixed, 120.f);
            ImGui::TableSetupColumn("folders", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##rm", ImGuiTableColumnFlags_WidthFixed, 36.f);
            ImGui::TableHeadersRow();
            for (int i = 0; i < static_cast<int>(hub.settings.platform_folders.size()); ++i) {
                auto& row = hub.settings.platform_folders[static_cast<size_t>(i)];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::InputText("##plat", row.platform, sizeof(row.platform)))
                    hub.settings.dirty = true;
                ImGui::TableNextColumn();
                if (ImGui::InputText("##folders", row.folders, sizeof(row.folders)))
                    hub.settings.dirty = true;
                ImGui::TableNextColumn();
                if (ImGui::Button("X")) {
                    hub.settings.platform_folders.erase(hub.settings.platform_folders.begin() + i);
                    hub.settings.dirty = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Add platform row")) hub.add_platform_folder_row();
    }

    ImGui::Dummy(ImVec2(0, 12));
    if (accent_button("Save", th, ImVec2(160, 0))) {
        std::string err;
        if (!hub.save_settings(&err)) {
            hub.append_log("settings save failed: " + err);
            hub.set_status("Save failed");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        hub.show_settings = false;
        hub.settings.dirty = false;
    }
    if (hub.settings.dirty) {
        ImGui::SameLine();
        ImGui::TextColored(th.warn, "unsaved changes");
    }
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextColored(th.text_muted, "%s", hub.paths.config_path.string().c_str());
    ImGui::EndChild();
}

bool draw_setup_path_card(BoxartCache& boxart, const Theme& th, const char* id,
                          const char* title, const char* subtitle, const char* asset_file,
                          float card_w, float card_h) {
    ImGui::PushID(id);
    const ImVec2 card_min = ImGui::GetCursorScreenPos();
    const ImVec2 card_max(card_min.x + card_w, card_min.y + card_h);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const bool hovered = ImGui::IsMouseHoveringRect(card_min, card_max);
    const ImU32 fill = ImGui::ColorConvertFloat4ToU32(hovered ? th.panel_hovered : th.panel);
    const ImU32 border =
        ImGui::ColorConvertFloat4ToU32(hovered ? th.accent_dim : th.border);
    dl->AddRectFilled(card_min, card_max, fill, th.radius_lg);
    dl->AddRect(card_min, card_max, border, th.radius_lg, 0, hovered ? 2.f : 1.f);

    const fs::path icon = find_hub_asset_file("setup", asset_file);
    const BoxartTexture* tex =
        icon.empty() ? nullptr : boxart.get(std::string("setup:") + id, icon);
    const float pad = 18.f;
    const float icon_max = std::min(card_w - pad * 2.f, card_h * 0.48f);
    float icon_w = 0.f, icon_h = 0.f;
    if (tex && tex->gl_id && tex->width > 0 && tex->height > 0) {
        const ImVec2 fit = contain_size(static_cast<float>(tex->width),
                                        static_cast<float>(tex->height), icon_max, icon_max);
        icon_w = fit.x;
        icon_h = fit.y;
        const float ix = card_min.x + (card_w - icon_w) * 0.5f;
        const float iy = card_min.y + pad + 8.f;
        dl->AddImage((ImTextureID)(intptr_t)tex->gl_id, ImVec2(ix, iy),
                     ImVec2(ix + icon_w, iy + icon_h));
    }

    const ImVec2 title_sz = ImGui::CalcTextSize(title);
    const float text_y = card_min.y + pad + 8.f + icon_max + 14.f;
    dl->AddText(ImVec2(card_min.x + (card_w - title_sz.x) * 0.5f, text_y),
                ImGui::ColorConvertFloat4ToU32(th.text), title);

    ImGui::SetCursorScreenPos(ImVec2(card_min.x + pad, text_y + title_sz.y + 8.f));
    ImGui::PushTextWrapPos(card_min.x + card_w - pad);
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped("%s", subtitle);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();

    ImGui::SetCursorScreenPos(card_min);
    const bool clicked = ImGui::InvisibleButton("##card", ImVec2(card_w, card_h));
    ImGui::PopID();
    return clicked;
}

void draw_setup_wizard(HubModel& hub, BoxartCache& boxart, const Theme& th, SDL_Window* window) {
    if (!hub.show_setup) return;

    ImGui::OpenPopup("Welcome to RetComM###setup_wizard");
    float wiz_w = 620.f;
    float wiz_h = 0.f;
    if (hub.setup_path == SetupPath::Chooser) {
        wiz_w = 640.f;
        wiz_h = 0.f;
    } else if (hub.setup_path == SetupPath::Easy) {
        wiz_w = 560.f;
    } else {
        wiz_w = hub.setup_step == 0 ? 560.f : 640.f;
        wiz_h = hub.setup_step == 0 ? 0.f : 520.f;
    }
    ImGui::SetNextWindowSize(ImVec2(wiz_w, wiz_h), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Welcome to RetComM###setup_wizard", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    auto advance_to_platform_step = [&]() {
        hub.seed_setup_platform_folders();
        hub.setup_step = 1;
        hub.setup_confirm_create_roots = false;
    };

    auto skip_setup = [&]() {
        std::string err;
        if (!hub.complete_setup(&err)) {
            hub.append_log("setup marker failed: " + err);
            hub.set_status("Setup marker failed");
        } else {
            hub.set_status("Setup skipped — set library root in Library settings");
            hub.append_log("First-time setup skipped");
        }
    };

    if (hub.setup_confirm_create_roots && hub.setup_path == SetupPath::Advanced) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 520.f);
        ImGui::TextWrapped("These folders do not exist yet. Create them now?");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 8));
        for (const auto& p : hub.setup_missing_roots) {
            ImGui::BulletText("%s", p.c_str());
        }
        ImGui::Dummy(ImVec2(0, 14));
        if (accent_button("Create folders", th, ImVec2(160, 0))) {
            std::string err;
            if (!hub.create_missing_setup_roots(&err)) {
                hub.append_log("setup create roots failed: " + err);
                hub.set_status("Could not create folders");
            } else {
                hub.set_status("Folders created");
                advance_to_platform_step();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Back", ImVec2(120, 0))) {
            hub.setup_confirm_create_roots = false;
            hub.setup_missing_roots.clear();
        }
        ImGui::EndPopup();
        return;
    }

    if (hub.setup_path == SetupPath::Chooser) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 600.f);
        ImGui::TextWrapped("How do you want to set up your library?");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 14));

        const float gap = 16.f;
        const float card_w = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
        const float card_h = 280.f;
        if (draw_setup_path_card(boxart, th, "easy", "Easy Setup",
                                 "Pick one Emulation folder. RetComM creates roms, bios, "
                                 "and saves under it with default platform folders.",
                                 "setup_easy_rocket.png", card_w, card_h)) {
            hub.setup_path = SetupPath::Easy;
            hub.apply_suggested_emulation_root(/*overwrite_nonempty=*/false);
            hub.apply_roots_from_emulation_parent();
        }
        ImGui::SameLine(0.f, gap);
        if (draw_setup_path_card(boxart, th, "advanced", "Advanced Setup",
                                 "Choose roms, bios, and saves separately. Optionally connect "
                                 "RomM and edit platform folder mappings.",
                                 "setup_advanced_wrench.png", card_w, card_h)) {
            hub.setup_path = SetupPath::Advanced;
            hub.setup_step = 0;
            hub.apply_suggested_library_roots(/*overwrite_nonempty=*/false);
        }

        ImGui::Dummy(ImVec2(0, 16));
        if (ImGui::Button("Skip for now", ImVec2(140, 0))) skip_setup();
    } else if (hub.setup_path == SetupPath::Easy) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 520.f);
        ImGui::TextWrapped(
            "Choose your Emulation folder. RetComM will use …/roms, …/bios, and …/saves "
            "under it, create any that are missing, and seed default platform folders.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 10));
        if (ImGui::Button("Use ~/Emulation")) {
            hub.apply_suggested_emulation_root(/*overwrite_nonempty=*/true);
            hub.apply_roots_from_emulation_parent();
            hub.set_status("Suggested Emulation folder applied");
        }
        ImGui::Dummy(ImVec2(0, 10));
        if (path_field_with_browse("Emulation folder", "##setup_emulation_root",
                                   hub.setup_emulation_root, sizeof(hub.setup_emulation_root),
                                   hub, window, FolderPickTarget::EmulationRoot, th)) {
            hub.apply_roots_from_emulation_parent();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        if (hub.setup_emulation_root[0] != '\0') {
            const fs::path emu(hub.setup_emulation_root);
            const std::string roms = (emu / "roms").string();
            const std::string bios = (emu / "bios").string();
            const std::string saves = (emu / "saves").string();
            ImGui::TextWrapped("Will use:\n  %s\n  %s\n  %s", roms.c_str(), bios.c_str(),
                               saves.c_str());
        } else {
            ImGui::TextWrapped("Pick a parent folder (for example ~/Emulation).");
        }
        ImGui::PopStyleColor();

        const bool emu_ok = hub.setup_emulation_root[0] != '\0';
        ImGui::Dummy(ImVec2(0, 16));
        if (ImGui::Button("Back", ImVec2(120, 0))) {
            hub.setup_path = SetupPath::Chooser;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!emu_ok);
        if (accent_button("Finish", th, ImVec2(160, 0))) {
            std::string err;
            if (!hub.finish_easy_setup(&err)) {
                hub.append_log("easy setup failed: " + err);
                hub.set_status("Easy setup failed");
            } else {
                hub.set_status("Setup complete");
                hub.append_log("First-time easy setup saved");
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Skip for now", ImVec2(140, 0))) skip_setup();
        if (!emu_ok) {
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextColored(th.warn, "Choose an Emulation folder to continue.");
        }
    } else if (hub.setup_step == 0) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 520.f);
        ImGui::TextWrapped(
            "Step 1 of 2 — Set your ROM library, BIOS, and game-saves folders. Suggested "
            "paths use ~/Emulation/{roms,bios,saves}. Optionally connect RomM for library "
            "sync later.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 8));
        if (ImGui::Button("Use suggested paths")) {
            hub.apply_suggested_library_roots(/*overwrite_nonempty=*/true);
            hub.set_status("Suggested Emulation paths applied");
        }
        ImGui::Dummy(ImVec2(0, 10));

        if (path_field_with_browse("ROM library root", "##setup_library_root",
                                   hub.settings.library_root, sizeof(hub.settings.library_root),
                                   hub, window, FolderPickTarget::LibraryRoot, th))
            hub.settings.dirty = true;
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped("Required — EmulationStation / RomM-style root (e.g. …/roms).");
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 10));
        if (path_field_with_browse("BIOS root", "##setup_bios_root", hub.settings.bios_root,
                                   sizeof(hub.settings.bios_root), hub, window,
                                   FolderPickTarget::BiosRoot, th))
            hub.settings.dirty = true;
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped("Optional — system BIOS / firmware dumps (e.g. …/bios).");
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 10));
        if (path_field_with_browse("Game saves root", "##setup_saves_root",
                                   hub.settings.saves_root, sizeof(hub.settings.saves_root), hub,
                                   window, FolderPickTarget::SavesRoot, th))
            hub.settings.dirty = true;
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Recommended — shared SRAM / memcard library (e.g. …/saves). RomM sync and "
            "launches use per-platform folders under this root.");
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 14));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::TextColored(th.text_muted, "RomM (optional)");
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Leave blank for local-only. Use a Client API Token from RomM → Administration → "
            "Client API Tokens (Bearer rmm_…).");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::TextColored(th.text_muted, "RomM Instance URL");
        if (ImGui::InputText("##setup_romm_base_url", hub.romm_settings.base_url,
                             sizeof(hub.romm_settings.base_url)))
            hub.romm_settings.dirty = true;
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::TextColored(th.text_muted, "RomM Client API Key");
        if (ImGui::InputText("##setup_romm_api_token", hub.romm_settings.api_token,
                             sizeof(hub.romm_settings.api_token), ImGuiInputTextFlags_Password))
            hub.romm_settings.dirty = true;

        const bool library_ok = hub.settings.library_root[0] != '\0';
        ImGui::Dummy(ImVec2(0, 16));
        if (ImGui::Button("Back", ImVec2(120, 0))) {
            hub.setup_path = SetupPath::Chooser;
            hub.setup_confirm_create_roots = false;
            hub.setup_missing_roots.clear();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!library_ok);
        if (accent_button("Next", th, ImVec2(160, 0))) {
            hub.collect_missing_setup_roots();
            if (!hub.setup_missing_roots.empty()) {
                hub.setup_confirm_create_roots = true;
            } else {
                advance_to_platform_step();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Skip for now", ImVec2(140, 0))) skip_setup();
        if (!library_ok) {
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextColored(th.warn, "Choose a ROM library folder to continue.");
        }
    } else {
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 600.f);
        ImGui::TextWrapped(
            "Step 2 of 2 — Platform folder mappings (ES-DE / RomM defaults). On Finish, "
            "RetComM creates any missing platform folders using the first name in each "
            "comma-separated list when none of the aliases already exist.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::Checkbox("Create missing platform folders under roms / bios / saves",
                        &hub.setup_create_platform_folders);

        ImGui::Dummy(ImVec2(0, 8));
        ImGui::BeginChild("##setup_platform_table", ImVec2(0, 280.f), ImGuiChildFlags_Borders);
        if (ImGui::BeginTable("setup_platform_folders", 3,
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("platform", ImGuiTableColumnFlags_WidthFixed, 120.f);
            ImGui::TableSetupColumn("folders", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##rm", ImGuiTableColumnFlags_WidthFixed, 36.f);
            ImGui::TableHeadersRow();
            for (int i = 0; i < static_cast<int>(hub.settings.platform_folders.size()); ++i) {
                auto& row = hub.settings.platform_folders[static_cast<size_t>(i)];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::InputText("##plat", row.platform, sizeof(row.platform)))
                    hub.settings.dirty = true;
                ImGui::TableNextColumn();
                if (ImGui::InputText("##folders", row.folders, sizeof(row.folders)))
                    hub.settings.dirty = true;
                ImGui::TableNextColumn();
                if (ImGui::Button("X")) {
                    hub.settings.platform_folders.erase(hub.settings.platform_folders.begin() +
                                                        i);
                    hub.settings.dirty = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
        if (ImGui::Button("Add platform row")) hub.add_platform_folder_row();

        ImGui::Dummy(ImVec2(0, 14));
        if (ImGui::Button("Back", ImVec2(120, 0))) {
            hub.setup_step = 0;
        }
        ImGui::SameLine();
        if (accent_button("Finish", th, ImVec2(160, 0))) {
            std::string err;
            if (!hub.save_settings(&err)) {
                hub.append_log("setup save failed: " + err);
                hub.set_status("Setup save failed");
            } else if (hub.setup_create_platform_folders &&
                       !hub.create_setup_platform_folders(&err)) {
                hub.append_log("setup platform folders failed: " + err);
                hub.set_status("Could not create platform folders");
            } else if (!hub.save_romm_settings(&err, /*refresh_boxart=*/false)) {
                hub.append_log("setup RomM save failed: " + err);
                hub.set_status("Setup RomM save failed");
            } else if (!hub.complete_setup(&err)) {
                hub.append_log("setup marker failed: " + err);
                hub.set_status("Setup marker failed");
            } else {
                hub.set_status("Setup complete");
                hub.append_log("First-time setup saved");
                hub.show_setup_scan_prompt = true;
            }
        }
    }

    ImGui::EndPopup();
}

void draw_setup_scan_prompt(HubModel& hub, const Theme& th) {
    if (!hub.show_setup_scan_prompt) return;
    ImGui::OpenPopup("Scan library?###setup_scan_prompt");
    center_modal_next();
    if (!ImGui::BeginPopupModal("Scan library?###setup_scan_prompt", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 420.f);
    ImGui::TextWrapped(
        "Folder layout is ready. Scan your library now? This indexes ROMs, BIOS, and save "
        "files (and refreshes the catalog first).");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 14));
    if (accent_button("Scan now", th, ImVec2(140, 0))) {
        hub.show_setup_scan_prompt = false;
        hub.job_prefetch_catalog = true;
        hub.start_job(HubJob::ScanRoms);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Not now", ImVec2(120, 0))) {
        hub.show_setup_scan_prompt = false;
        hub.set_status("Setup complete — refresh later from Library");
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void draw_romm_settings_panel(HubModel& hub, const Theme& th) {
    ImGui::BeginChild("romm_settings", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextUnformatted("ROMM SYNC SETTINGS");
    ImGui::PopStyleColor();
    ImGui::TextWrapped(
        "Connect to a RomM instance for future sync. Use a Client API Token from "
        "RomM → Administration → Client API Tokens (Bearer rmm_…). No username/password "
        "needed in RetComM.");
    ImGui::Separator();

    ImGui::TextColored(th.text_muted, "RomM Instance URL");
    if (ImGui::InputText("##romm_base_url", hub.romm_settings.base_url,
                         sizeof(hub.romm_settings.base_url)))
        hub.romm_settings.dirty = true;

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextColored(th.text_muted, "RomM Client API Key");
    if (ImGui::InputText("##romm_api_token", hub.romm_settings.api_token,
                         sizeof(hub.romm_settings.api_token), ImGuiInputTextFlags_Password))
        hub.romm_settings.dirty = true;

    ImGui::Dummy(ImVec2(0, 10));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6));
    if (ImGui::Checkbox("Sync Boxart", &hub.romm_settings.sync_boxart))
        hub.romm_settings.dirty = true;
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    if (hub.romm_settings.sync_boxart)
        ImGui::TextWrapped("On: all covers use RomM (requires URL + API key).");
    else
        ImGui::TextWrapped("Off (default): all covers use Libretro Named_Boxarts.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    if (hub.cfg.romm.enabled())
        ImGui::TextWrapped("Configured: %s", hub.cfg.romm.base_url.c_str());
    else
        ImGui::TextWrapped("Not configured — set a URL and save to enable RomM.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 12));
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "RomM matching is on-demand during Install. Enable Sync Boxart here, then "
        "use Find Missing Boxart in Library Settings to refresh covers.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 12));
    if (accent_button("Save", th, ImVec2(160, 0))) {
        std::string err;
        if (!hub.save_romm_settings(&err)) {
            hub.append_log("RomM settings save failed: " + err);
            hub.set_status("Save failed");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        hub.show_romm_settings = false;
        hub.romm_settings.dirty = false;
    }
    if (hub.romm_settings.dirty) {
        ImGui::SameLine();
        ImGui::TextColored(th.warn, "unsaved changes");
    }
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextColored(th.text_muted, "%s", hub.paths.config_path.string().c_str());
    ImGui::EndChild();
}


void draw_log_collapsed_bar(HubModel& hub, const Theme& th) {
    constexpr float kBarH = 40.f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.f, 6.f));
    ImGui::BeginChild("log_collapsed", ImVec2(0, kBarH), ImGuiChildFlags_Borders);
    // Compact Show button so it sits inside the bar with a little breathing room.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.f, 3.f));
    const float btn_h = ImGui::GetFrameHeight();
    const float y = std::max(0.f, (ImGui::GetContentRegionAvail().y - btn_h) * 0.5f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + y);
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextUnformatted("ACTIVITY");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextColored(th.text_muted, "(collapsed)");
    {
        constexpr float kShowW = 64.f;
        const float right = ImGui::GetWindowContentRegionMax().x;
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 8.f, right - kShowW));
        if (ImGui::Button("Show", ImVec2(kShowW, 0))) hub.log_expanded = true;
    }
    ImGui::PopStyleVar(); // FramePadding
    ImGui::EndChild();
    ImGui::PopStyleVar(); // WindowPadding
}

void draw_log(HubModel& hub, const Theme& th, float height) {
    if (height < 60.f) height = 60.f;
    ImGui::BeginChild("log", ImVec2(0, height), ImGuiChildFlags_Borders);
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextUnformatted("ACTIVITY");
    ImGui::PopStyleColor();

    std::vector<retcomm::hub::LogLine> lines;
    std::string plain;
    {
        std::lock_guard<std::mutex> lock(hub.mu);
        lines = hub.log_lines;
        plain = hub.log;
    }
    if (plain.empty()) plain = "(no activity yet)";

    ImGui::SameLine();
    {
        const float hide_w =
            ImGui::CalcTextSize("Hide").x + ImGui::GetStyle().FramePadding.x * 2.f;
        const float copy_w =
            ImGui::CalcTextSize("Copy").x + ImGui::GetStyle().FramePadding.x * 2.f;
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float right = ImGui::GetWindowContentRegionMax().x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - hide_w - gap - copy_w));
        if (ImGui::SmallButton("Hide")) hub.log_expanded = false;
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy")) {
            // Tail only — recent errors/status matter more than early startup noise.
            constexpr size_t kCopyLines = 100;
            const size_t n = lines.size();
            const size_t start = n > kCopyLines ? n - kCopyLines : 0;
            std::string clip;
            for (size_t i = start; i < n; ++i) {
                if (!clip.empty()) clip.push_back('\n');
                clip += lines[i].text;
            }
            ImGui::SetClipboardText(clip.empty() ? "(no activity yet)" : clip.c_str());
        }
    }
    ImGui::Separator();

    // Console-style log: TextWrapped lines in one scroller (InputTextMultiline
    // kept its own inner scroll and fought stick-to-bottom). Use Copy for
    // clipboard; scroll follows new lines until the user scrolls up.
    ImGui::BeginChild("activity_scroll", ImVec2(0, 0), ImGuiChildFlags_None);
    static bool auto_scroll = true;

    const float prev_sy = ImGui::GetScrollY();
    const float prev_sm = ImGui::GetScrollMaxY();
    if (prev_sm > 1.f && prev_sy < prev_sm - 16.f) auto_scroll = false;

    if (lines.empty()) {
        ImGui::TextColored(th.text_muted, "%s", plain.c_str());
    } else {
        for (const auto& line : lines) {
            ImVec4 col = th.text;
            switch (line.level) {
            case retcomm::hub::LogLevel::Info:
                col = th.text_muted;
                break;
            case retcomm::hub::LogLevel::Accent:
                col = th.accent;
                break;
            case retcomm::hub::LogLevel::Good:
                col = th.good;
                break;
            case retcomm::hub::LogLevel::Warn:
                col = th.warn;
                break;
            case retcomm::hub::LogLevel::Error:
                col = ImVec4(0.95f, 0.35f, 0.40f, 1.f); // no Theme::error token
                break;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextWrapped("%s", line.text.c_str());
            ImGui::PopStyleColor();
        }
    }

    if (auto_scroll) ImGui::SetScrollHereY(1.f);
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 16.f) auto_scroll = true;

    ImGui::EndChild();
    ImGui::EndChild();
}

// Drag handle between the library/detail body and the activity log.
// log_h_pref is the user-chosen height; displayed height may shrink with the window
// but restores up to log_h_pref when space returns (never auto-grows past it).
void draw_log_splitter(float& log_h, float& log_h_pref, float avail_y, const Theme& th) {
    constexpr float kSplitH = 6.f;
    constexpr float kMinLog = 64.f;
    constexpr float kMinBody = 160.f;
    const float chrome = kSplitH + ImGui::GetStyle().ItemSpacing.y * 2.f;
    const float max_log = std::max(kMinLog, avail_y - kMinBody - chrome);
    log_h = std::clamp(log_h_pref, kMinLog, max_log);

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##body_log_split", ImVec2(-1.f, kSplitH));
    const ImVec2 p1 = ImGui::GetItemRectMax();
    const bool active = ImGui::IsItemActive();
    const bool hover = ImGui::IsItemHovered() || active;
    if (hover) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (active) {
        log_h_pref = std::clamp(log_h_pref - ImGui::GetIO().MouseDelta.y, kMinLog,
                                std::max(kMinLog, avail_y - kMinBody - chrome));
        log_h = std::clamp(log_h_pref, kMinLog, max_log);
    }
    const ImU32 col =
        ImGui::ColorConvertFloat4ToU32(hover ? th.accent : th.border);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, col);
    const float mid_y = (p0.y + p1.y) * 0.5f;
    dl->AddLine(ImVec2(p0.x + 24.f, mid_y), ImVec2(p1.x - 24.f, mid_y),
                ImGui::ColorConvertFloat4ToU32(th.text_muted), 1.f);
}

} // namespace

int main(int argc, char** argv) {
    (void)argc;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    const char* glsl = "#version 150";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_Window* window = SDL_CreateWindow("RetComM Launcher", 1280, 800,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (!gl) {
        std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    load_hub_fonts();

    const Theme th = retcomm::hub::crt_theme();
    retcomm::hub::apply_imgui_style(th);

    ImGui_ImplSDL3_InitForOpenGL(window, gl);
    ImGui_ImplOpenGL3_Init(glsl);

    HubModel hub;
    hub.paths = retcomm::default_paths();
    if (argc > 0 && argv[0] && argv[0][0] != '\0') {
        std::error_code ec;
        hub.exe_dir = fs::weakly_canonical(fs::path(argv[0]).parent_path(), ec);
        if (ec || hub.exe_dir.empty()) hub.exe_dir = fs::path(argv[0]).parent_path();
    }
    hub.cfg = retcomm::load_app_config(hub.paths.config_path);
    bool catalog_updated = false;
    try {
        retcomm::ensure_dirs(hub.paths);
        const auto sync = retcomm::maybe_auto_update_catalog(hub.paths, hub.cfg);
        if (!sync.ok && !sync.skipped)
            hub.append_log(std::string("catalog auto-update: ") + sync.message);
        else if (sync.ok && !sync.skipped) {
            hub.append_log(sync.message);
            catalog_updated = true;
        }
        const fs::path cat = retcomm::resolve_catalog_dir(fs::path(argv[0]).parent_path(), {},
                                                          &hub.paths);
        hub.catalog = retcomm::load_catalog(cat);
        std::string cat_log = "Catalog: " + cat.string() + " (" +
                              std::to_string(hub.catalog.titles.size()) + " titles";
        if (!hub.catalog.release_tag.empty())
            cat_log += ", " + hub.catalog.release_tag;
        if (!hub.catalog.catalog_date.empty())
            cat_log += ", " + hub.catalog.catalog_date;
        cat_log += ")";
        hub.append_log(cat_log);
    } catch (const std::exception& e) {
        hub.append_log(std::string("catalog error: ") + e.what());
    }
    hub.launcher_version = retcomm::retcomm_app_version();
    hub.refresh_rows(false);
    // Prompt when neither a setup marker nor required config exists.
    // Updates wipe exe-dir markers; if library_root is already set, skip the
    // wizard and refresh the durable data-dir marker.
    if (!retcomm::hub_setup_completed(hub.paths, hub.exe_dir)) {
        if (!hub.cfg.library_root.empty()) {
            std::string marker_err;
            if (!retcomm::mark_hub_setup_completed(hub.paths, hub.exe_dir, &marker_err))
                hub.append_log("setup marker migrate failed: " + marker_err);
            hub.set_status("Ready");
        } else {
            hub.open_setup(); // pre-fills any partial config.json
            hub.set_status("First-time setup — choose your ROM library folder");
        }
    } else {
        hub.set_status("Ready");
    }
    // After a real catalog download, pull covers for titles missing from cache.
    if (catalog_updated) hub.start_job(HubJob::FetchBoxart);
    // Defer game + toolchain update checks until idle (after catalog boxart job).
    hub.pending_startup_update_check = hub.cfg.check_updates_on_startup;

    retcomm::hub::BoxartCache boxart;
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) running = false;
            if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                e.window.windowID == SDL_GetWindowID(window))
                running = false;
        }
        if (hub.request_exit.load()) running = false;

        hub.apply_pending_folder_pick();
        hub.apply_pending_file_pick();

        if (hub.pending_startup_update_check && !hub.job_running.load()) {
            hub.pending_startup_update_check = false;
            if (hub.cfg.check_updates_on_startup) hub.start_job(HubJob::CheckUpdates);
        }
        // Play preflight finished with no update → start Launch on the main thread.
        if (!hub.job_running.load() && !hub.launch_running.load()) {
            std::string launch_id;
            {
                std::lock_guard<std::mutex> lock(hub.mu);
                launch_id = std::move(hub.pending_launch_title_id);
                hub.pending_launch_title_id.clear();
            }
            if (!launch_id.empty()) hub.start_job(HubJob::Launch, launch_id);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##hub", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        draw_marquee(hub, th, ImGui::GetContentRegionAvail().x);

        // Body + optional activity log must fit the remaining region exactly — ImGui adds
        // ItemSpacing between each, so subtract that or the outer window scrolls.
        static float log_h_pref = 140.f; // manual height; window grow restores up to this
        const float avail_y = ImGui::GetContentRegionAvail().y;
        constexpr float kSplitH = 6.f;
        constexpr float kMinLog = 64.f;
        constexpr float kCollapsedLog = 40.f;
        constexpr float kMinBody = 160.f;
        const float gap_y = ImGui::GetStyle().ItemSpacing.y;
        float log_h = 0.f;
        float body_h = avail_y;
        if (hub.log_expanded) {
            const float chrome = kSplitH + gap_y * 2.f;
            const float max_log = std::max(kMinLog, avail_y - kMinBody - chrome);
            log_h = std::clamp(log_h_pref, kMinLog, max_log);
            body_h = std::max(kMinBody, avail_y - log_h - chrome);
        } else {
            const float chrome = gap_y;
            log_h = kCollapsedLog;
            body_h = std::max(kMinBody, avail_y - log_h - chrome);
        }

        ImGui::BeginChild("body", ImVec2(0, body_h), ImGuiChildFlags_None);

        if (hub.show_settings) {
            ImGui::BeginChild("settings_host", ImVec2(0, 0), ImGuiChildFlags_None);
            draw_settings_panel(hub, th, window);
            ImGui::EndChild();
        } else if (hub.show_romm_settings) {
            ImGui::BeginChild("romm_settings_host", ImVec2(0, 0), ImGuiChildFlags_None);
            draw_romm_settings_panel(hub, th);
            ImGui::EndChild();
        } else {
            // Extra width goes to the library; detail is flexible but capped at the
            // default (~1280) panel width so maximize doesn't stretch the right column.
            constexpr float kDetailMaxW = 480.f;
            constexpr float kDetailMinW = 280.f;
            constexpr float kLibraryMinW = 320.f;
            const float total_w = ImGui::GetContentRegionAvail().x;
            const float gap_x = ImGui::GetStyle().ItemSpacing.x;
            float right_w = std::min(kDetailMaxW, total_w * 0.42f);
            right_w = std::clamp(right_w, kDetailMinW,
                                 std::max(kDetailMinW, total_w - kLibraryMinW - gap_x));
            const float mid_w = std::max(kLibraryMinW, total_w - right_w - gap_x);

            ImGui::BeginChild("mid", ImVec2(mid_w, 0), ImGuiChildFlags_None);
            draw_library(hub, boxart, th);
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("right", ImVec2(right_w, 0), ImGuiChildFlags_None);
            draw_detail(hub, boxart, th, window);
            ImGui::EndChild();
        }

        ImGui::EndChild(); // body
        if (hub.log_expanded) {
            draw_log_splitter(log_h, log_h_pref, avail_y, th);
            draw_log(hub, th, log_h);
        } else {
            draw_log_collapsed_bar(hub, th);
        }
        draw_setup_wizard(hub, boxart, th, window);
        draw_setup_scan_prompt(hub, th);
        draw_menu_popup(hub, th, window);
        draw_library_popup(hub, th, window);

        // Import / scan toasts.
        {
            static std::string toast_text;
            static double toast_until = 0.0;
            if (hub.toast_pending.exchange(false)) {
                std::lock_guard<std::mutex> lock(hub.mu);
                toast_text = hub.toast_message;
                toast_until = ImGui::GetTime() + 4.5;
            }
            if (!toast_text.empty() && ImGui::GetTime() < toast_until) {
                const ImGuiViewport* tvp = ImGui::GetMainViewport();
                const ImVec2 ts = ImGui::CalcTextSize(toast_text.c_str(), nullptr, false, 420.f);
                const float pad = 14.f;
                const ImVec2 sz(std::min(440.f, ts.x + pad * 2.f), ts.y + pad * 2.f);
                const ImVec2 pos(tvp->WorkPos.x + (tvp->WorkSize.x - sz.x) * 0.5f,
                                 tvp->WorkPos.y + tvp->WorkSize.y - sz.y - 48.f);
                ImGui::SetNextWindowPos(pos);
                ImGui::SetNextWindowSize(sz);
                ImGui::PushStyleColor(ImGuiCol_WindowBg, th.background2);
                ImGui::PushStyleColor(ImGuiCol_Border, th.accent);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
                ImGui::Begin("##hub_toast", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoSavedSettings);
                ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + sz.x - pad * 2.f);
                ImGui::TextColored(th.text, "%s", toast_text.c_str());
                ImGui::PopTextWrapPos();
                ImGui::End();
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(2);
            } else if (ImGui::GetTime() >= toast_until) {
                toast_text.clear();
            }
        }

        if (hub.show_install_root_prompt) {
            ImGui::OpenPopup("Install location###install_root_prompt");
            hub.show_install_root_prompt = false;
        }
        if (ImGui::BeginPopupModal("Install location###install_root_prompt", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            const std::string tid = hub.install_root_prompt_id;
            const TitleRow* prow = nullptr;
            for (const auto& r : hub.rows) {
                if (r.id == tid) {
                    prow = &r;
                    break;
                }
            }
            const auto roots = retcomm::effective_install_roots(hub.cfg, hub.paths);
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 420.f);
            ImGui::TextWrapped("%s", prow ? prow->name.c_str() : tid.c_str());
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextWrapped("Choose where to install this game.");
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 8));
            if (roots.empty()) {
                ImGui::TextColored(th.warn, "No install locations configured.");
            } else {
                for (int i = 0; i < static_cast<int>(roots.size()); ++i) {
                    ImGui::PushID(i);
                    const auto& e = roots[static_cast<size_t>(i)];
                    char label[1152];
                    std::snprintf(label, sizeof(label), "%s\n%s",
                                  e.label.empty() ? "Install" : e.label.c_str(),
                                  e.path.string().c_str());
                    if (ImGui::RadioButton(label, &hub.install_root_prompt_index, i)) {
                        // index updated by RadioButton
                    }
                    ImGui::PopID();
                }
            }
            ImGui::Dummy(ImVec2(0, 10));
            const bool busy = hub.job_running.load() || tid.empty() || roots.empty();
            ImGui::BeginDisabled(busy);
            if (good_button("Confirm", th, ImVec2(-1, 0))) {
                hub.confirm_install_root_and_continue();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            if (ImGui::Button("Cancel", ImVec2(-1, 0))) {
                hub.install_root_prompt_id.clear();
                hub.job_apps_dir.clear();
                ImGui::CloseCurrentPopup();
            }
            close_modal_on_outside_click();
            ImGui::EndPopup();
        }

        if (hub.show_missing_rom_prompt) {
            ImGui::OpenPopup("ROM not found###missing_rom_prompt");
            hub.show_missing_rom_prompt = false;
        }
        if (ImGui::BeginPopupModal("ROM not found###missing_rom_prompt", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            const std::string tid = hub.missing_rom_prompt_id;
            const TitleRow* prow = nullptr;
            for (const auto& r : hub.rows) {
                if (r.id == tid) {
                    prow = &r;
                    break;
                }
            }
            const std::string plat = prow ? prow->platform : std::string{};
            const char* plat_label = plat.empty() ? "this platform" : platform_display_name(plat);
            const bool romm_ok = prow && prow->romm_ready && prow->has_rom_identity;
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 420.f);
            ImGui::TextWrapped("%s", prow ? prow->name.c_str() : tid.c_str());
            ImGui::Dummy(ImVec2(0, 6));
            if (romm_ok) {
                ImGui::TextWrapped(
                    "No verified ROM is in your library yet. Rescan %s for a matching dump, "
                    "or download from RomM (multi-track discs include the full .cue + track set).",
                    plat_label);
            } else {
                ImGui::TextWrapped(
                    "No verified ROM is in your library yet. Add the dump locally (Menu → "
                    "Import), then rescan %s. Configure RomM Sync Settings to enable download.",
                    plat_label);
            }
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 10));
            const bool busy = hub.job_running.load() || tid.empty();
            ImGui::BeginDisabled(busy || plat.empty());
            char scan_label[128];
            std::snprintf(scan_label, sizeof(scan_label), "Rescan %s Library", plat_label);
            if (good_button(scan_label, th, ImVec2(-1, 0))) {
                hub.scans_platform_filter = plat;
                hub.pending_scan_missing_rom_id = tid;
                hub.start_job(HubJob::ScanRoms);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(busy || !romm_ok);
            if (romm_button("Download from RomM", th, ImVec2(-1, 0))) {
                hub.start_job(HubJob::Install, tid, false, true);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            if (ImGui::Button("Cancel", ImVec2(-1, 0))) ImGui::CloseCurrentPopup();
            close_modal_on_outside_click();
            ImGui::EndPopup();
        }

        if (hub.open_rom_folder_prompt_pending.exchange(false))
            ImGui::OpenPopup("ROM still missing###open_rom_folder_prompt");
        if (ImGui::BeginPopupModal("ROM still missing###open_rom_folder_prompt", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            std::string tid, plat;
            {
                std::lock_guard<std::mutex> lock(hub.mu);
                tid = hub.open_rom_folder_prompt_id;
                plat = hub.open_rom_folder_prompt_platform;
            }
            const TitleRow* prow = nullptr;
            for (const auto& r : hub.rows) {
                if (r.id == tid) {
                    prow = &r;
                    break;
                }
            }
            const char* plat_label = plat.empty() ? "this platform" : platform_display_name(plat);
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 420.f);
            ImGui::TextWrapped("%s", prow ? prow->name.c_str() : tid.c_str());
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextWrapped(
                "Refresh did not find a verified dump for this title under %s. "
                "Open the ROM folder to add the files, then refresh again from Library "
                "(or download from RomM if configured).",
                plat_label);
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 10));
            ImGui::BeginDisabled(plat.empty() || hub.cfg.library_root.empty());
            char open_label[96];
            std::snprintf(open_label, sizeof(open_label), "Open %s ROM Folder", plat_label);
            if (ImGui::Button(open_label, ImVec2(-1, 0))) {
                const fs::path dir =
                    retcomm::ensure_platform_dir(hub.cfg.library_root,
                                                 hub.cfg.folders_for_platform(plat));
                std::string err;
                if (dir.empty() || !retcomm::open_path_in_file_manager(dir, &err)) {
                    hub.append_log("Open ROM folder failed: " +
                                   (err.empty() ? dir.string() : err));
                    hub.set_status("Could not open ROM folder");
                } else {
                    hub.set_status("Opened " + dir.string());
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            const bool romm_ok = prow && prow->romm_ready && prow->has_rom_identity;
            ImGui::BeginDisabled(hub.job_running.load() || !romm_ok || tid.empty());
            if (romm_button("Download from RomM", th, ImVec2(-1, 0))) {
                hub.start_job(HubJob::Install, tid, false, true);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            if (ImGui::Button("Close", ImVec2(-1, 0))) ImGui::CloseCurrentPopup();
            close_modal_on_outside_click();
            ImGui::EndPopup();
        }

        if (hub.launch_update_prompt_pending.exchange(false))
            ImGui::OpenPopup("Update available###launch_update_prompt");
        if (ImGui::BeginPopupModal("Update available###launch_update_prompt", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            std::string tid, from, to, name;
            {
                std::lock_guard<std::mutex> lock(hub.mu);
                tid = hub.launch_update_prompt_id;
                from = hub.launch_update_from;
                to = hub.launch_update_to;
            }
            for (const auto& r : hub.rows) {
                if (r.id == tid) {
                    name = r.name;
                    break;
                }
            }
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 420.f);
            ImGui::TextWrapped("%s", name.empty() ? tid.c_str() : name.c_str());
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextWrapped(
                "A newer release is available.\n\nInstalled: %s\nLatest: %s\n\n"
                "Update before playing?",
                from.empty() ? "?" : from.c_str(), to.empty() ? "?" : to.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 10));
            const bool busy = hub.job_running.load() || hub.launch_running.load();
            ImGui::BeginDisabled(busy || tid.empty());
            if (good_button("Update", th, ImVec2(-1, 0))) {
                hub.start_job(HubJob::Update, tid);
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::Button("Play Without Updating", ImVec2(-1, 0))) {
                hub.start_job(HubJob::Launch, tid);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            if (ImGui::Button("Cancel", ImVec2(-1, 0))) ImGui::CloseCurrentPopup();
            close_modal_on_outside_click();
            ImGui::EndPopup();
        }

        // Update prompts in order: launcher → games → toolchain.
        // Avoid opening the next modal on the same frame (IsPopupOpen can lag OpenPopup).
        const bool open_launcher_update = hub.launcher_update_prompt_pending.exchange(false);
        if (open_launcher_update) ImGui::OpenPopup("RetComM update###launcher_update");
        if (ImGui::BeginPopupModal("RetComM update###launcher_update", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            std::string cur, latest;
            {
                std::lock_guard<std::mutex> lock(hub.mu);
                cur = hub.launcher_current_version;
                latest = hub.launcher_latest_tag;
            }
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 380.f);
            ImGui::TextWrapped(
                "A newer RetComM Launcher release is available.\n\n"
                "Installed: %s\nLatest: %s\n\n"
                "Update now? The app will download the package and restart.",
                cur.empty() ? "?" : cur.c_str(),
                latest.empty() ? "?" : latest.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 10));
            // Never gate these on job_running — CheckUpdates used to arm this modal
            // mid-job and left Update greyed out for the whole game/toolchain scan.
            if (accent_button("Update RetComM", th, ImVec2(160, 0))) {
                hub.cancel_prefetch_updates();
                hub.discard_followup_update_prompts();
                hub.start_job(HubJob::SelfUpdate);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Later", ImVec2(120, 0))) {
                hub.release_deferred_followup_updates();
                ImGui::CloseCurrentPopup();
            }
            close_modal_on_outside_click();
            ImGui::EndPopup();
        }

        const bool launcher_self_updating =
            hub.request_exit.load() ||
            (hub.job_running.load() && hub.job == HubJob::SelfUpdate);
        // Outside-click dismiss is "Later" — release deferred game/toolchain prompts.
        {
            static bool launcher_modal_was_open = false;
            const bool launcher_modal_open =
                open_launcher_update || ImGui::IsPopupOpen("RetComM update###launcher_update");
            if (launcher_modal_was_open && !launcher_modal_open && !launcher_self_updating)
                hub.release_deferred_followup_updates();
            launcher_modal_was_open = launcher_modal_open;
        }
        const bool launcher_blocking =
            launcher_self_updating || open_launcher_update ||
            ImGui::IsPopupOpen("RetComM update###launcher_update") ||
            hub.launcher_update_prompt_pending.load();
        const bool open_game_updates =
            !launcher_blocking && hub.game_updates_prompt_pending.exchange(false);
        if (open_game_updates) ImGui::OpenPopup("Game updates###game_updates");
        if (ImGui::BeginPopupModal("Game updates###game_updates", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            int n = 0;
            {
                std::lock_guard<std::mutex> lock(hub.mu);
                n = hub.game_updates_prompt_count;
            }
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 380.f);
            if (n == 1) {
                ImGui::TextWrapped(
                    "1 installed game has an update available.\n\n"
                    "Open the title and use Update when you want to install it.");
            } else {
                ImGui::TextWrapped(
                    "%d installed games have updates available.\n\n"
                    "Open a title and use Update when you want to install it.",
                    n);
            }
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 10));
            if (ImGui::Button("OK", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
            close_modal_on_outside_click();
            ImGui::EndPopup();
        }

        const bool games_blocking =
            open_game_updates || ImGui::IsPopupOpen("Game updates###game_updates") ||
            hub.game_updates_prompt_pending.load();
        if (!launcher_blocking && !games_blocking &&
            hub.toolchain_prompt_pending.exchange(false))
            ImGui::OpenPopup("Toolchain update###toolchain_update");
        if (ImGui::BeginPopupModal("Toolchain update###toolchain_update", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            std::string cur, latest;
            {
                std::lock_guard<std::mutex> lock(hub.mu);
                cur = hub.toolchain_current_version;
                latest = hub.toolchain_latest_tag;
            }
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 380.f);
            ImGui::TextWrapped(
                "A newer portable toolchain (cmake-clang-v1) is available.\n\n"
                "Installed: %s\nLatest: %s\n\n"
                "Update now? Builds that use the shared RetComM toolchain cache "
                "will pick up the new pack.",
                cur.empty() ? "?" : cur.c_str(),
                latest.empty() ? "?" : latest.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 10));
            const bool busy = hub.job_running.load();
            ImGui::BeginDisabled(busy);
            if (accent_button("Update Toolchain", th, ImVec2(160, 0))) {
                hub.start_job(HubJob::UpdateToolchain);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Later", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndDisabled();
            close_modal_on_outside_click();
            ImGui::EndPopup();
        }

        if (hub.orphan_prompt_pending.exchange(false))
            ImGui::OpenPopup("Unlisted installs###orphan_cleanup");
        if (ImGui::BeginPopupModal("Unlisted installs###orphan_cleanup", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            std::vector<retcomm::OrphanInstall> orphans;
            {
                std::lock_guard<std::mutex> lock(hub.mu);
                orphans = hub.pending_orphans;
            }
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 420.f);
            ImGui::TextWrapped(
                "These local installs are no longer in the catalog. Remove them to free "
                "disk space? Library ROMs and managed saves_root files are not deleted.");
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::BeginChild("orphan_list", ImVec2(420, 140), ImGuiChildFlags_Borders);
            for (const auto& o : orphans) {
                std::string line = o.title_id;
                if (o.title_id != o.dir_name) line += " (" + o.dir_name + ")";
                if (!o.tag.empty()) line += " @" + o.tag;
                if (o.has_preserved_only) line += " [preserved saves only]";
                ImGui::BulletText("%s", line.c_str());
            }
            if (orphans.empty()) ImGui::TextDisabled("(none)");
            ImGui::EndChild();
            ImGui::Dummy(ImVec2(0, 10));
            const bool busy = hub.job_running.load();
            ImGui::BeginDisabled(busy || orphans.empty());
            if (accent_button("Remove (keep saves)", th, ImVec2(180, 0))) {
                hub.start_job(HubJob::CleanupOrphans);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Remove + delete saves", ImVec2(180, 0))) {
                hub.start_job(HubJob::CleanupOrphansPurge);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Keep", ImVec2(100, 0))) ImGui::CloseCurrentPopup();
            close_modal_on_outside_click();
            ImGui::EndPopup();
        }

        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(th.background.x, th.background.y, th.background.z, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    hub.join_worker();
    boxart.destroy_all();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
