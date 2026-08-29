#include "retcomm/config.hpp"
#include "retcomm/paths.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <system_error>

namespace retcomm {
namespace {

using nlohmann::json;

std::vector<std::string> string_array(const json& j) {
    std::vector<std::string> out;
    if (!j.is_array()) return out;
    for (const auto& v : j) {
        if (v.is_string()) out.push_back(v.get<std::string>());
    }
    return out;
}

} // namespace

std::map<std::string, std::vector<std::string>> default_platform_folders() {
    // RomM / ES-DE style folder names. Catalog uses short slugs; folders may differ
    // (notably PlayStation → "ps").
    return {
        {"snes", {"snes"}},
        {"n64", {"n64"}},
        {"psx", {"ps", "ps1", "psx"}},
        {"gba", {"gba"}},
        {"gbc", {"gbc"}},
        {"gb", {"gb"}},
        {"nes", {"nes"}},
        {"fds", {"fds"}},
        {"genesis", {"genesis", "smd", "megadrive"}},
        {"smd", {"smd", "genesis", "megadrive"}},
        {"sms", {"sms", "mastersystem"}},
        {"gg", {"gamegear", "gg"}},
        {"gamegear", {"gamegear", "gg"}},
        {"vb", {"vb", "virtualboy"}},
        {"nds", {"nds"}},
        {"psp", {"psp"}},
        {"gc", {"gc", "gamecube"}},
        {"wii", {"wii"}},
    };
}

std::vector<std::string> default_exclude_dirs() {
    return {"torrents", "emulators", ".git", "@eaDir"};
}

SuggestedLibraryRoots suggested_emulation_roots() {
    SuggestedLibraryRoots out;
    const fs::path home = user_home_dir();
    if (home.empty()) return out;
    const fs::path emu = home / "Emulation";
    out.library_root = emu / "roms";
    out.bios_root = emu / "bios";
    out.saves_root = emu / "saves";
    return out;
}

fs::path ensure_platform_dir(const fs::path& root, const std::vector<std::string>& folders) {
    if (root.empty()) return {};
    std::error_code ec;
    // Prefer an existing folder; otherwise create the first configured name.
    for (const auto& folder : folders) {
        if (folder.empty()) continue;
        fs::path p = root / folder;
        if (fs::is_directory(p, ec)) return p;
    }
    const std::string folder = folders.empty() ? std::string{} : folders.front();
    fs::path p = folder.empty() ? root : (root / folder);
    fs::create_directories(p, ec);
    if (ec) return {};
    return p;
}

bool ensure_configured_platform_dirs(const AppConfig& cfg, std::string* error) {
    auto process_root = [&](const fs::path& root) -> bool {
        if (root.empty()) return true;
        for (const auto& [plat, folders] : cfg.platform_folders) {
            (void)plat;
            if (folders.empty()) continue;
            if (ensure_platform_dir(root, folders).empty()) {
                if (error)
                    *error = "cannot create platform folder under " + root.string();
                return false;
            }
        }
        return true;
    };
    return process_root(cfg.library_root) && process_root(cfg.bios_root) &&
           process_root(cfg.saves_root);
}

AppConfig normalize_config(AppConfig cfg) {
    auto defaults = default_platform_folders();
    for (auto& [plat, folders] : defaults) {
        if (!cfg.platform_folders.count(plat) || cfg.platform_folders[plat].empty())
            cfg.platform_folders[plat] = folders;
    }
    if (cfg.exclude_dirs.empty()) cfg.exclude_dirs = default_exclude_dirs();
    if (cfg.netplay.lobby_url.empty()) cfg.netplay.lobby_url = kDefaultNetplayLobbyUrl;
    if (cfg.keep_toolchain_versions < 1) cfg.keep_toolchain_versions = 1;
    if (cfg.keep_sdk_versions < 1) cfg.keep_sdk_versions = 1;
    if (cfg.keep_orphan_engine_pins < 0) cfg.keep_orphan_engine_pins = 0;
    if (cfg.keep_release_zips_per_repo < 1) cfg.keep_release_zips_per_repo = 1;
    if (cfg.idle_build_keep_days < 0) cfg.idle_build_keep_days = 0;
    if (cfg.ccache_max_gb < 0) cfg.ccache_max_gb = 0;

    // Drop empty install-root rows; keep first label for a path.
    {
        std::vector<InstallRootEntry> cleaned;
        for (auto& e : cfg.install_roots) {
            if (e.path.empty()) continue;
            bool dup = false;
            for (const auto& c : cleaned) {
                if (c.path == e.path) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;
            if (e.label.empty()) e.label = e.path.filename().string();
            if (e.label.empty()) e.label = e.path.string();
            cleaned.push_back(std::move(e));
        }
        cfg.install_roots = std::move(cleaned);
    }
    return cfg;
}

fs::path builtin_apps_dir(const Paths& paths) {
    return paths.data_dir / "apps";
}

std::vector<InstallRootEntry> effective_install_roots(const AppConfig& cfg, const Paths& paths) {
    if (cfg.install_roots.empty())
        return {InstallRootEntry{"Default", builtin_apps_dir(paths)}};
    return cfg.install_roots;
}

std::vector<InstallRootEntry> scan_install_roots(const AppConfig& cfg, const Paths& paths) {
    std::vector<InstallRootEntry> out = effective_install_roots(cfg, paths);
    const fs::path builtin = builtin_apps_dir(paths);
    bool has_builtin = false;
    for (const auto& e : out) {
        if (e.path == builtin) {
            has_builtin = true;
            break;
        }
    }
    if (!has_builtin) out.insert(out.begin(), InstallRootEntry{"Default", builtin});
    return out;
}

fs::path resolve_default_install_root(const AppConfig& cfg, const Paths& paths) {
    const auto roots = effective_install_roots(cfg, paths);
    if (!cfg.default_install_root.empty()) {
        for (const auto& e : roots) {
            if (same_install_root_path(e.path, cfg.default_install_root)) return e.path;
        }
    }
    return roots.empty() ? builtin_apps_dir(paths) : roots.front().path;
}

bool same_install_root_path(const fs::path& a, const fs::path& b) {
    if (a.empty() || b.empty()) return false;
    std::error_code ec;
    if (fs::equivalent(a, b, ec)) return true;
    ec.clear();
    const fs::path ca = fs::weakly_canonical(a, ec);
    if (ec || ca.empty()) return a == b;
    ec.clear();
    const fs::path cb = fs::weakly_canonical(b, ec);
    if (ec || cb.empty()) return a == b;
    return ca == cb;
}

int find_install_root_index(const std::vector<InstallRootEntry>& roots, const fs::path& apps_dir) {
    if (apps_dir.empty()) return -1;
    for (size_t i = 0; i < roots.size(); ++i) {
        if (same_install_root_path(roots[i].path, apps_dir)) return static_cast<int>(i);
    }
    return -1;
}

Paths with_apps_dir(Paths paths, const fs::path& apps_dir) {
    if (!apps_dir.empty()) paths.apps_dir = apps_dir;
    return paths;
}

std::string AppConfig::resolve_netplay_lobby_url(const std::string& title_lobby_url) const {
    if (!title_lobby_url.empty()) return title_lobby_url;
    if (!netplay.lobby_url.empty()) return netplay.lobby_url;
    return kDefaultNetplayLobbyUrl;
}

std::vector<std::string> AppConfig::folders_for_platform(const std::string& platform) const {
    auto it = platform_folders.find(platform);
    if (it != platform_folders.end() && !it->second.empty()) return it->second;
    // Fall back to the slug itself (RomM often matches 1:1).
    return {platform};
}

std::vector<fs::path> AppConfig::platform_roots(const std::string& platform) const {
    std::vector<fs::path> out;
    if (library_root.empty()) return out;
    for (const auto& folder : folders_for_platform(platform)) {
        fs::path p = library_root / folder;
        std::error_code ec;
        if (fs::is_directory(p, ec)) out.push_back(p);
    }
    return out;
}

std::vector<fs::path> AppConfig::bios_roots_for_platform(const std::string& platform) const {
    std::vector<fs::path> out;
    if (bios_root.empty()) return out;
    std::error_code ec;
    if (fs::is_directory(bios_root, ec)) out.push_back(bios_root);
    for (const auto& folder : folders_for_platform(platform)) {
        fs::path p = bios_root / folder;
        if (fs::is_directory(p, ec)) out.push_back(p);
    }
    return out;
}

fs::path AppConfig::saves_dir_for_platform(const std::string& platform, bool create) const {
    if (saves_root.empty()) return {};
    const auto folders = folders_for_platform(platform);
    const std::string folder = folders.empty() ? platform : folders.front();
    fs::path dir = saves_root / folder;
    if (create) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) return {};
    }
    return dir;
}

AppConfig load_app_config(const fs::path& config_path) {
    AppConfig cfg;
    cfg.platform_folders = default_platform_folders();
    cfg.exclude_dirs = default_exclude_dirs();

    std::ifstream in(config_path);
    if (!in) return normalize_config(std::move(cfg));

    try {
        json j;
        in >> j;
        if (j.contains("library_root") && j.at("library_root").is_string())
            cfg.library_root = j.at("library_root").get<std::string>();
        if (j.contains("bios_root") && j.at("bios_root").is_string())
            cfg.bios_root = j.at("bios_root").get<std::string>();
        if (j.contains("saves_root") && j.at("saves_root").is_string())
            cfg.saves_root = j.at("saves_root").get<std::string>();

        if (j.contains("platform_folders") && j.at("platform_folders").is_object()) {
            for (auto it = j.at("platform_folders").begin();
                 it != j.at("platform_folders").end(); ++it) {
                if (it.value().is_string())
                    cfg.platform_folders[it.key()] = {it.value().get<std::string>()};
                else if (it.value().is_array())
                    cfg.platform_folders[it.key()] = string_array(it.value());
            }
        }

        if (j.contains("exclude_dirs")) cfg.exclude_dirs = string_array(j.at("exclude_dirs"));
        if (j.contains("prefer_local_boxart"))
            cfg.prefer_local_boxart = j.value("prefer_local_boxart", false);
        if (j.contains("filter_unsupported_titles"))
            cfg.filter_unsupported_titles = j.value("filter_unsupported_titles", false);
        if (j.contains("check_updates_on_startup"))
            cfg.check_updates_on_startup = j.value("check_updates_on_startup", true);
        if (j.contains("auto_scan_after_catalog_update"))
            cfg.auto_scan_after_catalog_update =
                j.value("auto_scan_after_catalog_update", true);
        if (j.contains("check_updates_before_launch"))
            cfg.check_updates_before_launch = j.value("check_updates_before_launch", true);
        if (j.contains("auto_clean_build_dirs"))
            cfg.auto_clean_build_dirs = j.value("auto_clean_build_dirs", false);
        if (j.contains("auto_gc_caches"))
            cfg.auto_gc_caches = j.value("auto_gc_caches", true);
        if (j.contains("keep_toolchain_versions"))
            cfg.keep_toolchain_versions = j.value("keep_toolchain_versions", 2);
        if (j.contains("keep_sdk_versions"))
            cfg.keep_sdk_versions = j.value("keep_sdk_versions", 3);
        if (j.contains("keep_orphan_engine_pins"))
            cfg.keep_orphan_engine_pins = j.value("keep_orphan_engine_pins", 1);
        if (j.contains("keep_release_zips_per_repo"))
            cfg.keep_release_zips_per_repo = j.value("keep_release_zips_per_repo", 1);
        if (j.contains("idle_build_keep_days"))
            cfg.idle_build_keep_days = j.value("idle_build_keep_days", 14);
        if (j.contains("ccache_max_gb")) cfg.ccache_max_gb = j.value("ccache_max_gb", 5);

        if (j.contains("default_install_root") && j.at("default_install_root").is_string())
            cfg.default_install_root = j.at("default_install_root").get<std::string>();
        if (j.contains("install_roots") && j.at("install_roots").is_array()) {
            cfg.install_roots.clear();
            for (const auto& item : j.at("install_roots")) {
                InstallRootEntry e;
                if (item.is_string()) {
                    e.path = item.get<std::string>();
                } else if (item.is_object()) {
                    e.label = item.value("label", "");
                    e.path = item.value("path", "");
                }
                if (!e.path.empty()) cfg.install_roots.push_back(std::move(e));
            }
        }

        if (j.contains("catalog") && j.at("catalog").is_object()) {
            const auto& c = j.at("catalog");
            cfg.catalog.url = c.value("url", "");
            cfg.catalog.github_repo = c.value("github_repo", "");
            cfg.catalog.auto_update = c.value("auto_update", true);
        }

        if (j.contains("romm") && j.at("romm").is_object()) {
            const auto& r = j.at("romm");
            cfg.romm.base_url = r.value("base_url", "");
            cfg.romm.api_token = r.value("api_token", "");
            cfg.romm.sync_boxart = r.value("sync_boxart", false);
            while (!cfg.romm.base_url.empty() && cfg.romm.base_url.back() == '/')
                cfg.romm.base_url.pop_back();
        }

        if (j.contains("netplay") && j.at("netplay").is_object()) {
            const auto& n = j.at("netplay");
            cfg.netplay.lobby_url = n.value("lobby_url", "");
            cfg.netplay.display_name = n.value("display_name", "");
            cfg.netplay.prefer_ice = n.value("prefer_ice", false);
        }

        if (j.contains("github_token") && j.at("github_token").is_string())
            cfg.github_token = j.at("github_token").get<std::string>();
    } catch (...) {
        // Keep defaults on parse errors.
    }
    return normalize_config(std::move(cfg));
}

bool save_app_config(const fs::path& config_path, const AppConfig& cfg, std::string* error) {
    std::error_code ec;
    if (!config_path.parent_path().empty())
        fs::create_directories(config_path.parent_path(), ec);
    if (ec) {
        if (error) *error = "cannot create config dir: " + ec.message();
        return false;
    }

    json folders = json::object();
    for (const auto& [plat, names] : cfg.platform_folders) {
        if (names.empty()) continue;
        folders[plat] = names;
    }

    json roots = json::array();
    for (const auto& e : cfg.install_roots) {
        if (e.path.empty()) continue;
        roots.push_back({{"label", e.label}, {"path", e.path.string()}});
    }

    json j = {{"library_root", cfg.library_root.string()},
              {"bios_root", cfg.bios_root.string()},
              {"saves_root", cfg.saves_root.string()},
              {"install_roots", roots},
              {"default_install_root", cfg.default_install_root.string()},
              {"platform_folders", folders},
              {"exclude_dirs", cfg.exclude_dirs},
              {"prefer_local_boxart", cfg.prefer_local_boxart},
              {"filter_unsupported_titles", cfg.filter_unsupported_titles},
              {"check_updates_on_startup", cfg.check_updates_on_startup},
              {"auto_scan_after_catalog_update", cfg.auto_scan_after_catalog_update},
              {"check_updates_before_launch", cfg.check_updates_before_launch},
              {"auto_clean_build_dirs", cfg.auto_clean_build_dirs},
              {"auto_gc_caches", cfg.auto_gc_caches},
              {"keep_toolchain_versions", cfg.keep_toolchain_versions},
              {"keep_sdk_versions", cfg.keep_sdk_versions},
              {"keep_orphan_engine_pins", cfg.keep_orphan_engine_pins},
              {"keep_release_zips_per_repo", cfg.keep_release_zips_per_repo},
              {"idle_build_keep_days", cfg.idle_build_keep_days},
              {"ccache_max_gb", cfg.ccache_max_gb},
              {"catalog",
               {{"url", cfg.catalog.url},
                {"github_repo", cfg.catalog.github_repo},
                {"auto_update", cfg.catalog.auto_update}}},
              {"romm",
               {{"base_url", cfg.romm.base_url},
                {"api_token", cfg.romm.api_token},
                {"sync_boxart", cfg.romm.sync_boxart}}},
              {"netplay",
               {{"lobby_url", cfg.netplay.lobby_url},
                {"display_name", cfg.netplay.display_name},
                {"prefer_ice", cfg.netplay.prefer_ice}}},
              {"github_token", cfg.github_token}};

    std::ofstream out(config_path);
    if (!out) {
        if (error) *error = "cannot write " + config_path.string();
        return false;
    }
    out << j.dump(2) << "\n";
    if (!out) {
        if (error) *error = "failed writing " + config_path.string();
        return false;
    }
    return true;
}

} // namespace retcomm
