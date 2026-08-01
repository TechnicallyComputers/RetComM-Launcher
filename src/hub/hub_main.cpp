#include "hub/hub_boxart.hpp"
#include "hub/hub_model.hpp"
#include "hub/hub_theme.hpp"

#include "retcomm/catalog_sync.hpp"
#include "retcomm/config.hpp"
#include "retcomm/paths.hpp"
#include "retcomm/romm_saves.hpp"
#include "retcomm/self_update.hpp"

#include "imgui.h"
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

// Resolve Lato Latin (same face as recomp-ui). Prefer packaged locations, then
// walk up from the app base path for a source-tree assets/fonts (local builds).
fs::path find_hub_font_file(const char* filename) {
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
        dirs.push_back(ad / "usr" / "share" / "retcomm" / "fonts");
        dirs.push_back(ad / "usr" / "bin" / "fonts");
        dirs.push_back(ad / "fonts");
    }
    // SDL3: cached string — do not free. Often …/usr/bin/ inside an AppImage.
    if (const char* base = SDL_GetBasePath()) {
        fs::path b(base);
        dirs.push_back(b);
        dirs.push_back(b / "fonts");
        dirs.push_back(b / ".." / "share" / "retcomm" / "fonts");
        dirs.push_back(b / ".." / "Resources" / "fonts");
        dirs.push_back(b / ".." / ".." / "Resources" / "fonts");
        // Local cmake: build/ → ../assets/fonts
        fs::path walk = b;
        for (int i = 0; i < 6 && !walk.empty(); ++i) {
            dirs.push_back(walk / "assets" / "fonts");
            dirs.push_back(walk / "fonts");
            dirs.push_back(walk / "share" / "retcomm" / "fonts");
            walk = walk.parent_path();
        }
    }

    for (const auto& dir : dirs) {
        if (auto hit = try_file(dir / filename); !hit.empty()) return hit;
    }
    return {};
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
    ImGui::PushStyleColor(ImGuiCol_Button, th.accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th.accent_dim);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, th.accent_dim);
    ImGui::PushStyleColor(ImGuiCol_Text, th.accent_text);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}

void draw_marquee(const Theme& th, float width) {
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
    ImGui::TextUnformatted("Multi-title recomp / decomp hub");
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h + 8.f));
}

void draw_library_card(const ImVec2& row_min, float row_w, float row_h, bool selected,
                       const Theme& th, const char* title, const char* subtitle,
                       const char* chip, const ImVec4& chip_col, float pad_x, float pad_y,
                       float line_h, float line_gap) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = ImGui::ColorConvertFloat4ToU32(selected ? th.panel_hovered : th.panel);
    dl->AddRectFilled(row_min, ImVec2(row_min.x + row_w, row_min.y + row_h), bg, th.radius_sm);
    dl->AddRect(row_min, ImVec2(row_min.x + row_w, row_min.y + row_h),
                ImGui::ColorConvertFloat4ToU32(selected ? th.accent : th.border), th.radius_sm);

    const float text_x = row_min.x + pad_x;
    const float text_y = row_min.y + pad_y;
    ImGui::SetCursorScreenPos(ImVec2(text_x, text_y));
    ImGui::Text("%s", title);
    if (subtitle && subtitle[0]) {
        ImGui::SetCursorScreenPos(ImVec2(text_x, text_y + line_h + line_gap));
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextUnformatted(subtitle);
        ImGui::PopStyleColor();
    }
    if (chip && chip[0]) {
        const ImVec2 chip_sz = ImGui::CalcTextSize(chip);
        ImGui::SetCursorScreenPos(
            ImVec2(row_min.x + row_w - chip_sz.x - pad_x, row_min.y + (row_h - chip_sz.y) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, chip_col);
        ImGui::TextUnformatted(chip);
        ImGui::PopStyleColor();
    }
}

void draw_library(HubModel& hub, const Theme& th) {
    const bool busy = hub.job_running.load();
    ImGui::BeginChild("library", ImVec2(0, 0), ImGuiChildFlags_Borders);

    // Title frame: LIBRARY + optional Back (titles level only).
    {
        const float back_w = 56.f;
        const bool show_back = hub.library_nav == retcomm::hub::LibraryNav::Titles;
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextUnformatted("LIBRARY");
        ImGui::PopStyleColor();
        if (show_back) {
            const float y = ImGui::GetItemRectMin().y;
            const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            ImGui::SetCursorScreenPos(ImVec2(right - back_w, y - 2.f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.f, 2.f));
            if (ImGui::SmallButton("Back") && !busy) {
                hub.library_nav = retcomm::hub::LibraryNav::Platforms;
                hub.library_platform.clear();
            }
            ImGui::PopStyleVar();
        }
        ImGui::Separator();
    }

    constexpr float kPadX = 14.f;
    constexpr float kPadY = 12.f;
    constexpr float kLineGap = 4.f;
    const float line_h = ImGui::GetTextLineHeight();
    const float content_h = line_h * 2.f + kLineGap;
    const float row_h = (std::max)(th.row_height, content_h + kPadY * 2.f);

    std::lock_guard<std::mutex> lock(hub.mu);

    if (hub.library_nav == retcomm::hub::LibraryNav::Platforms) {
        // Platforms present in the (filtered) catalog, plus All at top.
        std::map<std::string, int> counts;
        int visible_total = 0;
        for (const auto& r : hub.rows) {
            if (!title_passes_library_filter(r, hub)) continue;
            ++visible_total;
            if (!r.platform.empty()) counts[r.platform]++;
        }
        std::vector<std::string> platforms;
        platforms.reserve(counts.size());
        for (const auto& [plat, _] : counts) platforms.push_back(plat);
        std::sort(platforms.begin(), platforms.end());

        auto draw_platform_row = [&](const char* id, const char* title, const char* subtitle) {
            ImGui::PushID(id);
            const ImVec2 row_min = ImGui::GetCursorScreenPos();
            const float row_w = ImGui::GetContentRegionAvail().x;
            draw_library_card(row_min, row_w, row_h, false, th, title, subtitle, nullptr,
                              th.text_muted, kPadX, kPadY, line_h, kLineGap);
            ImGui::SetCursorScreenPos(row_min);
            if (ImGui::InvisibleButton("##plat", ImVec2(row_w, row_h)) && !busy) {
                hub.library_nav = retcomm::hub::LibraryNav::Titles;
                hub.library_platform = (std::strcmp(id, "__all__") == 0) ? std::string{} : id;
                // Prefer keeping current selection if it matches the filter.
                bool ok = hub.selected >= 0 && hub.selected < static_cast<int>(hub.rows.size());
                if (ok) {
                    const TitleRow& sel = hub.rows[static_cast<size_t>(hub.selected)];
                    if (!title_passes_library_filter(sel, hub)) ok = false;
                    if (ok && !hub.library_platform.empty() && sel.platform != hub.library_platform)
                        ok = false;
                }
                if (!ok) {
                    hub.selected = 0;
                    for (int i = 0; i < static_cast<int>(hub.rows.size()); ++i) {
                        const TitleRow& cand = hub.rows[static_cast<size_t>(i)];
                        if (!title_passes_library_filter(cand, hub)) continue;
                        if (hub.library_platform.empty() ||
                            cand.platform == hub.library_platform) {
                            hub.selected = i;
                            break;
                        }
                    }
                }
            }
            ImGui::Dummy(ImVec2(0, 6.f));
            ImGui::PopID();
        };

        const std::string all_sub =
            std::to_string(visible_total) + (visible_total == 1 ? " title" : " titles");
        draw_platform_row("__all__", "All platforms", all_sub.c_str());
        for (const auto& plat : platforms) {
            const int n = counts[plat];
            const std::string sub = std::to_string(n) + (n == 1 ? " title" : " titles");
            draw_platform_row(plat.c_str(), plat.c_str(), sub.c_str());
        }
    } else {
        // Titles for selected platform (or all).
        for (int i = 0; i < static_cast<int>(hub.rows.size()); ++i) {
            const TitleRow& r = hub.rows[static_cast<size_t>(i)];
            if (!hub.library_platform.empty() && r.platform != hub.library_platform) continue;
            if (!title_passes_library_filter(r, hub)) continue;

            const bool selected = (hub.selected == i);
            ImGui::PushID(r.id.c_str());

            const ImVec2 row_min = ImGui::GetCursorScreenPos();
            const float row_w = ImGui::GetContentRegionAvail().x;
            const char* chip = chip_label(r);
            std::string sub = r.platform + " · " + r.kind;
            if (r.has_romm && !r.has_rom) sub += " · RomM";
            draw_library_card(row_min, row_w, row_h, selected, th, r.name.c_str(), sub.c_str(),
                              chip, chip_color(r, th), kPadX, kPadY, line_h, kLineGap);

            ImGui::SetCursorScreenPos(row_min);
            if (ImGui::InvisibleButton("##row", ImVec2(row_w, row_h)) && !busy) hub.selected = i;

            ImGui::Dummy(ImVec2(0, 6.f));
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void draw_detail(HubModel& hub, BoxartCache& boxart, const Theme& th) {
    const bool busy = hub.job_running.load();
    ImGui::BeginChild("detail", ImVec2(0, 0), ImGuiChildFlags_Borders);

    TitleRow row;
    {
        std::lock_guard<std::mutex> lock(hub.mu);
        if (hub.rows.empty() || hub.selected < 0 ||
            hub.selected >= static_cast<int>(hub.rows.size())) {
            ImGui::TextColored(th.text_muted, "Select a title from the library.");
            ImGui::EndChild();
            return;
        }
        row = hub.rows[static_cast<size_t>(hub.selected)];
    }

    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextUnformatted("TITLE");
    ImGui::PopStyleColor();
    ImGui::Text("%s", row.name.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped("%s  ·  %s / %s", row.id.c_str(), row.platform.c_str(), row.kind.c_str());
    ImGui::PopStyleColor();

    if (!row.boxart_path.empty()) {
        ImGui::Dummy(ImVec2(0, 8));
        if (const BoxartTexture* tex = boxart.get(row.id, row.boxart_path)) {
            const float max_w = std::min(ImGui::GetContentRegionAvail().x, 220.f);
            const float scale = max_w / static_cast<float>(tex->width);
            const float draw_w = max_w;
            const float draw_h = static_cast<float>(tex->height) * scale;
            ImGui::Image((ImTextureID)(intptr_t)tex->gl_id, ImVec2(draw_w, draw_h));
        } else {
            ImGui::TextColored(th.text_muted, "(boxart failed to load)");
        }
    }

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextColored(th.text_muted, "App");
    if (row.installed) {
        ImGui::TextColored(th.good, "Installed %s%s",
                           row.installed_tag.empty() ? "" : row.installed_tag.c_str(),
                           row.runtime == "wine" ? " (Wine)" : "");
        if (row.update_available)
            ImGui::TextColored(th.warn, "Update available: %s", row.latest_tag.c_str());
    } else if (row.install_dir_present) {
        ImGui::TextColored(th.warn, "Install folder present — launch binary not found");
        if (!row.install_issue.empty())
            ImGui::TextWrapped("%s", row.install_issue.c_str());
        if (!row.expected_binary.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
            ImGui::TextWrapped(
                "Looking for executable \"%s\". Open the folder to run first-time setup "
                "or fix the binary, then use Install again (or Clean install folder).",
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

    ImGui::Dummy(ImVec2(0, 6));
    const char* author_label =
        (row.kind == "decomp") ? "Decomp Author" : "Recomp Author";
    ImGui::TextColored(th.text_muted, "%s", author_label);
    if (!row.author.empty())
        ImGui::Text("%s", row.author.c_str());
    else
        ImGui::TextColored(th.text_muted, "(unknown)");

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextColored(th.text_muted, "ROM / disc");
    if (row.has_rom)
        ImGui::TextWrapped("%s", row.rom_path.c_str());
    else if (row.has_romm) {
        ImGui::TextColored(th.accent, "Available on RomM — download to use locally");
        if (!row.romm_file_name.empty()) ImGui::TextWrapped("%s", row.romm_file_name.c_str());
    } else {
        ImGui::TextColored(th.warn, "No library match — run Scan ROMs (or Scan RomM library)");
        if (!row.suggested_rom.empty()) {
            ImGui::TextColored(th.text_muted, "Looking for:");
            ImGui::TextWrapped("%s", row.suggested_rom.c_str());
        }
    }

    if (row.needs_bios || row.supports_openbios) {
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::TextColored(th.text_muted, "BIOS");
        if (!row.bios_choice_ids.empty()) {
            const char* preview =
                (row.preferred_bios_index >= 0 &&
                 row.preferred_bios_index < static_cast<int>(row.bios_choice_labels.size()))
                    ? row.bios_choice_labels[static_cast<size_t>(row.preferred_bios_index)].c_str()
                    : "(select)";
            if (ImGui::BeginCombo("##bios_choice", preview)) {
                for (size_t i = 0; i < row.bios_choice_ids.size(); ++i) {
                    const bool selected = static_cast<int>(i) == row.preferred_bios_index;
                    // Unique ID: duplicate basenames (e.g. two SCPH1001.BIN) collide otherwise.
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
            } else if (!row.bios_path.empty()) {
                ImGui::TextWrapped("%s", row.bios_path.c_str());
            }
        } else if (row.has_bios) {
            ImGui::TextWrapped("%s", row.bios_path.c_str());
        } else {
            ImGui::TextColored(th.warn, "Missing — run Scan BIOS");
            if (row.supports_openbios)
                ImGui::TextColored(th.text_muted, "Or use OpenBIOS after a catalog refresh.");
        }
    }

    ImGui::Dummy(ImVec2(0, 12));
    ImGui::BeginDisabled(busy);

    const float btn_w = (ImGui::GetContentRegionAvail().x - 8.f) * 0.5f;
    if (row.installed) {
        if (accent_button("Play", th, ImVec2(btn_w, 0)))
            hub.start_job(HubJob::Launch, row.id);
        ImGui::SameLine(0, 8);
        if (ImGui::Button("Update", ImVec2(btn_w, 0))) hub.start_job(HubJob::Update, row.id);

        ImGui::Dummy(ImVec2(0, 6));
        if (row.save_labels.empty()) {
            ImGui::TextColored(th.text_muted, "Save file");
            ImGui::TextColored(th.text_muted,
                               "None yet — Create Save, Play (auto-creates), or Sync from RomM");
            if (ImGui::Button("Create Save", ImVec2(-1, 0))) {
                std::string err;
                if (!hub.create_title_save(row.id, &err))
                    hub.append_log("Create Save failed: " + err);
            }
        } else if (row.dual_memcard) {
            auto draw_memcard_combo = [&](const char* label, const char* combo_id, int index,
                                          bool is_card2) {
                ImGui::TextColored(th.text_muted, "%s", label);
                const char* preview = "(Blank card)";
                if (index >= 0 && index < static_cast<int>(row.save_labels.size()))
                    preview = row.save_labels[static_cast<size_t>(index)].c_str();
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo(combo_id, preview)) {
                    if (is_card2) {
                        const bool blank_sel = index < 0;
                        if (ImGui::Selectable("(Blank card)", blank_sel)) {
                            std::string err;
                            if (!hub.set_title_preferred_save_card2(
                                    row.id, retcomm::kBlankMemcardId, &err))
                                hub.append_log("Could not save preference: " + err);
                        }
                        if (blank_sel) ImGui::SetItemDefaultFocus();
                    }
                    for (size_t i = 0; i < row.save_labels.size(); ++i) {
                        const bool selected = static_cast<int>(i) == index;
                        if (ImGui::Selectable(row.save_labels[i].c_str(), selected)) {
                            std::string err;
                            const bool ok =
                                is_card2
                                    ? hub.set_title_preferred_save_card2(row.id, row.save_ids[i],
                                                                         &err)
                                    : hub.set_title_preferred_save(row.id, row.save_ids[i], &err);
                            if (!ok) hub.append_log("Could not save preference: " + err);
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            };
            draw_memcard_combo("Memcard 1", "##memcard1", row.preferred_save_index, false);
            ImGui::Dummy(ImVec2(0, 4));
            draw_memcard_combo("Memcard 2", "##memcard2", row.preferred_save_card2_index, true);
            if (ImGui::Button("Create Save", ImVec2(-1, 0))) {
                std::string err;
                if (!hub.create_title_save(row.id, &err))
                    hub.append_log("Create Save failed: " + err);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
            ImGui::TextWrapped(
                "Saves live in the save library (or install saves/). Play binds "
                "settings.toml [memcard] card1 / card2.");
            ImGui::PopStyleColor();
        } else {
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
            if (ImGui::Button("Create Save", ImVec2(-1, 0))) {
                std::string err;
                if (!hub.create_title_save(row.id, &err))
                    hub.append_log("Create Save failed: " + err);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
            ImGui::TextWrapped(
                "Canonical save in the save library when configured; Play passes --save-path.");
            ImGui::PopStyleColor();
        }

        if (ImGui::Button("Open Folder", ImVec2(-1, 0))) {
            std::string err;
            if (!retcomm::open_path_in_file_manager(row.install_root, &err))
                hub.append_log("Open Folder failed: " + err);
        }
        if (!row.github_url.empty() && ImGui::Button("GitHub Source", ImVec2(-1, 0))) {
            std::string err;
            if (!retcomm::open_url_in_browser(row.github_url, &err))
                hub.append_log("Open URL failed: " + err);
        }
        if (ImGui::Button("Uninstall (keep saves)", ImVec2(-1, 0)))
            hub.start_job(HubJob::Uninstall, row.id);
        if (ImGui::Button("Uninstall + delete saves", ImVec2(-1, 0)))
            hub.start_job(HubJob::UninstallPurge, row.id);
    } else {
        if ((row.install_dir_present || row.has_preserved_state) &&
            ImGui::Button("Open Folder", ImVec2(-1, 0))) {
            std::string err;
            if (!retcomm::open_path_in_file_manager(row.install_root, &err))
                hub.append_log("Open Folder failed: " + err);
        }
        if (row.supports_local_build) {
            if (!row.has_rom) {
                ImGui::PushStyleColor(ImGuiCol_Text, th.warn);
                ImGui::TextWrapped(
                    "Match a verified ROM in your library before Build & Install.");
                ImGui::PopStyleColor();
            }
            if (accent_button(row.install_dir_present ? "Retry Build & Install" : "Build & Install",
                              th, ImVec2(-1, 0)))
                hub.start_job(HubJob::Install, row.id);
            if (row.can_prebuilt_install &&
                ImGui::Button("Install prebuilt", ImVec2(-1, 0)))
                hub.start_job(HubJob::InstallPrebuilt, row.id);
        } else {
            if (accent_button(row.install_dir_present ? "Retry Install" : "Install", th,
                              ImVec2(-1, 0)))
                hub.start_job(HubJob::Install, row.id);
        }
        if (row.can_wine_install) {
            if (ImGui::Button("Install with WINE", ImVec2(-1, 0)))
                hub.start_job(HubJob::InstallWine, row.id);
            ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
            ImGui::TextWrapped("Uses the Windows release via wine/wine64 when a native "
                               "Linux/macOS build is missing or preferred.");
            ImGui::PopStyleColor();
        }
        if (!row.github_url.empty() && ImGui::Button("GitHub Source", ImVec2(-1, 0))) {
            std::string err;
            if (!retcomm::open_url_in_browser(row.github_url, &err))
                hub.append_log("Open URL failed: " + err);
        }
        if (row.install_dir_present) {
            if (ImGui::Button("Clean install folder (keep saves)", ImVec2(-1, 0)))
                hub.start_job(HubJob::Uninstall, row.id);
            if (ImGui::Button("Clean install folder + delete saves", ImVec2(-1, 0)))
                hub.start_job(HubJob::UninstallPurge, row.id);
        } else if (row.has_preserved_state) {
            if (ImGui::Button("Delete preserved data", ImVec2(-1, 0)))
                hub.start_job(HubJob::UninstallPurge, row.id);
            ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
            ImGui::TextWrapped(
                "Removes the entire apps folder for this title, including preserved "
                "saves and config.");
            ImGui::PopStyleColor();
        }
    }

    if (row.romm_ready && (row.has_rom_identity || row.needs_bios || row.installed)) {
        ImGui::Dummy(ImVec2(0, 10));
        ImGui::TextColored(th.text_muted, "RomM");
        if (row.has_rom_identity) {
            const char* rom_label =
                row.has_rom ? "Re-download ROM from RomM" : "Download ROM from RomM";
            if (ImGui::Button(rom_label, ImVec2(-1, 0)))
                hub.start_job(HubJob::FetchRommRom, row.id);
            ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
            ImGui::TextWrapped(
                "Matches catalog hashes against your RomM library, then saves into the "
                "configured ROM library folder.");
            ImGui::PopStyleColor();
        }
        if (row.needs_bios) {
            const char* bios_label =
                row.has_bios ? "Re-download BIOS from RomM" : "Download BIOS from RomM";
            if (ImGui::Button(bios_label, ImVec2(-1, 0)))
                hub.start_job(HubJob::FetchRommBios, row.id);
            ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
            ImGui::TextWrapped(
                "Matches catalog BIOS identity against RomM firmware and saves into the "
                "BIOS root.");
            ImGui::PopStyleColor();
        }
        if (row.installed) {
            if (ImGui::Button("Sync Game Saves", ImVec2(-1, 0)))
                hub.start_job(HubJob::SyncRommSaves, row.id);
            ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
            ImGui::TextWrapped(
                "Promotes install saves into the save library, then bidirectional sync with "
                "RomM. Newer file wins; identical hashes are skipped.");
            ImGui::PopStyleColor();

            if (ImGui::Button("Sync Savestates", ImVec2(-1, 0)))
                hub.start_job(HubJob::SyncRommStates, row.id);
            ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
            ImGui::TextWrapped(
                "Bidirectional sync of savestates (states/ / .state*) with RomM. Newer file "
                "wins. Note: savestates from a recomp build and from a traditional emulator "
                "are often incompatible with one another — syncing does not convert formats.");
            ImGui::PopStyleColor();
        }
    }

    ImGui::EndDisabled();

    if (!row.author_notes.empty()) {
        ImGui::Dummy(ImVec2(0, 10));
        ImGui::TextColored(th.text_muted, "Author's Notes");
        ImGui::TextWrapped("%s", row.author_notes.c_str());
    }

    ImGui::Dummy(ImVec2(0, 10));
    ImGui::TextColored(th.text_muted, "Paths");
    if (!row.install_root.empty()) ImGui::TextWrapped("install: %s", row.install_root.c_str());
    if (!row.binary_path.empty()) ImGui::TextWrapped("binary:  %s", row.binary_path.c_str());

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
    ImGui::TextColored(th.text_muted, "Library scans");
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "Scan uses the saved library/BIOS roots (Save above first if you changed paths). "
        "Normal scan reuses the index — files already hashed with unchanged size/mtime are "
        "skipped. Full rescan clears the index and re-hashes everything.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 6));
    {
        const bool busy = hub.job_running.load();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Scan ROMs", ImVec2(160, 0))) hub.start_job(HubJob::ScanRoms);
        ImGui::SameLine();
        if (ImGui::Button("Full Rescan ROMs", ImVec2(180, 0)))
            ImGui::OpenPopup("Full ROM rescan###confirm_full_rom_rescan");
        if (ImGui::Button("Scan BIOS", ImVec2(160, 0))) hub.start_job(HubJob::ScanBios);
        ImGui::SameLine();
        if (ImGui::Button("Full Rescan BIOS", ImVec2(180, 0)))
            ImGui::OpenPopup("Full BIOS rescan###confirm_full_bios_rescan");
        ImGui::EndDisabled();
    }
    // Confirmation modals (outside BeginDisabled so they stay interactive).
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
        ImGui::EndPopup();
    }

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
        "run Scan RomM library under RomM Sync Settings so remote-only matches appear.");
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
        "Set your ROM library, BIOS, and game-saves folders. RetComM scans these paths to "
        "match supported titles and keep saves outside each install. You can change them "
        "later under Library Settings.");
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

    const bool library_ok = hub.settings.library_root[0] != '\0';
    ImGui::Dummy(ImVec2(0, 16));
    ImGui::BeginDisabled(!library_ok);
    if (accent_button("Continue", th, ImVec2(160, 0))) {
        std::string err;
        if (!hub.save_settings(&err)) {
            hub.append_log("setup save failed: " + err);
            hub.set_status("Setup save failed");
        } else {
            hub.show_setup = false;
            hub.set_status("Setup complete");
            hub.append_log("First-time setup saved");
            // One background job at a time — scan ROMs; user can Scan BIOS after.
            hub.start_job(HubJob::ScanRoms);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Skip for now", ImVec2(140, 0))) {
        hub.show_setup = false;
        hub.set_status("Setup skipped — set library root in Library settings");
        hub.append_log("First-time setup skipped");
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
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextColored(th.text_muted, "Catalog ROMs");
    {
        const bool busy = hub.job_running.load();
        const bool can_scan =
            !busy && hub.cfg.romm.enabled() && !hub.cfg.romm.api_token.empty();
        ImGui::BeginDisabled(!can_scan);
        if (ImGui::Button("Scan RomM library", ImVec2(-1, 0)))
            hub.start_job(HubJob::ScanRommRoms);
        ImGui::EndDisabled();
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Match each catalog title's rom_identity against your RomM library and cache "
            "availability (ON ROMM chip). Used by Filter Unsupported Titles so titles that "
            "exist only on RomM stay visible. Requires a saved URL + API key.");
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, 12));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextColored(th.text_muted, "Cover art");
    {
        const bool busy = hub.job_running.load();
        const bool can_resync = !busy && hub.cfg.romm.enabled() && !hub.cfg.romm.api_token.empty() &&
                                hub.cfg.romm.sync_boxart;
        ImGui::BeginDisabled(!can_resync);
        if (ImGui::Button("Resync boxart", ImVec2(-1, 0)))
            hub.start_job(HubJob::FetchBoxart, {}, true);
        ImGui::EndDisabled();
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Deletes cached RomM covers and re-downloads each title's menu cover "
            "(path_cover) from your instance — including custom/themed artwork. "
            "Requires Sync Boxart on and saved URL + API key.");
        ImGui::PopStyleColor();
    }

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

void draw_sidebar_actions(HubModel& hub, const Theme& th) {
    const bool busy = hub.job_running.load();
    if (hub.show_settings || hub.show_romm_settings) {
        if (ImGui::Button("Back to Library", ImVec2(-1, 0))) {
            hub.show_settings = false;
            hub.show_romm_settings = false;
            hub.settings.dirty = false;
            hub.romm_settings.dirty = false;
        }
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::TextColored(th.text_muted, "Editing config.json");
        ImGui::Dummy(ImVec2(0, 8));
        std::string status;
        {
            std::lock_guard<std::mutex> lock(hub.mu);
            status = hub.status;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, busy ? th.warn : th.text_muted);
        ImGui::TextWrapped("%s", status.empty() ? "Ready" : status.c_str());
        ImGui::PopStyleColor();
        return;
    }

    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Library Settings", ImVec2(-1, 0))) hub.open_settings();
    if (ImGui::Button("RomM Sync Settings", ImVec2(-1, 0))) hub.open_romm_settings();
    ImGui::Dummy(ImVec2(0, 4));
    if (ImGui::Button("Check Updates", ImVec2(-1, 0))) hub.start_job(HubJob::CheckUpdates);
    if (ImGui::Button("Refresh Catalog", ImVec2(-1, 0)))
        hub.start_job(HubJob::RefreshCatalog);
    if (ImGui::Button("Refresh Library", ImVec2(-1, 0))) {
        hub.refresh_rows(false);
        hub.set_status("Library refreshed");
    }
    ImGui::Dummy(ImVec2(0, 10));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 4));
    if (ImGui::Button("Update RetComM", ImVec2(-1, 0))) hub.start_job(HubJob::SelfUpdate);
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    {
        std::string ver = hub.launcher_version;
        if (ver.empty()) ver = retcomm::retcomm_installed_tag(hub.paths);
        ImGui::TextWrapped("Launcher %s", ver.c_str());
    }
    ImGui::PopStyleColor();
    ImGui::EndDisabled();

    ImGui::Dummy(ImVec2(0, 8));
    std::string status;
    {
        std::lock_guard<std::mutex> lock(hub.mu);
        status = hub.status;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, busy ? th.warn : th.text_muted);
    ImGui::TextWrapped("%s", status.empty() ? "Ready" : status.c_str());
    ImGui::PopStyleColor();
}

void draw_log(HubModel& hub, const Theme& th, float height) {
    if (height < 60.f) height = 60.f;
    ImGui::BeginChild("log", ImVec2(0, height), ImGuiChildFlags_Borders);
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextUnformatted("ACTIVITY");
    ImGui::PopStyleColor();
    std::string log;
    {
        std::lock_guard<std::mutex> lock(hub.mu);
        log = hub.log;
    }
    if (log.empty()) log = "(no activity yet)";
    ImGui::SameLine();
    {
        const float copy_w = ImGui::CalcTextSize("Copy").x + ImGui::GetStyle().FramePadding.x * 2.f;
        const float right = ImGui::GetWindowContentRegionMax().x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - copy_w));
        if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(log.c_str());
    }
    ImGui::Separator();

    // Scroll the outer child (not InputTextMultiline's internal window). Size the
    // multiline to its content height so selection/copy still works.
    ImGui::BeginChild("activity_scroll", ImVec2(0, 0), ImGuiChildFlags_None);
    static size_t prev_len = 0;
    const bool grew = log.size() != prev_len;
    prev_len = log.size();
    // Measured before content grows so we only stick when the user was already
    // at the bottom (doesn't fight mouse-wheel scrolling up).
    const bool at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8.f;

    std::vector<char> buf(log.begin(), log.end());
    buf.push_back('\0');
    const float wrap_w = ImGui::GetContentRegionAvail().x;
    const float wrap = wrap_w > 1.f ? wrap_w : -1.f;
    const ImVec2 text_sz = ImGui::CalcTextSize(log.c_str(), nullptr, false, wrap);
    const float box_h =
        std::max(text_sz.y + ImGui::GetStyle().FramePadding.y * 2.f + 4.f,
                 ImGui::GetTextLineHeight() * 2.f);

    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::InputTextMultiline("##activity_log", buf.data(), buf.size(),
                              ImVec2(wrap_w > 0.f ? wrap_w : 0.f, box_h),
                              ImGuiInputTextFlags_ReadOnly);
    ImGui::PopStyleColor(2);

    if (grew && at_bottom) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
    ImGui::EndChild();
}

// Drag handle between the library/detail body and the activity log.
void draw_log_splitter(float& log_h, float avail_y, const Theme& th) {
    constexpr float kSplitH = 6.f;
    constexpr float kMinLog = 72.f;
    constexpr float kMinBody = 160.f;
    const float max_log = std::max(kMinLog, avail_y - kMinBody - kSplitH);
    log_h = std::clamp(log_h, kMinLog, max_log);

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##body_log_split", ImVec2(-1.f, kSplitH));
    const ImVec2 p1 = ImGui::GetItemRectMax();
    const bool active = ImGui::IsItemActive();
    const bool hover = ImGui::IsItemHovered() || active;
    if (hover) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (active) {
        log_h = std::clamp(log_h - ImGui::GetIO().MouseDelta.y, kMinLog, max_log);
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
    (void)argv;

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
    hub.launcher_version = retcomm::retcomm_installed_tag(hub.paths);
    hub.refresh_rows(false);
    if (hub.cfg.library_root.empty()) {
        hub.open_setup();
        hub.set_status("First-time setup — choose your ROM library folder");
    } else {
        hub.set_status("Ready");
    }
    // After a real catalog download, pull covers for titles missing from cache.
    if (catalog_updated) hub.start_job(HubJob::FetchBoxart);

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

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##hub", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

        draw_marquee(th, ImGui::GetContentRegionAvail().x);

        static float log_h = 150.f;
        const float avail_y = ImGui::GetContentRegionAvail().y;
        constexpr float kSplitH = 6.f;
        constexpr float kMinLog = 72.f;
        constexpr float kMinBody = 160.f;
        const float max_log = std::max(kMinLog, avail_y - kMinBody - kSplitH);
        log_h = std::clamp(log_h, kMinLog, max_log);
        const float body_h = avail_y - log_h - kSplitH;

        ImGui::BeginChild("body", ImVec2(0, body_h), ImGuiChildFlags_None);

        const float left_w = 280.f;
        ImGui::BeginChild("left", ImVec2(left_w, 0), ImGuiChildFlags_None);
        draw_sidebar_actions(hub, th);
        ImGui::EndChild();

        ImGui::SameLine();
        if (hub.show_settings) {
            ImGui::BeginChild("settings_host", ImVec2(0, 0), ImGuiChildFlags_None);
            draw_settings_panel(hub, th, window);
            ImGui::EndChild();
        } else if (hub.show_romm_settings) {
            ImGui::BeginChild("romm_settings_host", ImVec2(0, 0), ImGuiChildFlags_None);
            draw_romm_settings_panel(hub, th);
            ImGui::EndChild();
        } else {
            const float mid_w = ImGui::GetContentRegionAvail().x * 0.48f;
            ImGui::BeginChild("mid", ImVec2(mid_w, 0), ImGuiChildFlags_None);
            draw_library(hub, th);
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("right", ImVec2(0, 0), ImGuiChildFlags_None);
            draw_detail(hub, boxart, th);
            ImGui::EndChild();
        }

        ImGui::EndChild(); // body
        draw_log_splitter(log_h, avail_y, th);
        draw_log(hub, th, log_h);
        draw_setup_wizard(hub, th, window);
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
