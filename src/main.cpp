#include "retcomm/app_state.hpp"
#include "retcomm/bios_index.hpp"
#include "retcomm/build.hpp"
#include "retcomm/cache_gc.hpp"
#include "retcomm/catalog.hpp"
#include "retcomm/catalog_sync.hpp"
#include "retcomm/config.hpp"
#include "retcomm/install.hpp"
#include "retcomm/launch.hpp"
#include "retcomm/library_index.hpp"
#include "retcomm/paths.hpp"
#include "retcomm/release_tags.hpp"
#include "retcomm/romm.hpp"
#include "retcomm/romm_saves.hpp"
#include "retcomm/romscan.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace {

void print_help(const char* argv0) {
    std::cout
        << "RetComM Launcher — multi-title hub for recomp/decomp projects\n\n"
        << "Usage:\n"
        << "  " << argv0 << " [--catalog DIR] <command> [args]\n\n"
        << "Commands:\n"
        << "  list                         List catalog titles\n"
        << "  status                       Show paths, library, installs\n"
        << "  config                       Show / explain config.json\n"
        << "  scan [--full] [--rom-dir DIR ...]  Scan ROM library, match catalog, update index\n"
        << "  bios scan [--full] [--bios-dir DIR]  Scan BIOS tree, match titles that need BIOS\n"
        << "  bios list                    Show indexed title → BIOS bindings\n"
        << "  library [--check-updates]    Indexed title → ROM + install + BIOS status\n"
        << "  install <title-id> [opts]    Build locally when catalog has build recipe;\n"
        << "                               otherwise download prebuilt GitHub release\n"
        << "      --force                  Reinstall / rebuild even if same pin\n"
        << "      --prebuilt               Force zip install (skip local generate+build)\n"
        << "      --wine                   Linux/macOS: install Windows build via Wine\n"
        << "      --dry-run                Print install plan only\n"
        << "  build <title-id> [opts]      Local generate + cmake (requires matched ROM)\n"
        << "      --force                  Rebuild even if same source ref\n"
        << "      --force-generate         Re-run disc→C even if codegen-cache matches\n"
        << "      --rom PATH               ROM path (else library preferred)\n"
        << "  pack ensure toolchain|sdk [opts]\n"
        << "                               Fetch/cache a release pack (smoke-test downloads)\n"
        << "      --title ID               Use that title's build.toolchain / build.sdk\n"
        << "      --force                  Re-download even if cached\n"
        << "  update <title-id>|--all [opts]\n"
        << "                               Update installed title(s) if newer\n"
        << "      --force                  Force update even if pin matches\n"
        << "      --force-generate         Re-run disc→C on build updates\n"
        << "  uninstall <title-id> [opts]  Remove installed title (alias: remove)\n"
        << "      --keep-saves             Keep memcards/SRAM/savestates (default)\n"
        << "      --delete-saves           Also wipe saves / preserved stash\n"
        << "      --dry-run                Show what would be removed\n"
        << "  orphans [list|remove] [opts] List/remove installs not in the catalog\n"
        << "      --keep-saves             Keep memcards/SRAM/savestates (default)\n"
        << "      --delete-saves           Also wipe saves / preserved stash\n"
        << "      --dry-run                Show what would be removed\n"
        << "      --no-prune               Don't prune stale index/state entries\n"
        << "  cache gc                     Prune old toolchains/SDKs/engines/zips/idle builds\n"
        << "  launch <title-id> [opts]     Launch title into its dedicated launcher\n"
        << "      --rom PATH               ROM/disc path (else library index)\n"
        << "      --bios PATH              BIOS path (else bios index)\n"
        << "      --mode default|direct|netplay\n"
        << "      --detach                 Don't wait for the game to exit\n"
        << "      --dry-run                Print argv/cwd only\n"
        << "  romm                         Show RomM config / stub ping\n"
        << "  catalog update [--force]     Download/update remote catalog cache\n"
        << "  help                         This message\n\n"
        << "ROM scan uses config library_root (platform folders only).\n"
        << "BIOS scan uses config bios_root (flat + per-system folders).\n"
        << "Game saves use config saves_root/<platform>/<title_id>/ when set (else install saves/).\n"
        << "--full ignores the hash cache and rebuilds the index from disk\n"
        << "(drops missing files; recomputes digests for everything scanned).\n"
        << "Indexes: ~/.local/share/retcomm/library-index.json and bios-index.json.\n";
}

fs::path exe_dir_from(const char* argv0) {
    std::error_code ec;
    fs::path p = fs::absolute(argv0, ec);
    if (ec) p = fs::path(argv0);
    return p.parent_path();
}

retcomm::ScanProgressFn make_progress_printer() {
    return [](const retcomm::ScanProgress& p) {
        if (p.phase == "hash" && p.total > 0) {
            std::cerr << "\r  hashing [" << p.platform << "] " << p.current << "/"
                      << p.total << "  " << p.path.filename().string() << "          "
                      << std::flush;
            if (p.current == p.total) std::cerr << "\n";
        } else if (p.phase == "cache" && p.current > 0 && (p.current % 25 == 0)) {
            std::cerr << "\r  cache-hit [" << p.platform << "] " << p.current
                      << "          " << std::flush;
        } else if (p.phase == "walk" && p.current > 0 && (p.current % 50 == 0)) {
            std::cerr << "\r  indexing [" << p.platform << "] " << p.current
                      << " files…          " << std::flush;
        } else if (p.phase == "match") {
            std::cerr << "  matching catalog…\n";
        }
    };
}

int cmd_list(const retcomm::Catalog& cat) {
    std::cout << cat.name << " (" << cat.titles.size() << " titles)\n";
    for (const auto& t : cat.titles) {
        const char* id_mark = t.has_rom_identity() ? "*" : " ";
        std::cout << "  " << id_mark << " " << t.id << "\n"
                  << "      " << t.name << "  [" << t.kind << "/" << t.platform << "]\n";
    }
    std::cout << "\n* = rom_identity present (hash/serial matchable)\n";
    return 0;
}

int cmd_status(const retcomm::Paths& paths, const retcomm::AppConfig& cfg,
               const retcomm::Catalog& cat) {
    try {
        retcomm::ensure_dirs(paths);
    } catch (const std::exception& e) {
        std::cerr << "note: could not create data dirs (" << e.what() << ")\n";
    }
    auto idx = retcomm::load_library_index(paths.library_index_path);
    auto bios_idx = retcomm::load_bios_index(paths.bios_index_path);
    std::cout << "config:  " << paths.config_path.string() << "\n"
              << "data:    " << paths.data_dir.string() << "\n"
              << "apps:    " << paths.apps_dir.string() << "\n"
              << "index:   " << paths.library_index_path.string() << " ("
              << idx.titles.size() << " bound titles, " << idx.files.size()
              << " files)\n"
              << "bios:    " << paths.bios_index_path.string() << " ("
              << bios_idx.titles.size() << " bound titles, " << bios_idx.files.size()
              << " files)\n"
              << "catalog: " << cat.root.string() << "\n"
              << "catalog cache: " << paths.catalog_dir.string()
              << (retcomm::catalog_cache_valid(paths) ? " (valid)\n" : " (empty)\n")
              << "library: "
              << (cfg.library_root.empty() ? "(unset)" : cfg.library_root.string())
              << "\n"
              << "bios_root: "
              << (cfg.bios_root.empty() ? "(unset)" : cfg.bios_root.string()) << "\n"
              << "saves_root: "
              << (cfg.saves_root.empty() ? "(unset)" : cfg.saves_root.string()) << "\n\n";

    std::cout << "Platform folders (catalog → disk):\n";
    std::unordered_set<std::string> plats;
    for (const auto& t : cat.titles) plats.insert(t.platform);
    for (const auto& plat : plats) {
        auto folders = cfg.folders_for_platform(plat);
        std::cout << "  " << plat << " → ";
        for (size_t i = 0; i < folders.size(); ++i) {
            if (i) std::cout << ", ";
            std::cout << folders[i];
            if (!cfg.library_root.empty()) {
                const fs::path p = cfg.library_root / folders[i];
                std::cout << (fs::is_directory(p) ? " ✓" : " ✗");
            }
        }
        std::cout << "\n";
    }
    std::cout << "\n";

    for (const auto& t : cat.titles) {
        auto plan = retcomm::inspect_install(paths, t);
        const auto rom = idx.preferred_rom(t.id);
        std::cout << "  " << t.id << " — " << plan.message;
        if (!rom.empty()) std::cout << "\n      rom: " << rom.string();
        std::cout << "\n";
    }
    return 0;
}

int cmd_config(const retcomm::Paths& paths, const retcomm::AppConfig& cfg) {
    std::cout << "Config file: " << paths.config_path.string() << "\n"
              << "library_root: "
              << (cfg.library_root.empty() ? "(unset)" : cfg.library_root.string())
              << "\n"
              << "bios_root: "
              << (cfg.bios_root.empty() ? "(unset)" : cfg.bios_root.string()) << "\n"
              << "saves_root: "
              << (cfg.saves_root.empty() ? "(unset)" : cfg.saves_root.string()) << "\n"
              << "exclude_dirs: ";
    for (size_t i = 0; i < cfg.exclude_dirs.size(); ++i) {
        if (i) std::cout << ", ";
        std::cout << cfg.exclude_dirs[i];
    }
    std::cout << "\n"
              << "romm: "
              << (cfg.romm.enabled() ? cfg.romm.base_url : "(not configured)") << "\n"
              << "catalog url: "
              << (cfg.catalog.url.empty() ? retcomm::default_catalog_download_url()
                                          : cfg.catalog.url)
              << "\n"
              << "catalog auto_update: " << (cfg.catalog.auto_update ? "true" : "false") << "\n\n"
              << "Example config.json:\n"
              << "{\n"
              << "  \"library_root\": \"/mnt/crucial4tb/Emulation/roms\",\n"
              << "  \"bios_root\": \"/mnt/crucial4tb/Emulation/bios\",\n"
              << "  \"saves_root\": \"/mnt/crucial4tb/Emulation/saves\",\n"
              << "  \"platform_folders\": {\n"
              << "    \"psx\": [\"ps\", \"ps1\", \"psx\"],\n"
              << "    \"snes\": [\"snes\"],\n"
              << "    \"n64\": [\"n64\"]\n"
              << "  },\n"
              << "  \"exclude_dirs\": [\"torrents\", \"emulators\"],\n"
              << "  \"romm\": {\n"
              << "    \"base_url\": \"https://your-romm.example\",\n"
              << "    \"api_token\": \"…\"\n"
              << "  },\n"
              << "  \"catalog\": {\n"
              << "    \"url\": \"https://github.com/TechnicallyComputers/retcomm-catalog/releases/latest/download/catalog.zip\",\n"
              << "    \"github_repo\": \"TechnicallyComputers/retcomm-catalog\",\n"
              << "    \"auto_update\": true\n"
              << "  }\n"
              << "}\n";
    return 0;
}

int cmd_library(const retcomm::Paths& paths, const retcomm::Catalog& cat,
                bool check_updates) {
    auto idx = retcomm::load_library_index(paths.library_index_path);
    auto bios_idx = retcomm::load_bios_index(paths.bios_index_path);
    retcomm::ReleaseTagCache tag_cache(retcomm::release_tags_cache_path(paths));
    std::cout << "Library index: " << paths.library_index_path.string() << "\n"
              << "library_root: "
              << (idx.library_root.empty() ? "(unset)" : idx.library_root) << "\n"
              << "files: " << idx.files.size() << "  bound titles: " << idx.titles.size()
              << "\n\n";
    if (idx.titles.empty()) {
        std::cout << "No bound titles yet. Run: retcomm scan\n";
        return 0;
    }
    for (const auto& b : idx.titles) {
        const auto* t = cat.find(b.title_id);
        std::cout << "  " << b.title_id;
        if (t) std::cout << "  (" << t->name << ")";
        std::cout << "\n"
                  << "      preferred: " << b.preferred_path << "\n";
        if (b.paths.size() > 1) {
            for (size_t i = 1; i < b.paths.size(); ++i)
                std::cout << "      also:      " << b.paths[i] << "\n";
        }
        if (t) {
            auto plan = retcomm::inspect_install(paths, *t);
            if (plan.installed) {
                std::cout << "      app:       installed";
                if (!plan.installed_tag.empty())
                    std::cout << " " << plan.installed_tag;
                if (check_updates && !t->release.github.empty()) {
                    std::string err;
                    const std::string latest =
                        tag_cache.latest_tag(t->release.github, t->release.allow_prerelease,
                                             /*force=*/false, &err);
                    const std::string have = retcomm::install_release_compare_tag(plan);
                    if (!latest.empty() && !have.empty() && latest != have)
                        std::cout << "  [update available: " << latest << "]";
                    else if (!latest.empty())
                        std::cout << "  [up to date]";
                    else if (!err.empty())
                        std::cout << "  [update check failed]";
                }
                std::cout << "\n";
            } else {
                std::cout << "      app:       not installed\n";
            }
            if (t->requires_bios()) {
                const auto bios = bios_idx.preferred_bios(t->id);
                if (!bios.empty())
                    std::cout << "      bios:      " << bios.string() << "\n";
                else
                    std::cout << "      bios:      missing (retcomm bios scan)\n";
            }
        }
    }
    if (check_updates) {
        tag_cache.save_if_dirty();
        std::cout << "\nUpdate check: " << tag_cache.network_fetches() << " GitHub fetch(es), "
                  << tag_cache.cache_hits() << " cache hit(s)\n";
    }
    return 0;
}

int cmd_bios_list(const retcomm::Paths& paths, const retcomm::Catalog& cat) {
    auto idx = retcomm::load_bios_index(paths.bios_index_path);
    std::cout << "BIOS index: " << paths.bios_index_path.string() << "\n"
              << "bios_root: " << (idx.bios_root.empty() ? "(unset)" : idx.bios_root)
              << "\n"
              << "files: " << idx.files.size() << "  bound titles: " << idx.titles.size()
              << "\n\n";
    if (idx.titles.empty()) {
        std::cout << "No BIOS bindings yet. Run: retcomm bios scan\n";
        return 0;
    }
    for (const auto& b : idx.titles) {
        const auto* t = cat.find(b.title_id);
        std::cout << "  " << b.title_id;
        if (t) std::cout << "  (" << t->name << ")";
        std::cout << "\n"
                  << "      preferred: " << b.preferred_path << "\n";
        for (const auto& p : b.paths) {
            if (p == b.preferred_path) continue;
            std::cout << "      also:      " << p << "\n";
        }
    }
    return 0;
}

int cmd_bios_scan(const retcomm::Paths& paths, const retcomm::Catalog& cat,
                  const retcomm::AppConfig& cfg, const std::vector<fs::path>& bios_dirs,
                  bool full_rescan) {
    retcomm::BiosIndex index = retcomm::load_bios_index(paths.bios_index_path);

    retcomm::BiosScanOptions opts;
    opts.on_progress = [](const retcomm::BiosScanProgress& p) {
        if (p.phase == "hash" && p.total > 0) {
            std::cerr << "\r  hashing [" << p.platform << "] " << p.current << "/"
                      << p.total << "  " << p.path.filename().string() << "          "
                      << std::flush;
            if (p.current == p.total) std::cerr << "\n";
        } else if (p.phase == "match") {
            std::cerr << "  matching catalog…\n";
        }
    };
    opts.full_rescan = full_rescan;
    if (full_rescan) {
        if (bios_dirs.empty()) {
            std::cerr << "Full BIOS rescan: rebuilding index from scratch…\n";
            index = retcomm::BiosIndex{};
        } else {
            std::cerr << "Full BIOS rescan: ignoring hash cache…\n";
        }
        opts.index = nullptr;
    } else {
        opts.index = &index;
    }

    retcomm::BiosScanResult result;
    fs::path bios_root = cfg.bios_root;
    if (!bios_dirs.empty()) {
        std::cerr << "Scanning " << bios_dirs.size() << " BIOS path(s)…\n";
        result = retcomm::scan_bios_roots(cat, cfg, bios_dirs, opts);
        if (bios_root.empty()) bios_root = bios_dirs[0];
    } else {
        if (cfg.bios_root.empty()) {
            std::cerr << "bios scan requires bios_root in config.json or --bios-dir DIR\n"
                      << "  tip: retcomm config\n";
            return 2;
        }
        std::cerr << "Scanning BIOS tree " << cfg.bios_root.string() << "…\n";
        result = retcomm::scan_bios_library(cat, cfg, opts);
    }

    std::cout << "Scanned roots:\n";
    for (const auto& r : result.scanned_roots) std::cout << "  " << r.string() << "\n";
    std::cout << "Candidates: " << result.files.size()
              << "  hashed: " << result.hashed_files
              << "  cache-hits: " << result.cache_hits
              << "  size-skipped: " << result.skipped_hash << "\n";
    for (const auto& err : result.errors) std::cerr << "  warning: " << err << "\n";

    merge_bios_scan_into_index(index, cat, result, bios_root);
    try {
        retcomm::ensure_dirs(paths);
    } catch (const std::exception& e) {
        std::cerr << "note: " << e.what() << "\n";
    }
    if (!retcomm::save_bios_index(paths.bios_index_path, index)) {
        std::cerr << "warning: failed to write " << paths.bios_index_path.string() << "\n";
    } else {
        std::cout << "BIOS index updated: " << paths.bios_index_path.string() << " ("
                  << index.titles.size() << " titles, " << index.files.size()
                  << " files)\n";
    }

    if (result.matches.empty()) {
        std::cout << "No BIOS matches for catalog titles.\n"
                  << "  Tip: PSX titles expect SCPH1001.BIN (CRC32 37157331, 512 KiB).\n";
        return 0;
    }
    std::cout << "\nBIOS bindings:\n";
    for (const auto& m : result.matches) {
        const auto* t = cat.find(m.title_id);
        std::cout << "  " << m.title_id;
        if (t) std::cout << " (" << t->name << ")";
        std::cout << "\n"
                  << "      " << m.preferred_path << "\n";
    }
    return 0;
}

int cmd_scan(const retcomm::Paths& paths, const retcomm::Catalog& cat,
             const retcomm::AppConfig& cfg, const std::vector<fs::path>& rom_dirs,
             bool full_rescan) {
    retcomm::LibraryIndex index = retcomm::load_library_index(paths.library_index_path);

    retcomm::ScanOptions opts;
    opts.on_progress = make_progress_printer();
    opts.full_rescan = full_rescan;
    if (full_rescan) {
        if (rom_dirs.empty()) {
            std::cerr << "Full ROM rescan: rebuilding library index from scratch…\n";
            index = retcomm::LibraryIndex{};
        } else {
            std::cerr << "Full ROM rescan: ignoring hash cache for scanned roots…\n";
        }
        opts.index = nullptr;
    } else {
        opts.index = &index;
    }

    retcomm::ScanResult result;
    fs::path lib_root = cfg.library_root;
    if (!rom_dirs.empty()) {
        std::cerr << "Scanning " << rom_dirs.size() << " path(s) (platform-scoped)…\n";
        result = retcomm::scan_rom_roots(cat, cfg, rom_dirs, opts);
        if (lib_root.empty() && !rom_dirs.empty()) {
            // If user passed a library root, remember it on the index.
            std::error_code ec;
            if (fs::is_directory(rom_dirs[0] / "snes", ec) ||
                fs::is_directory(rom_dirs[0] / "ps", ec))
                lib_root = rom_dirs[0];
        }
    } else {
        if (cfg.library_root.empty()) {
            std::cerr << "scan requires library_root in config.json or --rom-dir DIR\n"
                      << "  tip: retcomm config\n";
            return 2;
        }
        std::cerr << "Scanning library " << cfg.library_root.string()
                  << " (catalog platforms only)…\n";
        result = retcomm::scan_rom_library(cat, cfg, opts);
    }

    std::cout << "Scanned roots:\n";
    for (const auto& r : result.scanned_roots) std::cout << "  " << r.string() << "\n";
    std::cout << "Candidates: " << result.files.size()
              << "  hashed: " << result.hashed_files
              << "  cache-hits: " << result.cache_hits
              << "  hash-skipped: " << result.skipped_hash << "\n";
    for (const auto& err : result.errors) std::cerr << "  warning: " << err << "\n";

    merge_scan_into_index(index, cat, result, lib_root);
    try {
        retcomm::ensure_dirs(paths);
    } catch (const std::exception& e) {
        std::cerr << "note: " << e.what() << "\n";
    }
    if (!retcomm::save_library_index(paths.library_index_path, index)) {
        std::cerr << "warning: failed to write " << paths.library_index_path.string()
                  << "\n";
    } else {
        std::cout << "Index updated: " << paths.library_index_path.string() << " ("
                  << index.titles.size() << " titles, " << index.files.size()
                  << " files)\n";
    }

    if (result.matches.empty()) {
        std::cout << "No catalog matches yet.\n"
                  << "  Tip: only titles with rom_identity hashes can match "
                     "(see metal-warriors-snes).\n";
        return 0;
    }
    std::cout << "\nRecommended / matched titles:\n";
    for (const auto& m : result.matches) {
        std::cout << "  " << m.title->name << " (" << m.title->id << ")\n"
                  << "      via " << m.matched_by << ": " << m.rom_path.string() << "\n";
        for (size_t i = 1; i < m.all_paths.size(); ++i)
            std::cout << "      also: " << m.all_paths[i].string() << "\n";
    }
    return 0;
}

int cmd_install(const retcomm::Paths& paths, const retcomm::Catalog& cat,
                const std::string& id, bool force, bool dry_run, bool use_wine,
                bool prefer_prebuilt) {
    const auto* t = cat.find(id);
    if (!t) {
        std::cerr << "unknown title: " << id << "\n";
        return 1;
    }
    try {
        retcomm::ensure_dirs(paths);
    } catch (const std::exception& e) {
        std::cerr << "note: " << e.what() << "\n";
    }
    retcomm::InstallOptions opts;
    opts.force = force;
    opts.check_latest = true;
    opts.use_wine = use_wine;
    opts.prefer_prebuilt = prefer_prebuilt || use_wine;
    if (dry_run) {
        if (t->supports_prebuilt_install()) {
            auto plan = retcomm::plan_install(paths, *t, opts);
            std::cout << "would prefer prebuilt zip install\n" << plan.message;
            if (!opts.prefer_prebuilt && t->supports_local_build())
                std::cout << "(local build fallback available from " << t->build.source.ref
                          << ")\n";
            return 0;
        }
        if (!opts.prefer_prebuilt && t->supports_local_build()) {
            std::cout << "would build " << t->id << " from " << t->build.source.ref
                      << " (sdk=" << t->build.sdk.id << ", toolchain=" << t->build.toolchain.id
                      << ")\n";
            return 0;
        }
        auto plan = retcomm::plan_install(paths, *t, opts);
        std::cout << plan.message;
        return 0;
    }
    retcomm::BuildOptions bopts;
    bopts.force = force;
    {
        const auto st = retcomm::load_app_state(paths.state_path);
        const std::string choice = retcomm::preferred_bios_for(st, id);
        if (choice == retcomm::kOpenBiosChoice) {
            bopts.use_openbios = true;
        } else if (!choice.empty()) {
            bopts.bios_path = choice;
        } else {
            auto bidx = retcomm::load_bios_index(paths.bios_index_path);
            bopts.bios_path = bidx.preferred_bios(id);
            if (bopts.bios_path.empty() &&
                t->build.generate.engine == "psxrecomp") {
                bopts.use_openbios = true;
            }
        }
    }
    auto result = retcomm::install_title_auto(paths, *t, opts, bopts);
    std::cout << result.message;
    return result.ok ? 0 : 1;
}

int cmd_pack_ensure(const retcomm::Paths& paths, const retcomm::Catalog& cat,
                    const std::string& kind, const std::string& title_id, bool force) {
    const bool toolchain = (kind == "toolchain");
    if (!toolchain && kind != "sdk") {
        std::cerr << "usage: retcomm pack ensure toolchain|sdk [--title ID] [--force]\n";
        return 2;
    }

    retcomm::TitleBuildPack pack;
    if (!title_id.empty()) {
        const auto* t = cat.find(title_id);
        if (!t) {
            std::cerr << "unknown title: " << title_id << "\n";
            return 1;
        }
        pack = toolchain ? t->build.toolchain : t->build.sdk;
        if (pack.id.empty() || pack.github.empty()) {
            std::cerr << "title " << title_id << " has no build." << kind << " pack\n";
            return 1;
        }
    } else if (toolchain) {
        // Default: shared RetComM toolchain repo.
        pack.id = "cmake-clang-v1";
        pack.github = "TechnicallyComputers/retcomm-toolchains";
        pack.asset_glob_linux = "*cmake-clang-v1*linux*";
        pack.asset_glob_windows = "*cmake-clang-v1*windows*";
        pack.asset_glob_macos = "*cmake-clang-v1*macos*";
    } else {
        std::cerr << "sdk pack ensure requires --title <id>\n";
        return 2;
    }

    try {
        retcomm::ensure_dirs(paths);
    } catch (const std::exception& e) {
        std::cerr << "note: " << e.what() << "\n";
    }

    if (force) {
        std::error_code ec;
        const fs::path base = toolchain ? paths.toolchains_dir : paths.sdks_dir;
        fs::remove_all(base / pack.id, ec);
    }

    auto r = retcomm::ensure_pack(paths, pack, toolchain, {}, {});
    if (!r.ok) {
        std::cerr << r.message << "\n";
        return 1;
    }
    std::cout << r.message << "\n";
    std::cout << "root: " << r.root.string() << "\n";
    std::cout << "tag:  " << r.tag << "\n";

    // Light verification for toolchain packs.
    if (toolchain) {
        std::error_code ec;
        const fs::path bin = r.root / "bin";
        const fs::path cmake =
#if defined(_WIN32)
            bin / "cmake.exe";
#else
            bin / "cmake";
#endif
        if (fs::is_regular_file(cmake, ec)) {
            std::cout << "cmake: " << cmake.string() << "\n";
        } else {
            std::cerr << "warning: cmake not found under " << bin.string() << "\n";
        }
        const fs::path meta = r.root / "retcomm-toolchain.json";
        if (fs::is_regular_file(meta, ec)) std::cout << "meta:  " << meta.string() << "\n";
    }
    return 0;
}

int cmd_build(const retcomm::Paths& paths, const retcomm::Catalog& cat, const std::string& id,
              bool force, bool force_generate, const fs::path& rom_override) {
    const auto* t = cat.find(id);
    if (!t) {
        std::cerr << "unknown title: " << id << "\n";
        return 1;
    }
    if (!t->supports_local_build()) {
        std::cerr << "title has no local build recipe: " << id << "\n";
        return 1;
    }
    try {
        retcomm::ensure_dirs(paths);
    } catch (const std::exception& e) {
        std::cerr << "note: " << e.what() << "\n";
    }
    retcomm::BuildOptions bopts;
    bopts.force = force;
    bopts.force_generate = force_generate;
    bopts.rom_path = rom_override;
    if (bopts.rom_path.empty()) {
        const auto idx = retcomm::load_library_index(paths.library_index_path);
        bopts.rom_path = idx.preferred_rom(id);
    }
    {
        const auto st = retcomm::load_app_state(paths.state_path);
        const std::string choice = retcomm::preferred_bios_for(st, id);
        if (choice == retcomm::kOpenBiosChoice) {
            bopts.use_openbios = true;
        } else if (!choice.empty()) {
            bopts.bios_path = choice;
        } else {
            auto bidx = retcomm::load_bios_index(paths.bios_index_path);
            bopts.bios_path = bidx.preferred_bios(id);
            if (bopts.bios_path.empty() && t->build.generate.engine == "psxrecomp")
                bopts.use_openbios = true;
        }
    }
    auto result = retcomm::build_title(paths, *t, bopts);
    std::cout << result.message;
    return result.ok ? 0 : 1;
}

int cmd_update(const retcomm::Paths& paths, const retcomm::Catalog& cat,
               const std::string& id_or_all, bool force, bool force_generate) {
    retcomm::InstallOptions opts;
    opts.force = force;
    opts.check_latest = true;

    std::vector<const retcomm::Title*> targets;
    if (id_or_all == "--all") {
        for (const auto& t : cat.titles) {
            auto plan = retcomm::inspect_install(paths, t);
            if (plan.installed) targets.push_back(&t);
        }
        if (targets.empty()) {
            std::cout << "No installed titles to update.\n";
            return 0;
        }
    } else {
        const auto* t = cat.find(id_or_all);
        if (!t) {
            std::cerr << "unknown title: " << id_or_all << "\n";
            return 1;
        }
        targets.push_back(t);
    }

    int failures = 0;
    for (const auto* t : targets) {
        retcomm::BuildOptions bopts;
        bopts.force = force;
        bopts.force_generate = force_generate;
        auto result = retcomm::update_title_auto(paths, *t, opts, bopts);
        std::cout << result.message;
        if (!result.ok) ++failures;
    }
    return failures ? 1 : 0;
}

int cmd_uninstall(const retcomm::Paths& paths, const retcomm::Catalog& cat,
                  const std::string& id, const retcomm::UninstallOptions& opts) {
    const auto* t = cat.find(id);
    if (!t) {
        std::cerr << "unknown title: " << id << "\n";
        return 1;
    }
    auto result = retcomm::uninstall_title(paths, *t, opts);
    std::cout << result.message;
    return result.ok ? 0 : 1;
}

int cmd_launch(const retcomm::Paths& paths, const retcomm::Catalog& cat,
               const std::string& id, retcomm::LaunchOptions opts) {
    const auto* t = cat.find(id);
    if (!t) {
        std::cerr << "unknown title: " << id << "\n";
        return 1;
    }
    const auto cfg = retcomm::load_app_config(paths.config_path);
    std::string rom_source;
    std::string bios_source;
    std::string save_source;
    if (opts.rom_path.empty()) {
        auto idx = retcomm::load_library_index(paths.library_index_path);
        opts.rom_path = idx.preferred_rom(id);
        if (!opts.rom_path.empty()) rom_source = "library-index";
    } else {
        rom_source = "--rom";
    }
    if (opts.bios_path.empty()) {
        const auto st = retcomm::load_app_state(paths.state_path);
        const std::string choice = retcomm::preferred_bios_for(st, id);
        if (choice == retcomm::kOpenBiosChoice) {
            opts.use_openbios = true;
            opts.bios_path.clear();
            bios_source = "openbios";
        } else if (!choice.empty()) {
            opts.use_openbios = false;
            opts.bios_path = choice;
            bios_source = "state";
        } else if (t->has_bios_identity()) {
            auto bidx = retcomm::load_bios_index(paths.bios_index_path);
            opts.bios_path = bidx.preferred_bios(id);
            if (!opts.bios_path.empty()) bios_source = "bios-index";
        }
    } else {
        opts.use_openbios = false;
        bios_source = "--bios";
    }
    if (opts.save_path.empty()) {
        auto ensured =
            retcomm::ensure_canonical_save(paths, cfg, *t, opts.rom_path, /*mint=*/true);
        if (ensured.ok) {
            opts.save_path = ensured.save.host_path;
            save_source = ensured.created ? "created" : "canonical";
        }
        auto st = retcomm::load_app_state(paths.state_path);
        if (retcomm::title_uses_memcards(*t)) {
            const std::string card2_id = retcomm::preferred_save_card2_for(st, id);
            if (card2_id == retcomm::kBlankMemcardId) {
                opts.save_path_card2_blank = true;
            } else if (!card2_id.empty()) {
                opts.save_path_card2 = retcomm::resolve_managed_save(paths, cfg, *t, card2_id);
            }
        }
    } else {
        save_source = "--save";
    }

    auto result = retcomm::launch_title(paths, *t, opts);
    std::cout << result.message;
    if (!opts.rom_path.empty() && result.plan.ready)
        std::cout << "  media source: " << rom_source << "\n";
    else if (opts.rom_path.empty() && result.plan.ready)
        std::cout << "  tip: run retcomm scan, or pass --rom PATH\n";
    if (opts.use_openbios && result.plan.ready)
        std::cout << "  bios source:  " << bios_source << "\n";
    else if (!opts.bios_path.empty() && result.plan.ready)
        std::cout << "  bios source:  " << bios_source << "\n";
    else if (t->requires_bios() && opts.bios_path.empty() && result.plan.ready)
        std::cout << "  tip: run retcomm bios scan, or pass --bios PATH\n";
    if (!opts.save_path.empty() && result.plan.ready)
        std::cout << "  save source:  " << save_source << "\n";

    if (!result.ok) return 1;
    if (opts.dry_run || opts.detach) return 0;
    return result.exit_code;
}

int cmd_catalog_update(const retcomm::Paths& paths, const retcomm::AppConfig& cfg,
                       bool force) {
    try {
        retcomm::ensure_dirs(paths);
    } catch (const std::exception& e) {
        std::cerr << "data dir error: " << e.what() << "\n";
        return 1;
    }
    const auto result = retcomm::sync_remote_catalog(paths, cfg, force);
    if (result.ok) {
        std::cout << result.message << "\n";
        if (!result.synced_at.empty()) std::cout << "  synced_at: " << result.synced_at << "\n";
        try {
            const auto cat = retcomm::load_catalog(paths.catalog_dir);
            const auto orphans = retcomm::list_orphan_installs(paths, cat);
            if (!orphans.empty()) {
                std::cout << "  unlisted installs: " << orphans.size()
                          << " (run: retcomm orphans list / remove)\n";
            }
        } catch (...) {
        }
        return 0;
    }
    std::cerr << result.message << "\n";
    return 1;
}

int cmd_orphans(const retcomm::Paths& paths, const retcomm::Catalog& cat, bool do_remove,
                const retcomm::OrphanCleanupOptions& opts) {
    try {
        retcomm::ensure_dirs(paths);
    } catch (const std::exception& e) {
        std::cerr << "data dir error: " << e.what() << "\n";
        return 1;
    }
    const auto orphans = retcomm::list_orphan_installs(paths, cat);
    if (!do_remove) {
        if (orphans.empty()) {
            std::cout << "No installs outside the catalog.\n";
            return 0;
        }
        std::cout << orphans.size() << " install(s) not in catalog:\n";
        for (const auto& o : orphans) {
            std::cout << "  " << o.title_id;
            if (o.title_id != o.dir_name) std::cout << " (" << o.dir_name << ")";
            if (!o.tag.empty()) std::cout << " @" << o.tag;
            if (o.has_preserved_only) std::cout << " [preserved]";
            std::cout << "\n    " << o.install_root.string() << "\n";
        }
        return 0;
    }
    if (orphans.empty() && !opts.prune_indexes) {
        std::cout << "Nothing to remove.\n";
        return 0;
    }
    auto result = retcomm::cleanup_removed_catalog_titles(paths, cat, opts);
    for (const auto& m : result.messages) std::cout << m;
    std::cout << result.message;
    return result.ok ? 0 : 1;
}

int cmd_romm(const retcomm::Paths& paths, const retcomm::AppConfig& cfg) {
    std::cout << "config: " << paths.config_path.string() << "\n";
    if (!cfg.romm.enabled()) {
        std::cout << "RomM: not configured\n\n"
                  << "See: retcomm config\n";
        return 0;
    }
    std::cout << "RomM base_url: " << cfg.romm.base_url << "\n"
              << "api_token:     " << (cfg.romm.api_token.empty() ? "(empty)" : "(set)")
              << "\n";
    retcomm::RommClient client(cfg.romm);
    if (!client.ping()) std::cout << client.last_error() << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    fs::path catalog_override;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--catalog" && i + 1 < argc) {
            catalog_override = argv[++i];
        } else if (a == "-h" || a == "--help") {
            print_help(argv[0]);
            return 0;
        } else {
            args.push_back(a);
        }
    }

    if (args.empty() || args[0] == "help") {
        print_help(argv[0]);
        return 0;
    }

    retcomm::Paths paths = retcomm::default_paths();
    retcomm::AppConfig cfg = retcomm::load_app_config(paths.config_path);

    const std::string& cmd = args[0];
    if (cmd == "catalog") {
        if (args.size() < 2 || args[1] != "update") {
            std::cerr << "usage: retcomm catalog update [--force]\n";
            return 2;
        }
        bool force = false;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--force")
                force = true;
            else {
                std::cerr << "unexpected catalog arg: " << args[i] << "\n";
                return 2;
            }
        }
        return cmd_catalog_update(paths, cfg, force);
    }

    if (catalog_override.empty()) {
        try {
            retcomm::ensure_dirs(paths);
            const auto sync = retcomm::maybe_auto_update_catalog(paths, cfg);
            if (!sync.ok && !sync.skipped)
                std::cerr << "catalog auto-update: " << sync.message << "\n";
        } catch (const std::exception& e) {
            std::cerr << "catalog auto-update: " << e.what() << "\n";
        }
    }

    retcomm::Catalog catalog;
    try {
        const fs::path cat_dir =
            retcomm::resolve_catalog_dir(exe_dir_from(argv[0]), catalog_override, &paths);
        catalog = retcomm::load_catalog(cat_dir);
    } catch (const std::exception& e) {
        std::cerr << "catalog error: " << e.what() << "\n";
        return 1;
    }

    try {
        if (cmd == "list") return cmd_list(catalog);
        if (cmd == "status") return cmd_status(paths, cfg, catalog);
        if (cmd == "config") return cmd_config(paths, cfg);
        if (cmd == "library") {
            bool check_updates = false;
            for (size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--check-updates")
                    check_updates = true;
                else if (args[i] == "list")
                    continue;
                else {
                    std::cerr << "unexpected library arg: " << args[i] << "\n";
                    return 2;
                }
            }
            return cmd_library(paths, catalog, check_updates);
        }
        if (cmd == "romm") return cmd_romm(paths, cfg);
        if (cmd == "bios") {
            if (args.size() < 2 || args[1] == "list") return cmd_bios_list(paths, catalog);
            if (args[1] == "scan") {
                std::vector<fs::path> roots;
                bool full = false;
                for (size_t i = 2; i < args.size(); ++i) {
                    if (args[i] == "--full") {
                        full = true;
                    } else if (args[i] == "--bios-dir" && i + 1 < args.size()) {
                        roots.emplace_back(args[++i]);
                    } else {
                        std::cerr << "unexpected bios scan arg: " << args[i] << "\n";
                        return 2;
                    }
                }
                return cmd_bios_scan(paths, catalog, cfg, roots, full);
            }
            std::cerr << "usage: retcomm bios scan [--full] [--bios-dir DIR] | "
                         "retcomm bios list\n";
            return 2;
        }
        if (cmd == "scan") {
            std::vector<fs::path> roots;
            bool full = false;
            for (size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--full") {
                    full = true;
                } else if (args[i] == "--rom-dir" && i + 1 < args.size()) {
                    roots.emplace_back(args[++i]);
                } else {
                    std::cerr << "unexpected scan arg: " << args[i] << "\n";
                    return 2;
                }
            }
            return cmd_scan(paths, catalog, cfg, roots, full);
        }
        if (cmd == "install") {
            if (args.size() < 2) {
                std::cerr << "usage: retcomm install <title-id> [--force] [--prebuilt] "
                             "[--wine] [--dry-run]\n";
                return 2;
            }
            bool force = false;
            bool dry_run = false;
            bool use_wine = false;
            bool prefer_prebuilt = false;
            for (size_t i = 2; i < args.size(); ++i) {
                if (args[i] == "--force")
                    force = true;
                else if (args[i] == "--dry-run")
                    dry_run = true;
                else if (args[i] == "--wine")
                    use_wine = true;
                else if (args[i] == "--prebuilt")
                    prefer_prebuilt = true;
                else {
                    std::cerr << "unexpected install arg: " << args[i] << "\n";
                    return 2;
                }
            }
            return cmd_install(paths, catalog, args[1], force, dry_run, use_wine, prefer_prebuilt);
        }
        if (cmd == "build") {
            if (args.size() < 2) {
                std::cerr << "usage: retcomm build <title-id> [--force] [--force-generate] "
                             "[--rom PATH]\n";
                return 2;
            }
            bool force = false;
            bool force_generate = false;
            fs::path rom;
            for (size_t i = 2; i < args.size(); ++i) {
                if (args[i] == "--force")
                    force = true;
                else if (args[i] == "--force-generate")
                    force_generate = true;
                else if (args[i] == "--rom" && i + 1 < args.size())
                    rom = args[++i];
                else {
                    std::cerr << "unexpected build arg: " << args[i] << "\n";
                    return 2;
                }
            }
            return cmd_build(paths, catalog, args[1], force, force_generate, rom);
        }
        if (cmd == "pack") {
            if (args.size() < 3 || args[1] != "ensure") {
                std::cerr << "usage: retcomm pack ensure toolchain|sdk [--title ID] [--force]\n";
                return 2;
            }
            std::string title_id;
            bool force = false;
            for (size_t i = 3; i < args.size(); ++i) {
                if (args[i] == "--title" && i + 1 < args.size())
                    title_id = args[++i];
                else if (args[i] == "--force")
                    force = true;
                else {
                    std::cerr << "unexpected pack arg: " << args[i] << "\n";
                    return 2;
                }
            }
            return cmd_pack_ensure(paths, catalog, args[2], title_id, force);
        }
        if (cmd == "update") {
            if (args.size() < 2) {
                std::cerr << "usage: retcomm update <title-id>|--all [--force] "
                             "[--force-generate]\n";
                return 2;
            }
            bool force = false;
            bool force_generate = false;
            for (size_t i = 2; i < args.size(); ++i) {
                if (args[i] == "--force")
                    force = true;
                else if (args[i] == "--force-generate")
                    force_generate = true;
                else {
                    std::cerr << "unexpected update arg: " << args[i] << "\n";
                    return 2;
                }
            }
            return cmd_update(paths, catalog, args[1], force, force_generate);
        }
        if (cmd == "uninstall" || cmd == "remove") {
            if (args.size() < 2) {
                std::cerr << "usage: retcomm uninstall <title-id> "
                             "[--keep-saves|--delete-saves] [--dry-run]\n";
                return 2;
            }
            retcomm::UninstallOptions opts;
            for (size_t i = 2; i < args.size(); ++i) {
                if (args[i] == "--keep-saves")
                    opts.keep_saves = true;
                else if (args[i] == "--delete-saves" || args[i] == "--purge")
                    opts.keep_saves = false;
                else if (args[i] == "--dry-run")
                    opts.dry_run = true;
                else {
                    std::cerr << "unexpected uninstall arg: " << args[i] << "\n";
                    return 2;
                }
            }
            return cmd_uninstall(paths, catalog, args[1], opts);
        }
        if (cmd == "orphans") {
            bool do_remove = false;
            retcomm::OrphanCleanupOptions opts;
            size_t i = 1;
            if (i < args.size() && (args[i] == "list" || args[i] == "remove")) {
                do_remove = (args[i] == "remove");
                ++i;
            } else if (i < args.size() && args[i].rfind("-", 0) != 0) {
                std::cerr << "usage: retcomm orphans [list|remove] "
                             "[--keep-saves|--delete-saves] [--dry-run] [--no-prune]\n";
                return 2;
            }
            for (; i < args.size(); ++i) {
                if (args[i] == "--keep-saves")
                    opts.keep_saves = true;
                else if (args[i] == "--delete-saves" || args[i] == "--purge")
                    opts.keep_saves = false;
                else if (args[i] == "--dry-run")
                    opts.dry_run = true;
                else if (args[i] == "--no-prune")
                    opts.prune_indexes = false;
                else {
                    std::cerr << "unexpected orphans arg: " << args[i] << "\n";
                    return 2;
                }
            }
            return cmd_orphans(paths, catalog, do_remove, opts);
        }
        if (cmd == "cache") {
            if (args.size() < 2 || args[1] != "gc") {
                std::cerr << "usage: retcomm cache gc\n";
                return 2;
            }
            retcomm::AppConfig gc_cfg = cfg;
            gc_cfg.auto_gc_caches = true;
            const auto cr = retcomm::run_cache_gc(paths, gc_cfg);
            for (const auto& m : cr.messages) std::cout << m << "\n";
            std::cout << cr.message << "\n";
            return cr.ok ? 0 : 1;
        }
        if (cmd == "launch") {
            if (args.size() < 2) {
                std::cerr << "usage: retcomm launch <title-id> [--rom PATH] [--bios PATH] "
                             "[--save PATH] [--mode default|direct|netplay] [--detach] "
                             "[--dry-run]\n";
                return 2;
            }
            retcomm::LaunchOptions opts;
            for (size_t i = 2; i < args.size(); ++i) {
                if (args[i] == "--rom" && i + 1 < args.size()) {
                    opts.rom_path = args[++i];
                } else if (args[i] == "--bios" && i + 1 < args.size()) {
                    opts.bios_path = args[++i];
                } else if ((args[i] == "--save" || args[i] == "--save-path") &&
                           i + 1 < args.size()) {
                    opts.save_path = args[++i];
                } else if (args[i] == "--mode" && i + 1 < args.size()) {
                    std::string err;
                    opts.mode = retcomm::parse_launch_mode(args[++i], &err);
                    if (!err.empty()) {
                        std::cerr << err << "\n";
                        return 2;
                    }
                } else if (args[i] == "--detach") {
                    opts.detach = true;
                } else if (args[i] == "--dry-run") {
                    opts.dry_run = true;
                } else {
                    std::cerr << "unexpected launch arg: " << args[i] << "\n";
                    return 2;
                }
            }
            return cmd_launch(paths, catalog, args[1], opts);
        }
        std::cerr << "unknown command: " << cmd << "\n";
        print_help(argv[0]);
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
