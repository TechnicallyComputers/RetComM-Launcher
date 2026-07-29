#include "retcomm/config.hpp"

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

AppConfig normalize_config(AppConfig cfg) {
    auto defaults = default_platform_folders();
    for (auto& [plat, folders] : defaults) {
        if (!cfg.platform_folders.count(plat) || cfg.platform_folders[plat].empty())
            cfg.platform_folders[plat] = folders;
    }
    if (cfg.exclude_dirs.empty()) cfg.exclude_dirs = default_exclude_dirs();
    if (cfg.netplay.lobby_url.empty()) cfg.netplay.lobby_url = kDefaultNetplayLobbyUrl;
    return cfg;
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

    json j = {{"library_root", cfg.library_root.string()},
              {"bios_root", cfg.bios_root.string()},
              {"saves_root", cfg.saves_root.string()},
              {"platform_folders", folders},
              {"exclude_dirs", cfg.exclude_dirs},
              {"prefer_local_boxart", cfg.prefer_local_boxart},
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
                {"prefer_ice", cfg.netplay.prefer_ice}}}};

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
