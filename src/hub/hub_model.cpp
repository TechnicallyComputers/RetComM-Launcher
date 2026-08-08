#include "hub/hub_model.hpp"
#include "hub/hub_boxart.hpp"

#include "retcomm/build.hpp"
#include "retcomm/romm_fetch.hpp"
#include "retcomm/romm_saves.hpp"
#include "retcomm/romscan.hpp"
#include "retcomm/self_update.hpp"
#include "retcomm/catalog_sync.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
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
        opts.bios_path.clear(); // runtime uses linked OpenBIOS
        return;
    }
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

size_t HubModel::refresh_orphan_installs() {
    auto orphans = list_orphan_installs(paths, catalog);
    const size_t n = orphans.size();
    std::lock_guard<std::mutex> lock(mu);
    pending_orphans = std::move(orphans);
    return n;
}

void HubModel::refresh_rows(bool check_updates) {
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

        const auto plan = inspect_install(paths, t);
        row.installed = plan.installed;
        row.install_dir_present = plan.install_dir_present;
        row.has_preserved_state = plan.has_preserved_state;
        row.expected_binary = plan.expected_binary;
        row.install_issue.clear();
        if (plan.install_dir_present && !plan.installed)
            row.install_issue = plan.message;
        row.installed_tag = plan.installed_tag;
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

        if (row.installed) {
            const auto saves = list_managed_saves(paths, cfg, t);
            row.save_ids.reserve(saves.size());
            row.save_labels.reserve(saves.size());
            for (const auto& s : saves) {
                row.save_ids.push_back(s.id);
                row.save_labels.push_back(s.label);
            }
            row.dual_memcard = title_uses_memcards(t);

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
                row.preferred_save_index = 0;
                row.preferred_save = row.save_ids[0];
                if (!had_preferred) {
                    set_preferred_save(app_state, t.id, row.preferred_save);
                    state_dirty = true;
                }
            }

            if (row.dual_memcard) {
                row.preferred_save_card2 = preferred_save_card2_for(app_state, t.id);
                if (row.preferred_save_card2 == kBlankMemcardId) {
                    row.preferred_save_card2_index = -1;
                } else if (row.preferred_save_card2.empty()) {
                    // No preference yet: default to a different file than card1, else blank.
                    row.preferred_save_card2_index = -1;
                    for (size_t i = 0; i < row.save_ids.size(); ++i) {
                        if (static_cast<int>(i) == row.preferred_save_index) continue;
                        row.preferred_save_card2_index = static_cast<int>(i);
                        row.preferred_save_card2 = row.save_ids[i];
                        break;
                    }
                    if (row.preferred_save_card2_index < 0)
                        row.preferred_save_card2 = kBlankMemcardId;
                } else {
                    row.preferred_save_card2_index = resolve_index(row.preferred_save_card2);
                    if (row.preferred_save_card2_index < 0)
                        row.preferred_save_card2 = kBlankMemcardId;
                }
            }
        }

        if (check_updates && row.installed && !t.release.github.empty()) {
            std::string err;
            row.latest_tag = fetch_latest_release_tag(t.release.github, &err,
                                                     t.release.allow_prerelease);
            if (!row.latest_tag.empty() && !row.installed_tag.empty() &&
                row.latest_tag != row.installed_tag)
                row.update_available = true;
        }

        next.push_back(std::move(row));
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

bool HubModel::start_job(HubJob j, const std::string& title_id, bool force_boxart,
                         bool fetch_romm_first) {
    // Play can overlap Build & Install / scans — dedicated thread.
    if (j == HubJob::Launch) {
        if (launch_running.exchange(true)) {
            append_log("Launch already in progress");
            return false;
        }
        if (launch_worker.joinable()) launch_worker.join();
        launch_worker = std::thread([this, title_id] {
            try {
                ensure_dirs(paths);
                cfg = load_app_config(paths.config_path);
                set_status("Launching " + title_id + "…");
                const Title* t = catalog.find(title_id);
                if (!t) {
                    append_log("unknown title: " + title_id);
                } else {
                    LaunchOptions opts;
                    opts.mode = LaunchMode::Default;
                    opts.detach = true;
                    opts.rom_path = library.preferred_rom(title_id);
                    {
                        auto ensured = ensure_canonical_save(paths, cfg, *t, opts.rom_path, true);
                        if (!ensured.message.empty()) append_log(ensured.message);
                        app_state = load_app_state(paths.state_path);
                        apply_bios_choice_to_launch(*t, bios, app_state, opts);
                        if (ensured.ok) opts.save_path = ensured.save.host_path;
                        else {
                            const std::string save_id = preferred_save_for(app_state, title_id);
                            if (!save_id.empty()) {
                                opts.save_path = resolve_managed_save(paths, cfg, *t, save_id);
                                if (opts.save_path.empty()) opts.save_path = save_id;
                            }
                        }
                        if (title_uses_memcards(*t)) {
                            const std::string card2_id = preferred_save_card2_for(app_state, title_id);
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
                    set_status(r.ok ? ("Launched " + title_id) : ("Launch failed: " + title_id));
                }
            } catch (const std::exception& e) {
                append_log(std::string("error: ") + e.what());
                set_status("Launch error");
            }
            launch_running = false;
        });
        return true;
    }

    if (job_running.exchange(true)) return false;
    if (worker.joinable()) worker.join();
    job = j;
    job_title_id = title_id;
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
                set_status("RomM: scanning library for new ROM…");
                library = load_library_index(paths.library_index_path);
                ScanOptions opts;
                opts.index = &library;
                auto scan = scan_rom_library(catalog, cfg, opts);
                merge_scan_into_index(library, catalog, scan, cfg.library_root);
                save_library_index(paths.library_index_path, library);
                const fs::path bound = library.preferred_rom(t.id);
                append_log("ROM scan after download: " + std::to_string(scan.matches.size()) +
                           " matches" +
                           (bound.empty() ? "" : (" (bound " + bound.string() + ")")));
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
                return !bound.empty();
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
                set_status(std::string(wine         ? "Installing (Wine) "
                                       : prebuilt   ? "Installing prebuilt "
                                       : "Building ") +
                           title_id + "…");
                const auto* t = find_title();
                if (!t) {
                    append_log("unknown title: " + title_id);
                    break;
                }
                // Local builds need a verified ROM; hub confirm sets fetch_romm_first.
                if (!prebuilt && t->build.enabled && fetch_romm_first) {
                    if (!ensure_rom_via_romm(*t)) {
                        set_status("Install failed: " + title_id);
                        break;
                    }
                }
                InstallOptions opts;
                opts.force = false;
                opts.check_latest = true;
                opts.use_wine = wine;
                opts.prefer_prebuilt = prebuilt;
                BuildOptions bopts;
                bopts.rom_path = library.preferred_rom(title_id);
                app_state = load_app_state(paths.state_path);
                apply_bios_choice_to_build(*t, bios, app_state, bopts);
                wire_build_activity(this, bopts);
                auto r = install_title_auto(paths, *t, opts, bopts);
                append_log(r.message);
                set_status(r.ok ? ((prebuilt ? "Installed " : "Built ") + title_id)
                                : ("Install failed: " + title_id));
                break;
            }
            case HubJob::Update: {
                set_status("Updating " + title_id + "…");
                const auto* t = find_title();
                if (!t) {
                    append_log("unknown title: " + title_id);
                    break;
                }
                BuildOptions bopts;
                bopts.rom_path = library.preferred_rom(title_id);
                app_state = load_app_state(paths.state_path);
                apply_bios_choice_to_build(*t, bios, app_state, bopts);
                wire_build_activity(this, bopts);
                auto r = update_title_auto(paths, *t, {}, bopts);
                append_log(r.message);
                set_status(r.ok ? (r.skipped ? ("Up to date: " + title_id)
                                             : ("Updated " + title_id))
                                : ("Update failed: " + title_id));
                break;
            }
            case HubJob::GenerateRebuild: {
                set_status("Generate & Rebuild " + title_id + "…");
                const auto* t = find_title();
                if (!t) {
                    append_log("unknown title: " + title_id);
                    break;
                }
                if (!t->supports_local_build()) {
                    append_log("catalog has no local build recipe for " + title_id);
                    set_status("Generate & Rebuild unavailable: " + title_id);
                    break;
                }
                BuildOptions bopts;
                bopts.force = true;
                bopts.force_generate = true;
                bopts.rom_path = library.preferred_rom(title_id);
                if (bopts.rom_path.empty()) {
                    append_log("Generate & Rebuild needs a matched .cue in the library");
                    set_status("Generate & Rebuild failed: no disc");
                    break;
                }
                app_state = load_app_state(paths.state_path);
                apply_bios_choice_to_build(*t, bios, app_state, bopts);
                wire_build_activity(this, bopts);
                auto r = build_title(paths, *t, bopts);
                append_log(r.message);
                set_status(r.ok ? ("Generated & rebuilt " + title_id)
                                : ("Generate & Rebuild failed: " + title_id));
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
            case HubJob::Launch:
                // Handled on launch_worker via start_job.
                break;
            case HubJob::ScanRoms:
            case HubJob::FullScanRoms: {
                const bool full = (j == HubJob::FullScanRoms);
                set_status(full ? "Full ROM rescan…" : "Scanning ROM library…");
                if (full) {
                    append_log("Full ROM rescan: clearing library index cache…");
                    library = LibraryIndex{};
                } else {
                    library = load_library_index(paths.library_index_path);
                    append_log("ROM scan: starting…");
                }
                ScanOptions opts;
                opts.full_rescan = full;
                opts.index = full ? nullptr : &library;
                opts.on_progress = [this, full](const ScanProgress& p) {
                    const char* label = full ? "Full ROM rescan" : "ROM scan";
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
                    set_status("ROM scan failed — set library_root");
                    break;
                }
                result = scan_rom_library(catalog, cfg, opts);
                merge_scan_into_index(library, catalog, result, cfg.library_root);
                save_library_index(paths.library_index_path, library);
                std::ostringstream oss;
                oss << (full ? "Full ROM rescan: " : "ROM scan: ") << result.files.size()
                    << " files, " << result.matches.size() << " matches, hashed "
                    << result.hashed_files << ", skipped-hash " << result.skipped_hash
                    << ", cache-hits " << result.cache_hits;
                if (!result.scanned_roots.empty()) {
                    oss << "\n  roots:";
                    for (const auto& r : result.scanned_roots) oss << " " << r.string();
                }
                for (const auto& err : result.errors) oss << "\n  error: " << err;
                append_log(oss.str());
                set_status(full ? "Full ROM rescan complete" : "ROM scan complete");
                break;
            }
            case HubJob::ScanBios:
            case HubJob::FullScanBios: {
                const bool full = (j == HubJob::FullScanBios);
                set_status(full ? "Full BIOS rescan…" : "Scanning BIOS tree…");
                if (full) {
                    append_log("Full BIOS rescan: clearing BIOS index cache…");
                    bios = BiosIndex{};
                } else {
                    bios = load_bios_index(paths.bios_index_path);
                    append_log("BIOS scan: starting…");
                }
                BiosScanOptions opts;
                opts.full_rescan = full;
                opts.index = full ? nullptr : &bios;
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
                break;
            }
            case HubJob::CheckUpdates: {
                set_status("Checking updates…");
                refresh_rows(true);
                {
                    auto tc = check_toolchain_update(paths);
                    append_log(tc.message);
                    std::lock_guard<std::mutex> lock(mu);
                    toolchain_current_version = tc.current_version;
                    toolchain_latest_tag = tc.latest_tag;
                    toolchain_update_available = tc.update_available;
                    toolchain_status = tc.message;
                    if (tc.update_available) toolchain_prompt_pending.store(true);
                }
                set_status("Update check complete");
                job_running = false;
                job = HubJob::None;
                return;
            }
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
            case HubJob::SyncRommSaves: {
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
                set_status(sr.ok ? ("Save sync complete: " + title_id)
                                 : ("Save sync failed: " + title_id));
                break;
            }
            case HubJob::SyncRommStates: {
                set_status("RomM: syncing savestates for " + title_id + "…");
                const auto* t = find_title();
                if (!t) {
                    append_log("unknown title: " + title_id);
                    break;
                }
                auto sr = sync_states_with_romm(paths, cfg, *t, [this](const std::string& s) {
                    set_status(s);
                });
                append_log(sr.message);
                set_status(sr.ok ? ("Savestate sync complete: " + title_id)
                                 : ("Savestate sync failed: " + title_id));
                break;
            }
            case HubJob::SelfUpdate: {
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
}

void HubModel::open_settings() {
    cfg = load_app_config(paths.config_path);
    copy_buf(settings.library_root, sizeof(settings.library_root), cfg.library_root.string());
    copy_buf(settings.bios_root, sizeof(settings.bios_root), cfg.bios_root.string());
    copy_buf(settings.saves_root, sizeof(settings.saves_root), cfg.saves_root.string());
    copy_buf(settings.exclude_dirs, sizeof(settings.exclude_dirs), join_csv(cfg.exclude_dirs));
    settings.prefer_local_boxart = cfg.prefer_local_boxart;
    settings.filter_unsupported_titles = cfg.filter_unsupported_titles;

    settings.platform_folders.clear();
    // Prefer catalog platforms, then any extra keys already in config.
    std::vector<std::string> plats;
    {
        std::unordered_set<std::string> seen;
        for (const auto& t : catalog.titles) {
            if (seen.insert(t.platform).second) plats.push_back(t.platform);
        }
        for (const auto& [plat, _] : cfg.platform_folders) {
            if (seen.insert(plat).second) plats.push_back(plat);
        }
    }
    for (const auto& plat : plats) {
        PlatformFolderEdit row;
        copy_buf(row.platform, sizeof(row.platform), plat);
        copy_buf(row.folders, sizeof(row.folders), join_csv(cfg.folders_for_platform(plat)));
        settings.platform_folders.push_back(row);
    }
    settings.dirty = false;
    show_romm_settings = false;
    show_setup = false;
    show_settings = true;
}

void HubModel::open_setup() {
    // Reuse the same draft buffers as Library / RomM settings.
    cfg = load_app_config(paths.config_path);
    copy_buf(settings.library_root, sizeof(settings.library_root), cfg.library_root.string());
    copy_buf(settings.bios_root, sizeof(settings.bios_root), cfg.bios_root.string());
    copy_buf(settings.saves_root, sizeof(settings.saves_root), cfg.saves_root.string());
    copy_buf(settings.exclude_dirs, sizeof(settings.exclude_dirs), join_csv(cfg.exclude_dirs));
    settings.prefer_local_boxart = cfg.prefer_local_boxart;
    settings.filter_unsupported_titles = cfg.filter_unsupported_titles;
    settings.platform_folders.clear();
    settings.dirty = false;
    copy_buf(romm_settings.base_url, sizeof(romm_settings.base_url), cfg.romm.base_url);
    copy_buf(romm_settings.api_token, sizeof(romm_settings.api_token), cfg.romm.api_token);
    romm_settings.sync_boxart = cfg.romm.sync_boxart;
    romm_settings.dirty = false;
    show_settings = false;
    show_romm_settings = false;
    show_setup = true;
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
    }
}

void HubModel::add_platform_folder_row() {
    PlatformFolderEdit row;
    copy_buf(row.platform, sizeof(row.platform), "");
    copy_buf(row.folders, sizeof(row.folders), "");
    settings.platform_folders.push_back(row);
    settings.dirty = true;
}

bool HubModel::save_settings(std::string* error) {
    AppConfig next = cfg;
    next.library_root = settings.library_root;
    next.bios_root = settings.bios_root;
    next.saves_root = settings.saves_root;
    next.exclude_dirs = split_csv(settings.exclude_dirs);
    next.prefer_local_boxart = settings.prefer_local_boxart;
    next.filter_unsupported_titles = settings.filter_unsupported_titles;

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
    show_romm_settings = true;
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

bool HubModel::create_title_save(const std::string& title_id, std::string* error) {
    const Title* t = catalog.find(title_id);
    if (!t) {
        if (error) *error = "unknown title: " + title_id;
        return false;
    }
    const fs::path rom = library.preferred_rom(title_id);
    auto created = create_managed_save(paths, cfg, *t, rom);
    if (!created.ok) {
        if (error) *error = created.message.empty() ? "could not create save" : created.message;
        return false;
    }
    append_log(created.message);
    set_status("Created save " + created.save.label);
    app_state = load_app_state(paths.state_path);
    refresh_rows(false);
    return true;
}

} // namespace retcomm::hub
