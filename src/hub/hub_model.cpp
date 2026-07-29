#include "hub/hub_model.hpp"
#include "hub/hub_boxart.hpp"

#include "retcomm/romm_fetch.hpp"
#include "retcomm/romm_saves.hpp"
#include "retcomm/romscan.hpp"
#include "retcomm/self_update.hpp"
#include "retcomm/catalog_sync.hpp"

#include <cstring>
#include <sstream>
#include <unordered_set>

namespace retcomm::hub {
using namespace retcomm;

namespace {

std::string trim_log(std::string s, size_t max_chars = 12000) {
    if (s.size() <= max_chars) return s;
    return s.substr(s.size() - max_chars);
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

void HubModel::append_log(const std::string& line) {
    std::lock_guard<std::mutex> lock(mu);
    if (!log.empty() && log.back() != '\n') log.push_back('\n');
    log += line;
    if (!line.empty() && line.back() != '\n') log.push_back('\n');
    log = trim_log(std::move(log));
}

void HubModel::set_status(const std::string& s) {
    std::lock_guard<std::mutex> lock(mu);
    status = s;
}

void HubModel::refresh_rows(bool check_updates) {
    library = load_library_index(paths.library_index_path);
    app_state = load_app_state(paths.state_path);
    bios = load_bios_index(paths.bios_index_path);

    std::vector<TitleRow> next;
    next.reserve(catalog.titles.size());
    for (const auto& t : catalog.titles) {
        TitleRow row;
        row.id = t.id;
        row.name = t.name;
        row.platform = t.platform;
        row.kind = t.kind;
        row.needs_bios = t.requires_bios();

        const auto plan = inspect_install(paths, t);
        row.installed = plan.installed;
        row.installed_tag = plan.installed_tag;
        row.install_root = plan.install_root.string();
        row.binary_path = plan.binary_path.string();
        if (plan.record) row.runtime = plan.record->runtime;
        row.can_wine_install =
            host_supports_wine() && t.supports_wine_install() && host_os_key() != "windows";
        row.has_rom_identity = t.has_rom_identity();
        row.romm_ready = cfg.romm.enabled() && !cfg.romm.api_token.empty();

        const auto rom = library.preferred_rom(t.id);
        row.has_rom = !rom.empty();
        row.rom_path = rom.string();
        if (!t.rom_identity.filenames.empty())
            row.suggested_rom = t.rom_identity.filenames.front();

        row.author = t.github_owner();
        row.github_url = t.github_source_url();
        {
            const fs::path art = resolve_boxart_path(cfg, t, rom, row.suggested_rom, paths);
            if (!art.empty()) row.boxart_path = art.string();
        }

        if (row.needs_bios) {
            const auto b = bios.preferred_bios(t.id);
            row.has_bios = !b.empty();
            row.bios_path = b.string();
        }

        if (row.installed) {
            const auto saves = list_managed_saves(paths, t);
            row.save_ids.reserve(saves.size());
            row.save_labels.reserve(saves.size());
            for (const auto& s : saves) {
                row.save_ids.push_back(s.id);
                row.save_labels.push_back(s.label);
            }
            row.preferred_save = preferred_save_for(app_state, t.id);
            row.preferred_save_index = -1;
            if (!row.preferred_save.empty()) {
                for (size_t i = 0; i < row.save_ids.size(); ++i) {
                    if (row.save_ids[i] == row.preferred_save) {
                        row.preferred_save_index = static_cast<int>(i);
                        break;
                    }
                }
                // Prefer bare filename match if id form drifted.
                if (row.preferred_save_index < 0) {
                    const std::string want = fs::path(row.preferred_save).filename().string();
                    for (size_t i = 0; i < row.save_labels.size(); ++i) {
                        if (row.save_labels[i] == want) {
                            row.preferred_save_index = static_cast<int>(i);
                            row.preferred_save = row.save_ids[i];
                            break;
                        }
                    }
                }
            }
            if (row.preferred_save_index < 0 && !row.save_ids.empty()) {
                row.preferred_save_index = 0;
                row.preferred_save = row.save_ids[0];
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

bool HubModel::start_job(HubJob j, const std::string& title_id, bool force_boxart) {
    if (job_running.exchange(true)) return false;
    join_worker();
    job = j;
    job_title_id = title_id;
    job_force_boxart = force_boxart;

    worker = std::thread([this, j, title_id, force_boxart] {
        try {
            ensure_dirs(paths);
            cfg = load_app_config(paths.config_path);

            auto find_title = [&]() -> const Title* {
                return catalog.find(title_id);
            };

            switch (j) {
            case HubJob::Install:
            case HubJob::InstallWine: {
                const bool wine = (j == HubJob::InstallWine);
                set_status(std::string(wine ? "Installing (Wine) " : "Installing ") + title_id +
                           "…");
                const auto* t = find_title();
                if (!t) {
                    append_log("unknown title: " + title_id);
                    break;
                }
                InstallOptions opts;
                opts.force = false;
                opts.check_latest = true;
                opts.use_wine = wine;
                auto r = install_title(paths, *t, opts);
                append_log(r.message);
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
                auto r = update_title(paths, *t, {});
                append_log(r.message);
                set_status(r.ok ? ("Updated " + title_id) : ("Update failed: " + title_id));
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
            case HubJob::Launch: {
                set_status("Launching " + title_id + "…");
                const auto* t = find_title();
                if (!t) {
                    append_log("unknown title: " + title_id);
                    break;
                }
                LaunchOptions opts;
                opts.mode = LaunchMode::Default;
                opts.detach = true;
                opts.rom_path = library.preferred_rom(title_id);
                if (t->has_bios_identity())
                    opts.bios_path = bios.preferred_bios(title_id);
                {
                    app_state = load_app_state(paths.state_path);
                    const std::string save_id = preferred_save_for(app_state, title_id);
                    if (!save_id.empty()) {
                        opts.save_path = resolve_managed_save(paths, *t, save_id);
                        if (opts.save_path.empty()) opts.save_path = save_id;
                    } else {
                        const auto saves = list_managed_saves(paths, *t);
                        if (!saves.empty()) opts.save_path = saves.front().host_path;
                    }
                }
                auto r = launch_title(paths, *t, opts);
                append_log(r.message);
                set_status(r.ok ? ("Launched " + title_id) : ("Launch failed: " + title_id));
                break;
            }
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
                set_status("Update check complete");
                job_running = false;
                job = HubJob::None;
                return;
            }
            case HubJob::FetchBoxart: {
                const char* src = active_boxart_source(cfg);
                set_status(std::string(force_boxart ? "Resyncing boxart (" : "Fetching boxart (") +
                           src + ")…");
                append_log(std::string("Boxart: source=") + src +
                           (cfg.romm.sync_boxart ? (" @ " + cfg.romm.base_url) : "") +
                           (cfg.prefer_local_boxart ? " (prefer local)" : "") +
                           (force_boxart ? " (force resync)" : ""));
                if (force_boxart) {
                    clear_boxart_cache(paths, cfg);
                    append_log(std::string("Boxart: cleared cache ") +
                               boxart_cache_dir(paths, cfg).string());
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
                    auto fr =
                        ensure_remote_boxart(paths, cfg, t, rom, suggested, force_boxart);
                    if (fr.ok) {
                        if (fr.source == "local" || fr.source == "cache" ||
                            fr.message == "already present")
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
                oss << "Boxart: fetched " << fetched << ", already had " << skipped << ", missed "
                    << failed << " (" << src << ")";
                append_log(oss.str());
                set_status(oss.str());
                break;
            }
            case HubJob::FetchRommRom: {
                set_status("RomM: finding ROM for " + title_id + "…");
                const auto* t = find_title();
                if (!t) {
                    append_log("unknown title: " + title_id);
                    break;
                }
                auto fr = fetch_rom_from_romm(cfg, *t, [this](const std::string& s) {
                    set_status(s);
                });
                append_log(fr.message);
                if (!fr.ok) {
                    set_status("RomM ROM fetch failed: " + title_id);
                    break;
                }
                // Re-index so the new file binds to the title.
                set_status("RomM: scanning library for new ROM…");
                library = load_library_index(paths.library_index_path);
                ScanOptions opts;
                opts.index = &library;
                auto scan = scan_rom_library(catalog, cfg, opts);
                merge_scan_into_index(library, catalog, scan, cfg.library_root);
                save_library_index(paths.library_index_path, library);
                append_log("ROM scan after download: " + std::to_string(scan.matches.size()) +
                           " matches");
                set_status("RomM ROM ready: " + title_id);
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
                append_log("Self-update: current=" + retcomm_installed_tag(paths) +
                           " repo=" + retcomm_github_slug());
                auto ur = self_update_retcomm(paths, {});
                append_log(ur.message);
                {
                    std::lock_guard<std::mutex> lock(mu);
                    launcher_version = ur.latest_tag.empty() ? retcomm_installed_tag(paths)
                                                             : ur.latest_tag;
                    if (ur.skipped) launcher_version = ur.current_tag;
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
                }
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
}

void HubModel::open_settings() {
    cfg = load_app_config(paths.config_path);
    copy_buf(settings.library_root, sizeof(settings.library_root), cfg.library_root.string());
    copy_buf(settings.bios_root, sizeof(settings.bios_root), cfg.bios_root.string());
    copy_buf(settings.exclude_dirs, sizeof(settings.exclude_dirs), join_csv(cfg.exclude_dirs));
    settings.prefer_local_boxart = cfg.prefer_local_boxart;

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
    // Reuse the same draft buffers as Library settings.
    cfg = load_app_config(paths.config_path);
    copy_buf(settings.library_root, sizeof(settings.library_root), cfg.library_root.string());
    copy_buf(settings.bios_root, sizeof(settings.bios_root), cfg.bios_root.string());
    copy_buf(settings.exclude_dirs, sizeof(settings.exclude_dirs), join_csv(cfg.exclude_dirs));
    settings.prefer_local_boxart = cfg.prefer_local_boxart;
    settings.platform_folders.clear();
    settings.dirty = false;
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
    next.exclude_dirs = split_csv(settings.exclude_dirs);
    next.prefer_local_boxart = settings.prefer_local_boxart;

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
    settings.dirty = false;
    set_status("Saved library settings");
    append_log("Wrote " + paths.config_path.string());
    refresh_rows(false);
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

bool HubModel::save_romm_settings(std::string* error) {
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
    // Apply active source immediately; fetch fills that source's cache.
    refresh_rows(false);
    // Re-pull covers so RomM menu art replaces any prior url_cover (IGDB) cache.
    start_job(HubJob::FetchBoxart, {}, true);
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

} // namespace retcomm::hub
