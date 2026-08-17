#include "hub/hub_boxart.hpp"
#include "hub/hub_model.hpp"
#include "hub/hub_theme.hpp"

#include "retcomm/catalog_sync.hpp"
#include "retcomm/config.hpp"
#include "retcomm/http.hpp"
#include "retcomm/paths.hpp"
#include "retcomm/psx_input_profiles.hpp"
#include "retcomm/romm_saves.hpp"
#include "retcomm/self_update.hpp"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#if defined(IMGUI_ENABLE_FREETYPE)
#include "misc/freetype/imgui_freetype.h"
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_opengl.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <set>
#include <unordered_set>

#include <algorithm>
#include <cctype>
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
// Merge symbols + (when FreeType is enabled) CBDT color emoji (Noto Color Emoji).
void load_hub_fonts() {
    ImGuiIO& io = ImGui::GetIO();
#if defined(IMGUI_ENABLE_FREETYPE)
    // Color-layered glyphs (CBDT/CBLC) for Noto Color Emoji / Segoe UI Emoji.
    io.Fonts->FontBuilderFlags |= ImGuiFreeTypeBuilderFlags_LoadColor;
#endif
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

    auto merge_font_if_present = [&](const char* path, const ImWchar* ranges,
                                     bool color_emoji) -> bool {
        if (!path || !*path || !ranges) return false;
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) return false;
        ImFontConfig merge_cfg;
        merge_cfg.MergeMode = true;
        merge_cfg.PixelSnapH = true;
        // Color emoji bitmaps ignore oversampling; keep outline fonts crisp.
        merge_cfg.OversampleH = color_emoji ? 1 : 2;
        merge_cfg.OversampleV = color_emoji ? 1 : 2;
#if defined(IMGUI_ENABLE_FREETYPE)
        if (color_emoji) merge_cfg.FontBuilderFlags |= ImGuiFreeTypeBuilderFlags_LoadColor;
#else
        if (color_emoji) return false;
#endif
        if (!io.Fonts->AddFontFromFileTTF(path, kBody, &merge_cfg, ranges)) return false;
        std::fprintf(stderr, "retcomm-hub: merged %s font %s\n",
                     color_emoji ? "color-emoji" : "symbol/emoji", path);
        return true;
    };
    static const ImWchar kSymbolRanges[] = {
        0x2000, 0x206F, // General Punctuation
        0x2190, 0x21FF, // Arrows
        0x2300, 0x23FF, // Misc Technical
        0x2460, 0x24FF, // Enclosed Alphanumerics
        0x25A0, 0x25FF, // Geometric Shapes
        0x2600, 0x26FF, // Misc Symbols (⚠)
        0x2700, 0x27BF, // Dingbats
        0x2B00, 0x2BFF, // Misc Symbols and Arrows
        0xFE00, 0xFE0F, // Variation Selectors
        0,
    };
    const char* kSymbolCandidates[] = {
        "/usr/share/fonts/noto/NotoSansSymbols2-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansSymbols-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf",
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\seguisym.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Apple Symbols.ttf",
        "/System/Library/Fonts/Supplemental/Apple Symbols.ttf",
#endif
        nullptr,
    };
    bool merged_symbols = false;
    for (int i = 0; kSymbolCandidates[i]; ++i) {
        if (merge_font_if_present(kSymbolCandidates[i], kSymbolRanges, false)) {
            merged_symbols = true;
            break;
        }
    }
#ifdef IMGUI_USE_WCHAR32
    static const ImWchar kEmojiRanges[] = {
        0x2600, 0x26FF,   // Misc Symbols (color ⚠ when available)
        0x2700, 0x27BF,   // Dingbats
        0x1F300, 0x1F5FF, // Misc Symbols and Pictographs
        0x1F600, 0x1F64F, // Emoticons
        0x1F680, 0x1F6FF, // Transport and Map
        0x1F900, 0x1F9FF, // Supplemental Symbols and Pictographs
        0,
    };
    const char* kColorEmojiCandidates[] = {
#if defined(IMGUI_ENABLE_FREETYPE)
        "/usr/share/fonts/noto/NotoColorEmoji.ttf",
        "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
        "/usr/share/fonts/google-noto-color-emoji/NotoColorEmoji.ttf",
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\seguiemj.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Apple Color Emoji.ttc",
#endif
#endif
        nullptr,
    };
    const char* kOutlineEmojiCandidates[] = {
        "/usr/share/fonts/noto/NotoEmoji-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoEmoji-Regular.ttf",
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\seguiemj.ttf",
#endif
        nullptr,
    };
    bool merged_emoji = false;
    for (int i = 0; kColorEmojiCandidates[i]; ++i) {
        if (merge_font_if_present(kColorEmojiCandidates[i], kEmojiRanges, true)) {
            merged_emoji = true;
            break;
        }
    }
    if (!merged_emoji) {
        for (int i = 0; kOutlineEmojiCandidates[i]; ++i) {
            if (merge_font_if_present(kOutlineEmojiCandidates[i], kEmojiRanges, false)) {
                merged_emoji = true;
                break;
            }
        }
    }
    if (!merged_emoji) {
#if defined(IMGUI_ENABLE_FREETYPE)
        std::fprintf(stderr, "retcomm-hub: no color/outline emoji font found\n");
#else
        std::fprintf(stderr,
                     "retcomm-hub: no outline emoji font found (rebuild with FreeType for "
                     "color emoji)\n");
#endif
    }
#endif
    if (!merged_symbols) {
        std::fprintf(stderr,
                     "retcomm-hub: no symbol fallback font found (⚠ may not render)\n");
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

void begin_export_activity_log(HubModel& hub, SDL_Window* window) {
    {
        std::lock_guard<std::mutex> lock(hub.file_pick_mu);
        if (hub.file_pick_busy) return;
        hub.file_pick_busy = true;
        hub.file_pick_kind = retcomm::hub::FilePickKind::ExportActivityLog;
        hub.file_pick_platform.clear();
        hub.file_pick_title_id.clear();
        hub.file_pick_paths.clear();
        hub.file_pick_filter_name = "Log files";
        hub.file_pick_filter_pattern = "log";
        hub.file_pick_default_location =
            (retcomm::user_home_dir() / "retcomm-launcher.log").string();
    }
    static SDL_DialogFileFilter filters[1];
    filters[0].name = hub.file_pick_filter_name.c_str();
    filters[0].pattern = hub.file_pick_filter_pattern.c_str();
    SDL_ShowSaveFileDialog(on_file_dialog, &hub, window, filters, 1,
                           hub.file_pick_default_location.c_str());
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
    Queued,     // pause mark — waiting in Install/Update backlog
};

TileStatusIcon tile_status_icon(const TitleRow& r) {
    if (r.update_available) return TileStatusIcon::Update;
    if (r.installed) return TileStatusIcon::Installed;
    if (r.install_dir_present) return TileStatusIcon::NeedsSetup;
    if (r.has_rom) return TileStatusIcon::RomReady;
    if (r.has_romm) return TileStatusIcon::OnRomm;
    // Default Install builds whenever a local recipe exists (even if a zip is
    // also advertised) — surface the missing-ROM badge for those titles.
    if (r.supports_local_build) return TileStatusIcon::NoRom;
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
    case TileStatusIcon::Queued: {
        // Pause bars (download waiting in queue).
        const float bw = rad * 0.22f;
        const float bh = rad * 0.55f;
        const float gap = rad * 0.18f;
        dl->AddRectFilled(ImVec2(c.x - gap - bw, c.y - bh * 0.5f),
                          ImVec2(c.x - gap, c.y + bh * 0.5f), ink, 1.4f);
        dl->AddRectFilled(ImVec2(c.x + gap, c.y - bh * 0.5f),
                          ImVec2(c.x + gap + bw, c.y + bh * 0.5f), ink, 1.4f);
        break;
    }
    case TileStatusIcon::RomReady: {
        // Check in a circle — local ROM verified / ready to install.
        dl->AddCircle(c, rad * 0.55f, ink, 16, 1.7f);
        const ImVec2 a(c.x - rad * 0.32f, c.y + rad * 0.02f);
        const ImVec2 b(c.x - rad * 0.06f, c.y + rad * 0.30f);
        const ImVec2 d(c.x + rad * 0.38f, c.y - rad * 0.28f);
        dl->AddLine(a, b, ink, 1.9f);
        dl->AddLine(b, d, ink, 1.9f);
        break;
    }
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
            chip = (!st.empty() && st != "Ready") ? st : "Launching…";
            if (chip.size() > 42) chip = chip.substr(0, 39) + "…";
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

        // Second chip: waiting Install/Update queue (only when non-empty).
        const std::size_t qn = hub.queued_job_count();
        if (qn > 0) {
            char qbuf[48];
            if (qn == 1)
                std::snprintf(qbuf, sizeof(qbuf), "1 queued");
            else
                std::snprintf(qbuf, sizeof(qbuf), "%zu queued", qn);
            const ImVec2 qsz = ImGui::CalcTextSize(qbuf);
            const float qw = qsz.x + chip_pad_x * 2.f;
            const float qh = qsz.y + chip_pad_y * 2.f;
            const float qx = chip_x + chip_w + 8.f;
            const float qy = p0.y + (h - qh) * 0.5f;
            const ImVec2 q0(qx, qy);
            const ImVec2 q1(qx + qw, qy + qh);
            dl->AddRectFilled(q0, q1, ImGui::ColorConvertFloat4ToU32(th.background2), 6.f);
            dl->AddRect(q0, q1, ImGui::ColorConvertFloat4ToU32(th.focus), 6.f, 0, 1.5f);
            dl->AddText(ImVec2(qx + chip_pad_x, qy + chip_pad_y),
                        ImGui::ColorConvertFloat4ToU32(th.focus), qbuf);
        }
    }

    // Top-right: Add/Scan Files + Check for Updates + Menu, or Back when editing settings.
    constexpr float kMenuH = 36.f;
    constexpr float kBtnGap = 8.f;
    const bool in_settings =
        hub.show_settings || hub.show_romm_settings || hub.show_psx_settings;
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
            hub.show_psx_settings = false;
            hub.settings.dirty = false;
            hub.romm_settings.dirty = false;
            hub.psx_settings.dirty = false;
            hub.psx_settings.capturing_hotkey = -1;
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

// Slightly smaller title under cover art: up to two centered lines, then ellipsis.
// Returns pixel height used (always reserves room for two lines when drawing).
float draw_wrapped_title_centered(const char* text, float max_w, const ImVec4& col,
                                  float font_scale = 0.84f) {
    ImGui::SetWindowFontScale(font_scale);
    const float line_h = ImGui::GetTextLineHeight();
    const float block_h = line_h * 2.f;
    if (!text || !text[0] || max_w <= 4.f) {
        ImGui::SetWindowFontScale(1.f);
        return block_h;
    }

    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize();
    const float scale = (font && font->FontSize > 0.f) ? (font_size / font->FontSize) : 1.f;
    const char* end = text + std::strlen(text);
    const char* mid =
        font ? font->CalcWordWrapPositionA(scale, text, end, max_w) : end;

    std::string l1(text, mid);
    while (!l1.empty() && (l1.back() == ' ' || l1.back() == '\t')) l1.pop_back();

    std::string l2;
    if (mid < end) {
        while (mid < end && (*mid == ' ' || *mid == '\t')) ++mid;
        l2.assign(mid, end);
        if (ImGui::CalcTextSize(l2.c_str()).x > max_w) {
            const char* ell = "...";
            while (l2.size() > 1 && ImGui::CalcTextSize((l2 + ell).c_str()).x > max_w)
                l2.pop_back();
            l2 += ell;
        }
    }

    const ImVec2 base = ImGui::GetCursorScreenPos();
    // Vertically center one-line titles inside the reserved two-line block.
    const float used_h = l2.empty() ? line_h : (line_h * 2.f);
    const float y0 = base.y + std::max(0.f, (block_h - used_h) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    auto draw_line = [&](const std::string& s, float y) {
        if (s.empty()) return;
        const ImVec2 sz = ImGui::CalcTextSize(s.c_str());
        ImGui::SetCursorScreenPos(ImVec2(base.x + std::max(0.f, (max_w - sz.x) * 0.5f), y));
        ImGui::TextUnformatted(s.c_str());
    };
    draw_line(l1, y0);
    if (!l2.empty()) draw_line(l2, y0 + line_h);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.f);
    return block_h;
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

void draw_queued_overlay(ImDrawList* dl, const ImVec2& art0, const ImVec2& art1, const Theme& th) {
    // Soft “paused download” disc — waiting in the Install/Update backlog.
    const ImVec2 c((art0.x + art1.x) * 0.5f, (art0.y + art1.y) * 0.5f);
    const float s = std::min(art1.x - art0.x, art1.y - art0.y);
    const float rad = s * 0.20f;
    dl->AddCircleFilled(c, rad, IM_COL32(12, 16, 28, 230), 36);
    dl->AddCircle(c, rad, ImGui::ColorConvertFloat4ToU32(th.focus), 36, 2.2f);
    const ImU32 ink = ImGui::ColorConvertFloat4ToU32(th.focus);
    // Pause bars.
    const float bw = rad * 0.20f;
    const float bh = rad * 0.52f;
    const float gap = rad * 0.16f;
    dl->AddRectFilled(ImVec2(c.x - gap - bw, c.y - bh * 0.55f),
                      ImVec2(c.x - gap, c.y + bh * 0.35f), ink, 2.f);
    dl->AddRectFilled(ImVec2(c.x + gap, c.y - bh * 0.55f),
                      ImVec2(c.x + gap + bw, c.y + bh * 0.35f), ink, 2.f);
    // Slim download tray under the pause — “download held”.
    dl->AddLine(ImVec2(c.x - rad * 0.42f, c.y + rad * 0.52f),
                ImVec2(c.x + rad * 0.42f, c.y + rad * 0.52f), ink, 2.0f);
}

void draw_rom_ready_overlay(ImDrawList* dl, const ImVec2& art0, const ImVec2& art1,
                            const Theme& th) {
    // Soft circle + check — verified local ROM ready to install.
    const ImVec2 c((art0.x + art1.x) * 0.5f, (art0.y + art1.y) * 0.5f);
    const float s = std::min(art1.x - art0.x, art1.y - art0.y);
    const float rad = s * 0.20f;
    dl->AddCircleFilled(c, rad, IM_COL32(12, 16, 28, 220), 36);
    dl->AddCircle(c, rad, ImGui::ColorConvertFloat4ToU32(th.focus), 36, 2.2f);
    const ImU32 ink = ImGui::ColorConvertFloat4ToU32(th.focus);
    const ImVec2 a(c.x - rad * 0.38f, c.y + rad * 0.02f);
    const ImVec2 b(c.x - rad * 0.06f, c.y + rad * 0.36f);
    const ImVec2 d(c.x + rad * 0.46f, c.y - rad * 0.34f);
    dl->AddLine(a, b, ink, 2.6f);
    dl->AddLine(b, d, ink, 2.6f);
}

float draw_grid_tile(const ImVec2& tile_min, float tile_w, bool selected, const Theme& th,
                     const BoxartTexture* tex, float art_aspect_wh, const char* title,
                     const char* subtitle, const ImVec4* badge_col, bool busy_spinner = false,
                     bool dim_art = false, bool update_overlay = false,
                     TileStatusIcon status_icon = TileStatusIcon::None,
                     bool queued_overlay = false, bool rom_ready_overlay = false) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    constexpr float kArtPad = 8.f;
    constexpr float kLabelGap = 6.f;
    constexpr float kFrameInset = 2.f; // keep art/badge inside the border stroke
    constexpr float kTitleScale = 0.84f;
    ImGui::SetWindowFontScale(kTitleScale);
    const float title_line_h = ImGui::GetTextLineHeight();
    ImGui::SetWindowFontScale(1.f);
    const float line_h = ImGui::GetTextLineHeight();
    // Always reserve two title lines so long names wrap instead of ellipsizing early.
    const float title_block_h = title_line_h * 2.f + 2.f;
    const float label_h =
        title_block_h + (subtitle && subtitle[0] ? line_h + 2.f : 0.f) + 8.f;

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

    if (dim_art || busy_spinner || queued_overlay || rom_ready_overlay)
        draw_art_dim(dl, art0, art1, th.radius_sm);
    if (busy_spinner) draw_busy_spinner(dl, art0, art1, th);

    // Queued Install/Update: pause-download disc (wins over update / ROM ready).
    if (queued_overlay && !busy_spinner) {
        draw_queued_overlay(dl, art0, art1, th);
    } else if (update_overlay && !busy_spinner) {
        // Update-available: centered soft disc + up arrow over dimmed cover.
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
    } else if (rom_ready_overlay && !busy_spinner) {
        draw_rom_ready_overlay(dl, art0, art1, th);
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
    draw_wrapped_title_centered(title, tile_w - 8.f, th.text, kTitleScale);
    if (subtitle && subtitle[0]) {
        ImGui::SetCursorScreenPos(
            ImVec2(tile_min.x + 4.f, tile_min.y + art_box_h + kLabelGap + title_block_h));
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
            const bool show_configure =
                retcomm::is_psx_platform(hub.library_platform);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kBackPadX, kBackPadY));
            float right_x = content_right - back_w;
            if (show_configure) {
                const char* cfg_label = "PSXrecomp Config";
                const ImVec2 cfg_txt = ImGui::CalcTextSize(cfg_label);
                const float cfg_w = cfg_txt.x + kBackPadX * 2.f;
                constexpr float kCfgGap = 8.f;
                ImGui::SetCursorScreenPos(
                    ImVec2(right_x - kCfgGap - cfg_w, row0.y + (row_h - back_h) * 0.5f));
                if (accent_button(cfg_label, th, ImVec2(cfg_w, back_h)))
                    hub.open_psx_settings();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                    ImGui::SetTooltip(
                        "Global PlayStation settings (Display, Audio, Input, Hotkeys).\n"
                        "Applied to titles on install, update, and launch unless excluded.");
                }
            }
            ImGui::SetCursorScreenPos(
                ImVec2(right_x, row0.y + (row_h - back_h) * 0.5f));
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

    // Snapshot while mu is held — do not call is_title_queued() here (it locks mu
    // again and deadlocks on a non-recursive mutex when entering a title grid).
    std::unordered_set<std::string> queued_titles;
    for (const auto& q : hub.job_queue) {
        if (!q.title_id.empty() && retcomm::hub::hub_job_is_queueable(q.job))
            queued_titles.insert(q.title_id);
    }

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
            const bool title_queued = !title_busy && queued_titles.count(r.id) > 0;
            const bool needs_update = r.update_available;
            const bool rom_ready =
                r.has_rom && !r.installed && !r.install_dir_present && !needs_update;
            const bool dim_art = !r.installed || needs_update || title_queued;
            const BoxartTexture* tex =
                r.boxart_path.empty() ? nullptr : boxart.get(r.id, r.boxart_path);
            ImVec4 badge = chip_color(r, th);
            TileStatusIcon status = tile_status_icon(r);
            if (title_queued) {
                badge = th.focus;
                status = TileStatusIcon::Queued;
            }
            const float tile_h = draw_grid_tile(
                tile_min, tile_w, selected, th, tex, aspect, r.name.c_str(), nullptr, &badge,
                title_busy, dim_art, needs_update && !title_queued, status, title_queued,
                rom_ready && !title_queued);

            ImGui::SetCursorScreenPos(tile_min);
            if (ImGui::InvisibleButton("##row", ImVec2(tile_w, tile_h))) {
                hub.selected = i;
                hub.detail_scroll_top = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip("%s\n%s · %s\n%s", r.name.c_str(),
                                  platform_display_name(r.platform), r.kind.c_str(),
                                  title_queued ? "QUEUED" : chip_label(r));
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
    if (can_open) {
        const auto roots = retcomm::effective_install_roots(hub.cfg, hub.paths);
        const bool can_move = roots.size() > 1;
        ImGui::BeginDisabled(!can_move);
        if (ImGui::Button("Move Installation Dir", ImVec2(-1, 0))) {
            hub.begin_move_install(row.id);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal |
                                 ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (can_move) {
                ImGui::SetTooltip(
                    "Move this title's install folder (releases, build data, preserved "
                    "saves/config) to another configured install location.");
            } else {
                ImGui::SetTooltip(
                    "Add another install location in Library Settings to enable Move.");
            }
        }
    }

    if (retcomm::is_psx_platform(row.platform)) {
        ImGui::Dummy(ImVec2(0, 6));
        bool exclude = retcomm::title_excludes_platform_config(hub.app_state, row.id);
        if (ImGui::Checkbox("Exclude from platform config", &exclude)) {
            std::string err;
            if (!hub.set_title_exclude_platform_config(row.id, exclude, &err)) {
                hub.append_log("Exclude toggle failed: " + err);
            } else {
                hub.app_state = retcomm::load_app_state(hub.paths.state_path);
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "When checked, RetComM will not overwrite this title's settings.toml / "
                "config.ini from global PlayStation Configure on install, update, or "
                "launch.\nExisting files are left as-is.");
        }
    }

    const bool can_uninstall =
        row.installed || row.install_dir_present || row.has_preserved_state;
    // Keep-saves uninstall left only apps/<title>/preserved/ — second action is a purge.
    const bool preserved_only =
        row.has_preserved_state && !row.installed && !row.install_dir_present;
    if (can_uninstall) {
        ImGui::Dummy(ImVec2(0, 6));
        if (preserved_only) {
            ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
            ImGui::TextWrapped(
                "Game files are already removed. Preserved saves/config remain under "
                "preserved/ for the next install.");
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 4));
            if (ImGui::Button("Remove Saves & Config", ImVec2(-1, 0))) {
                hub.start_job(HubJob::UninstallPurge, row.id);
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "Deletes preserved/ (saves, settings, keybinds stashed from the last "
                    "uninstall).\n"
                    "Library ROMs and managed saves_root files are not deleted.");
            }
        } else {
            if (row.supports_local_build &&
                (row.installed || row.install_dir_present || row.install_method == "build")) {
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
                            "Deletes cmake build intermediates (src/*/build/) to free disk "
                            "space.\n"
                            "The next app update will need a full rebuild instead of an "
                            "incremental one.\n"
                            "Does not remove the installed game, saves, or ROM library.");
                    } else {
                        ImGui::SetTooltip("No cmake build data on disk.");
                    }
                }
                ImGui::Dummy(ImVec2(0, 4));
            }
            static bool keep_saves = true;
            static std::string keep_saves_for_id;
            if (keep_saves_for_id != row.id) {
                keep_saves_for_id = row.id;
                keep_saves = true;
            }
            if (ImGui::Button("Uninstall", ImVec2(-1, 0))) {
                hub.start_job(keep_saves ? HubJob::Uninstall : HubJob::UninstallPurge, row.id);
                ImGui::CloseCurrentPopup();
            }
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::Checkbox("Keep save data", &keep_saves);
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
    if (romm_button("RomM Sync Settings", th, ImVec2(-1, 0))) {
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
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(th.text_muted, "Launcher %s", ver.c_str());
        if (install.self_update_supported && !install.channel_id.empty())
            ImGui::TextColored(th.text_muted, "Channel %s", install.channel_id.c_str());
        if (!tc_line.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, tc_upd ? th.warn : th.text_muted);
            ImGui::TextWrapped("%s", tc_line.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopTextWrapPos();
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
        const std::string& filt = hub.scans_platform_filter;
        const char* scope_preview = filt.empty() ? "All Platforms" : platform_display_name(filt);
        const bool any_scan_queued = hub.has_queued_scan();
        auto scan_active = [&](HubJob j) -> bool {
            return busy && hub.job == j && hub.job_platform_filter == filt;
        };
        auto scan_btn_label = [&](HubJob j, const char* idle, const char* active_lbl,
                                  const char* queue_lbl) -> const char* {
            if (scan_active(j)) return active_lbl;
            if (hub.is_job_queued(j, {}, filt)) return "Queued";
            // Another scan already waiting — keep idle label; button is disabled.
            if (any_scan_queued) return idle;
            if (busy) return queue_lbl;
            return idle;
        };

        ImGui::BeginDisabled(file_busy);
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
        ImGui::EndDisabled();

        // One queued library scan max (any type) — avoid spamming the backlog.
        ImGui::BeginDisabled(file_busy || scan_active(HubJob::ScanRoms) || any_scan_queued);
        if (ImGui::Button(scan_btn_label(HubJob::ScanRoms, "Scan new files", "Scanning…",
                                         "Queue Scan new files"),
                          ImVec2(-1, 0))) {
            hub.pending_scan_missing_rom_id.clear();
            hub.start_job(HubJob::ScanRoms);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();

        ImGui::BeginDisabled(file_busy || scan_active(HubJob::PurgeMissingFiles) ||
                             any_scan_queued);
        if (ImGui::Button(scan_btn_label(HubJob::PurgeMissingFiles, "Clean missing files",
                                         "Cleaning…", "Queue Clean missing files"),
                          ImVec2(-1, 0))) {
            hub.start_job(HubJob::PurgeMissingFiles);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();

        // Always open the confirmation modal (including when queueing).
        ImGui::BeginDisabled(file_busy || scan_active(HubJob::FullScanRoms) || any_scan_queued);
        if (ImGui::Button(scan_btn_label(HubJob::FullScanRoms, "Full rebuild index…", "Scanning…",
                                         "Queue Full rebuild index…"),
                          ImVec2(-1, 0)))
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
                "Re-hash every ROM and BIOS candidate and rebuild the indexes from scratch. "
                "On a large collection this often takes many minutes and will keep the "
                "worker busy until it finishes.");
        } else {
            ImGui::TextWrapped(
                "Re-hash every ROM and BIOS candidate for %s and rebuild that platform's "
                "index from scratch. Other platforms are left alone. On a large collection "
                "this often takes many minutes.",
                platform_display_name(hub.scans_platform_filter));
        }
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 8));
        const bool full_busy = hub.job_running.load();
        const bool any_scan_queued = hub.has_queued_scan();
        const bool this_active = full_busy && hub.job == HubJob::FullScanRoms &&
                                 hub.job_platform_filter == hub.scans_platform_filter;
        const bool this_queued =
            hub.is_job_queued(HubJob::FullScanRoms, {}, hub.scans_platform_filter);
        const char* rebuild_lbl = this_active  ? "Scanning…"
                                  : this_queued ? "Queued"
                                  : full_busy   ? "Queue Rebuild"
                                                : "Rebuild";
        ImGui::BeginDisabled(this_active || this_queued || any_scan_queued);
        if (accent_button(rebuild_lbl, th, ImVec2(120, 0))) {
            hub.pending_scan_missing_rom_id.clear();
            hub.start_job(HubJob::FullScanRoms);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
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
        // OpenBIOS-only build + retail dump selected → rebuild with SCPH + OpenBIOS.
        const bool reinstall_w_bios =
            row.built_with_openbios && row.supports_local_build &&
            row.bios_choice != retcomm::kOpenBiosChoice && !row.bios_choice.empty();
        if (reinstall_w_bios) {
            // Same queue pattern as Update — GenerateRebuild is hub_job_is_queueable.
            const bool reb_active =
                job_busy && hub.job == HubJob::GenerateRebuild && hub.job_title_id == row.id;
            const bool reb_queued = hub.is_job_queued(HubJob::GenerateRebuild, row.id);
            const char* reb_label =
                reb_active  ? "Reinstalling…"
                : reb_queued ? "Queued"
                : job_busy   ? "Queue Reinstall w BIOS"
                             : "Reinstall w BIOS";
            ImGui::BeginDisabled(reb_active || reb_queued ||
                                 (!job_busy && block_title_mutate));
            const bool reb_click = good_button(reb_label, th, ImVec2(btn_w, 0));
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "This install was built with OpenBIOS only.\n"
                    "Regenerate and rebuild with your selected retail BIOS "
                    "(SCPH1001) and OpenBIOS.\n"
                    "Can be queued while another install/update is running.");
            }
            if (reb_click) hub.start_job(HubJob::GenerateRebuild, row.id);
            ImGui::EndDisabled();
        } else {
            // Play (+ update preflight) uses launch_worker — stays free during Install.
            ImGui::BeginDisabled(hub.launch_running.load());
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
        }
        ImGui::SameLine(0, 8);
        {
            const bool upd_active =
                job_busy && hub.job == HubJob::Update && hub.job_title_id == row.id;
            const bool upd_queued = hub.is_job_queued(HubJob::Update, row.id);
            const char* upd_label =
                upd_active  ? "Updating…"
                : upd_queued ? "Queued"
                : job_busy   ? "Queue Update"
                             : "Update";
            ImGui::BeginDisabled(upd_active || upd_queued ||
                                 (!job_busy && block_title_mutate));
            const bool upd_click = row.update_available
                                       ? good_button(upd_label, th, ImVec2(btn_w, 0))
                                       : ImGui::Button(upd_label, ImVec2(btn_w, 0));
            if (upd_click) hub.start_job(HubJob::Update, row.id);
            ImGui::EndDisabled();
        }
    } else if (row.supports_local_build) {
        // Default Install uses local generate+cmake whenever a build recipe
        // exists (install_title_auto); release zip is source, not a Play shortcut.
        if (!row.has_rom) {
            ImGui::PushStyleColor(ImGuiCol_Text, th.warn);
            ImGui::TextWrapped(
                "No verified .cue / ROM in your library yet. Install will prompt to "
                "rescan %s, import files, or download from RomM.",
                platform_display_name(row.platform));
            ImGui::PopStyleColor();
        }
        const bool inst_active =
            job_busy && retcomm::hub::hub_job_is_install(hub.job) &&
            hub.job_title_id == row.id && hub.job != HubJob::Update;
        const bool inst_queued = hub.is_job_queued(HubJob::Install, row.id);
        const char* inst_label =
            inst_active ? (row.install_dir_present ? "Reinstalling…" : "Installing…")
            : inst_queued ? "Queued"
            : job_busy
                ? (row.install_dir_present ? "Queue Reinstall" : "Queue Install")
                : (row.install_dir_present ? "Reinstall" : "Install");
        ImGui::BeginDisabled(inst_active || inst_queued ||
                             (!job_busy && block_title_mutate));
        if (good_button(inst_label, th, ImVec2(-1, 0))) hub.begin_install(row.id);
        ImGui::EndDisabled();
    } else {
        const bool inst_active =
            job_busy && retcomm::hub::hub_job_is_install(hub.job) &&
            hub.job_title_id == row.id && hub.job != HubJob::Update;
        const bool inst_queued = hub.is_job_queued(HubJob::Install, row.id);
        const char* inst_label =
            inst_active ? (row.install_dir_present ? "Reinstalling…" : "Installing…")
            : inst_queued ? "Queued"
            : job_busy
                ? (row.install_dir_present ? "Queue Reinstall" : "Queue Install")
                : (row.install_dir_present ? "Reinstall" : "Install");
        ImGui::BeginDisabled(inst_active || inst_queued ||
                             (!job_busy && block_title_mutate));
        if (good_button(inst_label, th, ImVec2(-1, 0))) hub.begin_install(row.id);
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
            } else if (row.built_with_openbios && row.installed) {
                ImGui::TextColored(th.warn,
                                   "Install used OpenBIOS — Play becomes Reinstall w BIOS.");
            }
        } else if (dump_count == 1) {
            const char* label =
                (row.preferred_bios_index >= 0 &&
                 row.preferred_bios_index < static_cast<int>(row.bios_choice_labels.size()))
                    ? row.bios_choice_labels[static_cast<size_t>(row.preferred_bios_index)].c_str()
                    : "matched";
            ImGui::TextColored(th.good, "Using %s", label);
            if (row.built_with_openbios && row.installed &&
                row.bios_choice != retcomm::kOpenBiosChoice) {
                ImGui::TextColored(th.warn,
                                   "Install used OpenBIOS — Play becomes Reinstall w BIOS.");
            }
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
    if (!row.author_notes.empty()) {
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::TextColored(th.text_muted, "Author's Notes");
        ImGui::TextWrapped("%s", row.author_notes.c_str());
    }
    if (!row.github_url.empty()) {
        ImGui::Dummy(ImVec2(0, 6));
        if (ImGui::Button("GitHub Source", ImVec2(-1, 0))) {
            std::string err;
            if (!retcomm::open_url_in_browser(row.github_url, &err))
                hub.append_log("Open URL failed: " + err);
        }
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
        "Native SRAM / memcard files (RomM sync + launch). Each game gets "
        "…/<platform>/<title_id>/ under this root (e.g. …/saves/ps/masters-of-teras-kasi-psx). "
        "Leave empty to keep saves inside each install.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextColored(th.text_muted, "Exclude dirs (comma-separated basenames)");
    if (ImGui::InputText("##exclude_dirs", hub.settings.exclude_dirs,
                         sizeof(hub.settings.exclude_dirs)))
        hub.settings.dirty = true;

    ImGui::Dummy(ImVec2(0, 10));
    if (accent_button("Save##library_paths", th, ImVec2(160, 0))) {
        std::string err;
        if (!hub.save_settings(&err)) {
            hub.append_log("settings save failed: " + err);
            hub.set_status("Save failed");
        } else {
            hub.show_toast("Saved!");
        }
    }

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
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, 180.f);
        ImGui::TableSetupColumn("path", ImGuiTableColumnFlags_WidthStretch, 1.f);
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
            ImGui::SetNextItemWidth(-1.f);
            if (ImGui::InputText("##label", row.label, sizeof(row.label)))
                hub.settings.dirty = true;
            ImGui::TableNextColumn();
            {
                const float browse_w = 72.f;
                const float gap = ImGui::GetStyle().ItemSpacing.x;
                const float input_w = ImGui::GetContentRegionAvail().x - browse_w - gap;
                if (input_w > 40.f) ImGui::SetNextItemWidth(input_w);
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
    if (accent_button("Save##install_roots", th, ImVec2(120, 0))) {
        std::string err;
        if (!hub.save_settings(&err)) {
            hub.append_log("settings save failed: " + err);
            hub.set_status("Save failed");
        } else {
            hub.show_toast("Saved!");
        }
    }
    ImGui::SameLine();
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
        "When enabled, RetComM checks for launcher, toolchain, and game updates after the hub "
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
    ImGui::TextColored(th.text_muted, "GitHub token (optional)");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##github_token", hub.settings.github_token,
                         sizeof(hub.settings.github_token), ImGuiInputTextFlags_Password))
        hub.settings.dirty = true;
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "Optional PAT for api.github.com when downloading release assets (Install / Apply "
        "Update). Catalog, launcher, toolchain, and game *version checks* use github.com and "
        "do not need a token. Use a classic public_repo token or fine-grained Contents read "
        "if you hit API rate limits during downloads. GITHUB_TOKEN / GH_TOKEN in the "
        "environment still overrides this. Save to apply.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 10));
    if (ImGui::Checkbox("Hide Unowned Catalog Items", &hub.settings.filter_unsupported_titles))
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

        ImGui::Dummy(ImVec2(0, 8));
        if (ImGui::Checkbox("Auto-prune shared caches after builds", &hub.settings.auto_gc_caches))
            hub.settings.dirty = true;
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Keeps a few recent toolchain/SDK versions, drops unreferenced engine pins, old "
            "release zip downloads, and cmake build/ trees idle longer than the day limit. "
            "Shared ccache lives under the RetComM data dir with a size cap.");
        ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Keep toolchain versions", &hub.settings.keep_toolchain_versions)) {
            if (hub.settings.keep_toolchain_versions < 1) hub.settings.keep_toolchain_versions = 1;
            hub.settings.dirty = true;
        }
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Keep SDK versions", &hub.settings.keep_sdk_versions)) {
            if (hub.settings.keep_sdk_versions < 1) hub.settings.keep_sdk_versions = 1;
            hub.settings.dirty = true;
        }
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Keep orphan engine pins", &hub.settings.keep_orphan_engine_pins)) {
            if (hub.settings.keep_orphan_engine_pins < 0) hub.settings.keep_orphan_engine_pins = 0;
            hub.settings.dirty = true;
        }
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Idle build keep days", &hub.settings.idle_build_keep_days)) {
            if (hub.settings.idle_build_keep_days < 0) hub.settings.idle_build_keep_days = 0;
            hub.settings.dirty = true;
        }
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("ccache max GiB", &hub.settings.ccache_max_gb)) {
            if (hub.settings.ccache_max_gb < 0) hub.settings.ccache_max_gb = 0;
            hub.settings.dirty = true;
        }

        ImGui::Dummy(ImVec2(0, 6));
        {
            const bool busy = hub.job_running.load();
            ImGui::BeginDisabled(busy);
            if (ImGui::Button("Prune shared caches now", ImVec2(-1, 0)))
                hub.start_job(HubJob::CleanupSharedCaches);
            ImGui::EndDisabled();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Runs the same GC as post-build pruning (toolchains, SDKs, engines, release zips, "
            "idle builds). Safe: never deletes Play binaries or generated game C.");
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 6));
        {
            const bool busy = hub.job_running.load();
            ImGui::BeginDisabled(busy);
            if (ImGui::Button("Clean all cmake build dirs", ImVec2(-1, 0)))
                hub.start_job(HubJob::CleanupCmakeBuildDirs);
            ImGui::EndDisabled();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Immediately delete cmake build/ trees under every game install's src/ "
            "(Play binary, saves, and generated C are kept). The next Update or Generate & "
            "Rebuild will reconfigure from scratch.");
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 10));
        ImGui::Separator();
        // Use U+26A0 only (no U+FE0F) — variation selectors render as "?" without emoji fonts.
        {
            const bool busy = hub.job_running.load();
            ImGui::BeginDisabled(busy);
            if (danger_button("\xE2\x9A\xA0 Delete All Apps & Save Data \xE2\x9A\xA0", th,
                              ImVec2(-1, 0)))
                ImGui::OpenPopup("Delete everything?###delete_all_apps");
            ImGui::EndDisabled();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Permanently removes every installed game under your install locations, managed "
            "saves under the Game saves root, in-install saves/config, and global platform "
            "Configure prefs. Library ROMs and BIOS dumps are kept.");
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 8));
        {
            const bool busy = hub.job_running.load();
            ImGui::BeginDisabled(busy);
            if (danger_button("\xE2\x9A\xA0 Hard Reset Library Settings \xE2\x9A\xA0", th,
                              ImVec2(-1, 0)))
                ImGui::OpenPopup("Hard reset?###hard_reset_library");
            ImGui::EndDisabled();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Wipes library paths, scan databases, and RomM sync settings from config, then "
            "restarts RetComM into the first-time setup wizard. Installed games, ROM files, "
            "and BIOS dumps are not deleted.");
        ImGui::PopStyleColor();

        if (ImGui::BeginPopupModal("Delete everything?###delete_all_apps", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 420.f);
            ImGui::TextColored(th.warn, "This cannot be undone.");
            ImGui::TextWrapped(
                "Delete all app installations and clean up save/config data?\n\n"
                "• Every title under configured install locations\n"
                "• Managed files under your Game saves root\n"
                "• Preserved install saves/config and platform Configure prefs\n\n"
                "ROM library and BIOS folders are not deleted.");
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 10));
            const bool busy = hub.job_running.load();
            ImGui::BeginDisabled(busy);
            if (danger_button("Yes, delete everything", th, ImVec2(220, 0))) {
                hub.start_job(HubJob::DeleteAllAppsAndSaves);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
            close_modal_on_outside_click();
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("Hard reset?###hard_reset_library", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 420.f);
            ImGui::TextColored(th.warn, "RetComM will restart into first-time setup.");
            ImGui::TextWrapped(
                "Hard reset library settings?\n\n"
                "• Deletes config.json (library / BIOS / saves roots, RomM sync)\n"
                "• Clears library-index, bios-index, and RomM ROM index databases\n"
                "• Clears the setup-completed marker\n\n"
                "Installed apps, ROM library files, and BIOS dumps stay on disk.");
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 10));
            const bool busy = hub.job_running.load();
            ImGui::BeginDisabled(busy);
            if (danger_button("Yes, hard reset & restart", th, ImVec2(240, 0))) {
                hub.start_job(HubJob::HardResetLibrarySettings);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
            close_modal_on_outside_click();
            ImGui::EndPopup();
        }
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
        } else {
            hub.show_toast("Saved!");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        hub.show_settings = false;
        hub.settings.dirty = false;
    }
    if (hub.settings.dirty) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(th.warn, "unsaved changes");
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(th.text_muted, "%s", hub.paths.config_path.string().c_str());
    ImGui::EndChild();
}

// Draw-list only — must not submit ImGui items (breaks SameLine for side-by-side cards).
void draw_list_wrapped_text(ImDrawList* dl, ImVec2 pos, float wrap_w, ImU32 col, const char* text) {
    if (!dl || !text || !text[0] || wrap_w <= 1.f) return;
    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize();
    const float scale = (font && font->FontSize > 0.f) ? (font_size / font->FontSize) : 1.f;
    const float line_h = ImGui::GetTextLineHeightWithSpacing();
    const char* end = text + std::strlen(text);
    const char* s = text;
    float y = pos.y;
    while (s < end) {
        while (s < end && (*s == '\n' || *s == '\r')) {
            y += line_h;
            ++s;
        }
        if (s >= end) break;
        const char* line_end =
            font ? font->CalcWordWrapPositionA(scale, s, end, wrap_w) : end;
        if (line_end == s) line_end = s + 1; // always advance
        dl->AddText(font, font_size, ImVec2(pos.x, y), col, s, line_end);
        s = line_end;
        while (s < end && (*s == ' ' || *s == '\t')) ++s;
        y += line_h;
    }
}

bool draw_setup_path_card(BoxartCache& boxart, const Theme& th, const char* id,
                          const char* title, const char* subtitle, const char* asset_file,
                          float card_w, float card_h) {
    ImGui::PushID(id);
    // Single layout item so SameLine keeps both cards on one row.
    const ImVec2 card_min = ImGui::GetCursorScreenPos();
    const ImVec2 card_max(card_min.x + card_w, card_min.y + card_h);
    const bool clicked = ImGui::InvisibleButton("##card", ImVec2(card_w, card_h));
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 fill = ImGui::ColorConvertFloat4ToU32(hovered ? th.panel_hovered : th.panel);
    const ImU32 border =
        ImGui::ColorConvertFloat4ToU32(hovered ? th.accent_dim : th.border);
    dl->AddRectFilled(card_min, card_max, fill, th.radius_lg);
    dl->AddRect(card_min, card_max, border, th.radius_lg, 0, hovered ? 2.f : 1.f);
    dl->PushClipRect(ImVec2(card_min.x + 1.f, card_min.y + 1.f),
                     ImVec2(card_max.x - 1.f, card_max.y - 1.f), true);

    const fs::path icon = find_hub_asset_file("setup", asset_file);
    const BoxartTexture* tex =
        icon.empty() ? nullptr : boxart.get(std::string("setup:") + id, icon);
    const float pad = 20.f;
    const float icon_max = std::min(card_w - pad * 2.f, card_h * 0.42f);
    if (tex && tex->gl_id && tex->width > 0 && tex->height > 0) {
        const ImVec2 fit = contain_size(static_cast<float>(tex->width),
                                        static_cast<float>(tex->height), icon_max, icon_max);
        const float ix = card_min.x + (card_w - fit.x) * 0.5f;
        const float iy = card_min.y + pad + 6.f;
        dl->AddImage((ImTextureID)(intptr_t)tex->gl_id, ImVec2(ix, iy),
                     ImVec2(ix + fit.x, iy + fit.y));
    }

    const ImVec2 title_sz = ImGui::CalcTextSize(title);
    const float text_y = card_min.y + pad + 6.f + icon_max + 12.f;
    dl->AddText(ImVec2(card_min.x + (card_w - title_sz.x) * 0.5f, text_y),
                ImGui::ColorConvertFloat4ToU32(th.text), title);
    draw_list_wrapped_text(dl, ImVec2(card_min.x + pad, text_y + title_sz.y + 8.f),
                           card_w - pad * 2.f, ImGui::ColorConvertFloat4ToU32(th.text_muted),
                           subtitle);
    dl->PopClipRect();

    ImGui::PopID();
    return clicked;
}

void draw_setup_wizard(HubModel& hub, BoxartCache& boxart, const Theme& th, SDL_Window* window) {
    if (!hub.show_setup) return;

    ImGui::OpenPopup("Welcome to RetComM###setup_wizard");
    constexpr float kWizW = 820.f; // a bit wider so the two path cards can breathe
    constexpr float kWizH = 628.f;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float target_w = kWizW;
    float target_h = kWizH;
    if (hub.setup_path == SetupPath::Easy) {
        // Sparse content — ~30% smaller than the shared wizard size.
        target_w = 780.f * 0.70f;
        target_h = 628.f * 0.70f;
    }
    const float wiz_w = std::min(target_w, vp->WorkSize.x * 0.96f);
    const float wiz_h = std::min(target_h, vp->WorkSize.y * 0.92f);
    ImGui::SetNextWindowSizeConstraints(ImVec2(wiz_w, wiz_h), ImVec2(wiz_w, wiz_h));
    ImGui::SetNextWindowSize(ImVec2(wiz_w, wiz_h), ImGuiCond_Always);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Welcome to RetComM###setup_wizard", nullptr,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar))
        return;

    auto push_wrap = [&] {
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + ImGui::GetContentRegionAvail().x);
    };

    // Reserve footer space, then pin action buttons to the bottom-left with padding.
    auto footer_reserve = [&](float warn_h = 0.f) {
        const float gap_above = 24.f;
        const float pad_below = 18.f;
        return gap_above + ImGui::GetFrameHeight() + warn_h + pad_below;
    };
    auto pin_footer_row = [&](float warn_h = 0.f) {
        const float pad_below = 18.f;
        const float pad_left = 6.f; // on top of window padding
        const float row_h = ImGui::GetFrameHeight() + warn_h;
        ImGui::SetCursorPosY(ImGui::GetWindowContentRegionMax().y - pad_below - row_h);
        ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + pad_left);
    };

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
        const float footer_h = footer_reserve();
        ImGui::BeginChild("##setup_create_roots_body", ImVec2(0.f, -footer_h),
                          ImGuiChildFlags_None);
        push_wrap();
        ImGui::TextWrapped("These folders do not exist yet. Create them now?");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 8));
        for (const auto& p : hub.setup_missing_roots) {
            push_wrap();
            ImGui::BulletText("%s", p.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();
        pin_footer_row();
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
        const float footer_h = footer_reserve();
        ImGui::BeginChild("##setup_chooser_body", ImVec2(0.f, -footer_h), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar);
        push_wrap();
        ImGui::TextWrapped("How do you want to set up your library?");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 16));

        const float gap = 20.f;
        const float side_pad = 4.f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + side_pad);
        const float row_w = ImGui::GetContentRegionAvail().x - side_pad;
        const float card_w = (row_w - gap) * 0.5f;
        const float card_h = std::max(240.f, ImGui::GetContentRegionAvail().y - 4.f);
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
        ImGui::EndChild();

        pin_footer_row();
        if (ImGui::Button("Skip for now", ImVec2(150, 0))) skip_setup();
    } else if (hub.setup_path == SetupPath::Easy) {
        const bool emu_ok = hub.setup_emulation_root[0] != '\0';
        const float warn_h = emu_ok ? 0.f : ImGui::GetTextLineHeightWithSpacing();
        const float footer_h = footer_reserve(warn_h);
        ImGui::BeginChild("##setup_easy_body", ImVec2(0.f, -footer_h), ImGuiChildFlags_None);
        push_wrap();
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
        push_wrap();
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
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::EndChild();

        pin_footer_row(warn_h);
        if (ImGui::Button("Back", ImVec2(100, 0))) {
            hub.setup_path = SetupPath::Chooser;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!emu_ok);
        if (accent_button("Finish", th, ImVec2(120, 0))) {
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
        if (ImGui::Button("Skip for now", ImVec2(120, 0))) skip_setup();
        if (!emu_ok) {
            ImGui::TextColored(th.warn, "Choose an Emulation folder to continue.");
        }
    } else if (hub.setup_step == 0) {
        const bool library_ok = hub.settings.library_root[0] != '\0';
        const float warn_h = library_ok ? 0.f : ImGui::GetTextLineHeightWithSpacing();
        const float footer_h = footer_reserve(warn_h);
        ImGui::BeginChild("##setup_adv0_body", ImVec2(0.f, -footer_h), ImGuiChildFlags_None);
        push_wrap();
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
        push_wrap();
        ImGui::TextWrapped("Required — EmulationStation / RomM-style root (e.g. …/roms).");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 10));
        if (path_field_with_browse("BIOS root", "##setup_bios_root", hub.settings.bios_root,
                                   sizeof(hub.settings.bios_root), hub, window,
                                   FolderPickTarget::BiosRoot, th))
            hub.settings.dirty = true;
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        push_wrap();
        ImGui::TextWrapped("Optional — system BIOS / firmware dumps (e.g. …/bios).");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 10));
        if (path_field_with_browse("Game saves root", "##setup_saves_root",
                                   hub.settings.saves_root, sizeof(hub.settings.saves_root), hub,
                                   window, FolderPickTarget::SavesRoot, th))
            hub.settings.dirty = true;
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        push_wrap();
        ImGui::TextWrapped(
            "Recommended — SRAM / memcard library (e.g. …/saves). RomM sync and launches "
            "quarantine each game under …/<platform>/<title_id>/.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 14));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::TextColored(th.text_muted, "RomM (optional)");
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        push_wrap();
        ImGui::TextWrapped(
            "Leave blank for local-only. Use a Client API Token from RomM → Administration → "
            "Client API Tokens (Bearer rmm_…).");
        ImGui::PopTextWrapPos();
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
        ImGui::EndChild();

        pin_footer_row(warn_h);
        if (ImGui::Button("Back", ImVec2(100, 0))) {
            hub.setup_path = SetupPath::Chooser;
            hub.setup_confirm_create_roots = false;
            hub.setup_missing_roots.clear();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!library_ok);
        if (accent_button("Next", th, ImVec2(120, 0))) {
            hub.collect_missing_setup_roots();
            if (!hub.setup_missing_roots.empty()) {
                hub.setup_confirm_create_roots = true;
            } else {
                advance_to_platform_step();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Skip for now", ImVec2(120, 0))) skip_setup();
        if (!library_ok) {
            ImGui::TextColored(th.warn, "Choose a ROM library folder to continue.");
        }
    } else {
        const float footer_h = footer_reserve();
        // Outer body never scrolls — only the mappings table does.
        ImGui::BeginChild("##setup_adv1_body", ImVec2(0.f, -footer_h), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ImGui::GetStyle().ItemSpacing.x, 4.f));
        push_wrap();
        ImGui::PushStyleColor(ImGuiCol_Text, th.text);
        ImGui::TextWrapped(
            "Step 2 of 2 - Assign platform folder mappings.  Platform name is on the left, "
            "and on the right is a list of folder names to search.  RetComM will create empty "
            "folders for any platforms that are missing from your library, to import new files "
            "you provide.");
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::Checkbox("Create missing platform folders under roms / bios / saves",
                        &hub.setup_create_platform_folders);

        const float add_row_h = ImGui::GetFrameHeightWithSpacing();
        const float table_h = std::max(120.f, ImGui::GetContentRegionAvail().y - add_row_h);
        ImGui::BeginChild("##setup_platform_table", ImVec2(0, table_h), ImGuiChildFlags_Borders);
        if (ImGui::BeginTable("setup_platform_folders", 3,
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("platform", ImGuiTableColumnFlags_WidthFixed, 100.f);
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
        ImGui::PopStyleVar();
        ImGui::EndChild();

        pin_footer_row();
        if (ImGui::Button("Back", ImVec2(100, 0))) {
            hub.setup_step = 0;
        }
        ImGui::SameLine();
        if (accent_button("Finish", th, ImVec2(120, 0))) {
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
        } else {
            hub.show_toast("Saved!");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        hub.show_romm_settings = false;
        hub.romm_settings.dirty = false;
    }
    if (hub.romm_settings.dirty) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(th.warn, "unsaved changes");
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(th.text_muted, "%s", hub.paths.config_path.string().c_str());
    ImGui::EndChild();
}

void psx_settings_row_label(const char* text, const Theme& th, float col_w = 180.f) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(th.text_muted, "%s", text);
    ImGui::SameLine(col_w);
}

void poll_psx_hotkey_capture(HubModel& hub) {
    auto& draft = hub.psx_settings;
    if (draft.capturing_hotkey < 0 ||
        draft.capturing_hotkey >= retcomm::PsxPlatformSettings::kHotkeyCount)
        return;
    // Escape cancels.
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        draft.capturing_hotkey = -1;
        return;
    }
    for (ImGuiKey key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END;
         key = (ImGuiKey)(key + 1)) {
        if (key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl || key == ImGuiKey_LeftShift ||
            key == ImGuiKey_RightShift || key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt ||
            key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper || key == ImGuiKey_Escape)
            continue;
        if (!ImGui::IsKeyPressed(key, false)) continue;
        std::string name;
        if (ImGui::GetIO().KeyCtrl) name += "Ctrl+";
        if (ImGui::GetIO().KeyAlt) name += "Alt+";
        if (ImGui::GetIO().KeyShift) name += "Shift+";
        const char* kn = ImGui::GetKeyName(key);
        if (!kn || !kn[0]) continue;
        // Match recomp-ui / host_keymap vocabulary for common keys.
        if (std::strcmp(kn, "Enter") == 0) name += "Return";
        else if (std::strcmp(kn, "KeypadEnter") == 0) name += "Keypad Enter";
        else if (std::strncmp(kn, "Keypad", 6) == 0) {
            name += "Keypad ";
            name += (kn + 6);
        } else {
            name += kn;
        }
        draft.settings.hotkeys[static_cast<size_t>(draft.capturing_hotkey)] = name;
        draft.dirty = true;
        draft.capturing_hotkey = -1;
        return;
    }
}

struct HubGamepadOpt {
    char guid[40]{};
    char name[64]{};
    bool live = false;
    SDL_JoystickID id = 0;
};

// SDL3 only emits GAMEPAD_* events for pads that stay open. Keep a small table
// of open handles (same idea as recomp-ui launcher_input_poll).
constexpr int kMaxOpenHubPads = 16;
struct OpenHubPad {
    SDL_JoystickID id = 0;
    SDL_Gamepad* handle = nullptr;
    char guid[64]{};
};
OpenHubPad g_open_pads[kMaxOpenHubPads]{};

bool guid_eq_ci(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        const unsigned char ca = static_cast<unsigned char>(*a++);
        const unsigned char cb = static_cast<unsigned char>(*b++);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return *a == *b;
}

void hub_close_all_gamepads() {
    for (int i = 0; i < kMaxOpenHubPads; ++i) {
        if (g_open_pads[i].handle) SDL_CloseGamepad(g_open_pads[i].handle);
        g_open_pads[i] = {};
    }
}

// Open newly connected pads; close disconnected ones. Call once per frame.
void hub_sync_open_gamepads() {
    bool keep[kMaxOpenHubPads]{};
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids) {
        for (int i = 0; i < count; ++i) {
            const SDL_JoystickID id = ids[i];
            int slot = -1;
            for (int j = 0; j < kMaxOpenHubPads; ++j) {
                if (g_open_pads[j].handle && g_open_pads[j].id == id) {
                    slot = j;
                    break;
                }
            }
            if (slot < 0) {
                for (int j = 0; j < kMaxOpenHubPads; ++j) {
                    if (g_open_pads[j].handle) continue;
                    SDL_Gamepad* gp = SDL_OpenGamepad(id);
                    if (!gp) break;
                    g_open_pads[j].handle = gp;
                    g_open_pads[j].id = id;
                    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), g_open_pads[j].guid,
                                     static_cast<int>(sizeof(g_open_pads[j].guid)));
                    slot = j;
                    break;
                }
            }
            if (slot >= 0) keep[slot] = true;
        }
        SDL_free(ids);
    }
    for (int j = 0; j < kMaxOpenHubPads; ++j) {
        if (!g_open_pads[j].handle || keep[j]) continue;
        SDL_CloseGamepad(g_open_pads[j].handle);
        g_open_pads[j] = {};
    }
}

// Snapshot of pad state when bind capture starts — poll path commits the first
// button/axis that rises above this baseline (covers missed GAMEPAD_* events).
struct CapturePadBaseline {
    bool valid = false;
    SDL_JoystickID id = 0;
    bool buttons[static_cast<int>(SDL_GAMEPAD_BUTTON_COUNT)]{};
    Sint16 axes[static_cast<int>(SDL_GAMEPAD_AXIS_COUNT)]{};
    // Ignore commits until this SDL tick (avoids click/press bleed into capture).
    Uint64 arm_until_ms = 0;
    // Poll debounce: require the same candidate for N frames before committing.
    int debounce_kind = 0; // 1 button, 2 axis
    int debounce_code = -1;
    int debounce_dir = 0;
    int debounce_frames = 0;
};
CapturePadBaseline g_capture_baseline{};
constexpr int kCaptureArmMs = 180;
constexpr int kCapturePollDebounceFrames = 3;
constexpr int kCaptureAxisCommit = 20000;

SDL_Gamepad* gamepad_handle_for_id(SDL_JoystickID id) {
    if (!id) return nullptr;
    SDL_Gamepad* pad = SDL_GetGamepadFromID(id);
    if (pad) return pad;
    for (int i = 0; i < kMaxOpenHubPads; ++i) {
        if (g_open_pads[i].id == id && g_open_pads[i].handle) return g_open_pads[i].handle;
    }
    return nullptr;
}

void snapshot_capture_baseline(SDL_JoystickID id) {
    const Uint64 arm = g_capture_baseline.arm_until_ms;
    g_capture_baseline = {};
    g_capture_baseline.id = id;
    g_capture_baseline.arm_until_ms = arm;
    SDL_Gamepad* pad = gamepad_handle_for_id(id);
    if (!pad) return;
    for (int b = 0; b < static_cast<int>(SDL_GAMEPAD_BUTTON_COUNT); ++b)
        g_capture_baseline.buttons[b] =
            SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b)) != 0;
    for (int a = 0; a < static_cast<int>(SDL_GAMEPAD_AXIS_COUNT); ++a)
        g_capture_baseline.axes[a] = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(a));
    g_capture_baseline.valid = true;
}

void clear_capture_baseline() { g_capture_baseline = {}; }

bool capture_armed() {
    return SDL_GetTicks() >= g_capture_baseline.arm_until_ms;
}

void reset_capture_debounce() {
    g_capture_baseline.debounce_kind = 0;
    g_capture_baseline.debounce_code = -1;
    g_capture_baseline.debounce_dir = 0;
    g_capture_baseline.debounce_frames = 0;
}

// PSX slot layout: 0-3 d-pad, 9/11 L2/R2, 16-23 stick directions.
bool slot_is_dpad(int slot) { return slot >= 0 && slot <= 3; }
bool slot_is_stick_dir(int slot) { return slot >= 16 && slot < 24; }
bool slot_is_l2(int slot) { return slot == 9; }
bool slot_is_r2(int slot) { return slot == 11; }

bool is_dpad_button(int button) {
    return button == static_cast<int>(SDL_GAMEPAD_BUTTON_DPAD_UP) ||
           button == static_cast<int>(SDL_GAMEPAD_BUTTON_DPAD_DOWN) ||
           button == static_cast<int>(SDL_GAMEPAD_BUTTON_DPAD_LEFT) ||
           button == static_cast<int>(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
}

bool is_stick_axis(int axis) {
    return axis == static_cast<int>(SDL_GAMEPAD_AXIS_LEFTX) ||
           axis == static_cast<int>(SDL_GAMEPAD_AXIS_LEFTY) ||
           axis == static_cast<int>(SDL_GAMEPAD_AXIS_RIGHTX) ||
           axis == static_cast<int>(SDL_GAMEPAD_AXIS_RIGHTY);
}

bool is_trigger_axis(int axis) {
    return axis == static_cast<int>(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) ||
           axis == static_cast<int>(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
}

// Slot-aware accept filters — stops triggers/face noise from stealing D-pad binds.
bool capture_accepts_button(int slot, int button) {
    if (button < 0 || button >= static_cast<int>(SDL_GAMEPAD_BUTTON_COUNT)) return false;
    if (slot_is_dpad(slot)) return is_dpad_button(button);
    if (slot_is_stick_dir(slot)) return is_dpad_button(button); // digital fold remaps
    if (slot_is_l2(slot) || slot_is_r2(slot)) return false; // triggers: axes only
    return !is_dpad_button(button); // face/shoulders — reject stray D-pad edges
}

bool capture_accepts_axis(int slot, int axis) {
    if (axis < 0 || axis >= static_cast<int>(SDL_GAMEPAD_AXIS_COUNT)) return false;
    if (slot_is_dpad(slot)) return false;
    if (slot_is_stick_dir(slot)) return is_stick_axis(axis);
    if (slot_is_l2(slot)) return axis == static_cast<int>(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    if (slot_is_r2(slot)) return axis == static_cast<int>(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    // Other digital slots: never bind stick or trigger axes by accident.
    if (is_stick_axis(axis) || is_trigger_axis(axis)) return false;
    return true;
}

// When several D-pad buttons edge at once (diagonal), wait for a single cardinal.
int sole_new_dpad_button(SDL_Gamepad* pad) {
    int hit = -1;
    int n = 0;
    static const SDL_GamepadButton kDpad[] = {
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
    };
    for (SDL_GamepadButton b : kDpad) {
        const int bi = static_cast<int>(b);
        const bool now = SDL_GetGamepadButton(pad, b) != 0;
        if (now && !g_capture_baseline.buttons[bi]) {
            ++n;
            hit = bi;
        }
    }
    return n == 1 ? hit : -1;
}

SDL_JoystickID find_live_pad_id(const char* guid); // defined below

// Resolve SDL button/axis name the same way the runtime input.ini parser expects.
void fill_bind_source_string(int kind, int code, int axis_dir, char* out, size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    if (kind == 1) {
        const char* n = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(code));
        if (!n || !n[0]) return;
        if (std::strcmp(n, "south") == 0) n = "a";
        else if (std::strcmp(n, "east") == 0) n = "b";
        else if (std::strcmp(n, "west") == 0) n = "x";
        else if (std::strcmp(n, "north") == 0) n = "y";
        std::snprintf(out, cap, "%s", n);
    } else if (kind == 2) {
        const char* n = SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(code));
        if (!n || !n[0]) n = "axis";
        std::snprintf(out, cap, "%s%c", n, axis_dir < 0 ? '-' : '+');
    }
}

void append_live_sdl_dpad_hint(SDL_Gamepad* pad, char* out, size_t cap) {
    if (!pad || !out || cap < 8) return;
    char bits[64]{};
    auto add = [&](SDL_GamepadButton b, const char* name) {
        if (!SDL_GetGamepadButton(pad, b)) return;
        if (bits[0]) std::strncat(bits, "+", sizeof(bits) - std::strlen(bits) - 1);
        std::strncat(bits, name, sizeof(bits) - std::strlen(bits) - 1);
    };
    add(SDL_GAMEPAD_BUTTON_DPAD_UP, "up");
    add(SDL_GAMEPAD_BUTTON_DPAD_DOWN, "down");
    add(SDL_GAMEPAD_BUTTON_DPAD_LEFT, "left");
    add(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "right");
    if (!bits[0]) return;
    const size_t used = std::strlen(out);
    if (used + 16 >= cap) return;
    std::snprintf(out + used, cap - used, "\nSDL D-pad: %s", bits);
}

// True when the bound input.ini source string is currently pressed on `pad`.
bool gamepad_bound_source_pressed(SDL_Gamepad* pad, const char* raw, int deadzone_raw) {
    if (!pad || !raw || !raw[0]) return false;
    char s[48]{};
    std::snprintf(s, sizeof(s), "%s", raw);
    for (char* p = s; *p; ++p) *p = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    if (char* comma = std::strchr(s, ',')) *comma = '\0';

    int dir = 0;
    const size_t n = std::strlen(s);
    if (n >= 2) {
        const char last = s[n - 1];
        const char prev = s[n - 2];
        if ((last == '+' || last == '-') &&
            (std::isalnum(static_cast<unsigned char>(prev)) || prev == '_')) {
            dir = (last == '+') ? +1 : -1;
            s[n - 1] = '\0';
        }
    }

    auto as_btn = [&](SDL_GamepadButton b) {
        return SDL_GetGamepadButton(pad, b) != 0;
    };
    auto as_axis = [&](SDL_GamepadAxis a, int want_dir) {
        const int v = static_cast<int>(SDL_GetGamepadAxis(pad, a));
        const int dz = deadzone_raw > 0 ? deadzone_raw : 8000;
        if (want_dir < 0) return v < -dz;
        return v > dz;
    };

    // Face / legacy aliases first (match runtime parse_controller_source).
    if (std::strcmp(s, "a") == 0 || std::strcmp(s, "south") == 0)
        return as_btn(SDL_GAMEPAD_BUTTON_SOUTH);
    if (std::strcmp(s, "b") == 0 || std::strcmp(s, "east") == 0)
        return as_btn(SDL_GAMEPAD_BUTTON_EAST);
    if (std::strcmp(s, "x") == 0 || std::strcmp(s, "west") == 0)
        return as_btn(SDL_GAMEPAD_BUTTON_WEST);
    if (std::strcmp(s, "y") == 0 || std::strcmp(s, "north") == 0)
        return as_btn(SDL_GAMEPAD_BUTTON_NORTH);
    if (std::strcmp(s, "back") == 0 || std::strcmp(s, "view") == 0 || std::strcmp(s, "select") == 0)
        return as_btn(SDL_GAMEPAD_BUTTON_BACK);
    if (std::strcmp(s, "start") == 0 || std::strcmp(s, "menu") == 0)
        return as_btn(SDL_GAMEPAD_BUTTON_START);
    if (std::strcmp(s, "guide") == 0) return as_btn(SDL_GAMEPAD_BUTTON_GUIDE);
    if (std::strcmp(s, "leftstick") == 0) return as_btn(SDL_GAMEPAD_BUTTON_LEFT_STICK);
    if (std::strcmp(s, "rightstick") == 0) return as_btn(SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    if (std::strcmp(s, "leftshoulder") == 0 || std::strcmp(s, "lb") == 0 || std::strcmp(s, "l1") == 0)
        return as_btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    if (std::strcmp(s, "rightshoulder") == 0 || std::strcmp(s, "rb") == 0 ||
        std::strcmp(s, "r1") == 0)
        return as_btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    if (std::strcmp(s, "dpup") == 0 || std::strcmp(s, "dpadup") == 0)
        return as_btn(SDL_GAMEPAD_BUTTON_DPAD_UP);
    if (std::strcmp(s, "dpdown") == 0 || std::strcmp(s, "dpaddown") == 0)
        return as_btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    if (std::strcmp(s, "dpleft") == 0 || std::strcmp(s, "dpadleft") == 0)
        return as_btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    if (std::strcmp(s, "dpright") == 0 || std::strcmp(s, "dpadright") == 0)
        return as_btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    if (std::strcmp(s, "lefttrigger") == 0 || std::strcmp(s, "lt") == 0 || std::strcmp(s, "l2") == 0)
        return as_axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, dir == 0 ? +1 : dir);
    if (std::strcmp(s, "righttrigger") == 0 || std::strcmp(s, "rt") == 0 ||
        std::strcmp(s, "r2") == 0)
        return as_axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, dir == 0 ? +1 : dir);
    if (std::strcmp(s, "leftx") == 0 && dir != 0) return as_axis(SDL_GAMEPAD_AXIS_LEFTX, dir);
    if (std::strcmp(s, "lefty") == 0 && dir != 0) return as_axis(SDL_GAMEPAD_AXIS_LEFTY, dir);
    if (std::strcmp(s, "rightx") == 0 && dir != 0) return as_axis(SDL_GAMEPAD_AXIS_RIGHTX, dir);
    if (std::strcmp(s, "righty") == 0 && dir != 0) return as_axis(SDL_GAMEPAD_AXIS_RIGHTY, dir);

    const SDL_GamepadButton btn = SDL_GetGamepadButtonFromString(s);
    if (btn != SDL_GAMEPAD_BUTTON_INVALID) return as_btn(btn);
    const SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(s);
    if (axis != SDL_GAMEPAD_AXIS_INVALID) return as_axis(axis, dir == 0 ? +1 : dir);
    return false;
}

void cancel_psx_bind_capture(HubModel& hub) {
    hub.psx_settings.capturing_bind = -1;
    hub.psx_settings.map_all_active = false;
    hub.psx_settings.map_all_wait_release = false;
    hub.psx_settings.map_all_step = 0;
    clear_capture_baseline();
}

void begin_psx_bind_capture(HubModel& hub, int button, bool is_pad) {
    hub.psx_settings.capturing_bind = button;
    hub.psx_settings.capture_is_pad = is_pad;
    if (!hub.psx_settings.map_all_active) hub.psx_settings.map_all_wait_release = false;
    clear_capture_baseline();
    if (!is_pad) return;
    g_capture_baseline.arm_until_ms = SDL_GetTicks() + static_cast<Uint64>(kCaptureArmMs);
    reset_capture_debounce();
    const int p = hub.psx_settings.configuring_player;
    if (p < 0) return;
    const std::string& guid =
        hub.psx_settings.settings.player_guid[static_cast<size_t>(p)];
    SDL_JoystickID id = find_live_pad_id(guid.c_str());
    if (!id) {
        int live_n = 0;
        SDL_JoystickID only = 0;
        for (int i = 0; i < kMaxOpenHubPads; ++i) {
            if (!g_open_pads[i].handle) continue;
            ++live_n;
            only = g_open_pads[i].id;
        }
        if (live_n == 1) id = only;
    }
    if (id) snapshot_capture_baseline(id);
}

void begin_psx_map_all(HubModel& hub, bool is_pad) {
    hub.psx_settings.map_all_active = true;
    hub.psx_settings.map_all_wait_release = false;
    hub.psx_settings.map_all_step = 0;
    begin_psx_bind_capture(hub, retcomm::psx_pad_map_all_order()[0], is_pad);
}

void advance_psx_map_all(HubModel& hub) {
    auto& d = hub.psx_settings;
    if (!d.map_all_active) return;
    d.map_all_step++;
    if (d.map_all_step >= retcomm::kPsxPadButtonCount) {
        cancel_psx_bind_capture(hub);
        return;
    }
    // Gamepads need a release/rest gate so one press doesn't fill every slot.
    d.map_all_wait_release = d.capture_is_pad;
    begin_psx_bind_capture(hub, retcomm::psx_pad_map_all_order()[d.map_all_step], d.capture_is_pad);
}

SDL_JoystickID find_live_pad_id(const char* guid) {
    if (!guid || !guid[0]) return 0;
    for (int i = 0; i < kMaxOpenHubPads; ++i) {
        if (g_open_pads[i].handle && guid_eq_ci(g_open_pads[i].guid, guid))
            return g_open_pads[i].id;
    }
    // Fallback if sync hasn't run yet this frame.
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (!ids) return 0;
    SDL_JoystickID hit = 0;
    for (int i = 0; i < n; ++i) {
        char g[64]{};
        SDL_GUIDToString(SDL_GetGamepadGUIDForID(ids[i]), g, static_cast<int>(sizeof(g)));
        if (guid_eq_ci(g, guid)) {
            hit = ids[i];
            break;
        }
    }
    SDL_free(ids);
    return hit;
}

bool gamepad_at_rest(SDL_JoystickID id) {
    SDL_Gamepad* pad = gamepad_handle_for_id(id);
    if (!pad) return true;
    for (int b = 0; b < static_cast<int>(SDL_GAMEPAD_BUTTON_COUNT); ++b) {
        if (SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b))) return false;
    }
    for (int a = 0; a < static_cast<int>(SDL_GAMEPAD_AXIS_COUNT); ++a) {
        const Sint16 v = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(a));
        if (v > 8000 || v < -8000) return false;
    }
    return true;
}

// Live pads + remembered GUID profiles (input.ini) + currently assigned slots.
void collect_hub_gamepads(HubModel& hub, std::vector<HubGamepadOpt>& out) {
    out.clear();
    const auto& s = hub.psx_settings.settings;
    auto already = [&](const char* guid) {
        if (!guid || !guid[0]) return true;
        for (const auto& o : out)
            if (guid_eq_ci(o.guid, guid)) return true;
        return false;
    };
    auto push = [&](const char* guid, const char* name, bool live, SDL_JoystickID id) {
        if (!guid || !guid[0] || already(guid)) return;
        HubGamepadOpt o;
        std::snprintf(o.guid, sizeof(o.guid), "%s", guid);
        const char* nm = (name && name[0] && std::strcmp(name, "Gamepad") != 0) ? name : "Controller";
        std::snprintf(o.name, sizeof(o.name), "%s", nm);
        o.live = live;
        o.id = id;
        out.push_back(o);
    };

    const int known = retcomm::psx_pad_binds_known_count(hub.paths);
    for (int i = 0; i < known; ++i) {
        char guid[40]{}, name[64]{};
        if (!retcomm::psx_pad_binds_known_at(hub.paths, i, guid, sizeof(guid), name, sizeof(name)))
            continue;
        push(guid, name, false, 0);
    }

    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (ids) {
        for (int i = 0; i < n; ++i) {
            const SDL_JoystickID id = ids[i];
            char guid_str[64]{};
            SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), guid_str, static_cast<int>(sizeof(guid_str)));
            const char* name = SDL_GetGamepadNameForID(id);
            // Refresh live entry / prefer custom registry name.
            bool found = false;
            for (auto& o : out) {
                if (!guid_eq_ci(o.guid, guid_str)) continue;
                o.live = true;
                o.id = id;
                if (!retcomm::psx_pad_binds_name_is_custom(hub.paths, guid_str) && name && name[0] &&
                    std::strcmp(name, "Gamepad") != 0)
                    std::snprintf(o.name, sizeof(o.name), "%s", name);
                found = true;
                break;
            }
            if (!found) push(guid_str, name, true, id);
        }
        SDL_free(ids);
    }
    for (int p = 0; p < retcomm::PsxPlatformSettings::kMaxPlayers; ++p) {
        if (s.player_src[static_cast<size_t>(p)] != 2) continue;
        const std::string& g = s.player_guid[static_cast<size_t>(p)];
        if (g.empty()) continue;
        char nm[64]{};
        retcomm::psx_pad_binds_name(hub.paths, g, nm, sizeof(nm));
        push(g.c_str(), nm[0] ? nm : "Controller", false, 0);
    }
}

const char* psx_player_src_preview(const retcomm::PsxPlatformSettings& s, int p,
                                   const std::vector<HubGamepadOpt>& pads) {
    const int src = std::clamp(s.player_src[static_cast<size_t>(p)], 0, 2);
    if (src == 0) return "None";
    if (src == 1) return "Keyboard";
    const std::string& guid = s.player_guid[static_cast<size_t>(p)];
    if (guid.empty()) return "Gamepad";
    for (const auto& pad : pads) {
        if (guid == pad.guid) return pad.name;
    }
    return guid.c_str();
}

void assign_psx_player_source(HubModel& hub, int p, int src, const char* guid, const char* name,
                              bool* dirty) {
    auto& s = hub.psx_settings.settings;
    s.player_src[static_cast<size_t>(p)] = src;
    if (src == 1) {
        s.player_guid[static_cast<size_t>(p)].clear();
        s.player_mode[static_cast<size_t>(p)] = 2; // digital default for keyboard
    } else if (src == 2 && guid && guid[0]) {
        s.player_guid[static_cast<size_t>(p)] = guid;
        retcomm::psx_pad_binds_remember(hub.paths, guid, name ? name : "Controller", -1);
        const int dz = retcomm::psx_pad_binds_deadzone(hub.paths, guid);
        s.player_deadzone[static_cast<size_t>(p)] =
            std::clamp((dz * 32767 + 50) / 100, 0, 32767);
        // Default DualShock/analog only when unset — do not clobber an explicit
        // digital/analog choice (re-selecting the same pad used to wipe digital).
        if (s.player_mode[static_cast<size_t>(p)] != 1 &&
            s.player_mode[static_cast<size_t>(p)] != 2)
            s.player_mode[static_cast<size_t>(p)] = 1;
    } else {
        s.player_guid[static_cast<size_t>(p)].clear();
    }
    *dirty = true;
}

void draw_psx_player_source_combo(HubModel& hub, int p, const std::vector<HubGamepadOpt>& pads,
                                  bool* dirty) {
    auto& s = hub.psx_settings.settings;
    const char* preview = psx_player_src_preview(s, p, pads);
    ImGui::SetNextItemWidth(-1.f);
    if (!ImGui::BeginCombo("##src", preview)) return;

    if (ImGui::Selectable("None", s.player_src[static_cast<size_t>(p)] == 0))
        assign_psx_player_source(hub, p, 0, nullptr, nullptr, dirty);
    if (ImGui::Selectable("Keyboard", s.player_src[static_cast<size_t>(p)] == 1))
        assign_psx_player_source(hub, p, 1, nullptr, nullptr, dirty);

    if (pads.empty()) {
        ImGui::BeginDisabled();
        ImGui::Selectable("(no gamepad connected)");
        ImGui::EndDisabled();
    } else {
        for (const auto& pad : pads) {
            bool claimed = false;
            for (int o = 0; o < retcomm::PsxPlatformSettings::kMaxPlayers; ++o) {
                if (o == p) continue;
                if (s.player_src[static_cast<size_t>(o)] == 2 &&
                    s.player_guid[static_cast<size_t>(o)] == pad.guid) {
                    claimed = true;
                    break;
                }
            }
            char label[96];
            if (pad.live)
                std::snprintf(label, sizeof(label), "%s", pad.name);
            else
                std::snprintf(label, sizeof(label), "%s (disconnected)", pad.name);
            const bool sel = s.player_src[static_cast<size_t>(p)] == 2 &&
                             s.player_guid[static_cast<size_t>(p)] == pad.guid;
            if (claimed) ImGui::BeginDisabled();
            if (ImGui::Selectable(label, sel) && !claimed)
                assign_psx_player_source(hub, p, 2, pad.guid, pad.name, dirty);
            if (claimed) ImGui::EndDisabled();
        }
    }
    ImGui::EndCombo();
}

// Overlay chips — normalized centers on the stylized PS1 pad art
// (assets/controllers/pad_analog.png / pad_digital.png).
struct PsxPadHit {
    int button;
    float nx, ny;
};

// Short labels drawn on chips (binding string goes in the tooltip).
const char* psx_pad_chip_label(int b) {
    static const char* k[] = {
        "U",    "D",    "L",    "R",     // d-pad
        "Tri",  "Cir",  "Cro",  "Sq",    // face
        "L1",   "L2",   "R1",   "R2",    // shoulders
        "L3",   "R3",   "Start", "Select",
        "LS^",  "LSv",  "LS<",  "LS>",   // left stick
        "RS^",  "RSv",  "RS<",  "RS>",   // right stick
    };
    if (b < 0 || b >= retcomm::kPsxPadButtonCount) return "?";
    return k[b];
}

const PsxPadHit* psx_pad_hits(int* count) {
    // Exact centers from the procedural flat-retro pad art (720x400).
    // D-pad / stick dirs are pushed out from the art centers so chips don't overlap
    // after the UV crop zoom (chips are wider than the physical buttons).
    static const PsxPadHit kHits[] = {
        {9, 0.274f, 0.257f},  // L2
        {8, 0.268f, 0.302f},  // L1
        {11, 0.726f, 0.257f}, // R2
        {10, 0.733f, 0.302f}, // R1
        {0, 0.311f, 0.438f},  // Up
        {1, 0.311f, 0.572f},  // Down
        {2, 0.2665f, 0.505f},  // Left
        {3, 0.355f, 0.505f},  // Right
        {4, 0.689f, 0.421f},  // Triangle
        {5, 0.736f, 0.505f},  // Circle
        {6, 0.689f, 0.589f},  // Cross
        {7, 0.642f, 0.505f},  // Square
        {15, 0.447f, 0.520f}, // Select
        {14, 0.553f, 0.520f}, // Start
        {16, 0.409f, 0.610f}, // LS Up
        {17, 0.409f, 0.740f}, // LS Down
        {18, 0.350f, 0.675f}, // LS Left
        {19, 0.468f, 0.675f}, // LS Right
        {12, 0.409f, 0.675f}, // L3
        {20, 0.591f, 0.610f}, // RS Up
        {21, 0.591f, 0.740f}, // RS Down
        {22, 0.532f, 0.675f}, // RS Left
        {23, 0.650f, 0.675f}, // RS Right
        {13, 0.591f, 0.675f}, // R3
    };
    *count = static_cast<int>(sizeof(kHits) / sizeof(kHits[0]));
    return kHits;
}

bool poll_psx_bind_capture(HubModel& hub, const SDL_Event& e) {
    auto& d = hub.psx_settings;
    if (d.configuring_player < 0) return false;
    if (d.capturing_bind < 0 && !d.map_all_active) return false;

    if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
        cancel_psx_bind_capture(hub);
        return true;
    }

    const int p = d.configuring_player;
    const auto& s = d.settings;
    const bool is_pad = d.capture_is_pad;

    if (!is_pad) {
        if (e.type != SDL_EVENT_KEY_DOWN || e.key.repeat) return true;
        if (d.capturing_bind < 0) return true;
        retcomm::psx_keybinds_set_scancode(hub.paths, p, d.capturing_bind,
                                           static_cast<int>(e.key.scancode));
        d.dirty = true;
        if (d.map_all_active) advance_psx_map_all(hub);
        else cancel_psx_bind_capture(hub);
        return true;
    }

    const std::string& guid = s.player_guid[static_cast<size_t>(p)];
    SDL_JoystickID want = find_live_pad_id(guid.c_str());
    // If GUID lookup failed but exactly one pad is open, accept that device so
    // mapping still works across GUID string quirks.
    if (!want) {
        int live_n = 0;
        SDL_JoystickID only = 0;
        for (int i = 0; i < kMaxOpenHubPads; ++i) {
            if (!g_open_pads[i].handle) continue;
            ++live_n;
            only = g_open_pads[i].id;
        }
        if (live_n == 1) want = only;
    }
    auto from_selected = [&](SDL_JoystickID which) {
        if (!want) return false;
        return which == want;
    };

    auto try_clear_release = [&](SDL_JoystickID which) {
        if (!d.map_all_wait_release) return;
        if (!from_selected(which)) return;
        if (gamepad_at_rest(which)) {
            d.map_all_wait_release = false;
            snapshot_capture_baseline(which);
            reset_capture_debounce();
        }
    };

    auto commit = [&](int kind, int code, int axis_dir) {
        char src[48]{};
        fill_bind_source_string(kind, code, axis_dir, src, sizeof(src));
        if (src[0])
            retcomm::psx_pad_binds_set_source(hub.paths, guid, d.capturing_bind, src);
        else
            retcomm::psx_pad_binds_set(hub.paths, guid, d.capturing_bind, kind, code, axis_dir);
        retcomm::psx_pad_binds_remember(hub.paths, guid, "", -1);
        d.dirty = true;
        reset_capture_debounce();
        if (d.map_all_active) advance_psx_map_all(hub);
        else cancel_psx_bind_capture(hub);
    };

    if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        if (d.map_all_wait_release) {
            try_clear_release(e.gbutton.which);
            return true;
        }
        if (!capture_armed()) return true;
        if (d.capturing_bind >= 0 && from_selected(e.gbutton.which)) {
            const int btn = static_cast<int>(e.gbutton.button);
            if (capture_accepts_button(d.capturing_bind, btn)) commit(1, btn, 0);
        }
        return true;
    }
    if (e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        try_clear_release(e.gbutton.which);
        return true;
    }
    if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        if (!from_selected(e.gaxis.which)) return true;
        if (d.map_all_wait_release) {
            try_clear_release(e.gaxis.which);
            return true;
        }
        if (!capture_armed()) return true;
        if (d.capturing_bind < 0) return true;
        const int val = static_cast<int>(e.gaxis.value);
        if (val < kCaptureAxisCommit && val > -kCaptureAxisCommit) return true;
        const int axis = static_cast<int>(e.gaxis.axis);
        if (!capture_accepts_axis(d.capturing_bind, axis)) return true;
        commit(2, axis, val > 0 ? 1 : -1);
        return true;
    }
    return true; // swallow while capturing
}

// Poll-based capture fallback: commit after debounce when a filtered candidate
// rises above the baseline (covers missed GAMEPAD_* events).
void tick_psx_bind_capture_poll(HubModel& hub) {
    auto& d = hub.psx_settings;
    if (d.configuring_player < 0) return;
    if (!d.capture_is_pad) return;
    if (d.capturing_bind < 0 && !d.map_all_active) return;

    const int p = d.configuring_player;
    const std::string& guid = d.settings.player_guid[static_cast<size_t>(p)];
    SDL_JoystickID want = find_live_pad_id(guid.c_str());
    if (!want) {
        int live_n = 0;
        SDL_JoystickID only = 0;
        for (int i = 0; i < kMaxOpenHubPads; ++i) {
            if (!g_open_pads[i].handle) continue;
            ++live_n;
            only = g_open_pads[i].id;
        }
        if (live_n == 1) want = only;
    }
    if (!want) return;

    if (d.map_all_wait_release) {
        if (gamepad_at_rest(want)) {
            d.map_all_wait_release = false;
            snapshot_capture_baseline(want);
            reset_capture_debounce();
        }
        return;
    }
    if (d.capturing_bind < 0) return;
    if (!capture_armed()) return;

    SDL_Gamepad* pad = gamepad_handle_for_id(want);
    if (!pad) return;
    if (!g_capture_baseline.valid || g_capture_baseline.id != want)
        snapshot_capture_baseline(want);

    const int slot = d.capturing_bind;

    auto commit = [&](int kind, int code, int axis_dir) {
        char src[48]{};
        fill_bind_source_string(kind, code, axis_dir, src, sizeof(src));
        if (src[0])
            retcomm::psx_pad_binds_set_source(hub.paths, guid, d.capturing_bind, src);
        else
            retcomm::psx_pad_binds_set(hub.paths, guid, d.capturing_bind, kind, code, axis_dir);
        retcomm::psx_pad_binds_remember(hub.paths, guid, "", -1);
        d.dirty = true;
        reset_capture_debounce();
        if (d.map_all_active) advance_psx_map_all(hub);
        else cancel_psx_bind_capture(hub);
    };

    auto note_candidate = [&](int kind, int code, int axis_dir) {
        if (g_capture_baseline.debounce_kind == kind && g_capture_baseline.debounce_code == code &&
            g_capture_baseline.debounce_dir == axis_dir) {
            g_capture_baseline.debounce_frames++;
        } else {
            g_capture_baseline.debounce_kind = kind;
            g_capture_baseline.debounce_code = code;
            g_capture_baseline.debounce_dir = axis_dir;
            g_capture_baseline.debounce_frames = 1;
        }
        if (g_capture_baseline.debounce_frames >= kCapturePollDebounceFrames)
            commit(kind, code, axis_dir);
    };

    // D-pad slots: only a single new cardinal (reject diagonals / face noise).
    if (slot_is_dpad(slot)) {
        const int sole = sole_new_dpad_button(pad);
        if (sole >= 0) note_candidate(1, sole, 0);
        else reset_capture_debounce();
        return;
    }

    int cand_kind = 0, cand_code = -1, cand_dir = 0;
    int cand_n = 0;

    for (int b = 0; b < static_cast<int>(SDL_GAMEPAD_BUTTON_COUNT); ++b) {
        if (!capture_accepts_button(slot, b)) continue;
        const bool now = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b)) != 0;
        if (now && !g_capture_baseline.buttons[b]) {
            ++cand_n;
            cand_kind = 1;
            cand_code = b;
            cand_dir = 0;
        }
    }
    for (int a = 0; a < static_cast<int>(SDL_GAMEPAD_AXIS_COUNT); ++a) {
        if (!capture_accepts_axis(slot, a)) continue;
        const int val = static_cast<int>(SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(a)));
        if (val < kCaptureAxisCommit && val > -kCaptureAxisCommit) continue;
        const int base = static_cast<int>(g_capture_baseline.axes[a]);
        if (std::abs(base) >= kCaptureAxisCommit) continue;
        ++cand_n;
        cand_kind = 2;
        cand_code = a;
        cand_dir = val > 0 ? 1 : -1;
    }

    if (cand_n == 1) note_candidate(cand_kind, cand_code, cand_dir);
    else reset_capture_debounce();
}

void draw_psx_mapping_panel(HubModel& hub, const Theme& th, int p, bool is_pad,
                            const std::string& guid, BoxartCache& boxart) {
    auto& d = hub.psx_settings;
    const bool digital = hub.psx_settings.settings.player_mode[static_cast<size_t>(p)] == 2;
    fs::path art = find_hub_asset_file("controllers", digital ? "pad_digital.png" : "pad_analog.png");
    if (art.empty())
        art = find_hub_asset_file("controllers", digital ? "pad_digital.tga" : "pad_analog.tga");
    const BoxartTexture* tex =
        art.empty() ? nullptr
                    : boxart.get(digital ? "psx:pad_digital" : "psx:pad_analog", art);

    ImGui::BeginChild("psx_map_panel", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextColored(th.text_muted, is_pad ? "GAMEPAD BINDINGS" : "KEYBOARD BINDINGS");
    if (is_pad && !digital) {
        ImGui::TextColored(th.text_muted,
                           "Analog mode: D-pad folds onto the left stick. Chips light when the "
                           "bound control is pressed.");
    } else if (is_pad) {
        ImGui::TextColored(th.text_muted,
                           "Chips light when the bound control is pressed — verify each bind.");
    }
    ImGui::Separator();

    // Footer inside the panel: status only — action buttons live on the modal bar.
    const float status_reserve = (d.capturing_bind >= 0) ? 28.f : 8.f;
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float avail_h = std::max(260.f, ImGui::GetContentRegionAvail().y - status_reserve);

    // pad_*.png is 720x400 with large empty margins around the silhouette. Zoom the
    // UV rect to the controller content so the panel isn't mostly letterbox.
    // Hit coords (psx_pad_hits) are normalized in the full 720x400 canvas.
    constexpr float kCropU0 = 0.105f;
    constexpr float kCropV0 = 0.175f;
    constexpr float kCropU1 = 0.895f;
    constexpr float kCropV1 = 0.865f;
    const float crop_w = kCropU1 - kCropU0;
    const float crop_h = kCropV1 - kCropV0;
    const float full_aspect = (tex && tex->width > 0 && tex->height > 0)
                                  ? (static_cast<float>(tex->width) / static_cast<float>(tex->height))
                                  : (720.f / 400.f);
    const float aspect = full_aspect * (crop_w / std::max(0.01f, crop_h));

    // Fill the panel — thin edge pad, then center any leftover on the short axis.
    constexpr float kEdgePad = 4.f;
    float img_w = std::max(1.f, avail_w - kEdgePad * 2.f);
    float img_h = img_w / aspect;
    if (img_h > avail_h - kEdgePad * 2.f) {
        img_h = std::max(1.f, avail_h - kEdgePad * 2.f);
        img_w = img_h * aspect;
    }
    const float ox = (avail_w - img_w) * 0.5f;
    const float oy = (avail_h - img_h) * 0.5f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float img_x = origin.x + ox;
    const float img_y = origin.y + oy;

    // Reserve the full available area so the child layout matches the painted art.
    ImGui::Dummy(ImVec2(avail_w, avail_h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (tex && tex->gl_id) {
        dl->AddImage((ImTextureID)(intptr_t)tex->gl_id, ImVec2(img_x, img_y),
                     ImVec2(img_x + img_w, img_y + img_h), ImVec2(kCropU0, kCropV0),
                     ImVec2(kCropU1, kCropV1));
    } else {
        dl->AddRectFilled(ImVec2(img_x, img_y), ImVec2(img_x + img_w, img_y + img_h),
                          ImGui::ColorConvertFloat4ToU32(th.control));
        dl->AddText(ImVec2(img_x + 12.f, img_y + 12.f),
                    ImGui::ColorConvertFloat4ToU32(th.warn),
                    art.empty() ? "pad art missing (assets/controllers)" : "pad art failed to load");
    }

    int hit_n = 0;
    const PsxPadHit* hits = psx_pad_hits(&hit_n);
    // Scale chips with the on-screen pad (cropped reference width ~570px at 720 source).
    const float ui_scale = std::clamp(img_w / 570.f, 0.95f, 2.1f);
    const float chip_w = 44.f * ui_scale;
    const float chip_h = 16.f * ui_scale;

    SDL_Gamepad* live_pad = nullptr;
    int live_dz = 8000;
    if (is_pad && !guid.empty()) {
        const SDL_JoystickID id = find_live_pad_id(guid.c_str());
        live_pad = gamepad_handle_for_id(id);
        live_dz = (retcomm::psx_pad_binds_deadzone(hub.paths, guid) * 32767 + 50) / 100;
        if (live_dz < 1) live_dz = 8000;
    }
    int kb_nkeys = 0;
    const bool* kb_keys = is_pad ? nullptr : SDL_GetKeyboardState(&kb_nkeys);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.f * ui_scale, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.f * ui_scale);
    ImGui::SetWindowFontScale(0.78f * ui_scale);
    for (int i = 0; i < hit_n; ++i) {
        const int b = hits[i].button;
        if (digital && b >= 16) continue;
        char bound[48]{};
        if (is_pad)
            retcomm::psx_pad_binds_label(hub.paths, guid, b, bound, sizeof(bound));
        else
            retcomm::psx_keybinds_label(hub.paths, p, b, bound, sizeof(bound));

        bool live_pressed = false;
        if (is_pad && live_pad && bound[0] && std::strcmp(bound, "(unbound)") != 0) {
            live_pressed = gamepad_bound_source_pressed(live_pad, bound, live_dz);
        } else if (!is_pad && kb_keys) {
            const int sc = retcomm::psx_keybinds_get_scancode(hub.paths, p, b);
            if (sc > 0 && sc < kb_nkeys) live_pressed = kb_keys[sc] != 0;
        }

        const float cx = img_x + ((hits[i].nx - kCropU0) / crop_w) * img_w;
        const float cy = img_y + ((hits[i].ny - kCropV0) / crop_h) * img_h;
        ImGui::SetCursorScreenPos(ImVec2(cx - chip_w * 0.5f, cy - chip_h * 0.5f));
        ImGui::PushID(b);
        const bool capturing = d.capturing_bind == b;
        char btn[64];
        if (capturing && d.map_all_wait_release)
            std::snprintf(btn, sizeof(btn), "…");
        else if (capturing)
            std::snprintf(btn, sizeof(btn), "[%s]", psx_pad_chip_label(b));
        else
            std::snprintf(btn, sizeof(btn), "%s", psx_pad_chip_label(b));

        if (capturing)
            ImGui::PushStyleColor(ImGuiCol_Button, th.accent);
        else if (live_pressed)
            ImGui::PushStyleColor(ImGuiCol_Button, th.good_button);
        else
            ImGui::PushStyleColor(ImGuiCol_Button,
                                 ImVec4(th.control.x, th.control.y, th.control.z, 0.88f));
        if (ImGui::Button(btn, ImVec2(chip_w, chip_h))) {
            d.map_all_active = false;
            d.map_all_wait_release = false;
            begin_psx_bind_capture(hub, b, is_pad);
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            char tip[160]{};
            std::snprintf(tip, sizeof(tip), "%s\nBound: %s%s", retcomm::psx_pad_button_label(b),
                          bound[0] ? bound : "(unbound)", live_pressed ? "\n(pressed)" : "");
            if (live_pad) append_live_sdl_dpad_hint(live_pad, tip, sizeof(tip));
            ImGui::SetTooltip("%s", tip);
        }
        ImGui::PopID();
    }
    ImGui::SetWindowFontScale(1.f);
    ImGui::PopStyleVar(2);

    ImGui::SetCursorScreenPos(ImVec2(origin.x, img_y + img_h + 6.f));
    ImGui::Dummy(ImVec2(avail_w, 0));
    if (d.capturing_bind >= 0) {
        if (d.map_all_wait_release)
            ImGui::TextColored(th.warn, "Release controls… (Esc cancels%s)",
                               d.map_all_active ? " — Auto Map" : "");
        else
            ImGui::TextColored(th.warn, "Map an input to %s (Esc cancels%s)",
                               retcomm::psx_pad_button_label(d.capturing_bind),
                               d.map_all_active ? " — Auto Map" : "");
    }
    ImGui::EndChild();
}

void draw_psx_configure_modal(HubModel& hub, const Theme& th, const std::vector<HubGamepadOpt>& pads,
                              BoxartCache& boxart) {
    auto& draft = hub.psx_settings;
    if (draft.configuring_player < 0 ||
        draft.configuring_player >= retcomm::PsxPlatformSettings::kMaxPlayers)
        return;
    const int p = draft.configuring_player;
    auto& s = draft.settings;
    bool dirty = false;

    char title[48];
    std::snprintf(title, sizeof(title), "CONTROLLER - PLAYER %d", p + 1);
    ImGui::OpenPopup("##psx_pad_cfg");
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(std::min(1100.f, vp->WorkSize.x * 0.96f),
                                    std::min(860.f, vp->WorkSize.y * 0.94f)),
                             ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("##psx_pad_cfg", nullptr, ImGuiWindowFlags_None)) {
        ImGui::TextColored(th.accent, "%s", title);
        ImGui::SameLine();
        {
            const float close_w = 80.f;
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - close_w);
            if (ImGui::Button("Close", ImVec2(close_w, 0))) {
                cancel_psx_bind_capture(hub);
                draft.configuring_player = -1;
                draft.rename_open = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::Separator();
        ImGui::PushID(p);

        const bool is_pad =
            s.player_src[static_cast<size_t>(p)] == 2 && !s.player_guid[static_cast<size_t>(p)].empty();
        const bool is_kb = s.player_src[static_cast<size_t>(p)] == 1;
        if (is_kb) s.player_mode[static_cast<size_t>(p)] = 2;

        // Two-row table keeps labels and controls on the same baselines across columns.
        if (ImGui::BeginTable("psx_cfg_top", 3,
                              ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(th.text_muted, "Input source");
            ImGui::TableNextColumn();
            ImGui::TextColored(th.text_muted, "Pad mode");
            ImGui::TableNextColumn();
            ImGui::TextColored(th.text_muted, "Deadzone");

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            draw_psx_player_source_combo(hub, p, pads, &dirty);

            ImGui::TableNextColumn();
            {
                const bool dig = s.player_mode[static_cast<size_t>(p)] == 2;
                const char* mode_lab = dig ? "Digital (D-Pad)" : "Analog (DualShock)";
                if (is_kb) ImGui::BeginDisabled();
                if (ImGui::Button(mode_lab, ImVec2(-1.f, 0))) {
                    s.player_mode[static_cast<size_t>(p)] = dig ? 1 : 2;
                    dirty = true;
                }
                if (is_kb) {
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("Keyboard seats use digital mode.");
                    ImGui::EndDisabled();
                }
            }

            ImGui::TableNextColumn();
            {
                int pct = 0;
                if (is_pad)
                    pct = retcomm::psx_pad_binds_deadzone(hub.paths,
                                                         s.player_guid[static_cast<size_t>(p)]);
                else
                    pct = (s.player_deadzone[static_cast<size_t>(p)] * 100 + 32767 / 2) / 32767;
                pct = std::clamp(pct, 0, 100);
                ImGui::SetNextItemWidth(-1.f);
                if (ImGui::SliderInt("##dz", &pct, 0, 50, "%d%%")) {
                    s.player_deadzone[static_cast<size_t>(p)] =
                        std::clamp((pct * 32767 + 50) / 100, 0, 32767);
                    if (is_pad)
                        retcomm::psx_pad_binds_set_deadzone(
                            hub.paths, s.player_guid[static_cast<size_t>(p)], pct);
                    dirty = true;
                }
            }
            ImGui::EndTable();
        }
        ImGui::Dummy(ImVec2(0, 6));

        if (draft.rename_open) ImGui::OpenPopup("Rename Profile");
        if (ImGui::BeginPopupModal("Rename Profile", &draft.rename_open,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Display name for this gamepad profile:");
            ImGui::SetNextItemWidth(320.f);
            const bool enter = ImGui::InputText("##rename_pad", draft.rename_buf,
                                                sizeof(draft.rename_buf),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                draft.rename_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            const bool ok = draft.rename_buf[0] != '\0';
            ImGui::BeginDisabled(!ok);
            if ((ImGui::Button("OK", ImVec2(120, 0)) || enter) && ok) {
                retcomm::psx_pad_binds_rename(hub.paths, s.player_guid[static_cast<size_t>(p)],
                                             draft.rename_buf);
                draft.rename_open = false;
                dirty = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }

        // Bindings fill remaining height above the bottom action bar.
        const float footer_h = ImGui::GetFrameHeightWithSpacing() + 8.f;
        const float body_h = std::max(200.f, ImGui::GetContentRegionAvail().y - footer_h);
        ImGui::BeginChild("psx_cfg_body", ImVec2(0, body_h), ImGuiChildFlags_None);
        if (s.player_src[static_cast<size_t>(p)] == 0) {
            ImGui::BeginChild("psx_map_empty", ImVec2(0, 0), ImGuiChildFlags_Borders);
            ImGui::TextColored(th.text_muted, "Assign an input source to edit mappings.");
            ImGui::EndChild();
        } else {
            draw_psx_mapping_panel(hub, th, p, is_pad, s.player_guid[static_cast<size_t>(p)],
                                   boxart);
        }
        ImGui::EndChild();

        // Bottom bar: profile + map actions + Save (saves platform prefs and closes).
        // Shared width so Rename/Delete/Auto Map/Reset read as one even strip.
        const char* kFooterBtns[] = {"Rename", "Delete", "Auto Map", "Reset"};
        float footer_btn_w = 0.f;
        for (const char* lab : kFooterBtns)
            footer_btn_w = std::max(footer_btn_w, ImGui::CalcTextSize(lab).x);
        footer_btn_w += ImGui::GetStyle().FramePadding.x * 2.f + 16.f;
        const ImVec2 footer_btn_sz(footer_btn_w, 0);

        if (!is_pad) ImGui::BeginDisabled();
        if (ImGui::Button("Rename", footer_btn_sz)) {
            char nm[64]{};
            retcomm::psx_pad_binds_name(hub.paths, s.player_guid[static_cast<size_t>(p)], nm,
                                        sizeof(nm));
            if (!nm[0]) {
                for (const auto& pad : pads)
                    if (pad.guid == s.player_guid[static_cast<size_t>(p)]) {
                        std::snprintf(nm, sizeof(nm), "%s", pad.name);
                        break;
                    }
            }
            std::snprintf(draft.rename_buf, sizeof(draft.rename_buf), "%s", nm);
            draft.rename_open = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete", footer_btn_sz)) {
            const std::string g = s.player_guid[static_cast<size_t>(p)];
            retcomm::psx_pad_binds_delete(hub.paths, g);
            for (int o = 0; o < retcomm::PsxPlatformSettings::kMaxPlayers; ++o) {
                if (s.player_src[static_cast<size_t>(o)] == 2 &&
                    s.player_guid[static_cast<size_t>(o)] == g)
                    assign_psx_player_source(hub, o, 1, nullptr, nullptr, &dirty);
            }
            cancel_psx_bind_capture(hub);
        }
        if (!is_pad) ImGui::EndDisabled();

        ImGui::SameLine();
        if (accent_button("Auto Map", th, footer_btn_sz)) {
            if (s.player_src[static_cast<size_t>(p)] != 0) begin_psx_map_all(hub, is_pad);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset", footer_btn_sz)) {
            if (is_pad && !s.player_guid[static_cast<size_t>(p)].empty())
                retcomm::psx_pad_binds_reset(hub.paths, s.player_guid[static_cast<size_t>(p)]);
            else if (is_kb)
                retcomm::psx_keybinds_reset_player(hub.paths, p);
            cancel_psx_bind_capture(hub);
            dirty = true;
        }

        {
            const float save_w = 120.f;
            ImGui::SameLine();
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 8.f,
                                          ImGui::GetWindowContentRegionMax().x - save_w));
            if (accent_button("Save", th, ImVec2(save_w, 0))) {
                if (dirty) draft.dirty = true;
                // Persist profile name/deadzone/maps already written live; also
                // flush settings.toml player seats + close the modal.
                if (is_pad && !s.player_guid[static_cast<size_t>(p)].empty()) {
                    char nm[64]{};
                    retcomm::psx_pad_binds_name(hub.paths, s.player_guid[static_cast<size_t>(p)], nm,
                                                sizeof(nm));
                    const int dz = retcomm::psx_pad_binds_deadzone(
                        hub.paths, s.player_guid[static_cast<size_t>(p)]);
                    retcomm::psx_pad_binds_save_profile(
                        hub.paths, s.player_guid[static_cast<size_t>(p)], nm[0] ? nm : "Controller",
                        retcomm::psx_pad_binds_name_is_custom(hub.paths,
                                                              s.player_guid[static_cast<size_t>(p)]),
                        dz);
                }
                std::string err;
                if (!hub.save_psx_settings(&err)) {
                    hub.append_log("PlayStation settings save failed: " + err);
                } else {
                    hub.show_toast("Saved!");
                }
                cancel_psx_bind_capture(hub);
                draft.configuring_player = -1;
                draft.rename_open = false;
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::PopID();
        ImGui::EndPopup();
    }
    if (dirty) draft.dirty = true;
}

void draw_psx_gamepads_panel(HubModel& hub, const Theme& th, float panel_h, BoxartCache& boxart) {
    auto& s = hub.psx_settings.settings;
    bool dirty = false;
    std::vector<HubGamepadOpt> pads;
    collect_hub_gamepads(hub, pads);

    ImGui::BeginChild("psx_gamepads", ImVec2(0, panel_h), ImGuiChildFlags_Borders);
    ImGui::TextColored(th.text_muted, "CONTROLLERS");
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "Slots 1–8 for titles that support multitap / high player counts. Assignments "
        "write pN_device / pN_mode / pN_deadzone into the global PlayStation settings.toml "
        "and are applied to games on install, update, and launch.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 8));

    constexpr int kN = retcomm::PsxPlatformSettings::kMaxPlayers;
    const float gap = th.spacing_md;
    const float availw = ImGui::GetContentRegionAvail().x;
    const float pref = 280.f;
    int cols = static_cast<int>((availw + gap) / (pref + gap));
    if (cols < 1) cols = 1;
    if (cols > 4) cols = 4;
    float cardw = (availw - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
    if (cardw < 1.f) cardw = availw;

    for (int p = 0; p < kN; ++p) {
        if (p % cols) ImGui::SameLine(0, gap);
        else if (p) ImGui::Dummy(ImVec2(0, gap));

        ImGui::PushID(p);
        ImGui::BeginChild("pcard", ImVec2(cardw, 0),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        {
            char eb[24];
            std::snprintf(eb, sizeof(eb), "PLAYER %d", p + 1);
            ImGui::TextColored(th.text_muted, "%s", eb);
            ImGui::Dummy(ImVec2(0, 4));
            draw_psx_player_source_combo(hub, p, pads, &dirty);
            ImGui::Dummy(ImVec2(0, 4));
            const float cw = ImGui::GetContentRegionAvail().x;
            const float half = (cw - th.spacing_sm) * 0.5f;
            const float btnh = 32.f;
            if (ImGui::Button("Configure", ImVec2(half, btnh))) {
                cancel_psx_bind_capture(hub);
                hub.psx_settings.configuring_player = p;
                if (s.player_src[static_cast<size_t>(p)] == 1)
                    s.player_mode[static_cast<size_t>(p)] = 2;
                else if (s.player_src[static_cast<size_t>(p)] == 2 &&
                         !s.player_guid[static_cast<size_t>(p)].empty()) {
                    const char* nm = "Controller";
                    for (const auto& pad : pads)
                        if (pad.guid == s.player_guid[static_cast<size_t>(p)]) {
                            nm = pad.name;
                            break;
                        }
                    retcomm::psx_pad_binds_remember(hub.paths, s.player_guid[static_cast<size_t>(p)],
                                                   nm, -1);
                }
            }
            ImGui::SameLine(0, th.spacing_sm);
            const bool on = s.player_src[static_cast<size_t>(p)] != 0;
            const char* st = on ? "connected" : "not assigned";
            const float sw = 10.f + 8.f + ImGui::CalcTextSize(st).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (half - sw) * 0.5f));
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                                 (btnh - ImGui::GetTextLineHeight()) * 0.5f);
            const ImVec2 dot_c = ImGui::GetCursorScreenPos();
            const float r = 4.f;
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(dot_c.x + r, dot_c.y + ImGui::GetTextLineHeight() * 0.5f), r,
                ImGui::ColorConvertFloat4ToU32(on ? th.good : th.text_muted));
            ImGui::Dummy(ImVec2(r * 2.f + 6.f, 0));
            ImGui::SameLine(0, 0);
            ImGui::TextColored(on ? th.good : th.text_muted, "%s", st);
        }
        ImGui::EndChild();
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (dirty) hub.psx_settings.dirty = true;
    draw_psx_configure_modal(hub, th, pads, boxart);
}

void draw_psx_settings_panel(HubModel& hub, const Theme& th, BoxartCache& boxart) {
    poll_psx_hotkey_capture(hub);
    auto& s = hub.psx_settings.settings;
    auto mark = [&] { hub.psx_settings.dirty = true; };
    const bool gamepads = hub.psx_settings.gamepads_tab;

    ImGui::BeginChild("psx_settings", ImVec2(0, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextUnformatted("PLAYSTATION SETTINGS");
    ImGui::PopStyleColor();

    constexpr float kTabW = 110.f;
    const float right = ImGui::GetWindowContentRegionMax().x;
    const float wrap_x = ImGui::GetCursorPosX() + (right - ImGui::GetCursorPosX()) - kTabW - 12.f;
    const float desc_y = ImGui::GetCursorPosY();
    ImGui::PushTextWrapPos(wrap_x);
    ImGui::TextWrapped(
        "%s",
        gamepads
            ? "Global controller slots for PlayStation titles. Saved prefs are written into "
              "each game's settings.toml on install, update, and launch (unless excluded in "
              "Manage Game Data)."
            : "Global Display, Audio, Input, and Hotkeys for PlayStation titles. Saved prefs "
              "are written into each game's settings.toml / config.ini on install, update, and "
              "launch (unless excluded in Manage Game Data).");
    ImGui::PopTextWrapPos();
    ImGui::SameLine();
    ImGui::SetCursorPosY(desc_y);
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - kTabW));
    if (gamepads) {
        if (good_button("System", th, ImVec2(kTabW, 0))) {
            hub.psx_settings.gamepads_tab = false;
            cancel_psx_bind_capture(hub);
            hub.psx_settings.configuring_player = -1;
        }
    } else {
        if (good_button("Gamepads", th, ImVec2(kTabW, 0)))
            hub.psx_settings.gamepads_tab = true;
    }
    ImGui::Separator();

    constexpr float kCol = 200.f;
    auto cycle_btn = [&](const char* id, const char* label, float w = 160.f) -> bool {
        ImGui::PushID(id);
        const bool hit = ImGui::Button(label, ImVec2(w, 0));
        ImGui::PopID();
        return hit;
    };

    // Reserve gap + Save/Cancel row so panes don't crowd the footer.
    const float footer_h = ImGui::GetFrameHeight() + 24.f;
    const float gap = 12.f;
    const float avail_x = ImGui::GetContentRegionAvail().x;
    const float col_w = std::max(240.f, (avail_x - gap) * 0.5f);
    const float panel_h = std::max(120.f, ImGui::GetContentRegionAvail().y - footer_h);

    if (gamepads) {
        draw_psx_gamepads_panel(hub, th, panel_h, boxart);
    } else {
    ImGui::BeginChild("psx_display_audio", ImVec2(col_w, panel_h), ImGuiChildFlags_Borders);
    ImGui::TextColored(th.text_muted, "DISPLAY & AUDIO");
    ImGui::Separator();
    {
        static const int kWidths[] = {960, 1280, 1600, 1920};
        psx_settings_row_label("Window size", th, kCol);
        char lbl[32];
        std::snprintf(lbl, sizeof(lbl), "%d px", s.window_width);
        if (cycle_btn("ww", lbl, 120.f)) {
            int idx = 0;
            for (int i = 0; i < 4; ++i)
                if (kWidths[i] == s.window_width) {
                    idx = i;
                    break;
                }
            s.window_width = kWidths[(idx + 1) % 4];
            mark();
        }

        psx_settings_row_label("Renderer", th, kCol);
        const char* rlab =
            s.renderer == 0 ? "Software" : (s.renderer == 2 ? "Vulkan" : "OpenGL");
        if (cycle_btn("ren", rlab, 160.f)) {
            s.renderer = (s.renderer + 1) % 3;
            mark();
        }

        psx_settings_row_label("Supersampling", th, kCol);
        char ssl[16];
        std::snprintf(ssl, sizeof(ssl), "%dx", s.supersampling);
        if (cycle_btn("ss", ssl, 80.f)) {
            s.supersampling = s.supersampling >= 4 ? 1 : s.supersampling + 1;
            mark();
        }

        psx_settings_row_label("Fullscreen", th, kCol);
        static const char* kFs[] = {"Off", "Borderless", "Exclusive"};
        if (cycle_btn("fs", kFs[std::clamp(s.fullscreen, 0, 2)], 140.f)) {
            s.fullscreen = (std::clamp(s.fullscreen, 0, 2) + 1) % 3;
            mark();
        }

        psx_settings_row_label("View mode", th, kCol);
        static const char* kView[] = {"4:3 (Native)", "16:9 (Widescreen)", "21:9 (Ultrawide)",
                                      "Adaptive"};
        if (cycle_btn("view", kView[std::clamp(s.view_mode, 0, 3)], 180.f)) {
            s.view_mode = (std::clamp(s.view_mode, 0, 3) + 1) % 4;
            mark();
        }
        if (s.view_mode != 0) {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(th.warn, "⚠️ Experimental ⚠️");
        }

        psx_settings_row_label("Texture filtering", th, kCol);
        if (cycle_btn("tf", s.texture_filter_bilinear ? "Bilinear" : "Nearest", 120.f)) {
            s.texture_filter_bilinear = !s.texture_filter_bilinear;
            mark();
        }

        psx_settings_row_label("Antialiasing", th, kCol);
        if (cycle_btn("aa", s.antialiasing ? "On" : "Off", 80.f)) {
            s.antialiasing = !s.antialiasing;
            mark();
        }

        psx_settings_row_label("Perspective textures", th, kCol);
        if (ImGui::Checkbox("##persp", &s.perspective_texturing)) mark();

        psx_settings_row_label("Screen model", th, kCol);
        static const char* kScreen[] = {"Raw", "CRT", "Composite", "Trinitron"};
        if (cycle_btn("scr", kScreen[std::clamp(s.screen_kind, 0, 3)], 140.f)) {
            s.screen_kind = (std::clamp(s.screen_kind, 0, 3) + 1) % 4;
            mark();
        }

        if (s.renderer != 0) {
            psx_settings_row_label("Frame interpolation", th, kCol);
            if (ImGui::Checkbox("##fi", &s.frame_interpolation)) mark();
            if (s.frame_interpolation) {
                psx_settings_row_label("Presentation target", th, kCol);
                char fl[32];
                if (s.frame_interpolation_fps <= 0)
                    std::snprintf(fl, sizeof(fl), "Display");
                else
                    std::snprintf(fl, sizeof(fl), "%d fps", s.frame_interpolation_fps);
                if (cycle_btn("fifps", fl, 120.f)) {
                    static const int kFps[] = {0, 120, 144, 165, 240};
                    int idx = 0;
                    for (int i = 0; i < 5; ++i)
                        if (kFps[i] == s.frame_interpolation_fps) {
                            idx = i;
                            break;
                        }
                    s.frame_interpolation_fps = kFps[(idx + 1) % 5];
                    mark();
                }
            }
        }

        psx_settings_row_label("Skip FMVs", th, kCol);
        if (ImGui::Checkbox("##skipfmv", &s.auto_skip_fmv)) mark();

        psx_settings_row_label("Rewind buffer", th, kCol);
        {
            int d = s.rewind_depth;
            if (d != 50 && d != 100 && d != 150 && d != 200) d = 50;
            char lab[16];
            std::snprintf(lab, sizeof(lab), "%d", d);
            if (cycle_btn("rwbuf", lab, 80.f)) {
                static const int kDepth[] = {50, 100, 150, 200};
                int idx = 0;
                for (int i = 0; i < 4; ++i)
                    if (kDepth[i] == d) {
                        idx = i;
                        break;
                    }
                s.rewind_depth = kDepth[(idx + 1) % 4];
                mark();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "How many local rewind snapshots to keep (50 / 100 / 150 / 200).\n"
                    "Applied when a title launches.");
            }
        }

        psx_settings_row_label("Rewind interval", th, kCol);
        {
            int iv = s.rewind_interval;
            if (iv != 1 && iv != 4 && iv != 8 && iv != 12 && iv != 15) iv = 15;
            char lab[16];
            std::snprintf(lab, sizeof(lab), "%d", iv);
            if (cycle_btn("rwint", lab, 80.f)) {
                static const int kIv[] = {1, 4, 8, 12, 15};
                int idx = 4;
                for (int i = 0; i < 5; ++i)
                    if (kIv[i] == iv) {
                        idx = i;
                        break;
                    }
                s.rewind_interval = kIv[(idx + 1) % 5];
                mark();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "Frames between rewind snapshots (1 / 4 / 8 / 12 / 15).\n"
                    "FMV still densifies toward 4 when this is sparser.\n"
                    "Applied when a title launches.");
            }
        }

        psx_settings_row_label("Low-latency input", th, kCol);
        if (ImGui::Checkbox("##lli", &s.low_latency_input)) mark();

        psx_settings_row_label("VSync", th, kCol);
        const char* vlab = s.vsync == 0 ? "Immediate" : (s.vsync < 0 ? "Adaptive" : "On");
        if (cycle_btn("vs", vlab, 120.f)) {
            if (s.vsync == 1) s.vsync = 0;
            else if (s.vsync == 0) s.vsync = -1;
            else s.vsync = 1;
            mark();
        }

        ImGui::Dummy(ImVec2(0, 8));
        ImGui::Separator();
        ImGui::TextColored(th.text_muted, "AUDIO");
        psx_settings_row_label("High-quality SPU", th, kCol);
        if (ImGui::Checkbox("##spuhq", &s.spu_hq)) mark();
    }
    ImGui::EndChild();

    ImGui::SameLine(0.f, gap);
    ImGui::BeginChild("psx_input_hotkeys", ImVec2(0, panel_h), ImGuiChildFlags_Borders);
    ImGui::TextColored(th.text_muted, "INPUT & HOTKEYS");
    ImGui::Separator();
    {
        if (ImGui::Checkbox("Multitap", &s.multitap_enabled)) mark();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "Enable SCPH-1070 multitap for 3+ player seats. Off limits local play "
                "to two native controller ports.");
        }
        if (ImGui::Checkbox("Multitap analog (hack)", &s.multitap_analog)) mark();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "Allow DualShock sticks on multitap tap seats (not faithful). Not applied "
                "to titles with digital pad locked in game.toml.");
        }

        ImGui::Dummy(ImVec2(0, 10));
        ImGui::Separator();
        ImGui::TextColored(th.text_muted, "HOTKEYS");
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped("Click a binding, then press a key (Esc cancels).");
        ImGui::PopStyleColor();

        float label_w = 0.f;
        for (int i = 0; i < retcomm::PsxPlatformSettings::kHotkeyCount; ++i) {
            label_w = std::max(
                label_w, ImGui::CalcTextSize(retcomm::PsxPlatformSettings::hotkey_label(i)).x);
        }
        label_w += 16.f;
        if (ImGui::BeginTable("psx_hk", 1, ImGuiTableFlags_SizingStretchProp)) {
            for (int i = 0; i < retcomm::PsxPlatformSettings::kHotkeyCount; ++i) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(th.text_muted, "%s",
                                   retcomm::PsxPlatformSettings::hotkey_label(i));
                ImGui::SameLine(0.f, label_w - ImGui::CalcTextSize(
                                                  retcomm::PsxPlatformSettings::hotkey_label(i))
                                                  .x);
                const bool cap = hub.psx_settings.capturing_hotkey == i;
                const std::string& cur = s.hotkeys[static_cast<size_t>(i)];
                const char* bl =
                    cap ? "[ press... ]"
                        : (cur.empty() ? retcomm::PsxPlatformSettings::hotkey_default(i)
                                       : cur.c_str());
                if (cap) ImGui::PushStyleColor(ImGuiCol_Button, th.accent);
                if (ImGui::Button(bl, ImVec2(140.f, 0)))
                    hub.psx_settings.capturing_hotkey = i;
                if (cap) ImGui::PopStyleColor();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    } // system tab

    ImGui::Dummy(ImVec2(0, 12));
    const float footer_y = ImGui::GetCursorPosY();
    constexpr float kResetW = 150.f;
    if (accent_button("Save", th, ImVec2(160, 0))) {
        std::string err;
        if (!hub.save_psx_settings(&err)) {
            hub.append_log("PlayStation settings save failed: " + err);
            hub.set_status("Save failed");
        } else {
            hub.show_toast("Saved!");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        hub.show_psx_settings = false;
        hub.psx_settings.dirty = false;
        hub.psx_settings.capturing_hotkey = -1;
        cancel_psx_bind_capture(hub);
        hub.psx_settings.configuring_player = -1;
        hub.psx_settings.gamepads_tab = false;
    }
    if (hub.psx_settings.dirty) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(th.warn, "unsaved changes");
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(th.text_muted, "%s",
                       retcomm::psx_platform_settings_dir(hub.paths).string().c_str());
    // System tab only: restore Display / Audio / Multitap / Hotkeys defaults.
    if (!gamepads) {
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - kResetW, footer_y));
        if (ImGui::Button("Reset to Default", ImVec2(kResetW, 0))) {
            hub.psx_settings.capturing_hotkey = -1;
            s.reset_system_to_defaults();
            mark();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "Restore Display, Audio, Multitap, and Hotkeys to defaults.\n"
                "Gamepad seat assignments are left unchanged.");
        }
    }
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

void draw_log(HubModel& hub, const Theme& th, float height, SDL_Window* window) {
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
        const float export_w =
            ImGui::CalcTextSize("Export").x + ImGui::GetStyle().FramePadding.x * 2.f;
        const float copy_w =
            ImGui::CalcTextSize("Copy").x + ImGui::GetStyle().FramePadding.x * 2.f;
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float right = ImGui::GetWindowContentRegionMax().x;
        ImGui::SetCursorPosX(
            std::max(ImGui::GetCursorPosX(), right - hide_w - gap - export_w - gap - copy_w));
        if (ImGui::SmallButton("Hide")) hub.log_expanded = false;
        ImGui::SameLine();
        bool file_busy = false;
        {
            std::lock_guard<std::mutex> lock(hub.file_pick_mu);
            file_busy = hub.file_pick_busy;
        }
        ImGui::BeginDisabled(file_busy || window == nullptr);
        if (ImGui::SmallButton("Export")) begin_export_activity_log(hub, window);
        ImGui::EndDisabled();
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
    retcomm::set_github_token(hub.cfg.github_token);
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
        // Keep gamepads open so SDL3 delivers GAMEPAD_BUTTON / AXIS events.
        hub_sync_open_gamepads();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (poll_psx_bind_capture(hub, e)) {
                // Still feed ImGui so the modal stays responsive, but skip
                // duplicate key handling for capture commits.
                ImGui_ImplSDL3_ProcessEvent(&e);
            } else {
                ImGui_ImplSDL3_ProcessEvent(&e);
            }
            if (e.type == SDL_EVENT_QUIT) running = false;
            if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                e.window.windowID == SDL_GetWindowID(window))
                running = false;
        }
        tick_psx_bind_capture_poll(hub);
        if (hub.request_exit.load()) running = false;

        hub.apply_pending_folder_pick();
        hub.apply_pending_file_pick();

        if (hub.pending_startup_update_check && !hub.job_running.load()) {
            hub.pending_startup_update_check = false;
            if (hub.cfg.check_updates_on_startup) hub.start_job(HubJob::CheckUpdates);
        }
        // Legacy pending_launch (CheckLaunchUpdate now launches on its own worker).
        if (!hub.launch_running.load()) {
            std::string launch_id;
            {
                std::lock_guard<std::mutex> lock(hub.mu);
                launch_id = std::move(hub.pending_launch_title_id);
                hub.pending_launch_title_id.clear();
            }
            if (!launch_id.empty()) hub.start_job(HubJob::Launch, launch_id);
        }
        // Drain Install/Update queue when the main worker is free.
        if (!hub.job_running.load() && hub.queued_job_count() > 0)
            hub.start_next_queued_job();
        // After the queue drains, prune shared caches out-of-process (skipped
        // during each build so multi-title updates do not OOM the hub).
        if (!hub.job_running.load() && hub.queued_job_count() == 0)
            hub.maybe_run_deferred_cache_gc();

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
        } else if (hub.show_psx_settings) {
            ImGui::BeginChild("psx_settings_host", ImVec2(0, 0), ImGuiChildFlags_None);
            draw_psx_settings_panel(hub, th, boxart);
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
            draw_log(hub, th, log_h, window);
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
                // Bottom-right, above the collapsed Activity bar.
                const ImVec2 pos(tvp->WorkPos.x + tvp->WorkSize.x - sz.x - 16.f,
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
            const bool move_only = hub.install_root_prompt_move;
            const fs::path& from_apps = hub.install_root_prompt_from_apps;
            const int current_idx = retcomm::find_install_root_index(roots, from_apps);
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 420.f);
            ImGui::TextWrapped("%s", prow ? prow->name.c_str() : tid.c_str());
            ImGui::Dummy(ImVec2(0, 6));
            if (move_only) {
                ImGui::TextWrapped(
                    "Choose where to move this game's install data (releases, build files, "
                    "and preserved saves/config).");
                if (!from_apps.empty()) {
                    ImGui::Dummy(ImVec2(0, 4));
                    ImGui::TextColored(th.text_muted, "Currently installed in:");
                    if (current_idx >= 0) {
                        const auto& cur = roots[static_cast<size_t>(current_idx)];
                        ImGui::TextWrapped("%s — %s",
                                           cur.label.empty() ? "Install" : cur.label.c_str(),
                                           cur.path.string().c_str());
                    } else {
                        ImGui::TextWrapped("%s", from_apps.string().c_str());
                    }
                }
            } else if (prow && (prow->installed || prow->install_dir_present ||
                                prow->has_preserved_state)) {
                ImGui::TextWrapped(
                    "Choose where to install this game. Picking a different location moves "
                    "existing install / preserved data there first.");
            } else {
                ImGui::TextWrapped("Choose where to install this game.");
            }
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 8));
            if (roots.empty()) {
                ImGui::TextColored(th.warn, "No install locations configured.");
            } else {
                for (int i = 0; i < static_cast<int>(roots.size()); ++i) {
                    ImGui::PushID(i);
                    const auto& e = roots[static_cast<size_t>(i)];
                    const bool is_current =
                        current_idx == i ||
                        retcomm::same_install_root_path(e.path, from_apps);
                    char label[1152];
                    std::snprintf(label, sizeof(label), "%s%s\n%s",
                                  e.label.empty() ? "Install" : e.label.c_str(),
                                  is_current ? "  (current)" : "", e.path.string().c_str());
                    if (ImGui::RadioButton(label, &hub.install_root_prompt_index, i)) {
                        // index updated by RadioButton
                    }
                    ImGui::PopID();
                }
            }
            ImGui::Dummy(ImVec2(0, 10));
            const bool can_confirm = !tid.empty() && !roots.empty();
            const bool job_busy = hub.job_running.load();
            const HubJob confirm_job = move_only ? HubJob::MoveInstall : HubJob::Install;
            const bool already_queued = hub.is_job_queued(confirm_job, tid);
            const bool job_active =
                job_busy && hub.job == confirm_job && hub.job_title_id == tid;
            const bool same_as_current =
                move_only && can_confirm && hub.install_root_prompt_index >= 0 &&
                hub.install_root_prompt_index < static_cast<int>(roots.size()) &&
                retcomm::same_install_root_path(
                    roots[static_cast<size_t>(hub.install_root_prompt_index)].path, from_apps);
            const char* confirm_lbl =
                job_active      ? (move_only ? "Moving…" : "Installing…")
                : already_queued ? "Queued"
                : same_as_current ? "Already here"
                : job_busy ? (move_only ? "Queue Move" : "Queue Install")
                           : "Confirm";
            ImGui::BeginDisabled(!can_confirm || job_active || already_queued ||
                                 same_as_current);
            if (good_button(confirm_lbl, th, ImVec2(-1, 0))) {
                hub.confirm_install_root_and_continue();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            if (same_as_current && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled |
                                                        ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip("Pick a different location to move this install.");
            }
            if (ImGui::Button("Cancel", ImVec2(-1, 0))) {
                hub.install_root_prompt_id.clear();
                hub.install_root_prompt_move = false;
                hub.install_root_prompt_from_apps.clear();
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
            // Always offer the same actions; RomM download is disabled until sync is set up.
            const bool romm_sync_ok = prow && prow->romm_ready;
            const bool romm_download_ok =
                romm_sync_ok && prow->has_rom_identity && !tid.empty();
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 420.f);
            ImGui::TextWrapped("%s", prow ? prow->name.c_str() : tid.c_str());
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextWrapped(
                "No verified ROM is in your library yet. Rescan %s for a matching dump, "
                "import the files, or download from RomM when sync is configured "
                "(multi-track discs need the full .cue + track set).",
                plat_label);
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 10));
            const bool job_busy = hub.job_running.load();
            const bool any_scan_queued = hub.has_queued_scan();
            const bool scan_active = job_busy && hub.job == HubJob::ScanRoms &&
                                     hub.job_platform_filter == plat;
            const bool scan_queued = hub.is_job_queued(HubJob::ScanRoms, {}, plat);
            char scan_label[96];
            if (scan_active)
                std::snprintf(scan_label, sizeof(scan_label), "Scanning…");
            else if (scan_queued)
                std::snprintf(scan_label, sizeof(scan_label), "Queued");
            else if (job_busy && !any_scan_queued)
                std::snprintf(scan_label, sizeof(scan_label), "Queue Rescan Library");
            else
                std::snprintf(scan_label, sizeof(scan_label), "Rescan Library");
            ImGui::BeginDisabled(tid.empty() || plat.empty() || scan_active || scan_queued ||
                                 any_scan_queued);
            if (good_button(scan_label, th, ImVec2(-1, 0))) {
                hub.scans_platform_filter = plat;
                hub.pending_scan_missing_rom_id = tid;
                hub.start_job(HubJob::ScanRoms);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            bool file_busy = false;
            {
                std::lock_guard<std::mutex> lock(hub.file_pick_mu);
                file_busy = hub.file_pick_busy;
            }
            ImGui::BeginDisabled(plat.empty() || file_busy || job_busy);
            if (ImGui::Button("Import ROM", ImVec2(-1, 0))) {
                const auto exts = rom_exts_for_platform(hub.catalog, plat);
                begin_file_pick(hub, window, retcomm::hub::FilePickKind::ImportRom, plat,
                                "ROM files", exts, /*allow_many=*/true, tid);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            const bool inst_active =
                job_busy && hub.job == HubJob::Install && hub.job_title_id == tid;
            const bool inst_queued = hub.is_job_queued(HubJob::Install, tid);
            const char* romm_lbl = inst_active  ? "Installing…"
                                   : inst_queued ? "Queued"
                                   : job_busy    ? "Queue Download from RomM"
                                                 : "Download from RomM";
            ImGui::BeginDisabled(!romm_download_ok || inst_active || inst_queued);
            if (romm_button(romm_lbl, th, ImVec2(-1, 0))) {
                hub.start_job(HubJob::Install, tid, false, true);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            if (!romm_download_ok &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled |
                                    ImGuiHoveredFlags_DelayNormal)) {
                if (!romm_sync_ok)
                    ImGui::SetTooltip("Configure RomM Sync Settings to enable download.");
                else
                    ImGui::SetTooltip("This title has no catalog ROM identity for RomM match.");
            }
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
            if (plat.empty() && prow) plat = prow->platform;
            const char* plat_label = plat.empty() ? "this platform" : platform_display_name(plat);
            const bool romm_sync_ok = prow && prow->romm_ready;
            const bool romm_download_ok =
                romm_sync_ok && prow->has_rom_identity && !tid.empty();
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 420.f);
            ImGui::TextWrapped("%s", prow ? prow->name.c_str() : tid.c_str());
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextWrapped(
                "Still no verified dump for this title under %s. Rescan again, import the "
                "files, or download from RomM when sync is configured.",
                plat_label);
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 10));
            const bool job_busy = hub.job_running.load();
            const bool any_scan_queued = hub.has_queued_scan();
            const bool scan_active = job_busy && hub.job == HubJob::ScanRoms &&
                                     hub.job_platform_filter == plat;
            const bool scan_queued = hub.is_job_queued(HubJob::ScanRoms, {}, plat);
            char scan_label[96];
            if (scan_active)
                std::snprintf(scan_label, sizeof(scan_label), "Scanning…");
            else if (scan_queued)
                std::snprintf(scan_label, sizeof(scan_label), "Queued");
            else if (job_busy && !any_scan_queued)
                std::snprintf(scan_label, sizeof(scan_label), "Queue Rescan Library");
            else
                std::snprintf(scan_label, sizeof(scan_label), "Rescan Library");
            ImGui::BeginDisabled(tid.empty() || plat.empty() || scan_active || scan_queued ||
                                 any_scan_queued);
            if (good_button(scan_label, th, ImVec2(-1, 0))) {
                hub.scans_platform_filter = plat;
                hub.pending_scan_missing_rom_id = tid;
                hub.start_job(HubJob::ScanRoms);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            bool file_busy = false;
            {
                std::lock_guard<std::mutex> lock(hub.file_pick_mu);
                file_busy = hub.file_pick_busy;
            }
            ImGui::BeginDisabled(plat.empty() || file_busy || job_busy);
            if (ImGui::Button("Import ROM", ImVec2(-1, 0))) {
                const auto exts = rom_exts_for_platform(hub.catalog, plat);
                begin_file_pick(hub, window, retcomm::hub::FilePickKind::ImportRom, plat,
                                "ROM files", exts, /*allow_many=*/true, tid);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            const bool inst_active =
                job_busy && hub.job == HubJob::Install && hub.job_title_id == tid;
            const bool inst_queued = hub.is_job_queued(HubJob::Install, tid);
            const char* romm_lbl = inst_active  ? "Installing…"
                                   : inst_queued ? "Queued"
                                   : job_busy    ? "Queue Download from RomM"
                                                 : "Download from RomM";
            ImGui::BeginDisabled(!romm_download_ok || inst_active || inst_queued);
            if (romm_button(romm_lbl, th, ImVec2(-1, 0))) {
                hub.start_job(HubJob::Install, tid, false, true);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            if (!romm_download_ok &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled |
                                    ImGuiHoveredFlags_DelayNormal)) {
                if (!romm_sync_ok)
                    ImGui::SetTooltip("Configure RomM Sync Settings to enable download.");
                else
                    ImGui::SetTooltip("This title has no catalog ROM identity for RomM match.");
            }
            if (ImGui::Button("Cancel", ImVec2(-1, 0))) ImGui::CloseCurrentPopup();
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
            const bool job_busy = hub.job_running.load();
            const bool launch_busy = hub.launch_running.load();
            // Update needs the main worker — disabled while Install/etc. runs.
            ImGui::BeginDisabled(job_busy || launch_busy || tid.empty());
            if (good_button("Update", th, ImVec2(-1, 0))) {
                hub.start_job(HubJob::Update, tid);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            if (job_busy && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Wait for the current install/job to finish, or play "
                                  "without updating.");
            }
            // Play uses launch_worker — OK while another title installs.
            ImGui::BeginDisabled(launch_busy || tid.empty());
            if (ImGui::Button("Play Without Updating", ImVec2(-1, 0))) {
                hub.start_job(HubJob::Launch, tid);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            if (ImGui::Button("Cancel", ImVec2(-1, 0))) ImGui::CloseCurrentPopup();
            close_modal_on_outside_click();
            ImGui::EndPopup();
        }

        // Update prompts in order: launcher → toolchain → games.
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
        // Outside-click dismiss is "Later" — release deferred toolchain/game prompts.
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

        const bool open_toolchain_update =
            !launcher_blocking && hub.toolchain_prompt_pending.exchange(false);
        if (open_toolchain_update) ImGui::OpenPopup("Toolchain update###toolchain_update");
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

        const bool toolchain_blocking =
            open_toolchain_update || ImGui::IsPopupOpen("Toolchain update###toolchain_update") ||
            hub.toolchain_prompt_pending.load() ||
            (hub.job_running.load() && hub.job == HubJob::UpdateToolchain);

        const bool open_game_updates =
            !launcher_blocking && !toolchain_blocking &&
            hub.game_updates_prompt_pending.exchange(false);
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
                    "Queue it to download/install when the worker is free, or dismiss "
                    "for now.");
            } else {
                ImGui::TextWrapped(
                    "%d installed games have updates available.\n\n"
                    "Queue all updates to run one after another, or dismiss for now.",
                    n);
            }
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 10));
            if (good_button("Queue All Updates", th, ImVec2(-1, 0))) {
                hub.queue_all_updates();
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::Button("Not Right Now", ImVec2(-1, 0))) ImGui::CloseCurrentPopup();
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

    // Self-update / hard-reset apply scripts wait on this PID. Prefer a fast
    // exit over a graceful join that can hang on prefetch/launch workers and
    // leave the apply bat stuck until timeout.
    if (hub.request_exit.load()) {
        hub.cancel_prefetch_updates();
        if (hub.prefetch_worker.joinable()) hub.prefetch_worker.detach();
        if (hub.worker.joinable()) hub.worker.detach();
        if (hub.launch_worker.joinable()) hub.launch_worker.detach();
        hub_close_all_gamepads();
        SDL_Quit();
#if defined(_WIN32)
        ::ExitProcess(0);
#else
        std::_Exit(0);
#endif
    }

    hub.join_worker();
    boxart.destroy_all();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    hub_close_all_gamepads();
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
