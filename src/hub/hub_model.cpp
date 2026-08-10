#include "hub/hub_model.hpp"
#include "hub/hub_boxart.hpp"

#include "retcomm/build.hpp"
#include "retcomm/psx_platform_settings.hpp"
#include "retcomm/psx_input_profiles.hpp"
#include "retcomm/romm_fetch.hpp"
#include "retcomm/romm_saves.hpp"
#include "retcomm/romscan.hpp"
#include "retcomm/release_tags.hpp"
#include "retcomm/self_update.hpp"
#include "retcomm/catalog_sync.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace retcomm::hub {
using namespace retcomm;

namespace {

std::string trim_log(std::string s, size_t max_chars = 200000) {
    if (s.size() <= max_chars) return s;
    return s.substr(s.size() - max_chars);
}

void wire_build_activity(HubModel* hub, BuildOptions& bopts) {
    bopts.on_progress = [hub](const std::string& msg, float) { hub->set_status(msg); };
    bopts.on_output = [hub](const std::string& line) {
        if (line.empty()) return;
        hub->append_log(line);
        // Ninja/cmake percent lines: refresh status without a second activity entry.
        if (line.size() >= 3 && line[0] == '[') {
            std::lock_guard<std::mutex> lock(hub->mu);
            hub->status = line;
        }
    };
}

void copy_buf(char* dest, size_t dest_n, const std::string& src) {
    if (dest_n == 0) return;
    std::strncpy(dest, src.c_str(), dest_n - 1);
    dest[dest_n - 1] = '\0';
}

const char* platform_label(const std::string& slug) {
    if (slug == "psx" || slug == "ps1" || slug == "ps") return "PlayStation";
    if (slug == "snes") return "Super Nintendo";
    if (slug == "gba") return "Game Boy Advance";
    if (slug == "n64") return "Nintendo 64";
    if (slug == "genesis" || slug == "md" || slug == "megadrive") return "Genesis / Mega Drive";
    if (slug == "gb" || slug == "dmg") return "Game Boy";
    if (slug == "gbc") return "Game Boy Color";
    if (slug == "psp") return "PlayStation Portable";
    return slug.empty() ? "library" : slug.c_str();
}

void finish_pending_import_toast(HubModel& hub) {
    std::string name, platform, kind;
    {
        std::lock_guard<std::mutex> lock(hub.mu);
        name = std::move(hub.pending_import_toast_name);
        platform = std::move(hub.pending_import_toast_platform);
        kind = std::move(hub.pending_import_toast_kind);
        hub.pending_import_toast_name.clear();
        hub.pending_import_toast_platform.clear();
        hub.pending_import_toast_kind.clear();
    }
    if (name.empty()) return;
    const char* plat = platform_label(platform);
    if (kind == "bios")
        hub.show_toast("Imported " + name + " → " + plat + " BIOS ready.");
    else if (kind == "rom")
        hub.show_toast("Imported " + name + " → " + plat + " library ready.");
    else
        hub.show_toast("Imported " + name + " → " + plat + ".");
}

std::string join_csv(const std::vector<std::string>& parts) {
    std::string s;
    for (const auto& p : parts) {
        if (p.empty()) continue;
        if (!s.empty()) s += ", ";
        s += p;
    }
    return s;
}

std::string to_lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool title_supports_openbios(const Title& t) {
    return to_lower_ascii(t.build.generate.engine) == "psxrecomp";
}

void apply_bios_choice_to_build(const Title& t, const BiosIndex& bios_idx, const AppState& st,
                                BuildOptions& bopts) {
    const std::string choice = preferred_bios_for(st, t.id);
    if (choice == kOpenBiosChoice) {
        bopts.use_openbios = true;
        bopts.bios_path.clear();
        return;
    }
    std::error_code ec;
    if (!choice.empty() && fs::is_regular_file(choice, ec)) {
        bopts.bios_path = choice;
        bopts.use_openbios = false;
        return;
    }
    const fs::path pref = bios_idx.preferred_bios(t.id);
    if (!pref.empty() && fs::is_regular_file(pref, ec)) {
        bopts.bios_path = pref;
        bopts.use_openbios = false;
        return;
    }
    if (title_supports_openbios(t)) {
        bopts.use_openbios = true;
        bopts.bios_path.clear();
    }
}

void apply_bios_choice_to_launch(const Title& t, const BiosIndex& bios_idx, const AppState& st,
                                 LaunchOptions& opts) {
    const std::string choice = preferred_bios_for(st, t.id);
    if (choice == kOpenBiosChoice) {
        opts.use_openbios = true;
        opts.bios_path.clear(); // runtime uses linked OpenBIOS
        return;
    }
    opts.use_openbios = false;
    std::error_code ec;
    if (!choice.empty() && fs::is_regular_file(choice, ec)) {
        opts.bios_path = choice;
        return;
    }
    if (t.has_bios_identity()) {
        const fs::path pref = bios_idx.preferred_bios(t.id);
        if (!pref.empty()) opts.bios_path = pref;
    }
}

std::vector<std::string> split_csv(const char* text) {
    std::vector<std::string> out;
    if (!text) return out;
    std::string cur;
    for (const char* p = text; ; ++p) {
        const char c = *p;
        if (c == ',' || c == '\0') {
            // trim
            size_t b = 0, e = cur.size();
            while (b < e && (cur[b] == ' ' || cur[b] == '\t')) ++b;
            while (e > b && (cur[e - 1] == ' ' || cur[e - 1] == '\t')) --e;
            if (e > b) out.emplace_back(cur.substr(b, e - b));
            cur.clear();
            if (c == '\0') break;
        } else {
            cur.push_back(c);
        }
    }
    return out;
}

// Throttle activity-log spam while still updating the status line every tick.
bool should_log_progress(size_t current, size_t total, size_t every) {
    if (current == 0) return false;
    if (current == 1 || (total > 0 && current == total)) return true;
    return every > 0 && (current % every == 0);
}

} // namespace

LogLevel classify_log_line(const std::string& line) {
    const std::string l = to_lower_ascii(line);
    if (l.find("fail") != std::string::npos || l.find("error") != std::string::npos ||
        l.find("exception") != std::string::npos || l.find(" refused") != std::string::npos)
        return LogLevel::Error;
    if (l.find("warn") != std::string::npos || l.find("missing") != std::string::npos ||
        l.find("boxart miss") != std::string::npos || l.find("no match") != std::string::npos ||
        l.find("unavailable") != std::string::npos || l.find("not installed") != std::string::npos)
        return LogLevel::Warn;
    if (l.find("up to date") != std::string::npos || l.find("complete") != std::string::npos ||
        l.find("launched") != std::string::npos ||
        (l.find("installed") != std::string::npos && l.find("not installed") == std::string::npos) ||
        l.find("updated") != std::string::npos || l.find("ready:") != std::string::npos ||
        l.find("wrote ") != std::string::npos || l.find("success") != std::string::npos ||
        l.find("setup complete") != std::string::npos)
        return LogLevel::Good;
    if (l.find("toolchain") != std::string::npos || l.find("catalog") != std::string::npos ||
        l.find("update available") != std::string::npos)
        return LogLevel::Accent;
    return LogLevel::Info;
}

// High-frequency scan progress ("… 12/340") — keep out of the activity log.
bool status_is_progress_noise(const std::string& s) {
    if (s.find("…") == std::string::npos && s.find("...") == std::string::npos) return false;
    // "label… 12/34" or "label... 12/34"
    for (size_t i = 0; i + 2 < s.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(s[i]))) {
            size_t j = i;
            while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
            if (j < s.size() && s[j] == '/') {
                size_t k = j + 1;
                if (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) return true;
            }
        }
    }
    return false;
}

void HubModel::append_log(const std::string& line) {
    append_log(line, classify_log_line(line));
}

void HubModel::append_log(const std::string& line, LogLevel level) {
    if (line.empty()) return;
    std::string text = line;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    if (text.empty()) return;

    std::lock_guard<std::mutex> lock(mu);
    // Split multi-line blobs into per-line colored entries.
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string one = text.substr(start, end - start);
        while (!one.empty() && one.back() == '\r') one.pop_back();
        if (!one.empty()) {
            if (!log_lines.empty() && log_lines.back().text == one) {
                // Prefer a stronger level if the same line is re-posted (status + log).
                const LogLevel next =
                    (level != LogLevel::Info) ? level : classify_log_line(one);
                if (static_cast<int>(next) > static_cast<int>(log_lines.back().level))
                    log_lines.back().level = next;
            } else {
                LogLine entry;
                entry.level = (level != LogLevel::Info) ? level : classify_log_line(one);
                entry.text = std::move(one);
                log_lines.push_back(std::move(entry));
            }
        }
        start = end + 1;
    }
    constexpr size_t kMaxLines = 2500;
    if (log_lines.size() > kMaxLines)
        log_lines.erase(log_lines.begin(),
                        log_lines.begin() + static_cast<std::ptrdiff_t>(log_lines.size() - kMaxLines));

    log.clear();
    for (const auto& e : log_lines) {
        if (!log.empty()) log.push_back('\n');
        log += e.text;
    }
    log = trim_log(std::move(log));
}

void HubModel::set_status(const std::string& s) {
    std::string prev;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (s == status) return;
        prev = status;
        status = s;
    }
    // Mirror status into activity (replaces the old marquee status text), skipping
    // high-frequency scan ticks that are already throttled in append_log callers.
    if (!s.empty() && s != "Ready" && !status_is_progress_noise(s)) append_log(s);
    (void)prev;
}

void HubModel::show_toast(const std::string& message) {
    if (message.empty()) return;
    {
        std::lock_guard<std::mutex> lock(mu);
        toast_message = message;
    }
    toast_pending.store(true);
}

size_t HubModel::refresh_orphan_installs() {
    auto orphans = list_orphan_installs(paths, catalog);
    const size_t n = orphans.size();
    std::lock_guard<std::mutex> lock(mu);
    pending_orphans = std::move(orphans);
    return n;
}

void HubModel::refresh_rows(bool check_updates, bool force_github_tags) {
    // Keep last GitHub check results across refresh_rows(false) so one Update /
    // scan job does not clear UPDATE badges for every other title.
    struct CachedUpdate {
        std::string latest_tag;
        bool update_available = false;
    };
    std::unordered_map<std::string, CachedUpdate> prev_updates;
    {
        std::lock_guard<std::mutex> lock(mu);
        prev_updates.reserve(rows.size());
        for (const auto& r : rows) {
            if (r.latest_tag.empty() && !r.update_available) continue;
            prev_updates[r.id] = CachedUpdate{r.latest_tag, r.update_available};
        }
    }

    library = load_library_index(paths.library_index_path);
    romm_roms = load_romm_rom_index(paths.romm_rom_index_path);
    app_state = load_app_state(paths.state_path);
    bios = load_bios_index(paths.bios_index_path);
    // New catalog titles share dumps already in the BIOS index — rematch
    // without a filesystem rescan so Play shows BIOS after Refresh Catalog.
    {
        std::unordered_set<std::string> before;
        before.reserve(bios.titles.size());
        for (const auto& t : bios.titles) before.insert(t.title_id);
        rematch_bios_titles(bios, catalog);
        bool gained = false;
        for (const auto& t : bios.titles) {
            if (!before.count(t.title_id)) {
                gained = true;
                break;
            }
        }
        if (gained) save_bios_index(paths.bios_index_path, bios);
    }

    // Deduped + TTL-cached GitHub latest tags (only used when check_updates).
    ReleaseTagCache tag_cache(release_tags_cache_path(paths));

    std::vector<TitleRow> next;
    next.reserve(catalog.titles.size());
    bool state_dirty = false;
    for (const auto& t : catalog.titles) {
        TitleRow row;
        row.id = t.id;
        row.name = t.name;
        row.platform = t.platform;
        row.description = t.description;
        row.kind = t.kind;
        row.needs_bios = t.requires_bios();

        const auto plan = inspect_install_any(paths, cfg, t);
        row.installed = plan.installed;
        row.install_dir_present = plan.install_dir_present;
        row.has_preserved_state = plan.has_preserved_state;
        row.expected_binary = plan.expected_binary;
        row.install_issue.clear();
        if (plan.install_dir_present && !plan.installed)
            row.install_issue = plan.message;
        row.installed_tag = plan.installed_tag;
        row.release_compare_tag = install_release_compare_tag(plan);
        row.install_root = plan.install_root.string();
        row.binary_path = plan.binary_path.string();
        if (plan.record) {
            row.runtime = plan.record->runtime;
            row.install_method = plan.record->method;
        }
        row.can_wine_install =
            host_supports_wine() && t.supports_wine_install() && host_os_key() != "windows";
        row.supports_local_build = t.supports_local_build();
        row.can_prebuilt_install = t.supports_prebuilt_install();
        row.built_with_openbios =
            row.installed && row.supports_local_build && install_built_with_openbios(plan);
        row.has_cmake_build_data = false;
        if (row.supports_local_build && !plan.install_root.empty()) {
            const fs::path src_base = plan.install_root / "src";
            const fs::path build_rel = t.build.cmake.build_dir.empty()
                                          ? fs::path("build")
                                          : fs::path(t.build.cmake.build_dir);
            std::error_code bec;
            if (fs::is_directory(src_base, bec)) {
                for (auto it = fs::directory_iterator(src_base, bec);
                     !bec && it != fs::directory_iterator(); it.increment(bec)) {
                    if (!it->is_directory(bec)) continue;
                    const auto name = it->path().filename().string();
                    if (name.empty() || name[0] == '.') continue;
                    if (fs::is_directory(it->path() / build_rel, bec)) {
                        row.has_cmake_build_data = true;
                        break;
                    }
                }
            }
        }
        row.has_rom_identity = t.has_rom_identity();
        row.romm_ready = cfg.romm.enabled() && !cfg.romm.api_token.empty();

        const auto rom = library.preferred_rom(t.id);
        row.has_rom = !rom.empty();
        row.rom_path = rom.string();
        {
            const auto it = romm_roms.by_title.find(t.id);
            if (it != romm_roms.by_title.end() && it->second.available) {
                row.has_romm = true;
                row.romm_file_name = it->second.file_name;
            }
        }
        if (!t.rom_identity.filenames.empty())
            row.suggested_rom = t.rom_identity.filenames.front();

        row.author = t.github_owner();
        row.github_url = t.github_source_url();
        row.author_notes = t.author_notes;

        row.netplay_supported = t.supports_netplay();
        if (row.netplay_supported) {
            row.netplay_game_name = t.netplay.game_name;
            row.netplay_game_version = t.netplay.game_version;
            row.netplay_lobby_url = cfg.resolve_netplay_lobby_url(t.netplay.lobby_url);
            row.netplay_max_slots = t.netplay.max_slots;
            row.netplay_joinable = row.installed;
            row.netplay_version_ok =
                row.installed &&
                (row.installed_tag.empty() ||
                 netplay_versions_equal(row.installed_tag, t.netplay.game_version));
        }

        {
            const fs::path art = resolve_boxart_path(cfg, t, rom, row.suggested_rom, paths);
            if (!art.empty()) row.boxart_path = art.string();
        }

        row.supports_openbios = title_supports_openbios(t);
        if (row.needs_bios || row.supports_openbios) {
            const auto b = bios.preferred_bios(t.id);
            row.has_bios = !b.empty();
            row.bios_path = b.string();

            std::unordered_set<std::string> seen;
            std::vector<std::string> dump_paths;
            auto add_dump = [&](const std::string& path) {
                if (path.empty() || !seen.insert(path).second) return;
                std::error_code ec;
                if (!fs::is_regular_file(path, ec)) return;
                dump_paths.push_back(path);
            };
            if (const auto* bind = bios.find_title(t.id)) {
                if (!bind->preferred_path.empty()) add_dump(bind->preferred_path);
                for (const auto& p : bind->paths) add_dump(p);
            }
            for (const auto& f : bios.files) {
                if (!f.title_id.empty() && f.title_id != t.id) continue;
                if (!f.platform.empty() && f.platform != t.platform) continue;
                add_dump(f.path);
            }
            std::unordered_map<std::string, int> basename_counts;
            for (const auto& path : dump_paths) {
                basename_counts[fs::path(path).filename().string()]++;
            }
            for (const auto& path : dump_paths) {
                row.bios_choice_ids.push_back(path);
                const fs::path p(path);
                const std::string base = p.filename().string();
                if (basename_counts[base] > 1) {
                    const std::string parent = p.parent_path().filename().string();
                    row.bios_choice_labels.push_back(parent.empty() ? path : (parent + "/" + base));
                } else {
                    row.bios_choice_labels.push_back(base);
                }
            }
            if (row.supports_openbios) {
                row.bios_choice_ids.push_back(kOpenBiosChoice);
                row.bios_choice_labels.push_back("OpenBIOS (MIT, bundled)");
            }

            row.bios_choice = preferred_bios_for(app_state, t.id);
            if (row.bios_choice.empty()) {
                if (!row.bios_path.empty())
                    row.bios_choice = row.bios_path;
                else if (row.supports_openbios)
                    row.bios_choice = kOpenBiosChoice;
            }
            row.preferred_bios_index = -1;
            for (size_t i = 0; i < row.bios_choice_ids.size(); ++i) {
                if (row.bios_choice_ids[i] == row.bios_choice) {
                    row.preferred_bios_index = static_cast<int>(i);
                    break;
                }
            }
            if (row.preferred_bios_index < 0 && !row.bios_choice_ids.empty()) {
                row.preferred_bios_index = 0;
                row.bios_choice = row.bios_choice_ids[0];
            }
            if (row.bios_choice == kOpenBiosChoice) {
                row.has_bios = true;
            } else if (!row.bios_choice.empty()) {
                row.bios_path = row.bios_choice;
                row.has_bios = true;
            }
        }

        row.dual_memcard = title_uses_memcards(t);
        {
            const auto saves = list_managed_saves(paths, cfg, t);
            row.save_ids.reserve(saves.size());
            row.save_labels.reserve(saves.size());
            for (const auto& s : saves) {
                row.save_ids.push_back(s.id);
                row.save_labels.push_back(s.label);
            }

            auto resolve_index = [&](std::string& id) -> int {
                if (id.empty() || id == kBlankMemcardId) return -1;
                for (size_t i = 0; i < row.save_ids.size(); ++i) {
                    if (row.save_ids[i] == id) return static_cast<int>(i);
                }
                const std::string want = fs::path(id).filename().string();
                for (size_t i = 0; i < row.save_labels.size(); ++i) {
                    if (row.save_labels[i] == want) {
                        id = row.save_ids[i];
                        return static_cast<int>(i);
                    }
                }
                return -1;
            };

            row.preferred_save = preferred_save_for(app_state, t.id);
            const bool had_preferred = !row.preferred_save.empty();
            row.preferred_save_index = resolve_index(row.preferred_save);
            if (row.preferred_save_index < 0 && !row.save_ids.empty()) {
                // Prefer a pool file named for this title — never the arbitrary first card.
                int best = -1;
                const std::string want_name = to_lower_ascii(t.name);
                const std::string want_id = to_lower_ascii(t.id);
                auto label_matches = [&](const std::string& label) {
                    // Alphanumeric fold + min length — avoid "ps"/"a" false hits.
                    auto fold = [](std::string s) {
                        std::string out;
                        for (char c : s) {
                            const unsigned char u = static_cast<unsigned char>(c);
                            if (std::isalnum(u))
                                out.push_back(static_cast<char>(std::tolower(u)));
                        }
                        return out;
                    };
                    const std::string stem = fold(fs::path(label).stem().string());
                    if (stem.size() < 6) return false;
                    auto overlaps = [&](const std::string& other) {
                        const std::string o = fold(other);
                        if (o.size() < 6) return false;
                        if (stem == o) return true;
                        const std::string& a = stem.size() >= o.size() ? stem : o;
                        const std::string& b = stem.size() >= o.size() ? o : stem;
                        return b.size() >= 6 && a.find(b) != std::string::npos;
                    };
                    return overlaps(want_name) || overlaps(want_id);
                };
                for (size_t i = 0; i < row.save_labels.size(); ++i) {
                    if (label_matches(row.save_labels[i])) {
                        best = static_cast<int>(i);
                        break;
                    }
                }
                if (best >= 0) {
                    row.preferred_save_index = best;
                    row.preferred_save = row.save_ids[static_cast<size_t>(best)];
                    if (!had_preferred) {
                        set_preferred_save(app_state, t.id, row.preferred_save);
                        state_dirty = true;
                    }
                } else if (!row.dual_memcard) {
                    // Cart / single-save: keep prior "first file" fallback.
                    row.preferred_save_index = 0;
                    row.preferred_save = row.save_ids[0];
                    if (!had_preferred) {
                        set_preferred_save(app_state, t.id, row.preferred_save);
                        state_dirty = true;
                    }
                }
                // Dual memcard with no title-named file: leave unset until Play/ensure mints.
            }

            if (row.dual_memcard) {
                row.preferred_save_card2 = preferred_save_card2_for(app_state, t.id);
                if (row.preferred_save_card2 == kBlankMemcardId) {
                    row.preferred_save_card2_index = -1;
                } else if (row.preferred_save_card2.empty()) {
                    // New installs: slot 2 stays blank until the user picks a card.
                    row.preferred_save_card2_index = -1;
                    row.preferred_save_card2 = kBlankMemcardId;
                    set_preferred_save_card2(app_state, t.id, kBlankMemcardId);
                    state_dirty = true;
                } else {
                    row.preferred_save_card2_index = resolve_index(row.preferred_save_card2);
                    if (row.preferred_save_card2_index < 0) {
                        row.preferred_save_card2 = kBlankMemcardId;
                        row.preferred_save_card2_index = -1;
                    }
                }
            }
        }

        if (row.installed && !t.release.github.empty()) {
            if (check_updates) {
                std::string err;
                // One network fetch per unique repo; shared repos reuse in-session.
                // TTL (4h) applies unless force_github_tags (manual Check for Updates).
                row.latest_tag = tag_cache.latest_tag(t.release.github, t.release.allow_prerelease,
                                                      force_github_tags, &err);
            } else if (const auto it = prev_updates.find(row.id); it != prev_updates.end()) {
                row.latest_tag = it->second.latest_tag;
            }
            // Compare GitHub latest to release_compare_tag (source_ref for build
            // pins; install.json tag is "build-<ref>").
            const std::string& have = row.release_compare_tag.empty() ? row.installed_tag
                                                                     : row.release_compare_tag;
            if (!have.empty() && !row.latest_tag.empty()) {
                // Installed ahead of a stale TTL cache (Update fetched newer live):
                // promote cache/UI so we don't offer a downgrade to the old "latest".
                if (release_tag_cmp(have, row.latest_tag) > 0) {
                    tag_cache.note_latest_tag_if_newer(t.release.github, t.release.allow_prerelease,
                                                      have);
                    row.latest_tag = have;
                    row.update_available = false;
                } else if (release_tag_cmp(have, row.latest_tag) < 0) {
                    row.update_available = true;
                } else {
                    row.update_available = false;
                }
            }
        }

        next.push_back(std::move(row));
    }

    // Persist network fetches and any "installed ahead of stale cache" promotions.
    tag_cache.save_if_dirty();
    if (check_updates) {
        append_log("Game update check: " + std::to_string(tag_cache.network_fetches()) +
                   " GitHub fetch(es), " + std::to_string(tag_cache.cache_hits()) +
                   " cache hit(s)");
    }

    std::sort(next.begin(), next.end(), [](const TitleRow& a, const TitleRow& b) {
        const size_t n = std::min(a.name.size(), b.name.size());
        for (size_t i = 0; i < n; ++i) {
            const auto ca =
                static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(a.name[i])));
            const auto cb =
                static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(b.name[i])));
            if (ca != cb) return ca < cb;
        }
        if (a.name.size() != b.name.size()) return a.name.size() < b.name.size();
        return a.id < b.id;
    });

    if (state_dirty) save_app_state(paths.state_path, app_state, nullptr);

    std::lock_guard<std::mutex> lock(mu);
    // Preserve selection by id when possible.
    std::string sel_id;
    if (selected >= 0 && selected < static_cast<int>(rows.size()))
        sel_id = rows[static_cast<size_t>(selected)].id;
    rows = std::move(next);
    selected = 0;
    if (!sel_id.empty()) {
        for (size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].id == sel_id) {
                selected = static_cast<int>(i);
                break;
            }
        }
    }
}

void HubModel::fetch_boxart_for_catalog(bool force) {
    const char* src = active_boxart_source(cfg);
    set_status(std::string(force ? "Resyncing boxart (" : "Fetching boxart (") + src + ")…");
    append_log(std::string("Boxart: source=") + src +
               (cfg.romm.sync_boxart ? (" @ " + cfg.romm.base_url) : "") +
               (cfg.prefer_local_boxart ? " (prefer local)" : "") +
               (force ? " (force resync)" : ""));
    if (force) {
        clear_boxart_cache(paths, cfg);
        append_log(std::string("Boxart: cleared cache ") + boxart_cache_dir(paths, cfg).string());
    }
    size_t fetched = 0, skipped = 0, failed = 0;
    const size_t total = catalog.titles.size();
    size_t i = 0;
    for (const auto& t : catalog.titles) {
        ++i;
        const fs::path rom = library.preferred_rom(t.id);
        const std::string suggested =
            t.rom_identity.filenames.empty() ? "" : t.rom_identity.filenames.front();
        {
            std::ostringstream st;
            st << "Boxart (" << src << "): " << i << "/" << total << " — " << t.name;
            set_status(st.str());
        }
        auto fr = ensure_remote_boxart(paths, cfg, t, rom, suggested, force);
        if (fr.ok) {
            if (fr.source == "local" || fr.source == "cache" || fr.message == "already present")
                ++skipped;
            else {
                ++fetched;
                append_log("Boxart [" + fr.source + "] " + t.id + ": " + fr.message);
            }
        } else {
            ++failed;
            if (should_log_progress(i, total, 5) || failed <= 8)
                append_log("Boxart miss " + t.id + ": " + fr.message);
        }
    }
    std::ostringstream oss;
    oss << "Boxart: fetched " << fetched << ", already had " << skipped << ", missed " << failed
        << " (" << src << ")";
    append_log(oss.str());
    set_status(oss.str());
}

std::size_t HubModel::queued_job_count() const {
    std::lock_guard<std::mutex> lock(mu);
    return job_queue.size();
}

bool HubModel::is_job_queued(HubJob j, const std::string& title_id,
                             const std::string& platform_filter) const {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& q : job_queue) {
        if (q.job != j) continue;
        if (hub_job_is_scan(j)) {
            if (q.platform_filter == platform_filter) return true;
        } else if (q.title_id == title_id) {
            return true;
        }
    }
    return false;
}

bool HubModel::has_queued_scan() const {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& q : job_queue) {
        if (hub_job_is_scan(q.job)) return true;
    }
    return false;
}

bool HubModel::is_title_queued(const std::string& title_id) const {
    if (title_id.empty()) return false;
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& q : job_queue) {
        if (q.title_id == title_id && hub_job_is_queueable(q.job)) return true;
    }
    return false;
}

static const char* queued_hub_job_kind(HubJob j) {
    switch (j) {
    case HubJob::Update:
        return "Update";
    case HubJob::GenerateRebuild:
        return "Rebuild";
    case HubJob::MoveInstall:
        return "Move install";
    case HubJob::ScanRoms:
        return "Scan";
    case HubJob::FullScanRoms:
        return "Full scan";
    case HubJob::ScanBios:
        return "BIOS scan";
    case HubJob::FullScanBios:
        return "Full BIOS scan";
    case HubJob::PurgeMissingFiles:
        return "Clean missing";
    default:
        return "Install";
    }
}

static bool enqueue_hub_job(HubModel* hub, QueuedHubJob item) {
    if (!hub || !hub_job_is_queueable(item.job)) return false;
    const bool is_scan = hub_job_is_scan(item.job);
    if (!is_scan && item.title_id.empty()) return false;
    std::string note;
    {
        std::lock_guard<std::mutex> lock(hub->mu);
        if (!is_scan) {
            if (item.apps_dir.empty() && !hub->job_apps_dir.empty())
                item.apps_dir = std::move(hub->job_apps_dir);
            hub->job_apps_dir.clear();
        } else if (item.prefetch_catalog) {
            hub->job_prefetch_catalog = false;
        }

        if (hub->job_running.load() && hub->job == item.job) {
            if (is_scan) {
                if (hub->job_platform_filter == item.platform_filter) return true;
            } else if (hub->job_title_id == item.title_id) {
                return true;
            }
        }
        // At most one library scan/purge waiting — avoid spam-filling the queue.
        if (is_scan) {
            for (const auto& q : hub->job_queue) {
                if (!hub_job_is_scan(q.job)) continue;
                if (q.job == item.job && q.platform_filter == item.platform_filter)
                    return true;
                return false;
            }
        } else {
            for (const auto& q : hub->job_queue) {
                if (q.job == item.job && q.title_id == item.title_id) return true;
            }
        }
        hub->job_queue.push_back(std::move(item));
        const QueuedHubJob& back = hub->job_queue.back();
        note = std::string("Queued ") + queued_hub_job_kind(back.job);
        if (is_scan) {
            note += back.platform_filter.empty()
                        ? ": all platforms"
                        : (": " + back.platform_filter);
        } else {
            note += ": " + back.title_id;
        }
        note += " (" + std::to_string(hub->job_queue.size()) + " in queue)";
    }
    if (!note.empty()) hub->append_log(note);
    return true;
}

static bool enqueue_hub_job(HubModel* hub, HubJob j, const std::string& title_id,
                            bool force_boxart, bool fetch_romm_first) {
    QueuedHubJob item;
    item.job = j;
    item.title_id = title_id;
    item.force_boxart = force_boxart;
    item.fetch_romm_first = fetch_romm_first;
    if (hub) {
        std::lock_guard<std::mutex> lock(hub->mu);
        if (!hub_job_is_scan(j))
            item.apps_dir = hub->job_apps_dir;
        else {
            item.platform_filter = hub->scans_platform_filter;
            item.prefetch_catalog = hub->job_prefetch_catalog;
        }
    }
    return enqueue_hub_job(hub, std::move(item));
}

int HubModel::queue_all_updates() {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(mu);
        for (const auto& r : rows) {
            if (r.installed && r.update_available) ids.push_back(r.id);
        }
    }
    int n = 0;
    for (const auto& id : ids) {
        if (enqueue_hub_job(this, HubJob::Update, id, false, false)) ++n;
    }
    if (!job_running.load()) start_next_queued_job();
    if (n > 0) set_status("Queued " + std::to_string(n) + " update(s)");
    return n;
}

bool HubModel::start_next_queued_job() {
    if (job_running.load()) return false;
    QueuedHubJob next;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (job_queue.empty()) return false;
        next = job_queue.front();
        job_queue.pop_front();
        // Restore per-job options so start_job / the worker see them.
        job_apps_dir = next.apps_dir;
        if (hub_job_is_scan(next.job)) {
            scans_platform_filter = next.platform_filter;
            job_prefetch_catalog = next.prefetch_catalog;
        }
    }
    // start_job will claim the worker; if a race loses the slot, re-queue.
    if (start_job(next.job, next.title_id, next.force_boxart, next.fetch_romm_first))
        return true;
    std::lock_guard<std::mutex> lock(mu);
    job_queue.push_front(next);
    return false;
}

bool HubModel::start_job(HubJob j, const std::string& title_id, bool force_boxart,
                         bool fetch_romm_first) {
    // Play (+ optional per-title update preflight) overlaps Build & Install /
    // scans on a dedicated thread so Install can keep the main worker.
    if (j == HubJob::Launch || j == HubJob::CheckLaunchUpdate) {
        if (launch_running.exchange(true)) {
            append_log("Launch already in progress");
            return false;
        }
        if (launch_worker.joinable()) launch_worker.join();
        launch_worker = std::thread([this, j, title_id] {
            try {
                ensure_dirs(paths);
                cfg = load_app_config(paths.config_path);
                const Title* t = catalog.find(title_id);
                if (!t) {
                    append_log("unknown title: " + title_id);
                    launch_running = false;
                    return;
                }

                bool do_launch = (j == HubJob::Launch);
                if (j == HubJob::CheckLaunchUpdate) {
                    set_status("Checking for updates…");
                    if (t->release.github.empty()) {
                        do_launch = true;
                    } else {
                        const auto plan = inspect_install_any(paths, cfg, *t);
                        std::string err;
                        ReleaseTagCache tag_cache(release_tags_cache_path(paths));
                        std::string latest = tag_cache.latest_tag(
                            t->release.github, t->release.allow_prerelease,
                            /*force=*/false, &err);
                        const std::string have = install_release_compare_tag(plan);
                        // Installed ahead of stale TTL cache → promote, don't block launch.
                        if (!have.empty() && !latest.empty() &&
                            release_tag_cmp(have, latest) > 0) {
                            tag_cache.note_latest_tag_if_newer(
                                t->release.github, t->release.allow_prerelease, have);
                            latest = have;
                        }
                        tag_cache.save_if_dirty();
                        const bool upd = !latest.empty() && !have.empty() &&
                                         release_tag_cmp(have, latest) < 0;
                        if (!latest.empty())
                            append_log(title_id + ": installed " + have + ", latest " +
                                       latest);
                        else if (!err.empty())
                            append_log(title_id + ": update check failed (" + err +
                                       ") — launching");
                        {
                            std::lock_guard<std::mutex> lock(mu);
                            for (auto& r : rows) {
                                if (r.id != title_id) continue;
                                r.installed_tag = plan.installed_tag;
                                r.release_compare_tag = have;
                                if (!latest.empty()) r.latest_tag = latest;
                                r.update_available = upd;
                                break;
                            }
                            if (upd) {
                                launch_update_prompt_id = title_id;
                                launch_update_from = have;
                                launch_update_to = latest;
                                launch_update_prompt_pending.store(true);
                                status = "Update available for " + title_id;
                            }
                        }
                        if (upd) {
                            start_prefetch_updates({title_id});
                            launch_running = false;
                            return;
                        }
                        do_launch = true;
                    }
                }

                if (do_launch) {
                    set_status("Launching " + title_id + "…");
                    LaunchOptions opts;
                    opts.mode = LaunchMode::Default;
                    opts.detach = true;
                    opts.rom_path = library.preferred_rom(title_id);
                    {
                        auto ensured =
                            ensure_canonical_save(paths, cfg, *t, opts.rom_path, true);
                        if (!ensured.message.empty()) append_log(ensured.message);
                        app_state = load_app_state(paths.state_path);
                        apply_bios_choice_to_launch(*t, bios, app_state, opts);
                        if (ensured.ok) opts.save_path = ensured.save.host_path;
                        else {
                            const std::string save_id =
                                preferred_save_for(app_state, title_id);
                            if (!save_id.empty()) {
                                opts.save_path =
                                    resolve_managed_save(paths, cfg, *t, save_id);
                                if (opts.save_path.empty()) opts.save_path = save_id;
                            }
                        }
                        if (title_uses_memcards(*t)) {
                            const std::string card2_id =
                                preferred_save_card2_for(app_state, title_id);
                            if (card2_id == kBlankMemcardId) {
                                opts.save_path_card2_blank = true;
                            } else if (!card2_id.empty()) {
                                opts.save_path_card2 =
                                    resolve_managed_save(paths, cfg, *t, card2_id);
                            }
                        }
                    }
                    auto r = launch_title(paths, *t, opts);
                    append_log(r.message);
                    set_status(r.ok ? ("Launched " + title_id)
                                    : ("Launch failed: " + title_id));
                }
            } catch (const std::exception& e) {
                append_log(std::string("error: ") + e.what());
                set_status("Launch error");
            }
            launch_running = false;
        });
        return true;
    }

    if (job_running.load()) {
        if (hub_job_is_queueable(j))
            return enqueue_hub_job(this, j, title_id, force_boxart, fetch_romm_first);
        return false;
    }
    if (job_running.exchange(true)) {
        // Lost the race to another starter — queue instead of failing.
        if (hub_job_is_queueable(j))
            return enqueue_hub_job(this, j, title_id, force_boxart, fetch_romm_first);
        return false;
    }
    if (worker.joinable()) worker.join();
    job = j;
    job_title_id = title_id;
    job_platform_filter = hub_job_is_scan(j) ? scans_platform_filter : std::string{};
    job_force_boxart = force_boxart;
    job_fetch_romm_first = fetch_romm_first;

    worker = std::thread([this, j, title_id, force_boxart, fetch_romm_first] {
        try {
            ensure_dirs(paths);
            cfg = load_app_config(paths.config_path);

            auto find_title = [&]() -> const Title* {
                return catalog.find(title_id);
            };

            auto rescan_library_after_romm = [&](const Title& t,
                                                 const RommFetchResult& fr) -> bool {
                library = load_library_index(paths.library_index_path);

                // Prefer a direct hash+bind of the just-downloaded set (multi-track
                // .cue+.bin). Full library walk is a fallback when that fails.
                set_status("RomM: verifying downloaded ROM…");
                auto direct = bind_downloaded_rom_to_index(library, t, fr.saved_path,
                                                           cfg.library_root);
                append_log(direct.message);
                if (direct.ok) {
                    save_library_index(paths.library_index_path, library);
                } else {
                    set_status("RomM: scanning library for new ROM…");
                    ScanOptions opts;
                    opts.index = &library;
                    opts.platforms = {t.platform};
                    auto scan = scan_rom_library(catalog, cfg, opts);
                    merge_scan_into_index(library, catalog, scan, cfg.library_root);
                    save_library_index(paths.library_index_path, library);
                    const fs::path bound = library.preferred_rom(t.id);
                    append_log("ROM scan after download: " +
                               std::to_string(scan.matches.size()) + " match(es)" +
                               (bound.empty() ? (" — still unbound for " + t.id)
                                              : (" (bound " + bound.string() + ")")));
                }

                {
                    romm_roms = load_romm_rom_index(paths.romm_rom_index_path);
                    RommRomIndexEntry e;
                    e.available = true;
                    e.file_name = fr.remote_name;
                    e.matched_by = fr.matched_by;
                    e.checked_at = "";
                    romm_roms.by_title[t.id] = std::move(e);
                    save_romm_rom_index(paths.romm_rom_index_path, romm_roms, nullptr);
                }
                return !library.preferred_rom(t.id).empty();
            };

            auto fetch_romm_and_bind = [&](const Title& t) -> bool {
                if (!t.has_rom_identity()) {
                    append_log("verified ROM required — catalog has no rom_identity for " + t.id);
                    return false;
                }
                if (!cfg.romm.enabled() || cfg.romm.api_token.empty()) {
                    append_log("verified ROM required — scan your library or configure RomM");
                    return false;
                }
                if (cfg.library_root.empty()) {
                    append_log("verified ROM required — set library_root before RomM download");
                    return false;
                }
                set_status("RomM: finding ROM for " + t.id + "…");
                auto fr = fetch_rom_from_romm(cfg, t, [this](const std::string& s) {
                    set_status(s);
                });
                append_log(fr.message);
                if (!fr.ok) return false;
                if (!rescan_library_after_romm(t, fr)) {
                    append_log("RomM download finished but library still has no verified match "
                               "for " +
                               t.id + " (multi-track discs need .cue + all tracks)");
                    set_status("RomM ROM incomplete: " + t.id);
                    return false;
                }
                set_status("RomM ROM ready: " + t.id);
                return true;
            };

            auto ensure_rom_via_romm = [&](const Title& t) -> bool {
                if (!library.preferred_rom(t.id).empty()) return true;
                return fetch_romm_and_bind(t);
            };

            switch (j) {
            case HubJob::Install:
            case HubJob::InstallPrebuilt:
            case HubJob::InstallWine: {
                const bool wine = (j == HubJob::InstallWine);
                const bool prebuilt = (j == HubJob::InstallPrebuilt) || wine;
                const auto* t = find_title();
                if (!t) {
                    append_log("unknown title: " + title_id);
                    break;
                }
                // Zip-first when host release assets exist; ROM only required for
                // build-only titles (or build fallback after a failed zip).
                const bool can_zip = t->supports_prebuilt_install();
                const bool build_only =
                    !prebuilt && t->supports_local_build() && !can_zip;
                set_status(std::string(wine ? "Installing (Wine) "
                                       : (prebuilt || can_zip) ? "Installing "
                                                               : "Building ") +
                           title_id + "…");
                // Local builds need a verified ROM; hub confirm sets fetch_romm_first.
                if (build_only || (!prebuilt && t->build.enabled && fetch_romm_first)) {
                    library = load_library_index(paths.library_index_path);
                    std::error_code rom_ec;
                    fs::path rom = library.preferred_rom(title_id);
                    if ((rom.empty() || !fs::is_regular_file(rom, rom_ec)) && !fetch_romm_first) {
                        // Stale index bind: drop missing paths and ask the UI to prompt.
                        const auto* bind = library.find_title(title_id);
                        if (bind && (!bind->paths.empty() || !bind->preferred_path.empty())) {
                            append_log("Library DB path missing for " + title_id +
                                       " — removing missing files from index");
                            auto pr = purge_missing_library_files(library, t->platform);
                            save_library_index(paths.library_index_path, library);
                            append_log("Removed " + std::to_string(pr.removed_files) +
                                       " missing file(s) from library DB");
                        }
                        {
                            std::lock_guard<std::mutex> lock(mu);
                            missing_rom_prompt_id = title_id;
                            show_missing_rom_prompt = true;
                        }
                        set_status("ROM missing from disk — scan or download from RomM");
                        break;
                    }
                    if (fetch_romm_first) {
                        if (!ensure_rom_via_romm(*t)) {
                            set_status("Install failed: " + title_id);
                            break;
                        }
                    }
                }
                InstallOptions opts;
                opts.force = false;
                opts.check_latest = true;
                opts.use_wine = wine;
                opts.prefer_prebuilt = prebuilt;
                {
                    fs::path apps;
                    {
                        std::lock_guard<std::mutex> lock(mu);
                        apps = std::move(job_apps_dir);
                        job_apps_dir.clear();
                    }
                    if (apps.empty())
                        apps = resolve_default_install_root(cfg, paths);
                    opts.apps_dir = std::move(apps);
                }
                // Existing install/preserved under another apps root → migrate first.
                {
                    const InstallPlan elsewhere = inspect_install_any(paths, cfg, *t);
                    if ((elsewhere.installed || elsewhere.install_dir_present ||
                         elsewhere.has_preserved_state) &&
                        !elsewhere.install_root.empty()) {
                        std::error_code eq_ec;
                        const bool same_root =
                            fs::equivalent(elsewhere.install_root.parent_path(), opts.apps_dir,
                                           eq_ec) ||
                            elsewhere.install_root.parent_path() == opts.apps_dir;
                        if (!same_root) {
                            set_status("Moving install data for " + title_id + "…");
                            auto mr = move_title_install(paths, cfg, *t, opts.apps_dir);
                            append_log(mr.message);
                            if (!mr.ok) {
                                set_status("Install failed: could not move existing data");
                                break;
                            }
                        }
                    }
                }
                BuildOptions bopts;
                bopts.rom_path = library.preferred_rom(title_id);
                bopts.apps_dir = opts.apps_dir;
                // Reinstall (partial / NEEDS SETUP): clear apps/<title> first. Mid-setup
                // crashes leave leftovers that a plain Install can refuse to overwrite.
                // Keep saves/config under preserved/ (same default as Manage Game Data).
                {
                    Paths job_paths = with_apps_dir(paths, opts.apps_dir);
                    const InstallPlan existing = inspect_install(job_paths, *t);
                    if (existing.install_dir_present) {
                        set_status("Clearing game data for " + title_id + "…");
                        UninstallOptions uopts;
                        uopts.keep_saves = true;
                        auto ur = uninstall_title(job_paths, *t, uopts);
                        append_log(ur.message);
                        if (!ur.ok) {
                            set_status("Reinstall failed: could not clear game data");
                            break;
                        }
                        opts.force = true;
                        bopts.force = true;
                        set_status(std::string(wine ? "Reinstalling (Wine) "
                                               : (prebuilt || can_zip) ? "Reinstalling "
                                                                       : "Rebuilding ") +
                                   title_id + "…");
                    }
                }
                app_state = load_app_state(paths.state_path);
                apply_bios_choice_to_build(*t, bios, app_state, bopts);
                wire_build_activity(this, bopts);
                auto r = install_title_auto(paths, *t, opts, bopts);
                append_log(r.message);
                // Zip failed → build fallback needs a ROM: offer the missing-ROM chooser.
                if (!r.ok && !prebuilt && t->supports_local_build() && can_zip) {
                    library = load_library_index(paths.library_index_path);
                    std::error_code rom_ec;
                    const fs::path rom = library.preferred_rom(title_id);
                    const bool no_rom = rom.empty() || !fs::is_regular_file(rom, rom_ec);
                    if (no_rom && r.message.find("fell back to local build") != std::string::npos) {
                        std::lock_guard<std::mutex> lock(mu);
                        missing_rom_prompt_id = title_id;
                        show_missing_rom_prompt = true;
                        set_status("Prebuilt install failed — add a ROM to build locally");
                        break;
                    }
                }
                if (r.ok) {
                    // Assign title-named memcard/SRAM to slot 1; leave memcard 2 blank.
                    const fs::path rom = library.preferred_rom(title_id);
                    auto ensured = ensure_canonical_save(paths, cfg, *t, rom, true);
                    if (!ensured.message.empty()) append_log(ensured.message);
                    app_state = load_app_state(paths.state_path);
                    if (is_psx_platform(t->platform)) {
                        const fs::path cwd = resolve_current_release_dir(r.plan.install_root);
                        const fs::path apply_cwd = cwd.empty() ? r.plan.install_root : cwd;
                        auto ar = apply_psx_platform_defaults(paths, app_state, *t, apply_cwd);
                        if (!ar.message.empty()) append_log(ar.message);
                    }
                }
                set_status(r.ok ? ("Installed " + title_id) : ("Install failed: " + title_id));
                break;
            }
            case HubJob::Update: {
                set_status("Updating " + title_id + "…");
                const auto* t = find_title();
                if (!t) {
                    append_log("unknown title: " + title_id);
                    break;
                }
                InstallOptions iopts;
                {
                    std::lock_guard<std::mutex> lock(mu);
                    for (const auto& r : rows) {
                        if (r.id != title_id) continue;
                        iopts.hint_latest_tag = r.latest_tag;
                        if (!r.install_root.empty())
                            iopts.apps_dir = fs::path(r.install_root).parent_path();
                        break;
                    }
                }
                BuildOptions bopts;
                bopts.rom_path = library.preferred_rom(title_id);
                bopts.apps_dir = iopts.apps_dir;
                app_state = load_app_state(paths.state_path);
                apply_bios_choice_to_build(*t, bios, app_state, bopts);
                wire_build_activity(this, bopts);
                auto r = update_title_auto(paths, *t, iopts, bopts);
                append_log(r.message);
                if (r.ok) {
                    // Align Check Updates cache with whatever Update just resolved/installed.
                    std::string noted;
                    if (r.plan.record && !r.plan.record->source_ref.empty())
                        noted = r.plan.record->source_ref;
                    else if (!r.plan.latest_tag.empty())
                        noted = r.plan.latest_tag;
                    else if (!iopts.hint_latest_tag.empty())
                        noted = iopts.hint_latest_tag;
                    if (!noted.empty() && !t->release.github.empty()) {
                        ReleaseTagCache cache(release_tags_cache_path(paths));
                        cache.note_latest_tag(t->release.github, t->release.allow_prerelease,
                                              noted);
                        cache.save_if_dirty();
                    }
                    if (is_psx_platform(t->platform)) {
                        app_state = load_app_state(paths.state_path);
                        const fs::path cwd = resolve_current_release_dir(r.plan.install_root);
                        const fs::path apply_cwd = cwd.empty() ? r.plan.install_root : cwd;
                        auto ar = apply_psx_platform_defaults(paths, app_state, *t, apply_cwd);
                        if (!ar.message.empty()) append_log(ar.message);
                    }
                }
                set_status(r.ok ? (r.skipped ? ("Up to date: " + title_id)
                                             : ("Updated " + title_id))
                                : ("Update failed: " + title_id));
                break;
            }
            case HubJob::GenerateRebuild: {
                set_status("Reinstalling with BIOS…");
                const auto* t = find_title();
                if (!t) {
                    append_log("unknown title: " + title_id);
                    break;
                }
                if (!t->supports_local_build()) {
                    append_log("catalog has no local build recipe for " + title_id);
                    set_status("Reinstall w BIOS unavailable: " + title_id);
                    break;
                }
                BuildOptions bopts;
                bopts.force = true;
                bopts.force_generate = true;
                bopts.force_bios = true; // regenerate SCPH1001 + OpenBIOS backends
                bopts.rom_path = library.preferred_rom(title_id);
                if (bopts.rom_path.empty()) {
                    append_log("Reinstall w BIOS needs a matched .cue in the library");
                    set_status("Reinstall w BIOS failed: no disc");
                    break;
                }
                {
                    const InstallPlan plan = inspect_install_any(paths, cfg, *t);
                    if (!plan.install_root.empty())
                        bopts.apps_dir = plan.install_root.parent_path();
                }
                app_state = load_app_state(paths.state_path);
                apply_bios_choice_to_build(*t, bios, app_state, bopts);
                if (bopts.use_openbios || bopts.bios_path.empty()) {
                    append_log("Reinstall w BIOS needs a retail BIOS dump selected");
                    set_status("Reinstall w BIOS failed: select SCPH1001.bin");
                    break;
                }
                wire_build_activity(this, bopts);
                append_log("Reinstall w BIOS: " + bopts.bios_path.string() +
                           " (+ OpenBIOS regen)");
                auto r = build_title(paths, *t, bopts);
                append_log(r.message);
                if (r.ok && is_psx_platform(t->platform)) {
                    app_state = load_app_state(paths.state_path);
                    const InstallPlan plan = inspect_install_any(paths, cfg, *t);
                    const fs::path cwd = resolve_current_release_dir(plan.install_root);
                    const fs::path apply_cwd = cwd.empty() ? plan.install_root : cwd;
                    auto ar = apply_psx_platform_defaults(paths, app_state, *t, apply_cwd);
                    if (!ar.message.empty()) append_log(ar.message);
                }
                set_status(r.ok ? ("Reinstalled with BIOS: " + title_id)
                                : ("Reinstall w BIOS failed: " + title_id));
                break;
            }
            case HubJob::Uninstall:
            case HubJob::UninstallPurge: {
                set_status("Uninstalling " + title_id + "…");
                const auto* t = find_title();
                if (!t) {
                    append_log("unknown title: " + title_id);
                    break;
                }
                UninstallOptions opts;
                opts.keep_saves = (j != HubJob::UninstallPurge);
                auto r = uninstall_title(paths, *t, opts);
                append_log(r.message);
                set_status(r.ok ? ("Uninstalled " + title_id) : ("Uninstall failed: " + title_id));
                break;
            }
            case HubJob::MoveInstall: {
                set_status("Moving install for " + title_id + "…");
                const auto* t = find_title();
                if (!t) {
                    append_log("unknown title: " + title_id);
                    break;
                }
                fs::path apps;
                {
                    std::lock_guard<std::mutex> lock(mu);
                    apps = std::move(job_apps_dir);
                    job_apps_dir.clear();
                }
                if (apps.empty()) {
                    append_log("move install: no destination apps root");
                    set_status("Move failed: " + title_id);
                    break;
                }
                auto mr = move_title_install(paths, cfg, *t, apps);
                append_log(mr.message);
                set_status(mr.ok ? ("Moved " + title_id) : ("Move failed: " + title_id));
                break;
            }
            case HubJob::DeleteBuildData: {
                set_status("Deleting build data for " + title_id + "…");
                const auto* t = find_title();
                if (!t) {
                    append_log("unknown title: " + title_id);
                    break;
                }
                const InstallPlan plan = inspect_install_any(paths, cfg, *t);
                const fs::path src_base =
                    (plan.install_root.empty() ? (paths.apps_dir / t->install_dir_name)
                                               : plan.install_root) /
                    "src";
                const fs::path build_rel = t->build.cmake.build_dir.empty()
                                              ? fs::path("build")
                                              : fs::path(t->build.cmake.build_dir);
                std::error_code ec;
                int removed = 0;
                auto wipe_build = [&](const fs::path& src_root) {
                    const fs::path build_dir = src_root / build_rel;
                    if (!fs::is_directory(build_dir, ec)) return;
                    fs::remove_all(build_dir, ec);
                    if (!ec) {
                        ++removed;
                        append_log("Removed " + build_dir.string());
                    } else {
                        append_log("Failed to remove " + build_dir.string() + ": " +
                                   ec.message());
                    }
                };
                // Stable working tree + any leftover src/<tag>/build from older layouts.
                if (fs::is_directory(src_base, ec)) {
                    for (auto it = fs::directory_iterator(src_base, ec);
                         !ec && it != fs::directory_iterator(); it.increment(ec)) {
                        if (!it->is_directory(ec)) continue;
                        const auto name = it->path().filename().string();
                        if (name.empty() || name[0] == '.') continue;
                        wipe_build(it->path());
                    }
                }
                if (removed > 0) {
                    set_status("Deleted build data: " + title_id);
                    append_log("Deleted " + std::to_string(removed) +
                               " cmake build tree(s) for " + title_id +
                               " (Play binary and saves kept)");
                } else {
                    set_status("No build data: " + title_id);
                    append_log("No cmake build/ directory found under " + src_base.string());
                }
                break;
            }
            case HubJob::Launch:
                // Handled on launch_worker via start_job.
                break;
            case HubJob::ScanRoms:
            case HubJob::FullScanRoms: {
                const bool full = (j == HubJob::FullScanRoms);
                const std::string plat_filter = scans_platform_filter;
                const bool prefetch = job_prefetch_catalog;
                job_prefetch_catalog = false;
                if (prefetch) {
                    // Quiet catalog sync so ROM matching has an up-to-date title list.
                    set_status("Updating catalog…");
                    auto cr = sync_remote_catalog(paths, cfg, true);
                    if (cr.ok) {
                        try {
                            catalog = load_catalog(paths.catalog_dir);
                        } catch (const std::exception& e) {
                            append_log(std::string("catalog reload after prefetch: ") + e.what());
                        }
                    } else {
                        append_log("Catalog prefetch: " + cr.message +
                                   " — continuing ROM scan with current catalog");
                    }
                }
                const char* rom_label = full ? "Full rebuild" : "Scan new files";
                set_status(std::string(rom_label) + (plat_filter.empty()
                                                         ? "…"
                                                         : (" [" + plat_filter + "]…")));
                // Only wipe the whole index on a full all-platforms rescan.
                if (full && plat_filter.empty()) {
                    append_log("Full rebuild: clearing library index cache…");
                    library = LibraryIndex{};
                } else {
                    library = load_library_index(paths.library_index_path);
                    append_log(std::string(rom_label) + ": starting…");
                }
                ScanOptions opts;
                opts.full_rescan = full;
                opts.index = full ? nullptr : &library;
                if (!plat_filter.empty()) opts.platforms = {plat_filter};
                opts.on_progress = [this, full](const ScanProgress& p) {
                    const char* label = full ? "Full rebuild" : "Scan new files";
                    if (p.phase == "walk") {
                        if (should_log_progress(p.current, 0, 50)) {
                            std::ostringstream oss;
                            oss << label << ": indexing [" << p.platform << "] " << p.current
                                << " files…";
                            append_log(oss.str());
                        }
                        std::ostringstream st;
                        st << label << ": indexing [" << p.platform << "] " << p.current
                           << " files…";
                        set_status(st.str());
                    } else if (p.phase == "cache") {
                        if (should_log_progress(p.current, 0, 25)) {
                            std::ostringstream oss;
                            oss << label << ": cache-hit [" << p.platform << "] " << p.current;
                            append_log(oss.str());
                        }
                    } else if (p.phase == "hash" && p.total > 0) {
                        std::ostringstream st;
                        st << label << ": hashing " << p.current << "/" << p.total;
                        if (!p.platform.empty()) st << " [" << p.platform << "]";
                        if (!p.path.empty()) st << " — " << p.path.filename().string();
                        set_status(st.str());
                        if (should_log_progress(p.current, p.total, full ? 10 : 25)) {
                            append_log(st.str());
                        }
                    } else if (p.phase == "match") {
                        append_log(std::string(label) + ": matching catalog…");
                        set_status(std::string(label) + ": matching catalog…");
                    }
                };
                ScanResult result;
                if (cfg.library_root.empty()) {
                    append_log("scan requires library_root in config.json");
                    set_status("Scan failed — set library_root");
                    break;
                }
                result = scan_rom_library(catalog, cfg, opts);
                merge_scan_into_index(library, catalog, result, cfg.library_root);
                save_library_index(paths.library_index_path, library);
                std::ostringstream oss;
                oss << rom_label << ": " << result.files.size() << " files, "
                    << result.matches.size() << " matches, hashed " << result.hashed_files
                    << ", skipped-hash " << result.skipped_hash << ", cache-hits "
                    << result.cache_hits;
                if (!plat_filter.empty()) oss << " [platform=" << plat_filter << "]";
                if (!result.scanned_roots.empty()) {
                    oss << "\n  roots:";
                    for (const auto& r : result.scanned_roots) oss << " " << r.string();
                }
                for (const auto& err : result.errors) oss << "\n  error: " << err;
                append_log(oss.str());

                // BIOS scan follows ROM scan (same scope / full flag).
                if (!cfg.bios_root.empty()) {
                    set_status(std::string(rom_label) + ": scanning BIOS…");
                    if (full && plat_filter.empty()) {
                        append_log("Full rebuild: clearing BIOS index cache…");
                        bios = BiosIndex{};
                    } else {
                        bios = load_bios_index(paths.bios_index_path);
                    }
                    BiosScanOptions bopts;
                    bopts.full_rescan = full;
                    bopts.index = full ? nullptr : &bios;
                    if (!plat_filter.empty()) bopts.platforms = {plat_filter};
                    bopts.on_progress = [this, full](const BiosScanProgress& p) {
                        const char* label = full ? "Full rebuild" : "Scan new files";
                        if (p.phase == "hash" && p.total > 0) {
                            std::ostringstream st;
                            st << label << ": BIOS hashing " << p.current << "/" << p.total;
                            if (!p.platform.empty()) st << " [" << p.platform << "]";
                            set_status(st.str());
                        } else if (p.phase == "walk") {
                            std::ostringstream st;
                            st << label << ": BIOS indexing " << p.current << " files…";
                            set_status(st.str());
                        }
                    };
                    auto bios_result = scan_bios_library(catalog, cfg, bopts);
                    merge_bios_scan_into_index(bios, catalog, bios_result, cfg.bios_root);
                    save_bios_index(paths.bios_index_path, bios);
                    std::ostringstream boss;
                    boss << rom_label << " BIOS: " << bios_result.files.size()
                         << " files, " << bios_result.matches.size() << " matches, hashed "
                         << bios_result.hashed_files;
                    for (const auto& err : bios_result.errors) boss << "\n  error: " << err;
                    append_log(boss.str());
                }

                // Discover / promote managed saves for titles in scope.
                {
                    set_status(std::string(rom_label) + ": scanning saves…");
                    int promoted = 0;
                    int found = 0;
                    for (const auto& t : catalog.titles) {
                        if (!plat_filter.empty() && t.platform != plat_filter) continue;
                        const fs::path rom = library.preferred_rom(t.id);
                        promoted += promote_install_saves_to_library(paths, cfg, t, rom);
                        found += static_cast<int>(list_managed_saves(paths, cfg, t).size());
                    }
                    std::ostringstream soss;
                    soss << rom_label << " saves: " << found << " file(s)";
                    if (promoted > 0) soss << ", promoted " << promoted << " from installs";
                    if (!plat_filter.empty()) soss << " [platform=" << plat_filter << "]";
                    append_log(soss.str());
                }

                // Install → Quick Scan follow-up: still missing → open folder.
                std::string scan_missing_id;
                {
                    std::lock_guard<std::mutex> lock(mu);
                    scan_missing_id = std::move(pending_scan_missing_rom_id);
                    pending_scan_missing_rom_id.clear();
                }
                if (!scan_missing_id.empty()) {
                    if (!library.preferred_rom(scan_missing_id).empty()) {
                        set_status("ROM found for " + scan_missing_id + " — ready to Install");
                        append_log("Missing-ROM scan: bound " + scan_missing_id);
                    } else {
                        std::string plat;
                        if (const Title* mt = catalog.find(scan_missing_id))
                            plat = mt->platform;
                        {
                            std::lock_guard<std::mutex> lock(mu);
                            open_rom_folder_prompt_id = scan_missing_id;
                            open_rom_folder_prompt_platform = plat;
                            open_rom_folder_prompt_pending.store(true);
                        }
                        set_status("Still no verified ROM for " + scan_missing_id);
                        append_log("Missing-ROM scan: no match for " + scan_missing_id +
                                   " — offering to open library folder");
                    }
                } else {
                    set_status(std::string(rom_label) + " complete");
                }
                finish_pending_import_toast(*this);
                break;
            }
            case HubJob::PurgeMissingFiles: {
                const std::string plat_filter = scans_platform_filter;
                set_status(plat_filter.empty() ? "Cleaning missing files…"
                                               : ("Cleaning missing [" + plat_filter + "]…"));
                library = load_library_index(paths.library_index_path);
                auto pr = purge_missing_library_files(library, plat_filter);
                save_library_index(paths.library_index_path, library);
                std::ostringstream oss;
                oss << "Clean missing: removed " << pr.removed_files << " ROM file(s)";
                if (pr.removed_title_binds)
                    oss << ", " << pr.removed_title_binds << " title bind(s)";
                if (!plat_filter.empty()) oss << " [platform=" << plat_filter << "]";
                for (size_t i = 0; i < pr.removed_paths.size() && i < 40; ++i)
                    oss << "\n  - " << pr.removed_paths[i];
                if (pr.removed_paths.size() > 40)
                    oss << "\n  … +" << (pr.removed_paths.size() - 40) << " more";
                append_log(oss.str());

                if (!cfg.bios_root.empty()) {
                    bios = load_bios_index(paths.bios_index_path);
                    auto bpr = purge_missing_bios_files(bios, catalog, plat_filter);
                    save_bios_index(paths.bios_index_path, bios);
                    std::ostringstream boss;
                    boss << "Clean missing BIOS: removed " << bpr.removed_files << " file(s)";
                    if (bpr.removed_title_binds)
                        boss << ", " << bpr.removed_title_binds << " title bind(s)";
                    append_log(boss.str());
                    pr.removed_files += bpr.removed_files;
                }
                set_status(pr.removed_files == 0
                               ? "Clean missing complete — nothing missing"
                               : ("Clean missing complete — removed " +
                                  std::to_string(pr.removed_files) + " file(s)"));
                break;
            }
            case HubJob::ScanBios:
            case HubJob::FullScanBios: {
                const bool full = (j == HubJob::FullScanBios);
                const std::string plat_filter = scans_platform_filter;
                set_status(full ? "Full BIOS rescan…" : "Scanning BIOS tree…");
                if (full && plat_filter.empty()) {
                    append_log("Full BIOS rescan: clearing BIOS index cache…");
                    bios = BiosIndex{};
                } else {
                    bios = load_bios_index(paths.bios_index_path);
                    append_log("BIOS scan: starting…");
                }
                BiosScanOptions opts;
                opts.full_rescan = full;
                opts.index = full ? nullptr : &bios;
                if (!plat_filter.empty()) opts.platforms = {plat_filter};
                opts.on_progress = [this, full](const BiosScanProgress& p) {
                    const char* label = full ? "Full BIOS rescan" : "BIOS scan";
                    if (p.phase == "walk") {
                        if (should_log_progress(p.current, 0, 50)) {
                            std::ostringstream oss;
                            oss << label << ": indexing ["
                                << (p.platform.empty() ? "flat" : p.platform) << "] "
                                << p.current << " files…";
                            append_log(oss.str());
                        }
                        std::ostringstream st;
                        st << label << ": indexing " << p.current << " files…";
                        set_status(st.str());
                    } else if (p.phase == "hash" && p.total > 0) {
                        std::ostringstream st;
                        st << label << ": hashing " << p.current << "/" << p.total;
                        if (!p.platform.empty()) st << " [" << p.platform << "]";
                        if (!p.path.empty()) st << " — " << p.path.filename().string();
                        set_status(st.str());
                        if (should_log_progress(p.current, p.total, full ? 5 : 10)) {
                            append_log(st.str());
                        }
                    } else if (p.phase == "match") {
                        append_log(std::string(label) + ": matching catalog…");
                        set_status(std::string(label) + ": matching catalog…");
                    }
                };
                if (cfg.bios_root.empty()) {
                    append_log("bios scan requires bios_root in config.json");
                    set_status("BIOS scan failed — set bios_root");
                    break;
                }
                auto result = scan_bios_library(catalog, cfg, opts);
                merge_bios_scan_into_index(bios, catalog, result, cfg.bios_root);
                save_bios_index(paths.bios_index_path, bios);
                std::ostringstream oss;
                oss << (full ? "Full BIOS rescan: " : "BIOS scan: ") << result.files.size()
                    << " files, " << result.matches.size() << " title bindings, cache-hits "
                    << result.cache_hits;
                append_log(oss.str());
                set_status(full ? "Full BIOS rescan complete" : "BIOS scan complete");
                finish_pending_import_toast(*this);
                break;
            }
            case HubJob::CheckUpdates: {
                set_status("Checking updates…");
                // Catalog → launcher → games → toolchain (prompt order matches).
                // Catalog sync always asks GitHub for the latest catalog release
                // (downloads only when newer). Game tags use force_github_tags so
                // the 4h release-tag TTL cannot hide a new title release — GitHub
                // latest is authoritative for installed versions.
                {
                    set_status("Checking catalog…");
                    auto cr = sync_remote_catalog(paths, cfg, /*force=*/false);
                    append_log(cr.message);
                    if (cr.ok && !cr.skipped) {
                        try {
                            catalog = load_catalog(paths.catalog_dir);
                        } catch (const std::exception& e) {
                            append_log(std::string("catalog reload: ") + e.what());
                        }
                    }
                }
                bool launcher_upd = false;
                {
                    set_status("Checking RetComM…");
                    auto lc = check_retcomm_update(paths);
                    append_log(lc.message);
                    std::lock_guard<std::mutex> lock(mu);
                    launcher_current_version = lc.current_tag;
                    launcher_latest_tag = lc.latest_tag;
                    // Do not open the modal yet — job_running would disable Update.
                    launcher_upd = lc.ok && lc.update_available;
                }
                set_status("Checking game updates…");
                refresh_rows(/*check_updates=*/true, /*force_github_tags=*/true);
                int game_updates = 0;
                {
                    std::lock_guard<std::mutex> lock(mu);
                    for (const auto& r : rows)
                        if (r.update_available) ++game_updates;
                    game_updates_prompt_count = game_updates;
                }
                if (game_updates > 0) {
                    append_log(std::to_string(game_updates) + " game update(s) available");
                }
                bool toolchain_upd = false;
                {
                    set_status("Checking toolchain…");
                    auto tc = check_toolchain_update(paths);
                    append_log(tc.message);
                    std::lock_guard<std::mutex> lock(mu);
                    toolchain_current_version = tc.current_version;
                    toolchain_latest_tag = tc.latest_tag;
                    toolchain_update_available = tc.update_available;
                    toolchain_status = tc.message;
                    toolchain_upd = tc.update_available;
                }
                if (launcher_upd) {
                    // Hold game/toolchain prompts until Later. Accepting self-update
                    // discards them so nothing else starts before restart.
                    // Arm the modal only after this job clears so Update is clickable.
                    {
                        std::lock_guard<std::mutex> lock(mu);
                        deferred_followup_updates = true;
                        deferred_game_updates_count = game_updates;
                        deferred_toolchain_prompt = toolchain_upd;
                        game_updates_prompt_pending.store(false);
                        toolchain_prompt_pending.store(false);
                        job_running = false;
                        job = HubJob::None;
                    }
                    set_status("Updates available");
                    launcher_update_prompt_pending.store(true);
                    return;
                }
                if (game_updates > 0) game_updates_prompt_pending.store(true);
                if (toolchain_upd) toolchain_prompt_pending.store(true);
                if (toolchain_prompt_pending.load() || game_updates > 0)
                    set_status("Updates available");
                else
                    set_status("Up to date");
                // Warm release zip cache so Apply Update is mostly extract.
                if (game_updates > 0) start_prefetch_updates();
                job_running = false;
                job = HubJob::None;
                return;
            }
            case HubJob::CheckLaunchUpdate:
                // Handled on launch_worker via start_job (overlaps Install).
                break;
            case HubJob::CheckToolchainUpdate: {
                set_status("Checking toolchain updates…");
                auto tc = check_toolchain_update(paths);
                append_log(tc.message);
                {
                    std::lock_guard<std::mutex> lock(mu);
                    toolchain_current_version = tc.current_version;
                    toolchain_latest_tag = tc.latest_tag;
                    toolchain_update_available = tc.update_available;
                    toolchain_status = tc.message;
                    if (tc.update_available) toolchain_prompt_pending.store(true);
                }
                if (tc.update_available)
                    set_status("Toolchain update available (" + tc.current_version + " → " +
                               tc.latest_tag + ")");
                else if (tc.ok)
                    set_status(tc.installed ? ("Toolchain up to date (" + tc.current_version + ")")
                                            : "No toolchain installed yet");
                else
                    set_status("Toolchain update check failed");
                break;
            }
            case HubJob::UpdateToolchain: {
                set_status("Updating portable toolchain…");
                append_log("Toolchain update: fetching latest cmake-clang-v1…");
                auto ur = update_toolchain_to_latest(
                    paths, [this](const std::string& msg, float) {
                        set_status(msg);
                        append_log(msg);
                    });
                append_log(ur.message);
                if (ur.ok) {
                    auto tc = check_toolchain_update(paths);
                    // Assign status under mu — do not call set_status/append_log
                    // while holding the lock (non-recursive mutex → SIGABRT).
                    std::lock_guard<std::mutex> lock(mu);
                    toolchain_current_version = tc.current_version.empty() ? ur.tag
                                                                          : tc.current_version;
                    toolchain_latest_tag = tc.latest_tag;
                    toolchain_update_available = false;
                    toolchain_status = "toolchain " + toolchain_current_version;
                    status = "Toolchain updated (" + toolchain_current_version + ")";
                } else {
                    set_status("Toolchain update failed");
                }
                break;
            }
            case HubJob::FetchBoxart: {
                fetch_boxart_for_catalog(force_boxart);
                break;
            }
            case HubJob::FetchRommRom: {
                const auto* t = find_title();
                if (!t) {
                    append_log("unknown title: " + title_id);
                    break;
                }
                if (!fetch_romm_and_bind(*t)) {
                    std::string st;
                    {
                        std::lock_guard<std::mutex> lock(mu);
                        st = status;
                    }
                    if (st.find("incomplete") == std::string::npos)
                        set_status("RomM ROM fetch failed: " + title_id);
                    break;
                }
                break;
            }
            case HubJob::ScanRommRoms: {
                set_status("Scanning RomM for catalog ROMs…");
                auto sr = scan_romm_rom_index(paths, cfg, catalog, [this](const std::string& s) {
                    set_status(s);
                });
                append_log(sr.message);
                if (sr.ok) {
                    romm_roms = load_romm_rom_index(paths.romm_rom_index_path);
                    set_status(sr.message);
                } else {
                    set_status("RomM scan failed");
                }
                break;
            }
            case HubJob::FetchRommBios: {
                set_status("RomM: finding BIOS for " + title_id + "…");
                const auto* t = find_title();
                if (!t) {
                    append_log("unknown title: " + title_id);
                    break;
                }
                auto fr = fetch_bios_from_romm(cfg, *t, [this](const std::string& s) {
                    set_status(s);
                });
                append_log(fr.message);
                if (!fr.ok) {
                    set_status("RomM BIOS fetch failed: " + title_id);
                    break;
                }
                set_status("RomM: scanning BIOS tree…");
                bios = load_bios_index(paths.bios_index_path);
                BiosScanOptions opts;
                opts.index = &bios;
                auto scan = scan_bios_library(catalog, cfg, opts);
                merge_bios_scan_into_index(bios, catalog, scan, cfg.bios_root);
                save_bios_index(paths.bios_index_path, bios);
                append_log("BIOS scan after download: " + std::to_string(scan.matches.size()) +
                           " title bindings");
                set_status("RomM BIOS ready: " + title_id);
                break;
            }
            case HubJob::SyncRommSaves:
            case HubJob::SyncRommStates: {
                // One hub action syncs native saves + savestates together.
                set_status("RomM: syncing saves for " + title_id + "…");
                const auto* t = find_title();
                if (!t) {
                    append_log("unknown title: " + title_id);
                    break;
                }
                auto sr = sync_saves_with_romm(paths, cfg, *t, [this](const std::string& s) {
                    set_status(s);
                });
                append_log(sr.message);
                set_status("RomM: syncing savestates for " + title_id + "…");
                auto st = sync_states_with_romm(paths, cfg, *t, [this](const std::string& s) {
                    set_status(s);
                });
                append_log(st.message);
                const bool ok = sr.ok && st.ok;
                set_status(ok ? ("Save sync complete: " + title_id)
                              : ("Save sync finished with errors: " + title_id));
                break;
            }
            case HubJob::SelfUpdate: {
                // Do not surface game/toolchain prompts while we download + restart.
                // Stop background prefetch so exit is not blocked waiting on zip downloads.
                cancel_prefetch_updates();
                discard_followup_update_prompts();
                set_status("Checking RetComM Launcher updates…");
                const auto install = retcomm_install_info();
                append_log("Self-update: current=" + retcomm_app_version() +
                           " channel=" + install.channel_id + " repo=" + retcomm_github_slug());
                if (!install.path.empty())
                    append_log("Self-update: path=" + install.path.string());
                auto ur = self_update_retcomm(paths, {});
                append_log(ur.message);
                {
                    std::lock_guard<std::mutex> lock(mu);
                    // Keep displaying the running binary version until restart applies the new one.
                    launcher_version = retcomm_app_version();
                }
                if (ur.ok && ur.restart_scheduled) {
                    discard_followup_update_prompts();
                    set_status("Updating RetComM — restarting…");
                    request_exit.store(true);
                    job_running = false;
                    job = HubJob::None;
                    return;
                }
                if (ur.ok && ur.skipped)
                    set_status("RetComM up to date (" + ur.current_tag + ")");
                else if (ur.ok)
                    set_status("RetComM update complete");
                else
                    set_status("RetComM update failed");
                break;
            }
            case HubJob::RefreshCatalog: {
                set_status("Refreshing catalog…");
                append_log("Catalog refresh: url=" +
                           (cfg.catalog.url.empty() ? default_catalog_download_url()
                                                    : cfg.catalog.url));
                auto cr = sync_remote_catalog(paths, cfg, true);
                append_log(cr.message);
                if (!cr.ok) {
                    set_status("Catalog refresh failed");
                    break;
                }
                try {
                    catalog = load_catalog(paths.catalog_dir);
                    append_log("Catalog loaded: " + paths.catalog_dir.string() + " (" +
                               std::to_string(catalog.titles.size()) + " titles)");
                    set_status("Catalog refreshed (" + std::to_string(catalog.titles.size()) +
                               " titles)");
                } catch (const std::exception& e) {
                    append_log(std::string("catalog reload failed: ") + e.what());
                    set_status("Catalog refresh failed — reload error");
                    break;
                }
                // Rematch BIOS against the new catalog from cached dumps (no rehash).
                {
                    bios = load_bios_index(paths.bios_index_path);
                    const size_t n = rematch_bios_titles(bios, catalog);
                    save_bios_index(paths.bios_index_path, bios);
                    append_log("BIOS rematch: " + std::to_string(n) +
                               " title binding(s) from index");
                }
                // Fill covers for any new/missing titles (skip already-cached).
                if (!cr.skipped) {
                    library = load_library_index(paths.library_index_path);
                    fetch_boxart_for_catalog(false);
                }
                {
                    const size_t n = refresh_orphan_installs();
                    if (n > 0) {
                        append_log("Catalog: " + std::to_string(n) +
                                   " local install(s) no longer in catalog");
                        set_status("Catalog refreshed — " + std::to_string(n) +
                                   " unlisted install(s)");
                        orphan_prompt_pending.store(true);
                    }
                }
                break;
            }
            case HubJob::CleanupOrphans:
            case HubJob::CleanupOrphansPurge: {
                const bool purge = (j == HubJob::CleanupOrphansPurge);
                set_status(purge ? "Removing unlisted installs (delete saves)…"
                                 : "Removing unlisted installs (keep saves)…");
                OrphanCleanupOptions opts;
                opts.keep_saves = !purge;
                opts.prune_indexes = true;
                auto cr = cleanup_removed_catalog_titles(paths, catalog, opts);
                for (const auto& m : cr.messages) append_log(m);
                append_log(cr.message);
                library = load_library_index(paths.library_index_path);
                bios = load_bios_index(paths.bios_index_path);
                romm_roms = load_romm_rom_index(paths.romm_rom_index_path);
                app_state = load_app_state(paths.state_path);
                refresh_orphan_installs();
                set_status(cr.ok ? ("Removed " + std::to_string(cr.removed) + " unlisted install(s)")
                                 : ("Orphan cleanup finished with " + std::to_string(cr.failed) +
                                    " error(s)"));
                break;
            }
            case HubJob::CleanupOldReleases: {
                set_status("Cleaning old update files…");
                auto cr = cleanup_old_release_dirs(paths, catalog);
                for (const auto& m : cr.messages) append_log(m);
                append_log(cr.message);
                set_status(cr.message);
                break;
            }
            case HubJob::CleanupCmakeBuildDirs: {
                set_status("Cleaning cmake build directories…");
                std::unordered_map<std::string, std::string> build_dir_by_install;
                for (const auto& t : catalog.titles) {
                    if (t.install_dir_name.empty()) continue;
                    build_dir_by_install[t.install_dir_name] =
                        t.build.cmake.build_dir.empty() ? "build" : t.build.cmake.build_dir;
                }
                int removed = 0;
                int failed = 0;
                int installs_scanned = 0;
                std::error_code ec;
                for (const auto& root : scan_install_roots(cfg, paths)) {
                    if (root.path.empty() || !fs::is_directory(root.path, ec)) continue;
                    for (auto it = fs::directory_iterator(root.path, ec);
                         !ec && it != fs::directory_iterator(); it.increment(ec)) {
                        if (!it->is_directory(ec)) continue;
                        const fs::path install_root = it->path();
                        const fs::path src_base = install_root / "src";
                        if (!fs::is_directory(src_base, ec)) continue;
                        ++installs_scanned;
                        const std::string install_name = install_root.filename().string();
                        std::string build_rel = "build";
                        if (const auto found = build_dir_by_install.find(install_name);
                            found != build_dir_by_install.end())
                            build_rel = found->second;
                        auto wipe_build = [&](const fs::path& build_dir) {
                            if (!fs::is_directory(build_dir, ec)) return;
                            ec.clear();
                            fs::remove_all(build_dir, ec);
                            if (!ec) {
                                ++removed;
                                append_log("Removed " + build_dir.string());
                            } else {
                                ++failed;
                                append_log("Failed to remove " + build_dir.string() + ": " +
                                           ec.message());
                            }
                        };
                        for (auto sit = fs::directory_iterator(src_base, ec);
                             !ec && sit != fs::directory_iterator(); sit.increment(ec)) {
                            if (!sit->is_directory(ec)) continue;
                            const auto name = sit->path().filename().string();
                            if (name.empty() || name[0] == '.') continue;
                            wipe_build(sit->path() / build_rel);
                            if (build_rel != "build") wipe_build(sit->path() / "build");
                        }
                    }
                }
                std::ostringstream oss;
                oss << "Cleaned cmake build dirs: removed " << removed << " under "
                    << installs_scanned << " install(s)";
                if (failed > 0) oss << " (" << failed << " failed)";
                append_log(oss.str());
                set_status(oss.str());
                break;
            }
            case HubJob::DeleteAllAppsAndSaves: {
                set_status("Deleting all apps and save data…");
                int removed = 0;
                int failed = 0;
                std::error_code ec;
                auto same_path = [](const fs::path& a, const fs::path& b) {
                    if (a.empty() || b.empty()) return false;
                    std::error_code lec;
                    if (fs::equivalent(a, b, lec)) return true;
                    lec.clear();
                    const fs::path ca = fs::weakly_canonical(a, lec);
                    const fs::path cb = fs::weakly_canonical(b, lec);
                    if (!lec && !ca.empty() && !cb.empty()) return ca == cb;
                    return a == b;
                };

                UninstallOptions uopts;
                uopts.keep_saves = false;
                for (const auto& root : scan_install_roots(cfg, paths)) {
                    if (root.path.empty() || !fs::is_directory(root.path, ec)) continue;
                    std::vector<fs::path> children;
                    for (auto it = fs::directory_iterator(root.path, ec);
                         !ec && it != fs::directory_iterator(); it.increment(ec)) {
                        if (it->is_directory(ec)) children.push_back(it->path());
                    }
                    for (const auto& install_root : children) {
                        auto ur = uninstall_install_root(paths, install_root, uopts,
                                                         install_root.filename().string());
                        append_log(ur.message);
                        if (ur.ok && !ur.skipped) ++removed;
                        else if (!ur.ok) ++failed;
                    }
                }

                // Managed saves root (…/<platform>/<title>/). Never wipe library/BIOS roots.
                if (!cfg.saves_root.empty() && fs::is_directory(cfg.saves_root, ec) &&
                    !same_path(cfg.saves_root, cfg.library_root) &&
                    !same_path(cfg.saves_root, cfg.bios_root)) {
                    int wiped = 0;
                    for (auto it = fs::directory_iterator(cfg.saves_root, ec);
                         !ec && it != fs::directory_iterator(); it.increment(ec)) {
                        ec.clear();
                        fs::remove_all(it->path(), ec);
                        if (!ec) {
                            ++wiped;
                            append_log("Removed " + it->path().string());
                        } else {
                            ++failed;
                            append_log("Failed to remove " + it->path().string() + ": " +
                                       ec.message());
                        }
                    }
                    append_log("Cleared saves_root (" + std::to_string(wiped) + " entries): " +
                               cfg.saves_root.string());
                } else if (!cfg.saves_root.empty()) {
                    append_log("Skipped saves_root cleanup (missing or unsafe path): " +
                               cfg.saves_root.string());
                }

                // Global platform Configure prefs (settings.toml / config.ini).
                {
                    const fs::path platform_dir = paths.data_dir / "platform";
                    if (fs::is_directory(platform_dir, ec)) {
                        ec.clear();
                        fs::remove_all(platform_dir, ec);
                        if (!ec) append_log("Removed " + platform_dir.string());
                        else {
                            ++failed;
                            append_log("Failed to remove " + platform_dir.string() + ": " +
                                       ec.message());
                        }
                    }
                }

                // Drop per-title save/config prefs from state.json (keep BIOS choices).
                {
                    AppState st = load_app_state(paths.state_path);
                    st.preferred_save.clear();
                    st.preferred_save_card2.clear();
                    st.exclude_platform_config.clear();
                    std::string err;
                    if (!save_app_state(paths.state_path, st, &err)) {
                        ++failed;
                        append_log("Failed to update state.json: " + err);
                    } else {
                        append_log("Cleared preferred save / platform-config exclusions");
                    }
                    app_state = std::move(st);
                }

                refresh_orphan_installs();
                std::ostringstream oss;
                oss << "Deleted " << removed << " install(s)";
                if (failed > 0) oss << " (" << failed << " error(s))";
                append_log(oss.str());
                set_status(oss.str());
                show_toast(failed == 0 ? "Deleted all apps & save data"
                                       : "Delete finished with errors — see Activity");
                break;
            }
            case HubJob::HardResetLibrarySettings: {
                set_status("Hard-resetting library settings…");
                int failed = 0;
                std::error_code ec;
                auto wipe_file = [&](const fs::path& p, const char* label) {
                    if (p.empty() || !fs::exists(p, ec)) return;
                    ec.clear();
                    fs::remove(p, ec);
                    if (!ec) {
                        append_log(std::string("Removed ") + label + ": " + p.string());
                    } else {
                        ++failed;
                        append_log(std::string("Failed to remove ") + label + ": " + p.string() +
                                   " (" + ec.message() + ")");
                    }
                };

                // Library + RomM config, scan databases, and RomM availability cache.
                wipe_file(paths.config_path, "config");
                wipe_file(paths.library_index_path, "library index");
                wipe_file(paths.bios_index_path, "BIOS index");
                wipe_file(paths.romm_rom_index_path, "RomM ROM index");

                std::string marker_err;
                if (!clear_hub_setup_completed(paths, exe_dir, &marker_err)) {
                    ++failed;
                    append_log("Failed to clear setup marker: " + marker_err);
                } else {
                    append_log("Cleared first-run setup marker");
                }

                {
                    std::lock_guard<std::mutex> lock(mu);
                    cfg = AppConfig{};
                    library = LibraryIndex{};
                    bios = BiosIndex{};
                    romm_roms = RommRomIndex{};
                    show_settings = false;
                    show_romm_settings = false;
                    show_psx_settings = false;
                    settings.dirty = false;
                    romm_settings.dirty = false;
                }

                std::string relaunch_err;
                if (!schedule_retcomm_relaunch(&relaunch_err)) {
                    ++failed;
                    append_log("Relaunch failed: " + relaunch_err);
                    set_status("Hard reset incomplete — relaunch failed (see Activity)");
                    break;
                }

                append_log(failed == 0 ? "Hard reset complete — restarting into setup…"
                                       : "Hard reset finished with errors — restarting…");
                set_status("Restarting into first-time setup…");
                discard_followup_update_prompts();
                request_exit.store(true);
                job_running = false;
                job = HubJob::None;
                return;
            }
            case HubJob::None:
                break;
            }

            refresh_rows(false);
        } catch (const std::exception& e) {
            append_log(std::string("error: ") + e.what());
            set_status("Error");
        }
        job_running = false;
        job = HubJob::None;
    });
    return true;
}

void HubModel::join_worker() {
    if (worker.joinable()) worker.join();
    if (launch_worker.joinable()) launch_worker.join();
    if (prefetch_worker.joinable()) {
        // Self-update apply scripts wait for this PID. Never block exit on prefetch.
        if (request_exit.load()) {
            cancel_prefetch_updates();
            prefetch_worker.detach();
        } else {
            prefetch_worker.join();
        }
    }
}

void HubModel::discard_followup_update_prompts() {
    std::lock_guard<std::mutex> lock(mu);
    deferred_followup_updates = false;
    deferred_game_updates_count = 0;
    deferred_toolchain_prompt = false;
    game_updates_prompt_pending.store(false);
    toolchain_prompt_pending.store(false);
}

void HubModel::cancel_prefetch_updates() {
    prefetch_cancel.store(true);
}

void HubModel::release_deferred_followup_updates() {
    int games = 0;
    bool toolchain = false;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (!deferred_followup_updates) return;
        deferred_followup_updates = false;
        games = deferred_game_updates_count;
        toolchain = deferred_toolchain_prompt;
        deferred_game_updates_count = 0;
        deferred_toolchain_prompt = false;
        if (games > 0) {
            game_updates_prompt_count = games;
            game_updates_prompt_pending.store(true);
        }
        if (toolchain) toolchain_prompt_pending.store(true);
    }
    if (games > 0) start_prefetch_updates();
}

void HubModel::start_prefetch_updates(const std::vector<std::string>& title_ids) {
    std::vector<std::string> ids = title_ids;
    if (ids.empty()) {
        std::lock_guard<std::mutex> lock(mu);
        for (const auto& r : rows) {
            if (r.update_available && r.installed) ids.push_back(r.id);
        }
    }
    if (ids.empty()) return;
    if (request_exit.load()) return;
    if (prefetch_running.exchange(true)) {
        // Already prefetching — a second pass can wait for the next Check Updates.
        return;
    }
    prefetch_cancel.store(false);
    if (prefetch_worker.joinable()) prefetch_worker.join();
    prefetch_worker = std::thread([this, ids = std::move(ids)] {
        try {
            ensure_dirs(paths);
            append_log("Prefetching " + std::to_string(ids.size()) + " update download(s)…");
            int ready = 0;
            for (const auto& id : ids) {
                if (request_exit.load() || prefetch_cancel.load()) break;
                const Title* t = catalog.find(id);
                if (!t || t->release.github.empty()) continue;
                if (!job_running.load()) set_status("Prefetching " + id + "…");
                InstallOptions opts;
                opts.check_latest = true;
                auto pr = prefetch_title_release(paths, *t, opts);
                append_log(pr.message);
                if (pr.ok) ++ready;
            }
            if (prefetch_cancel.load() || request_exit.load())
                append_log("Prefetch cancelled");
            else
                append_log("Prefetch complete (" + std::to_string(ready) + "/" +
                           std::to_string(ids.size()) + " ready)");
            if (!job_running.load() && !request_exit.load()) set_status("Prefetch complete");
        } catch (const std::exception& e) {
            append_log(std::string("prefetch error: ") + e.what());
        }
        prefetch_running = false;
    });
}

void HubModel::seed_setup_platform_folders() {
    settings.platform_folders.clear();
    // Prefer catalog platforms, then any extra keys already in config / defaults.
    std::vector<std::string> plats;
    {
        std::unordered_set<std::string> seen;
        for (const auto& t : catalog.titles) {
            if (seen.insert(t.platform).second) plats.push_back(t.platform);
        }
        for (const auto& [plat, _] : cfg.platform_folders) {
            if (seen.insert(plat).second) plats.push_back(plat);
        }
        for (const auto& [plat, _] : default_platform_folders()) {
            if (seen.insert(plat).second) plats.push_back(plat);
        }
    }
    for (const auto& plat : plats) {
        PlatformFolderEdit row;
        copy_buf(row.platform, sizeof(row.platform), plat);
        copy_buf(row.folders, sizeof(row.folders), join_csv(cfg.folders_for_platform(plat)));
        settings.platform_folders.push_back(row);
    }
    settings.dirty = true;
}

void HubModel::apply_suggested_library_roots(bool overwrite_nonempty) {
    const auto sug = suggested_emulation_roots();
    if (sug.library_root.empty()) return;
    auto fill = [&](char* buf, size_t n, const fs::path& path) {
        if (!overwrite_nonempty && buf[0] != '\0') return;
        copy_buf(buf, n, path.string());
        settings.dirty = true;
    };
    fill(settings.library_root, sizeof(settings.library_root), sug.library_root);
    fill(settings.bios_root, sizeof(settings.bios_root), sug.bios_root);
    fill(settings.saves_root, sizeof(settings.saves_root), sug.saves_root);
}

void HubModel::apply_suggested_emulation_root(bool overwrite_nonempty) {
    if (!overwrite_nonempty && setup_emulation_root[0] != '\0') return;
    const fs::path home = user_home_dir();
    if (home.empty()) return;
    copy_buf(setup_emulation_root, sizeof(setup_emulation_root), (home / "Emulation").string());
}

void HubModel::apply_roots_from_emulation_parent() {
    if (setup_emulation_root[0] == '\0') return;
    const fs::path emu(setup_emulation_root);
    copy_buf(settings.library_root, sizeof(settings.library_root), (emu / "roms").string());
    copy_buf(settings.bios_root, sizeof(settings.bios_root), (emu / "bios").string());
    copy_buf(settings.saves_root, sizeof(settings.saves_root), (emu / "saves").string());
    settings.dirty = true;
}

void HubModel::collect_missing_setup_roots() {
    setup_missing_roots.clear();
    auto consider = [&](const char* path) {
        if (!path || !path[0]) return;
        std::error_code ec;
        if (!fs::is_directory(path, ec)) setup_missing_roots.push_back(path);
    };
    consider(settings.library_root);
    consider(settings.bios_root);
    consider(settings.saves_root);
}

bool HubModel::create_missing_setup_roots(std::string* error) {
    for (const auto& path : setup_missing_roots) {
        std::error_code ec;
        fs::create_directories(path, ec);
        if (ec) {
            if (error) *error = "cannot create " + path + ": " + ec.message();
            return false;
        }
        append_log("Created " + path);
    }
    setup_missing_roots.clear();
    setup_confirm_create_roots = false;
    return true;
}

bool HubModel::create_setup_platform_folders(std::string* error) {
    // Persist draft into cfg first so ensure_configured_platform_dirs sees mappings.
    AppConfig next = cfg;
    next.library_root = settings.library_root;
    next.bios_root = settings.bios_root;
    next.saves_root = settings.saves_root;
    if (!settings.platform_folders.empty()) {
        next.platform_folders.clear();
        for (const auto& row : settings.platform_folders) {
            if (row.platform[0] == '\0') continue;
            auto folders = split_csv(row.folders);
            if (folders.empty()) continue;
            next.platform_folders[row.platform] = std::move(folders);
        }
    }
    next = normalize_config(std::move(next));
    if (!ensure_configured_platform_dirs(next, error)) return false;
    append_log("Created missing platform folders under library/BIOS/saves roots");
    return true;
}

bool HubModel::finish_easy_setup(std::string* error) {
    if (setup_emulation_root[0] == '\0') {
        if (error) *error = "choose an Emulation folder";
        return false;
    }
    apply_roots_from_emulation_parent();
    seed_setup_platform_folders();
    collect_missing_setup_roots();
    if (!setup_missing_roots.empty() && !create_missing_setup_roots(error)) return false;
    if (!save_settings(error)) return false;
    if (setup_create_platform_folders && !create_setup_platform_folders(error)) return false;
    // Easy path skips RomM; keep any existing token blanks already in drafts.
    if (!save_romm_settings(error, /*refresh_boxart=*/false)) return false;
    if (!complete_setup(error)) return false;
    show_setup_scan_prompt = true;
    return true;
}

void HubModel::open_settings() {
    cfg = load_app_config(paths.config_path);
    copy_buf(settings.library_root, sizeof(settings.library_root), cfg.library_root.string());
    copy_buf(settings.bios_root, sizeof(settings.bios_root), cfg.bios_root.string());
    copy_buf(settings.saves_root, sizeof(settings.saves_root), cfg.saves_root.string());
    copy_buf(settings.exclude_dirs, sizeof(settings.exclude_dirs), join_csv(cfg.exclude_dirs));
    settings.prefer_local_boxart = cfg.prefer_local_boxart;
    settings.filter_unsupported_titles = cfg.filter_unsupported_titles;
    settings.check_updates_on_startup = cfg.check_updates_on_startup;
    settings.check_updates_before_launch = cfg.check_updates_before_launch;
    settings.auto_clean_build_dirs = cfg.auto_clean_build_dirs;
    settings.install_roots.clear();
    settings.default_install_root_index = 0;
    {
        const auto roots = effective_install_roots(cfg, paths);
        const fs::path def = resolve_default_install_root(cfg, paths);
        for (size_t i = 0; i < roots.size(); ++i) {
            InstallRootEdit row;
            copy_buf(row.label, sizeof(row.label), roots[i].label);
            copy_buf(row.path, sizeof(row.path), roots[i].path.string());
            settings.install_roots.push_back(row);
            if (roots[i].path == def)
                settings.default_install_root_index = static_cast<int>(i);
        }
    }

    seed_setup_platform_folders();
    settings.dirty = false;
    show_romm_settings = false;
    show_psx_settings = false;
    show_setup = false;
    show_settings = true;
}

void HubModel::open_setup() {
    // Reuse the same draft buffers as Library / RomM settings.
    // Pre-populate from existing config.json when present (e.g. reinstall).
    cfg = load_app_config(paths.config_path);
    copy_buf(settings.library_root, sizeof(settings.library_root), cfg.library_root.string());
    copy_buf(settings.bios_root, sizeof(settings.bios_root), cfg.bios_root.string());
    copy_buf(settings.saves_root, sizeof(settings.saves_root), cfg.saves_root.string());
    copy_buf(settings.exclude_dirs, sizeof(settings.exclude_dirs), join_csv(cfg.exclude_dirs));
    settings.prefer_local_boxart = cfg.prefer_local_boxart;
    settings.filter_unsupported_titles = cfg.filter_unsupported_titles;
    settings.check_updates_on_startup = cfg.check_updates_on_startup;
    settings.check_updates_before_launch = cfg.check_updates_before_launch;
    settings.auto_clean_build_dirs = cfg.auto_clean_build_dirs;
    settings.platform_folders.clear();
    settings.dirty = false;
    apply_suggested_library_roots(/*overwrite_nonempty=*/false);
    setup_emulation_root[0] = '\0';
    // Prefer parent of an existing library root (…/roms → …), else ~/Emulation.
    if (settings.library_root[0] != '\0') {
        const fs::path lib(settings.library_root);
        if (lib.filename() == "roms" && !lib.parent_path().empty())
            copy_buf(setup_emulation_root, sizeof(setup_emulation_root),
                     lib.parent_path().string());
    }
    apply_suggested_emulation_root(/*overwrite_nonempty=*/false);
    setup_path = SetupPath::Chooser;
    setup_step = 0;
    setup_confirm_create_roots = false;
    setup_missing_roots.clear();
    setup_create_platform_folders = true;
    show_setup_scan_prompt = false;
    copy_buf(romm_settings.base_url, sizeof(romm_settings.base_url), cfg.romm.base_url);
    copy_buf(romm_settings.api_token, sizeof(romm_settings.api_token), cfg.romm.api_token);
    romm_settings.sync_boxart = cfg.romm.sync_boxart;
    romm_settings.dirty = false;
    show_settings = false;
    show_romm_settings = false;
    show_setup = true;
}

bool HubModel::complete_setup(std::string* error) {
    if (!mark_hub_setup_completed(paths, exe_dir, error)) return false;
    show_setup = false;
    setup_path = SetupPath::Chooser;
    setup_step = 0;
    setup_confirm_create_roots = false;
    setup_missing_roots.clear();
    return true;
}

void HubModel::apply_pending_folder_pick() {
    std::string path;
    FolderPickTarget target = FolderPickTarget::None;
    {
        std::lock_guard<std::mutex> lock(folder_pick_mu);
        if (folder_pick_path.empty()) return;
        path = std::move(folder_pick_path);
        folder_pick_path.clear();
        target = folder_pick_target;
        folder_pick_target = FolderPickTarget::None;
        folder_pick_busy = false;
    }
    if (path.empty() || target == FolderPickTarget::None) return;

    if (target == FolderPickTarget::LibraryRoot) {
        copy_buf(settings.library_root, sizeof(settings.library_root), path);
        settings.dirty = true;
        set_status("Library folder selected");
    } else if (target == FolderPickTarget::BiosRoot) {
        copy_buf(settings.bios_root, sizeof(settings.bios_root), path);
        settings.dirty = true;
        set_status("BIOS folder selected");
    } else if (target == FolderPickTarget::SavesRoot) {
        copy_buf(settings.saves_root, sizeof(settings.saves_root), path);
        settings.dirty = true;
        set_status("Saves folder selected");
    } else if (target == FolderPickTarget::EmulationRoot) {
        copy_buf(setup_emulation_root, sizeof(setup_emulation_root), path);
        apply_roots_from_emulation_parent();
        set_status("Emulation folder selected");
    } else if (target == FolderPickTarget::InstallRoot) {
        if (folder_pick_install_index >= 0 &&
            folder_pick_install_index < static_cast<int>(settings.install_roots.size())) {
            auto& row = settings.install_roots[static_cast<size_t>(folder_pick_install_index)];
            copy_buf(row.path, sizeof(row.path), path);
            if (row.label[0] == '\0')
                copy_buf(row.label, sizeof(row.label), fs::path(path).filename().string());
            settings.dirty = true;
            set_status("Install folder selected");
        }
        folder_pick_install_index = -1;
    }
}

void HubModel::apply_pending_file_pick() {
    FilePickKind kind = FilePickKind::None;
    std::string platform;
    std::string title_id;
    std::vector<std::string> picked;
    {
        std::lock_guard<std::mutex> lock(file_pick_mu);
        if (file_pick_paths.empty() || file_pick_kind == FilePickKind::None) return;
        kind = file_pick_kind;
        platform = file_pick_platform;
        title_id = std::move(file_pick_title_id);
        file_pick_title_id.clear();
        picked = std::move(file_pick_paths);
        file_pick_paths.clear();
        file_pick_kind = FilePickKind::None;
        file_pick_platform.clear();
        file_pick_busy = false;
    }
    if (picked.empty()) return;
    if (platform.empty() && title_id.empty()) return;

    cfg = load_app_config(paths.config_path);
    std::error_code ec;
    auto copy_into = [&](const fs::path& dest_dir) -> bool {
        fs::create_directories(dest_dir, ec);
        if (ec) {
            append_log("Import: cannot create " + dest_dir.string() + ": " + ec.message());
            return false;
        }
        int ok = 0;
        for (const auto& p : picked) {
            const fs::path src(p);
            if (!fs::is_regular_file(src, ec)) {
                append_log("Import: not a file: " + p);
                continue;
            }
            const fs::path dest = dest_dir / src.filename();
            fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                append_log("Import failed (" + src.filename().string() + "): " + ec.message());
                continue;
            }
            append_log("Imported " + dest.string());
            ++ok;
        }
        return ok > 0;
    };

    if (kind == FilePickKind::ImportRom) {
        if (cfg.library_root.empty()) {
            set_status("Import ROM failed — set library_root in Library Settings");
            return;
        }
        fs::path dest_dir =
            ensure_platform_dir(cfg.library_root, cfg.folders_for_platform(platform));
        if (dest_dir.empty()) {
            set_status("Import ROM failed — cannot create platform folder");
            return;
        }
        if (picked.size() >= 2) {
            std::string set_name;
            for (const auto& p : picked) {
                const fs::path src(p);
                std::string ext = src.extension().string();
                for (char& c : ext)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (ext == ".cue") {
                    set_name = src.stem().string();
                    break;
                }
            }
            if (set_name.empty()) set_name = fs::path(picked.front()).stem().string();
            if (!set_name.empty()) dest_dir /= set_name;
        }
        if (!copy_into(dest_dir)) {
            set_status("Import ROM failed");
            return;
        }
        {
            std::string name = fs::path(picked.front()).filename().string();
            for (const auto& p : picked) {
                std::string ext = fs::path(p).extension().string();
                for (char& c : ext)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (ext == ".cue") {
                    name = fs::path(p).filename().string();
                    break;
                }
            }
            std::lock_guard<std::mutex> lock(mu);
            pending_import_toast_name = std::move(name);
            pending_import_toast_platform = platform;
            pending_import_toast_kind = "rom";
        }
        scans_platform_filter = platform;
        pending_scan_missing_rom_id.clear();
        set_status("Imported ROM — scanning " + std::string(platform_label(platform)) + "…");
        start_job(HubJob::ScanRoms);
        return;
    }

    if (kind == FilePickKind::ImportSave) {
        fs::path dest_dir;
        const Title* bind_title = nullptr;
        if (!title_id.empty()) {
            bind_title = catalog.find(title_id);
            if (bind_title)
                dest_dir = title_saves_dir(paths, cfg, *bind_title, true);
        }
        if (dest_dir.empty()) dest_dir = cfg.saves_dir_for_platform(platform, true);
        if (dest_dir.empty()) {
            set_status("Import save failed — set saves_root in Library Settings");
            return;
        }
        if (!copy_into(dest_dir)) {
            set_status("Import save failed");
            return;
        }
        const std::string name = fs::path(picked.front()).filename().string();
        if (bind_title) {
            const std::string save_id = "saves/" + name;
            app_state = load_app_state(paths.state_path);
            set_preferred_save(app_state, bind_title->id, save_id);
            save_app_state(paths.state_path, app_state, nullptr);
            show_toast("Imported " + name + " → " + bind_title->name);
        } else {
            show_toast("Imported " + name + " → " + platform_label(platform) + " save ready.");
        }
        refresh_rows(false);
        set_status("Imported save");
        return;
    }

    if (kind == FilePickKind::ImportBios) {
        if (cfg.bios_root.empty()) {
            set_status("Import BIOS failed — set bios_root in Library Settings");
            return;
        }
        const fs::path dest_dir =
            ensure_platform_dir(cfg.bios_root, cfg.folders_for_platform(platform));
        if (dest_dir.empty()) {
            set_status("Import BIOS failed — cannot create BIOS folder");
            return;
        }
        if (!copy_into(dest_dir)) {
            set_status("Import BIOS failed");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mu);
            pending_import_toast_name = fs::path(picked.front()).filename().string();
            pending_import_toast_platform = platform;
            pending_import_toast_kind = "bios";
        }
        scans_platform_filter = platform;
        set_status("Imported BIOS — scanning " + std::string(platform_label(platform)) + "…");
        start_job(HubJob::ScanBios);
        return;
    }
}

void HubModel::add_platform_folder_row() {
    PlatformFolderEdit row;
    copy_buf(row.platform, sizeof(row.platform), "");
    copy_buf(row.folders, sizeof(row.folders), "");
    settings.platform_folders.push_back(row);
    settings.dirty = true;
}

void HubModel::add_install_root_row() {
    InstallRootEdit row;
    copy_buf(row.label, sizeof(row.label), "Games");
    row.path[0] = '\0';
    settings.install_roots.push_back(row);
    settings.dirty = true;
}

bool HubModel::begin_install(const std::string& title_id) {
    const Title* t = catalog.find(title_id);
    if (!t) {
        append_log("unknown title: " + title_id);
        set_status("Unknown title");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mu);
        job_apps_dir.clear();
        show_install_root_prompt = false;
        install_root_prompt_id.clear();
        install_root_prompt_move = false;
        install_root_prompt_from_apps.clear();
    }
    cfg = load_app_config(paths.config_path);
    const auto existing = inspect_install_any(paths, cfg, *t);
    const auto roots = effective_install_roots(cfg, paths);

    if (roots.size() <= 1) {
        if (existing.installed || existing.install_dir_present || existing.has_preserved_state)
            job_apps_dir = existing.install_root.parent_path();
        else
            job_apps_dir = resolve_default_install_root(cfg, paths);
        if (t->supports_local_build() && !t->supports_prebuilt_install()) {
            if (prepare_build_rom_or_prompt(title_id)) return true;
        }
        return start_job(HubJob::Install, title_id);
    }

    // Multi-root: always let the user pick (including when preserved/leftover/installed
    // already exists — choosing another root migrates that tree first).
    fs::path prefer = resolve_default_install_root(cfg, paths);
    if (existing.installed || existing.install_dir_present || existing.has_preserved_state) {
        prefer = existing.install_root.parent_path();
        install_root_prompt_from_apps = prefer;
    }
    install_root_prompt_index = 0;
    if (const int cur = find_install_root_index(roots, prefer); cur >= 0)
        install_root_prompt_index = cur;
    install_root_prompt_id = title_id;
    install_root_prompt_move = false;
    show_install_root_prompt = true;
    set_status("Choose install location for " + title_id);
    return true;
}

bool HubModel::begin_move_install(const std::string& title_id) {
    const Title* t = catalog.find(title_id);
    if (!t) {
        append_log("unknown title: " + title_id);
        set_status("Unknown title");
        return false;
    }
    cfg = load_app_config(paths.config_path);
    const auto existing = inspect_install_any(paths, cfg, *t);
    if (!existing.installed && !existing.install_dir_present && !existing.has_preserved_state) {
        append_log("move install: nothing to move for " + title_id);
        set_status("Nothing to move");
        return false;
    }
    const auto roots = effective_install_roots(cfg, paths);
    if (roots.size() <= 1) {
        append_log("move install: add another install location in Library Settings first");
        set_status("Add another install location first");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mu);
        job_apps_dir.clear();
    }
    // Detect the live apps root (Home / Raid / …) and preselect it so the chooser
    // shows where the title is now before picking a destination.
    const fs::path current_apps = existing.install_root.parent_path();
    install_root_prompt_from_apps = current_apps;
    install_root_prompt_index = 0;
    if (const int cur = find_install_root_index(roots, current_apps); cur >= 0) {
        install_root_prompt_index = cur;
    } else {
        append_log("move install: current location not in configured roots: " +
                   current_apps.string());
    }
    install_root_prompt_id = title_id;
    install_root_prompt_move = true;
    show_install_root_prompt = true;
    set_status("Choose new location for " + title_id);
    return true;
}

void HubModel::confirm_install_root_and_continue() {
    cfg = load_app_config(paths.config_path);
    const auto roots = effective_install_roots(cfg, paths);
    const std::string title_id = install_root_prompt_id;
    const bool move_only = install_root_prompt_move;
    show_install_root_prompt = false;
    install_root_prompt_id.clear();
    install_root_prompt_move = false;
    install_root_prompt_from_apps.clear();
    if (title_id.empty() || roots.empty()) return;
    int idx = install_root_prompt_index;
    if (idx < 0 || idx >= static_cast<int>(roots.size())) idx = 0;
    job_apps_dir = roots[static_cast<size_t>(idx)].path;
    append_log(std::string(move_only ? "Move" : "Install") +
               " location: " + job_apps_dir.string());

    if (move_only) {
        start_job(HubJob::MoveInstall, title_id);
        return;
    }

    const Title* t = catalog.find(title_id);
    if (!t) {
        append_log("unknown title: " + title_id);
        return;
    }
    if (t->supports_local_build() && !t->supports_prebuilt_install()) {
        if (prepare_build_rom_or_prompt(title_id)) return;
    }
    start_job(HubJob::Install, title_id);
}

bool HubModel::save_settings(std::string* error) {
    AppConfig next = cfg;
    next.library_root = settings.library_root;
    next.bios_root = settings.bios_root;
    next.saves_root = settings.saves_root;
    next.exclude_dirs = split_csv(settings.exclude_dirs);
    next.prefer_local_boxart = settings.prefer_local_boxart;
    next.filter_unsupported_titles = settings.filter_unsupported_titles;
    next.check_updates_on_startup = settings.check_updates_on_startup;
    next.check_updates_before_launch = settings.check_updates_before_launch;
    next.auto_clean_build_dirs = settings.auto_clean_build_dirs;
    next.install_roots.clear();
    next.default_install_root.clear();
    for (int i = 0; i < static_cast<int>(settings.install_roots.size()); ++i) {
        const auto& row = settings.install_roots[static_cast<size_t>(i)];
        if (row.path[0] == '\0') continue;
        InstallRootEntry e;
        e.label = row.label;
        e.path = row.path;
        if (i == settings.default_install_root_index) next.default_install_root = e.path;
        next.install_roots.push_back(std::move(e));
    }
    // Single builtin Default with no extras → omit from config (implicit).
    if (next.install_roots.size() == 1 &&
        next.install_roots.front().path == builtin_apps_dir(paths)) {
        next.install_roots.clear();
        next.default_install_root.clear();
    }

    // Setup wizard leaves platform_folders empty — keep whatever was already in cfg.
    if (!settings.platform_folders.empty()) {
        next.platform_folders.clear();
        for (const auto& row : settings.platform_folders) {
            if (row.platform[0] == '\0') continue;
            auto folders = split_csv(row.folders);
            if (folders.empty()) continue;
            next.platform_folders[row.platform] = std::move(folders);
        }
    }
    next = normalize_config(std::move(next));

    if (!save_app_config(paths.config_path, next, error)) return false;
    cfg = std::move(next);
    settings.prefer_local_boxart = cfg.prefer_local_boxart;
    settings.filter_unsupported_titles = cfg.filter_unsupported_titles;
    settings.check_updates_on_startup = cfg.check_updates_on_startup;
    settings.check_updates_before_launch = cfg.check_updates_before_launch;
    settings.auto_clean_build_dirs = cfg.auto_clean_build_dirs;
    settings.dirty = false;
    set_status("Saved library settings");
    append_log("Wrote " + paths.config_path.string());
    refresh_rows(false);
    // If the filter hid the current selection, jump to the first visible row.
    if (cfg.filter_unsupported_titles) {
        std::lock_guard<std::mutex> lock(mu);
        auto visible = [&](const TitleRow& r) {
            if (r.has_rom || r.has_romm || r.installed || r.install_dir_present ||
                r.has_preserved_state)
                return true;
            return false;
        };
        bool ok = selected >= 0 && selected < static_cast<int>(rows.size()) &&
                  visible(rows[static_cast<size_t>(selected)]);
        if (!ok) {
            selected = 0;
            for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
                if (visible(rows[static_cast<size_t>(i)])) {
                    selected = i;
                    break;
                }
            }
        }
    }
    return true;
}

void HubModel::open_romm_settings() {
    cfg = load_app_config(paths.config_path);
    copy_buf(romm_settings.base_url, sizeof(romm_settings.base_url), cfg.romm.base_url);
    copy_buf(romm_settings.api_token, sizeof(romm_settings.api_token), cfg.romm.api_token);
    romm_settings.sync_boxart = cfg.romm.sync_boxart;
    romm_settings.dirty = false;
    show_settings = false;
    show_psx_settings = false;
    show_romm_settings = true;
}

void HubModel::open_psx_settings() {
    psx_settings.settings = load_psx_platform_settings(paths);
    psx_settings.settings.apply_hotkey_defaults_if_empty();
    psx_settings.settings.apply_controller_defaults_if_unset();
    psx_pad_binds_init(paths);
    psx_keybinds_init(paths);
    psx_settings.dirty = false;
    psx_settings.capturing_hotkey = -1;
    psx_settings.gamepads_tab = false;
    psx_settings.configuring_player = -1;
    psx_settings.capturing_bind = -1;
    psx_settings.map_all_active = false;
    psx_settings.map_all_wait_release = false;
    psx_settings.map_all_step = 0;
    psx_settings.rename_open = false;
    show_settings = false;
    show_romm_settings = false;
    show_setup = false;
    show_psx_settings = true;
}

bool HubModel::save_psx_settings(std::string* error) {
    psx_settings.settings.apply_hotkey_defaults_if_empty();
    psx_settings.settings.apply_controller_defaults_if_unset();
    if (!save_psx_platform_settings(paths, psx_settings.settings, error)) return false;
    // Ensure input.ini / keybinds.ini exist next to settings.toml.
    psx_pad_binds_init(paths);
    psx_keybinds_init(paths);
    psx_settings.dirty = false;
    psx_settings.capturing_hotkey = -1;
    psx_settings.configuring_player = -1;
    psx_settings.capturing_bind = -1;
    psx_settings.map_all_active = false;
    set_status("Saved PlayStation settings");
    append_log("Wrote PlayStation platform settings to " +
               psx_platform_settings_dir(paths).string());
    return true;
}

bool HubModel::set_title_exclude_platform_config(const std::string& title_id, bool exclude,
                                                 std::string* error) {
    if (title_id.empty()) {
        if (error) *error = "empty title id";
        return false;
    }
    app_state = load_app_state(paths.state_path);
    set_title_excludes_platform_config(app_state, title_id, exclude);
    if (!save_app_state(paths.state_path, app_state, error)) return false;
    append_log(std::string(exclude ? "Excluded " : "Included ") + title_id +
               " from platform config");
    return true;
}

bool HubModel::save_romm_settings(std::string* error, bool refresh_boxart) {
    AppConfig next = cfg;
    next.romm.base_url = romm_settings.base_url;
    next.romm.api_token = romm_settings.api_token;
    next.romm.sync_boxart = romm_settings.sync_boxart;
    while (!next.romm.base_url.empty() && next.romm.base_url.back() == '/')
        next.romm.base_url.pop_back();
    // Trim whitespace around the token.
    {
        std::string tok = next.romm.api_token;
        size_t b = 0, e = tok.size();
        while (b < e && (tok[b] == ' ' || tok[b] == '\t' || tok[b] == '\n' || tok[b] == '\r'))
            ++b;
        while (e > b && (tok[e - 1] == ' ' || tok[e - 1] == '\t' || tok[e - 1] == '\n' ||
                         tok[e - 1] == '\r'))
            --e;
        next.romm.api_token = tok.substr(b, e - b);
    }
    next = normalize_config(std::move(next));

    if (!save_app_config(paths.config_path, next, error)) return false;
    cfg = std::move(next);
    copy_buf(romm_settings.base_url, sizeof(romm_settings.base_url), cfg.romm.base_url);
    copy_buf(romm_settings.api_token, sizeof(romm_settings.api_token), cfg.romm.api_token);
    romm_settings.sync_boxart = cfg.romm.sync_boxart;
    romm_settings.dirty = false;
    set_status(cfg.romm.enabled() ? "Saved RomM sync settings" : "Saved RomM sync settings (URL empty)");
    append_log("Wrote RomM settings to " + paths.config_path.string() +
               (cfg.romm.sync_boxart ? " (boxart=RomM)" : " (boxart=Libretro)"));
    refresh_rows(false);
    // Re-pull covers so RomM menu art replaces any prior url_cover (IGDB) cache.
    // Setup wizard skips this so ScanRoms can run as the first post-setup job.
    if (refresh_boxart) start_job(HubJob::FetchBoxart, {}, true);
    return true;
}

bool HubModel::set_title_preferred_bios(const std::string& title_id, const std::string& bios_choice,
                                        std::string* error) {
    app_state = load_app_state(paths.state_path);
    set_preferred_bios(app_state, title_id, bios_choice);
    if (!save_app_state(paths.state_path, app_state, error)) return false;

    std::lock_guard<std::mutex> lock(mu);
    for (auto& row : rows) {
        if (row.id != title_id) continue;
        row.bios_choice = bios_choice;
        row.preferred_bios_index = -1;
        for (size_t i = 0; i < row.bios_choice_ids.size(); ++i) {
            if (row.bios_choice_ids[i] == bios_choice) {
                row.preferred_bios_index = static_cast<int>(i);
                break;
            }
        }
        if (bios_choice == kOpenBiosChoice) {
            row.has_bios = true;
        } else if (!bios_choice.empty()) {
            row.bios_path = bios_choice;
            row.has_bios = true;
        }
        break;
    }
    return true;
}

bool HubModel::set_title_preferred_save(const std::string& title_id, const std::string& save_id,
                                        std::string* error) {
    app_state = load_app_state(paths.state_path);
    set_preferred_save(app_state, title_id, save_id);
    if (!save_app_state(paths.state_path, app_state, error)) return false;

    std::lock_guard<std::mutex> lock(mu);
    for (auto& row : rows) {
        if (row.id != title_id) continue;
        row.preferred_save = save_id;
        row.preferred_save_index = -1;
        for (size_t i = 0; i < row.save_ids.size(); ++i) {
            if (row.save_ids[i] == save_id) {
                row.preferred_save_index = static_cast<int>(i);
                break;
            }
        }
        break;
    }
    return true;
}

bool HubModel::set_title_preferred_save_card2(const std::string& title_id,
                                              const std::string& save_id, std::string* error) {
    app_state = load_app_state(paths.state_path);
    const std::string id = save_id.empty() ? kBlankMemcardId : save_id;
    set_preferred_save_card2(app_state, title_id, id);
    if (!save_app_state(paths.state_path, app_state, error)) return false;

    std::lock_guard<std::mutex> lock(mu);
    for (auto& row : rows) {
        if (row.id != title_id) continue;
        row.preferred_save_card2 = id;
        row.preferred_save_card2_index = -1;
        if (id != kBlankMemcardId) {
            for (size_t i = 0; i < row.save_ids.size(); ++i) {
                if (row.save_ids[i] == id) {
                    row.preferred_save_card2_index = static_cast<int>(i);
                    break;
                }
            }
        }
        break;
    }
    return true;
}

bool HubModel::create_title_save(const std::string& title_id, std::string* error,
                                 bool for_card2) {
    const Title* t = catalog.find(title_id);
    if (!t) {
        if (error) *error = "unknown title: " + title_id;
        return false;
    }
    app_state = load_app_state(paths.state_path);
    const std::string prev_card1 = preferred_save_for(app_state, title_id);
    const fs::path rom = library.preferred_rom(title_id);
    auto created = create_managed_save(paths, cfg, *t, rom);
    if (!created.ok) {
        if (error) *error = created.message.empty() ? "could not create save" : created.message;
        return false;
    }
    if (for_card2) {
        app_state = load_app_state(paths.state_path);
        if (!prev_card1.empty()) set_preferred_save(app_state, title_id, prev_card1);
        set_preferred_save_card2(app_state, title_id, created.save.id);
        if (!save_app_state(paths.state_path, app_state, error)) return false;
    }
    append_log(created.message);
    set_status(for_card2 ? ("Created memory card 2: " + created.save.label)
                         : ("Created save " + created.save.label));
    refresh_rows(false);
    return true;
}

bool HubModel::delete_title_save(const std::string& title_id, const std::string& save_id,
                                 std::string* error) {
    if (save_id.empty() || save_id == kBlankMemcardId) {
        if (error) *error = "nothing to delete";
        return false;
    }
    const Title* t = catalog.find(title_id);
    if (!t) {
        if (error) *error = "unknown title: " + title_id;
        return false;
    }
    const fs::path path = resolve_managed_save(paths, cfg, *t, save_id);
    if (path.empty()) {
        if (error) *error = "save not found: " + save_id;
        return false;
    }
    std::error_code ec;
    fs::remove(path, ec);
    if (ec) {
        if (error) *error = "delete failed: " + ec.message();
        return false;
    }
    app_state = load_app_state(paths.state_path);
    if (preferred_save_for(app_state, title_id) == save_id)
        set_preferred_save(app_state, title_id, "");
    if (preferred_save_card2_for(app_state, title_id) == save_id)
        set_preferred_save_card2(app_state, title_id, kBlankMemcardId);
    if (!save_app_state(paths.state_path, app_state, error)) return false;
    append_log("Deleted save " + path.filename().string());
    set_status("Deleted " + path.filename().string());
    refresh_rows(false);
    return true;
}

bool HubModel::rename_title_save(const std::string& title_id, const std::string& save_id,
                                 const std::string& new_label, std::string* error) {
    if (save_id.empty() || save_id == kBlankMemcardId) {
        if (error) *error = "nothing to rename";
        return false;
    }
    const Title* t = catalog.find(title_id);
    if (!t) {
        if (error) *error = "unknown title: " + title_id;
        return false;
    }
    std::string label = new_label;
    while (!label.empty() && (label.front() == ' ' || label.front() == '\t')) label.erase(label.begin());
    while (!label.empty() && (label.back() == ' ' || label.back() == '\t')) label.pop_back();
    if (label.empty()) {
        if (error) *error = "name is empty";
        return false;
    }
    for (char c : label) {
        if (c == '/' || c == '\\' || c == ':' || c == '\0') {
            if (error) *error = "invalid characters in name";
            return false;
        }
    }
    const fs::path src = resolve_managed_save(paths, cfg, *t, save_id);
    if (src.empty()) {
        if (error) *error = "save not found: " + save_id;
        return false;
    }
    fs::path dest_name = label;
    if (!dest_name.has_extension()) dest_name += src.extension();
    const fs::path dest = src.parent_path() / dest_name;
    if (dest.filename() == src.filename()) return true;
    std::error_code ec;
    if (fs::exists(dest, ec)) {
        if (error) *error = "a file named " + dest.filename().string() + " already exists";
        return false;
    }
    fs::rename(src, dest, ec);
    if (ec) {
        if (error) *error = "rename failed: " + ec.message();
        return false;
    }
    const std::string new_id = "saves/" + dest.filename().string();
    app_state = load_app_state(paths.state_path);
    if (preferred_save_for(app_state, title_id) == save_id)
        set_preferred_save(app_state, title_id, new_id);
    if (preferred_save_card2_for(app_state, title_id) == save_id)
        set_preferred_save_card2(app_state, title_id, new_id);
    if (!save_app_state(paths.state_path, app_state, error)) return false;
    append_log("Renamed save " + src.filename().string() + " → " + dest.filename().string());
    set_status("Renamed to " + dest.filename().string());
    refresh_rows(false);
    return true;
}

bool HubModel::prepare_build_rom_or_prompt(const std::string& title_id) {
    const Title* t = catalog.find(title_id);
    if (!t) {
        append_log("unknown title: " + title_id);
        set_status("Unknown title");
        return true;
    }

    library = load_library_index(paths.library_index_path);
    std::error_code ec;
    fs::path rom = library.preferred_rom(title_id);
    if (!rom.empty() && fs::is_regular_file(rom, ec)) return false; // ready to build

    const auto* bind = library.find_title(title_id);
    const bool had_stale_bind =
        bind && (!bind->paths.empty() || !bind->preferred_path.empty());
    if (had_stale_bind) {
        append_log("Library DB path missing for " + title_id +
                   " — removing missing files from index [" + t->platform + "]");
        auto pr = purge_missing_library_files(library, t->platform);
        save_library_index(paths.library_index_path, library);
        std::ostringstream oss;
        oss << "Removed " << pr.removed_files << " missing file(s) from library DB";
        if (pr.removed_title_binds)
            oss << ", " << pr.removed_title_binds << " title bind(s)";
        append_log(oss.str());
        rom = library.preferred_rom(title_id);
        if (!rom.empty() && fs::is_regular_file(rom, ec)) {
            refresh_rows(false);
            return false;
        }
    }

    refresh_rows(false);
    if (!t->has_rom_identity()) {
        set_status("No verified ROM for " + title_id);
        return true;
    }
    missing_rom_prompt_id = title_id;
    show_missing_rom_prompt = true;
    set_status(had_stale_bind ? "ROM missing from disk — scan or download from RomM"
                              : "No verified ROM — scan or download from RomM");
    return true;
}

} // namespace retcomm::hub
