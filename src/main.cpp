#include "retcomm/catalog.hpp"
#include "retcomm/config.hpp"
#include "retcomm/install.hpp"
#include "retcomm/paths.hpp"
#include "retcomm/romm.hpp"
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
        << "Retcomm Launcher — multi-title hub for recomp/decomp projects\n\n"
        << "Usage:\n"
        << "  " << argv0 << " [--catalog DIR] <command> [args]\n\n"
        << "Commands:\n"
        << "  list                         List catalog titles\n"
        << "  status                       Show paths, library, installs\n"
        << "  config                       Show / explain config.json\n"
        << "  scan [--rom-dir DIR ...]     Scan RomM-style library + match catalog\n"
        << "  install <title-id>           Show install plan (stub)\n"
        << "  launch <title-id> [--rom P]  Show launch plan (stub)\n"
        << "  romm                         Show RomM config / stub ping\n"
        << "  help                         This message\n\n"
        << "Scan uses ~/.config/retcomm/config.json library_root and only walks\n"
        << "platform folders needed by the catalog (e.g. snes/, n64/, ps/).\n"
        << "Pass --rom-dir to override for one run (library root or platform folder).\n";
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
    std::cout << "config:  " << paths.config_path.string() << "\n"
              << "data:    " << paths.data_dir.string() << "\n"
              << "apps:    " << paths.apps_dir.string() << "\n"
              << "catalog: " << cat.root.string() << "\n"
              << "library: "
              << (cfg.library_root.empty() ? "(unset)" : cfg.library_root.string())
              << "\n\n";

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
        std::cout << "  " << t.id << " — " << plan.message << "\n";
    }
    return 0;
}

int cmd_config(const retcomm::Paths& paths, const retcomm::AppConfig& cfg) {
    std::cout << "Config file: " << paths.config_path.string() << "\n"
              << "library_root: "
              << (cfg.library_root.empty() ? "(unset)" : cfg.library_root.string())
              << "\n"
              << "exclude_dirs: ";
    for (size_t i = 0; i < cfg.exclude_dirs.size(); ++i) {
        if (i) std::cout << ", ";
        std::cout << cfg.exclude_dirs[i];
    }
    std::cout << "\n"
              << "romm: "
              << (cfg.romm.enabled() ? cfg.romm.base_url : "(not configured)") << "\n\n"
              << "Example config.json:\n"
              << "{\n"
              << "  \"library_root\": \"/mnt/crucial4tb/Emulation/roms\",\n"
              << "  \"platform_folders\": {\n"
              << "    \"psx\": [\"ps\", \"ps1\"],\n"
              << "    \"snes\": [\"snes\"],\n"
              << "    \"n64\": [\"n64\"]\n"
              << "  },\n"
              << "  \"exclude_dirs\": [\"torrents\", \"emulators\"],\n"
              << "  \"romm\": {\n"
              << "    \"base_url\": \"https://your-romm.example\",\n"
              << "    \"api_token\": \"…\"\n"
              << "  }\n"
              << "}\n";
    return 0;
}

int cmd_scan(const retcomm::Catalog& cat, const retcomm::AppConfig& cfg,
             const std::vector<fs::path>& rom_dirs) {
    retcomm::ScanOptions opts;
    opts.on_progress = make_progress_printer();

    retcomm::ScanResult result;
    if (!rom_dirs.empty()) {
        std::cerr << "Scanning " << rom_dirs.size() << " path(s) (platform-scoped)…\n";
        result = retcomm::scan_rom_roots(cat, cfg, rom_dirs, opts);
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
              << "  hash-skipped: " << result.skipped_hash << "\n";
    for (const auto& err : result.errors) std::cerr << "  warning: " << err << "\n";

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
    }
    return 0;
}

int cmd_install(const retcomm::Paths& paths, const retcomm::Catalog& cat,
                const std::string& id) {
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
    auto plan = retcomm::plan_install(paths, *t);
    std::cout << plan.message;
    return 0;
}

int cmd_launch(const retcomm::Paths& paths, const retcomm::Catalog& cat,
               const std::string& id, const fs::path& rom) {
    const auto* t = cat.find(id);
    if (!t) {
        std::cerr << "unknown title: " << id << "\n";
        return 1;
    }
    auto plan = retcomm::plan_launch(paths, *t, rom);
    std::cout << plan.message;
    return plan.ready ? 0 : 1;
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
    retcomm::Catalog catalog;
    try {
        const fs::path cat_dir =
            retcomm::resolve_catalog_dir(exe_dir_from(argv[0]), catalog_override);
        catalog = retcomm::load_catalog(cat_dir);
    } catch (const std::exception& e) {
        std::cerr << "catalog error: " << e.what() << "\n";
        return 1;
    }

    const std::string& cmd = args[0];
    try {
        if (cmd == "list") return cmd_list(catalog);
        if (cmd == "status") return cmd_status(paths, cfg, catalog);
        if (cmd == "config") return cmd_config(paths, cfg);
        if (cmd == "romm") return cmd_romm(paths, cfg);
        if (cmd == "scan") {
            std::vector<fs::path> roots;
            for (size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--rom-dir" && i + 1 < args.size()) {
                    roots.emplace_back(args[++i]);
                } else {
                    std::cerr << "unexpected scan arg: " << args[i] << "\n";
                    return 2;
                }
            }
            return cmd_scan(catalog, cfg, roots);
        }
        if (cmd == "install") {
            if (args.size() < 2) {
                std::cerr << "usage: retcomm install <title-id>\n";
                return 2;
            }
            return cmd_install(paths, catalog, args[1]);
        }
        if (cmd == "launch") {
            if (args.size() < 2) {
                std::cerr << "usage: retcomm launch <title-id> [--rom PATH]\n";
                return 2;
            }
            fs::path rom;
            for (size_t i = 2; i < args.size(); ++i) {
                if (args[i] == "--rom" && i + 1 < args.size()) {
                    rom = args[++i];
                } else {
                    std::cerr << "unexpected launch arg: " << args[i] << "\n";
                    return 2;
                }
            }
            return cmd_launch(paths, catalog, args[1], rom);
        }
        std::cerr << "unknown command: " << cmd << "\n";
        print_help(argv[0]);
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
