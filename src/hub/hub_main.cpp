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

void draw_hub_menu(HubModel& hub, const Theme& th) {
    const bool busy = hub.job_running.load();
    // Settings stay reachable during Build & Install / scans.
    if (ImGui::MenuItem("Library Settings")) hub.open_settings();
    if (ImGui::MenuItem("RomM Sync Settings")) hub.open_romm_settings();
    if (ImGui::MenuItem("Scans")) hub.pending_open_scans = true;
    ImGui::Separator();
    ImGui::BeginDisabled(busy);
    if (ImGui::MenuItem("Check Updates")) hub.start_job(HubJob::CheckUpdates);
    if (ImGui::MenuItem("Refresh Catalog")) hub.start_job(HubJob::RefreshCatalog);
    if (ImGui::MenuItem("Refresh Library")) {
        hub.refresh_rows(false);
        hub.set_status("Library refreshed");
    }
    if (ImGui::MenuItem("Clean Unlisted Installs…")) {
        const size_t n = hub.refresh_orphan_installs();
        if (n == 0) {
            hub.set_status("No installs outside the catalog");
            hub.append_log("Orphan scan: none");
        } else {
            hub.orphan_prompt_pending.store(true);
        }
    }
    ImGui::Separator();
    {
        const retcomm::RetcommInstallInfo install = retcomm::retcomm_install_info();
        ImGui::BeginDisabled(!install.self_update_supported);
        if (ImGui::MenuItem("Update RetComM")) hub.start_job(HubJob::SelfUpdate);
        ImGui::EndDisabled();
    }
    {
        bool tc_upd = false;
        std::string tc_line;
        {
            std::lock_guard<std::mutex> lock(hub.mu);
            tc_upd = hub.toolchain_update_available;
            tc_line = hub.toolchain_status;
            if (tc_line.empty() && !hub.toolchain_current_version.empty())
                tc_line = "Toolchain " + hub.toolchain_current_version;
        }
        if (ImGui::MenuItem(tc_upd ? "Update Toolchain (available)" : "Update Toolchain"))
            hub.start_job(HubJob::UpdateToolchain);
        ImGui::Separator();
        {
            // Binary compile version is authoritative (not launcher.json).
            const std::string ver = retcomm::retcomm_app_version();
            const retcomm::RetcommInstallInfo install = retcomm::retcomm_install_info();
            ImGui::TextColored(th.text_muted, "Launcher %s", ver.c_str());
            if (install.self_update_supported && !install.channel_id.empty())
                ImGui::TextColored(th.text_muted, "Channel %s", install.channel_id.c_str());
            if (!tc_line.empty()) ImGui::TextColored(th.text_muted, "%s", tc_line.c_str());
        }
    }
    ImGui::EndDisabled();
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
    // Neon underline
    dl->AddRectFilledMultiColor(ImVec2(p0.x, p0.y + h - 3), ImVec2(p0.x + width, p0.y + h),
                                ImGui::ColorConvertFloat4ToU32(th.accent),
                                ImGui::ColorConvertFloat4ToU32(th.focus),
                                ImGui::ColorConvertFloat4ToU32(th.focus),
                                ImGui::ColorConvertFloat4ToU32(th.accent));

    ImGui::Dummy(ImVec2(width, h));
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 20.f, p0.y + 14.f));
    ImGui::PushStyleColor(ImGuiCol_Text, th.accent);
    ImGui::TextUnformatted("RetComM");
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 20.f, p0.y + 40.f));
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextUnformatted("Retro Compilation Manager");
    ImGui::PopStyleColor();

    // Top-right: Menu popup, or a one-click Back when editing settings.
    constexpr float kMenuH = 36.f;
    const bool in_settings = hub.show_settings || hub.show_romm_settings;
    const char* btn_label = in_settings ? "Back to Library" : "Menu";
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.f, 8.f));
    const float btn_w =
        std::max(88.f, ImGui::CalcTextSize(btn_label).x + ImGui::GetStyle().FramePadding.x * 2.f);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + width - btn_w - 16.f, p0.y + (h - kMenuH) * 0.5f));
    if (ImGui::Button(btn_label, ImVec2(btn_w, kMenuH))) {
        if (in_settings) {
            hub.show_settings = false;
            hub.show_romm_settings = false;
            hub.settings.dirty = false;
            hub.romm_settings.dirty = false;
        } else {
            ImGui::OpenPopup("##hub_menu");
        }
    }
    ImGui::PopStyleVar();
    if (!in_settings && ImGui::BeginPopup("##hub_menu")) {
        draw_hub_menu(hub, th);
        ImGui::EndPopup();
    }

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
                     bool dim_art = false) {
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

    if (badge_col) {
        const float r = 7.f;
        const ImVec2 c(art1.x - 12.f, art0.y + 12.f);
        dl->AddCircleFilled(c, r, ImGui::ColorConvertFloat4ToU32(*badge_col), 16);
        dl->AddCircle(c, r, ImGui::ColorConvertFloat4ToU32(th.background), 16, 1.5f);
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
            const bool dim_uninstalled = !r.installed;
            const BoxartTexture* tex =
                r.boxart_path.empty() ? nullptr : boxart.get(r.id, r.boxart_path);
            const ImVec4 badge = chip_color(r, th);
            const float tile_h =
                draw_grid_tile(tile_min, tile_w, selected, th, tex, aspect, r.name.c_str(),
                               nullptr, &badge, title_busy, dim_uninstalled);

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

void draw_detail_save_controls(HubModel& hub, const TitleRow& row, const Theme& th) {
    // Match spacing used below Create Save before Manage Data / RomM Sync.
    constexpr float kSaveBtnPad = 12.f;

    if (row.save_labels.empty()) {
        ImGui::TextColored(th.text_muted, "Save file");
        ImGui::TextColored(th.text_muted,
                           "None yet — Create Save, Play (auto-creates), or RomM Sync");
        ImGui::Dummy(ImVec2(0, kSaveBtnPad));
        if (ImGui::Button("Create Save", ImVec2(-1, 0))) {
            std::string err;
            if (!hub.create_title_save(row.id, &err))
                hub.append_log("Create Save failed: " + err);
        }
        return;
    }

    if (row.dual_memcard) {
        auto draw_memcard_combo = [&](const char* combo_id, int index, bool is_card2) {
            const char* preview = "(Blank card)";
            if (index >= 0 && index < static_cast<int>(row.save_labels.size()))
                preview = row.save_labels[static_cast<size_t>(index)].c_str();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo(combo_id, preview)) {
                if (is_card2) {
                    const bool blank_sel = index < 0;
                    if (ImGui::Selectable("(Blank card)", blank_sel)) {
                        std::string err;
                        if (!hub.set_title_preferred_save_card2(row.id, retcomm::kBlankMemcardId,
                                                                &err))
                            hub.append_log("Could not save preference: " + err);
                    }
                    if (blank_sel) ImGui::SetItemDefaultFocus();
                }
                for (size_t i = 0; i < row.save_labels.size(); ++i) {
                    const bool selected = static_cast<int>(i) == index;
                    if (ImGui::Selectable(row.save_labels[i].c_str(), selected)) {
                        std::string err;
                        const bool ok =
                            is_card2 ? hub.set_title_preferred_save_card2(row.id, row.save_ids[i],
                                                                          &err)
                                     : hub.set_title_preferred_save(row.id, row.save_ids[i], &err);
                        if (!ok) hub.append_log("Could not save preference: " + err);
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        };
        ImGui::TextColored(th.text_muted, "Memcard 1 & 2");
        draw_memcard_combo("##memcard1", row.preferred_save_index, false);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 2.f));
        draw_memcard_combo("##memcard2", row.preferred_save_card2_index, true);
        ImGui::PopStyleVar();
        ImGui::Dummy(ImVec2(0, kSaveBtnPad));
        if (ImGui::Button("Create Save", ImVec2(-1, 0))) {
            std::string err;
            if (!hub.create_title_save(row.id, &err))
                hub.append_log("Create Save failed: " + err);
        }
        return;
    }

    ImGui::TextColored(th.text_muted, "Save file");
    const char* preview =
        (row.preferred_save_index >= 0 &&
         row.preferred_save_index < static_cast<int>(row.save_labels.size()))
            ? row.save_labels[static_cast<size_t>(row.preferred_save_index)].c_str()
            : row.save_labels.front().c_str();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##save_file", preview)) {
        for (size_t i = 0; i < row.save_labels.size(); ++i) {
            const bool selected = static_cast<int>(i) == row.preferred_save_index;
            if (ImGui::Selectable(row.save_labels[i].c_str(), selected)) {
                std::string err;
                if (!hub.set_title_preferred_save(row.id, row.save_ids[i], &err))
                    hub.append_log("Could not save preference: " + err);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::Dummy(ImVec2(0, kSaveBtnPad));
    if (ImGui::Button("Create Save", ImVec2(-1, 0))) {
        std::string err;
        if (!hub.create_title_save(row.id, &err))
            hub.append_log("Create Save failed: " + err);
    }
}

void draw_detail_manage_data_popup(HubModel& hub, const TitleRow& row, const Theme& th,
                                   bool busy) {
    center_modal_next();
    ImGui::SetNextWindowSize(ImVec2(420.f, 0.f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Manage Data###detail_manage_data", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 380.f);
    ImGui::TextWrapped("%s", row.name.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    ImGui::BeginDisabled(busy);

    const bool can_open =
        row.installed || row.install_dir_present || row.has_preserved_state;
    if (can_open && ImGui::Button("Open Folder", ImVec2(-1, 0))) {
        std::string err;
        if (!retcomm::open_path_in_file_manager(row.install_root, &err))
            hub.append_log("Open Folder failed: " + err);
    }

    if (row.installed) {
        if (ImGui::Button("Uninstall (keep saves)", ImVec2(-1, 0))) {
            hub.start_job(HubJob::Uninstall, row.id);
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::Button("Uninstall + delete saves", ImVec2(-1, 0))) {
            hub.start_job(HubJob::UninstallPurge, row.id);
            ImGui::CloseCurrentPopup();
        }
    } else if (row.install_dir_present) {
        if (ImGui::Button("Clean install folder (keep saves)", ImVec2(-1, 0))) {
            hub.start_job(HubJob::Uninstall, row.id);
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::Button("Clean install folder + delete saves", ImVec2(-1, 0))) {
            hub.start_job(HubJob::UninstallPurge, row.id);
            ImGui::CloseCurrentPopup();
        }
    } else if (row.has_preserved_state) {
        if (ImGui::Button("Delete preserved data", ImVec2(-1, 0))) {
            hub.start_job(HubJob::UninstallPurge, row.id);
            ImGui::CloseCurrentPopup();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Removes the entire apps folder for this title, including preserved "
            "saves and config.");
        ImGui::PopStyleColor();
    } else {
        ImGui::TextColored(th.text_muted, "No install folder or preserved data yet.");
    }

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextColored(th.text_muted, "Paths");
    if (!row.install_root.empty())
        ImGui::TextWrapped("install: %s", row.install_root.c_str());
    if (!row.binary_path.empty())
        ImGui::TextWrapped("binary:  %s", row.binary_path.c_str());
    if (row.install_root.empty() && row.binary_path.empty())
        ImGui::TextColored(th.text_muted, "(no install paths yet)");

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextColored(th.text_muted, "ROM / disc");
    if (row.has_rom)
        ImGui::TextWrapped("%s", row.rom_path.c_str());
    else if (row.has_romm) {
        ImGui::TextColored(th.accent, "Available on RomM — use RomM Sync to download");
        if (!row.romm_file_name.empty()) ImGui::TextWrapped("%s", row.romm_file_name.c_str());
    } else {
        ImGui::TextColored(th.warn, "No library match");
        if (!row.suggested_rom.empty()) {
            ImGui::TextColored(th.text_muted, "Looking for:");
            ImGui::TextWrapped("%s", row.suggested_rom.c_str());
        }
    }

    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, 8));
    if (ImGui::Button("Close", ImVec2(-1, 0))) ImGui::CloseCurrentPopup();
    close_modal_on_outside_click();
    ImGui::EndPopup();
}

void draw_detail_romm_sync_popup(HubModel& hub, const TitleRow& row, const Theme& th, bool busy) {
    center_modal_next();
    ImGui::SetNextWindowSize(ImVec2(440.f, 0.f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("RomM Sync###detail_romm_sync", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 400.f);
    ImGui::TextWrapped("%s", row.name.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    if (!row.romm_ready) {
        ImGui::TextColored(th.warn, "RomM is not configured.");
        ImGui::TextColored(th.text_muted, "Open Menu → RomM Sync Settings to add a URL and API key.");
        if (ImGui::Button("Close", ImVec2(-1, 0))) ImGui::CloseCurrentPopup();
        close_modal_on_outside_click();
        ImGui::EndPopup();
        return;
    }

    ImGui::BeginDisabled(busy);
    if (row.has_rom_identity) {
        const char* rom_label =
            row.has_rom ? "Re-download ROM from RomM" : "Download ROM from RomM";
        if (ImGui::Button(rom_label, ImVec2(-1, 0))) {
            hub.start_job(HubJob::FetchRommRom, row.id);
            ImGui::CloseCurrentPopup();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Matches catalog hashes against your RomM library, then saves into the "
            "configured ROM library folder.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 4));
    }
    if (row.needs_bios) {
        const char* bios_label =
            row.has_bios ? "Re-download BIOS from RomM" : "Download BIOS from RomM";
        if (ImGui::Button(bios_label, ImVec2(-1, 0))) {
            hub.start_job(HubJob::FetchRommBios, row.id);
            ImGui::CloseCurrentPopup();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Matches catalog BIOS identity against RomM firmware and saves into the "
            "BIOS root.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 4));
    }
    if (row.installed) {
        if (ImGui::Button("Sync Game Saves", ImVec2(-1, 0))) {
            hub.start_job(HubJob::SyncRommSaves, row.id);
            ImGui::CloseCurrentPopup();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Promotes install saves into the save library, then bidirectional sync with "
            "RomM. Newer file wins; identical hashes are skipped.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 4));

        if (ImGui::Button("Sync Savestates", ImVec2(-1, 0))) {
            hub.start_job(HubJob::SyncRommStates, row.id);
            ImGui::CloseCurrentPopup();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Bidirectional sync of savestates with RomM. Newer file wins. Recomp and "
            "emulator savestates are often incompatible — sync does not convert formats.");
        ImGui::PopStyleColor();
    }
    if (!row.has_rom_identity && !row.needs_bios && !row.installed) {
        ImGui::TextColored(th.text_muted, "No RomM actions available for this title yet.");
    }
    ImGui::EndDisabled();

    ImGui::Dummy(ImVec2(0, 8));
    if (ImGui::Button("Close", ImVec2(-1, 0))) ImGui::CloseCurrentPopup();
    close_modal_on_outside_click();
    ImGui::EndPopup();
}

void draw_scans_popup(HubModel& hub, const Theme& th) {
    if (hub.pending_open_scans) {
        ImGui::OpenPopup("Scans###scan_panel");
        hub.pending_open_scans = false;
    }

    // Only set next-window pos/size when this popup will actually begin — otherwise
    // SetNextWindow* leaks onto the following modal in the frame.
    if (!ImGui::IsPopupOpen("Scans###scan_panel")) return;
    center_modal_next();
    ImGui::SetNextWindowSize(ImVec2(440.f, 0.f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Scans###scan_panel", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextUnformatted("SCANS");
    ImGui::PopStyleColor();
    ImGui::TextWrapped(
        "Scan uses the saved library/BIOS roots and RomM settings. Save path changes in "
        "Library Settings or RomM Sync Settings before scanning. Normal scan reuses the "
        "index; full rescan clears caches and re-hashes everything.");
    ImGui::Separator();

    const bool busy = hub.job_running.load();

    ImGui::TextColored(th.text_muted, "Library");
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Scan ROMs", ImVec2(-1, 0))) hub.start_job(HubJob::ScanRoms);
    if (ImGui::Button("Full Rescan ROMs", ImVec2(-1, 0)))
        ImGui::OpenPopup("Full ROM rescan###confirm_full_rom_rescan");
    if (ImGui::Button("Scan BIOS", ImVec2(-1, 0))) hub.start_job(HubJob::ScanBios);
    if (ImGui::Button("Full Rescan BIOS", ImVec2(-1, 0)))
        ImGui::OpenPopup("Full BIOS rescan###confirm_full_bios_rescan");
    ImGui::EndDisabled();

    ImGui::Dummy(ImVec2(0, 10));
    ImGui::Separator();
    ImGui::TextColored(th.text_muted, "RomM");
    {
        const bool can_scan =
            !busy && hub.cfg.romm.enabled() && !hub.cfg.romm.api_token.empty();
        ImGui::BeginDisabled(!can_scan);
        if (ImGui::Button("Scan RomM library", ImVec2(-1, 0)))
            hub.start_job(HubJob::ScanRommRoms);
        ImGui::EndDisabled();
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        if (!hub.cfg.romm.enabled() || hub.cfg.romm.api_token.empty())
            ImGui::TextWrapped("RomM not configured — set URL + API key in RomM Sync Settings.");
        else
            ImGui::TextWrapped(
                "Match catalog rom_identity against your RomM library (ON ROMM chip).");
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 6));
        const bool can_resync = can_scan && hub.cfg.romm.sync_boxart;
        ImGui::BeginDisabled(!can_resync);
        if (ImGui::Button("Resync RomM boxart", ImVec2(-1, 0)))
            hub.start_job(HubJob::FetchBoxart, {}, true);
        ImGui::EndDisabled();
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        if (!hub.cfg.romm.sync_boxart)
            ImGui::TextWrapped("Enable Sync Boxart in RomM Sync Settings to refresh covers.");
        else
            ImGui::TextWrapped(
                "Deletes cached RomM covers and re-downloads each title's menu cover.");
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, 10));
    if (ImGui::Button("Close", ImVec2(-1, 0))) ImGui::CloseCurrentPopup();

    // Nested confirms (must be inside the Scans modal's ID stack).
    if (ImGui::BeginPopupModal("Full ROM rescan###confirm_full_rom_rescan", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 360.f);
        ImGui::TextWrapped(
            "This clears the library index cache and re-hashes every candidate ROM under "
            "your library root. On a large collection it can take a long time.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 8));
        if (accent_button("Rescan", th, ImVec2(120, 0))) {
            hub.start_job(HubJob::FullScanRoms);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        close_modal_on_outside_click();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Full BIOS rescan###confirm_full_bios_rescan", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 360.f);
        ImGui::TextWrapped(
            "This clears the BIOS index cache and re-hashes every candidate dump under "
            "your BIOS root.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 8));
        if (accent_button("Rescan", th, ImVec2(120, 0))) {
            hub.start_job(HubJob::FullScanBios);
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

void draw_detail(HubModel& hub, BoxartCache& boxart, const Theme& th) {
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
    const bool has_native_install = row.supports_local_build || row.can_prebuilt_install;
    if (row.installed) {
        // Play stays available during Build & Install / scans (own worker thread).
        ImGui::BeginDisabled(hub.launch_running.load());
        if (good_button("Play", th, ImVec2(btn_w, 0)))
            hub.start_job(HubJob::Launch, row.id);
        ImGui::EndDisabled();
        ImGui::SameLine(0, 8);
        ImGui::BeginDisabled(block_title_mutate);
        if (ImGui::Button("Update", ImVec2(btn_w, 0))) hub.start_job(HubJob::Update, row.id);
        ImGui::EndDisabled();
    } else if (row.supports_local_build) {
        if (!row.has_rom) {
            ImGui::PushStyleColor(ImGuiCol_Text, th.warn);
            if (row.romm_ready && row.has_rom_identity) {
                ImGui::TextWrapped(
                    "No verified .cue / ROM in your library yet. Build & Install can search "
                    "RomM and download the matched dump first.");
            } else if (row.has_rom_identity) {
                ImGui::TextWrapped(
                    "Match a verified .cue / ROM in your library, or configure RomM Sync "
                    "to download it.");
            } else {
                ImGui::TextWrapped(
                    "Match a verified .cue / ROM in your library before Build & Install.");
            }
            ImGui::PopStyleColor();
        }
        ImGui::BeginDisabled(block_title_mutate ||
                             (!row.has_rom && !(row.romm_ready && row.has_rom_identity)));
        if (accent_button(row.install_dir_present ? "Retry Build & Install" : "Build & Install", th,
                          ImVec2(-1, 0))) {
            if (!row.has_rom && row.romm_ready && row.has_rom_identity) {
                hub.romm_install_prompt_id = row.id;
                hub.show_romm_install_prompt = true;
            } else {
                hub.start_job(HubJob::Install, row.id);
            }
        }
        // Wine only when there is no native release path for this OS.
        if (row.can_wine_install && !has_native_install) {
            if (ImGui::Button("Install with WINE", ImVec2(-1, 0)))
                hub.start_job(HubJob::InstallWine, row.id);
        }
        ImGui::EndDisabled();
    } else {
        ImGui::BeginDisabled(block_title_mutate);
        if (accent_button(row.install_dir_present ? "Retry Install" : "Install", th, ImVec2(-1, 0)))
            hub.start_job(HubJob::Install, row.id);
        if (row.can_wine_install && !has_native_install) {
            if (ImGui::Button("Install with WINE", ImVec2(-1, 0)))
                hub.start_job(HubJob::InstallWine, row.id);
        }
        ImGui::EndDisabled();
    }

    // Saves / memcards sit under Play; ROM paths live in Manage Data.
    if (row.installed) {
        ImGui::Dummy(ImVec2(0, 8));
        draw_detail_save_controls(hub, row, th);
    }

    // Folder / uninstall and RomM actions live in centered modals.
    ImGui::Dummy(ImVec2(0, 12));
    const bool show_romm = row.romm_ready;
    const float gap = show_romm ? 8.f : 0.f;
    const float manage_w =
        show_romm ? (ImGui::GetContentRegionAvail().x - gap) * 0.5f : -1.f;
    ImGui::BeginDisabled(install_op);
    if (ImGui::Button("Manage Data", ImVec2(manage_w, 0)))
        ImGui::OpenPopup("Manage Data###detail_manage_data");
    ImGui::EndDisabled();
    if (show_romm) {
        ImGui::SameLine(0, gap);
        ImGui::BeginDisabled(block_title_mutate);
        if (accent_button("RomM Sync", th, ImVec2(manage_w, 0)))
            ImGui::OpenPopup("RomM Sync###detail_romm_sync");
        ImGui::EndDisabled();
    }

    draw_detail_manage_data_popup(hub, row, th, block_title_mutate);
    if (show_romm) draw_detail_romm_sync_popup(hub, row, th, block_title_mutate);

    // Seldom-touched: BIOS just above author. ROM / disc paths live in Manage Data.
    if (row.needs_bios || row.supports_openbios) {
        ImGui::Dummy(ImVec2(0, 16));
        ImGui::TextColored(th.text_muted, "BIOS");
        if (!row.bios_choice_ids.empty()) {
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
                                   "OpenBIOS regenerates at Build & Install (no dump needed).");
            }
        } else if (row.has_bios) {
            ImGui::TextColored(th.good, "BIOS matched");
        } else {
            ImGui::TextColored(th.warn, "Missing — run Scan BIOS");
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
        ImGui::TextColored(th.good, "Installed %s%s",
                           row.installed_tag.empty() ? "" : row.installed_tag.c_str(),
                           row.runtime == "wine" ? " (Wine)" : "");
        if (row.update_available)
            ImGui::TextColored(th.warn, "Update available: %s", row.latest_tag.c_str());
    } else if (row.install_dir_present) {
        ImGui::TextColored(th.warn, "Install folder present — launch binary not found");
        if (!row.install_issue.empty()) ImGui::TextWrapped("%s", row.install_issue.c_str());
        if (!row.expected_binary.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
            ImGui::TextWrapped(
                "Looking for executable \"%s\". Use Manage Data → Open Folder to fix "
                "setup, then Install again.",
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
    ImGui::TextWrapped("ROM and BIOS scans are under Menu → Scans.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 10));
    ImGui::Separator();
    if (ImGui::Checkbox("Prefer Local Filesystem Boxart", &hub.settings.prefer_local_boxart))
        hub.settings.dirty = true;
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "Off (default): covers come from Libretro, or RomM when Sync Boxart is enabled. "
        "On: use sibling/library images next to ROMs when present, otherwise fall back to "
        "the remote source.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 10));
    if (ImGui::Checkbox("Filter Unsupported Titles", &hub.settings.filter_unsupported_titles))
        hub.settings.dirty = true;
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "Hide catalog titles that do not have a ROM available — neither on your local ROM "
        "library path nor (when scanned) on RomM. Installed titles stay visible. Save, then "
        "run Scan RomM library under Menu → Scans so remote-only matches appear.");
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

    ImGui::Dummy(ImVec2(0, 10));
    ImGui::TextColored(th.text_muted, "Platform folders");
    ImGui::TextWrapped("Catalog platform slug → folder name(s) under the library root "
                       "(e.g. psx → ps, ps1).");
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

void draw_setup_wizard(HubModel& hub, const Theme& th, SDL_Window* window) {
    if (!hub.show_setup) return;

    ImGui::OpenPopup("Welcome to RetComM###setup_wizard");
    ImGui::SetNextWindowSize(ImVec2(560.f, 0.f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Welcome to RetComM###setup_wizard", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 520.f);
    ImGui::TextWrapped(
        "Set your ROM library, BIOS, and game-saves folders. Optionally connect RomM for "
        "library sync later. You can change all of this under Library Settings and RomM Sync "
        "Settings.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 12));

    if (path_field_with_browse("ROM library root", "##setup_library_root",
                               hub.settings.library_root, sizeof(hub.settings.library_root), hub,
                               window, FolderPickTarget::LibraryRoot, th))
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
    if (path_field_with_browse("Game saves root", "##setup_saves_root", hub.settings.saves_root,
                               sizeof(hub.settings.saves_root), hub, window,
                               FolderPickTarget::SavesRoot, th))
        hub.settings.dirty = true;
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "Recommended — shared SRAM / memcard library (e.g. …/saves). RomM sync and launches "
        "use per-platform folders under this root.");
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
    ImGui::BeginDisabled(!library_ok);
    if (accent_button("Continue", th, ImVec2(160, 0))) {
        std::string err;
        if (!hub.save_settings(&err)) {
            hub.append_log("setup save failed: " + err);
            hub.set_status("Setup save failed");
        } else if (!hub.save_romm_settings(&err, /*refresh_boxart=*/false)) {
            hub.append_log("setup RomM save failed: " + err);
            hub.set_status("Setup RomM save failed");
        } else if (!hub.complete_setup(&err)) {
            hub.append_log("setup marker failed: " + err);
            hub.set_status("Setup marker failed");
        } else {
            hub.set_status("Setup complete");
            hub.append_log("First-time setup saved");
            // One background job at a time — scan ROMs; user can Scan BIOS after.
            hub.start_job(HubJob::ScanRoms);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Skip for now", ImVec2(140, 0))) {
        std::string err;
        if (!hub.complete_setup(&err)) {
            hub.append_log("setup marker failed: " + err);
            hub.set_status("Setup marker failed");
        } else {
            hub.set_status("Setup skipped — set library root in Library settings");
            hub.append_log("First-time setup skipped");
        }
    }
    if (!library_ok) {
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::TextColored(th.warn, "Choose a ROM library folder to continue.");
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
        ImGui::TextWrapped(
            "On: all covers use RomM (requires URL + API key). Local filesystem art is "
            "ignored unless Prefer Local Filesystem Boxart is enabled in Library Settings.");
    else
        ImGui::TextWrapped(
            "Off (default): all covers use Libretro Named_Boxarts. Local filesystem art is "
            "ignored unless Prefer Local Filesystem Boxart is enabled in Library Settings.");
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
        "Scan RomM library and Resync RomM boxart are under Menu → Scans "
        "(requires a saved URL + API key; Sync Boxart for cover resync).");
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
        const float copy_w = ImGui::CalcTextSize("Copy").x + ImGui::GetStyle().FramePadding.x * 2.f;
        const float right = ImGui::GetWindowContentRegionMax().x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - copy_w));
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
    // Defer toolchain update check until the hub is idle (after catalog boxart job).
    hub.pending_startup_toolchain_check = true;

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

        if (hub.pending_startup_toolchain_check && !hub.job_running.load()) {
            hub.pending_startup_toolchain_check = false;
            hub.start_job(HubJob::CheckToolchainUpdate);
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

        // Body + splitter + activity must fit the remaining region exactly — ImGui adds
        // ItemSpacing between each, so subtract that or the outer window scrolls.
        static float log_h_pref = 140.f; // manual height; window grow restores up to this
        const float avail_y = ImGui::GetContentRegionAvail().y;
        constexpr float kSplitH = 6.f;
        constexpr float kMinLog = 64.f;
        constexpr float kMinBody = 160.f;
        const float gap_y = ImGui::GetStyle().ItemSpacing.y;
        const float chrome = kSplitH + gap_y * 2.f;
        const float max_log = std::max(kMinLog, avail_y - kMinBody - chrome);
        // Shrink with the window when needed; never auto-grow past the user's preference.
        float log_h = std::clamp(log_h_pref, kMinLog, max_log);
        const float body_h = std::max(kMinBody, avail_y - log_h - chrome);

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
            draw_detail(hub, boxart, th);
            ImGui::EndChild();
        }

        ImGui::EndChild(); // body
        draw_log_splitter(log_h, log_h_pref, avail_y, th);
        draw_log(hub, th, log_h);
        draw_setup_wizard(hub, th, window);
        draw_scans_popup(hub, th);

        if (hub.show_romm_install_prompt) {
            ImGui::OpenPopup("Download ROM from RomM###romm_install_prompt");
            hub.show_romm_install_prompt = false;
        }
        if (ImGui::BeginPopupModal("Download ROM from RomM###romm_install_prompt", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            const std::string tid = hub.romm_install_prompt_id;
            const TitleRow* prow = nullptr;
            for (const auto& r : hub.rows) {
                if (r.id == tid) {
                    prow = &r;
                    break;
                }
            }
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 420.f);
            ImGui::TextWrapped("%s", prow ? prow->name.c_str() : tid.c_str());
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextWrapped(
                "No verified ROM is in your library yet. Search RomM for a matching dump "
                "(multi-track discs download the full .cue + track set), then run Build & "
                "Install?");
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 10));
            ImGui::BeginDisabled(hub.job_running.load() || tid.empty());
            if (accent_button("Download from RomM & Build", th, ImVec2(-1, 0))) {
                hub.start_job(HubJob::Install, tid, false, true);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            if (ImGui::Button("Cancel", ImVec2(-1, 0))) ImGui::CloseCurrentPopup();
            close_modal_on_outside_click();
            ImGui::EndPopup();
        }

        if (hub.toolchain_prompt_pending.exchange(false))
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
