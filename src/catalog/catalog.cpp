#include "retcomm/catalog.hpp"
#include "retcomm/hash.hpp"
#include "retcomm/paths.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace retcomm {
namespace {

using nlohmann::json;

std::vector<std::string> string_array(const json& j, const char* key) {
    std::vector<std::string> out;
    if (!j.contains(key) || !j.at(key).is_array()) return out;
    for (const auto& v : j.at(key)) {
        if (v.is_string()) out.push_back(v.get<std::string>());
    }
    return out;
}

BiosIdentity parse_bios_identity(const json& id) {
    BiosIdentity out;
    out.required = id.value("required", true);
    for (auto& c : string_array(id, "crc32")) out.crc32.push_back(to_lower_hex(c));
    for (auto& c : string_array(id, "md5")) out.md5.push_back(to_lower_hex(c));
    for (auto& c : string_array(id, "sha1")) out.sha1.push_back(to_lower_hex(c));
    for (auto& c : string_array(id, "sha256")) out.sha256.push_back(to_lower_hex(c));
    out.filenames = string_array(id, "filenames");
    if (id.contains("sizes") && id.at("sizes").is_array()) {
        for (const auto& v : id.at("sizes")) {
            if (v.is_number_unsigned() || v.is_number_integer())
                out.sizes.push_back(v.get<std::uint64_t>());
        }
    }
    return out;
}

std::unordered_map<std::string, BiosIdentity> parse_platform_bios_defaults(
    const json& index) {
    std::unordered_map<std::string, BiosIdentity> out;
    if (!index.contains("platform_defaults") || !index.at("platform_defaults").is_object())
        return out;
    for (const auto& [platform, defs] : index.at("platform_defaults").items()) {
        if (!defs.is_object() || !defs.contains("bios_identity")) continue;
        const auto& bio = defs.at("bios_identity");
        if (bio.is_object()) out[platform] = parse_bios_identity(bio);
    }
    return out;
}

Title parse_title(const json& j) {
    Title t;
    t.id = j.value("id", "");
    t.name = j.value("name", t.id);
    t.kind = j.value("kind", "recomp");
    t.platform = j.value("platform", "");
    t.description = j.value("description", "");
    t.homepage = j.value("homepage", "");
    t.notes = j.value("notes", "");
    t.author_notes = j.value("author_notes", "");
    t.install_dir_name = j.value("install_dir_name", t.id);
    t.rom_extensions = string_array(j, "rom_extensions");

    if (j.contains("rom_identity") && j.at("rom_identity").is_object()) {
        const auto& id = j.at("rom_identity");
        for (auto& c : string_array(id, "crc32"))
            t.rom_identity.crc32.push_back(to_lower_hex(c));
        for (auto& c : string_array(id, "md5"))
            t.rom_identity.md5.push_back(to_lower_hex(c));
        for (auto& c : string_array(id, "sha1"))
            t.rom_identity.sha1.push_back(to_lower_hex(c));
        for (auto& c : string_array(id, "sha256"))
            t.rom_identity.sha256.push_back(to_lower_hex(c));
        t.rom_identity.disc_serials = string_array(id, "disc_serials");
        t.rom_identity.filenames = string_array(id, "filenames");
        if (id.contains("sizes") && id.at("sizes").is_array()) {
            for (const auto& v : id.at("sizes")) {
                if (v.is_number_unsigned() || v.is_number_integer())
                    t.rom_identity.sizes.push_back(v.get<std::uint64_t>());
            }
        }
    }

    if (j.contains("bios_identity") && j.at("bios_identity").is_object())
        t.bios_identity = parse_bios_identity(j.at("bios_identity"));

    if (j.contains("release") && j.at("release").is_object()) {
        const auto& r = j.at("release");
        t.release.github = r.value("github", "");
        t.release.allow_prerelease = r.value("allow_prerelease", false);
        if (r.contains("asset_glob") && r.at("asset_glob").is_object()) {
            const auto& g = r.at("asset_glob");
            t.release.asset_glob_linux = g.value("linux", "");
            t.release.asset_glob_windows = g.value("windows", "");
            t.release.asset_glob_macos = g.value("macos", "");
        }
    }

    if (j.contains("launch") && j.at("launch").is_object()) {
        const auto& l = j.at("launch");
        t.launch.linux = l.value("linux", "");
        t.launch.windows = l.value("windows", "");
        t.launch.macos = l.value("macos", "");
    }

    if (j.contains("romm") && j.at("romm").is_object()) {
        t.romm_platforms = string_array(j.at("romm"), "platforms");
    }

    if (j.contains("saves") && j.at("saves").is_object()) {
        const auto& s = j.at("saves");
        t.saves_sram_glob = string_array(s, "sram_glob");
        t.saves_memcard_glob = string_array(s, "memcard_glob");
    }

    if (t.id.empty()) throw std::runtime_error("title missing id");
    return t;
}

} // namespace

bool Title::has_rom_identity() const {
    return !rom_identity.crc32.empty() || !rom_identity.md5.empty() ||
           !rom_identity.sha1.empty() || !rom_identity.sha256.empty() ||
           !rom_identity.disc_serials.empty();
}

bool Title::has_bios_identity() const {
    return !bios_identity.crc32.empty() || !bios_identity.md5.empty() ||
           !bios_identity.sha1.empty() || !bios_identity.sha256.empty() ||
           !bios_identity.sizes.empty() || !bios_identity.filenames.empty() ||
           bios_identity.required;
}

bool Title::requires_bios() const {
    return has_bios_identity() && bios_identity.required;
}

const std::string& Title::launch_binary_for_os(const std::string& os) const {
    if (os == "windows") return launch.windows;
    if (os == "macos") return launch.macos;
    return launch.linux;
}

const std::string& Title::asset_glob_for_os(const std::string& os) const {
    if (os == "windows") return release.asset_glob_windows;
    if (os == "macos") return release.asset_glob_macos;
    return release.asset_glob_linux;
}

const std::string& Title::launch_binary_for_host() const {
    return launch_binary_for_os(host_os_key());
}

const std::string& Title::asset_glob_for_host() const {
    return asset_glob_for_os(host_os_key());
}

bool Title::supports_wine_install() const {
    return !release.asset_glob_windows.empty() && !launch.windows.empty();
}

std::string Title::github_owner() const {
    const auto slash = release.github.find('/');
    if (slash == std::string::npos || slash == 0) return {};
    return release.github.substr(0, slash);
}

std::string Title::github_source_url() const {
    if (!homepage.empty()) return homepage;
    if (release.github.empty()) return {};
    return "https://github.com/" + release.github;
}

const Title* Catalog::find(const std::string& id) const {
    for (const auto& t : titles) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

Catalog load_catalog(const fs::path& catalog_dir) {
    const fs::path index_path = catalog_dir / "index.json";
    std::ifstream in(index_path);
    if (!in) throw std::runtime_error("cannot open " + index_path.string());

    json index;
    in >> index;

    Catalog cat;
    cat.root = catalog_dir;
    cat.schema_version = index.value("schema_version", 1);
    cat.name = index.value("name", "catalog");
    const auto platform_bios = parse_platform_bios_defaults(index);

    if (!index.contains("titles") || !index.at("titles").is_array())
        throw std::runtime_error("index.json missing titles[]");

    for (const auto& idj : index.at("titles")) {
        if (!idj.is_string()) continue;
        const std::string id = idj.get<std::string>();
        const fs::path tip = catalog_dir / "titles" / (id + ".json");
        std::ifstream tin(tip);
        if (!tin) throw std::runtime_error("missing title manifest: " + tip.string());
        json tj;
        tin >> tj;
        Title t = parse_title(tj);
        if (t.id != id)
            throw std::runtime_error("title id mismatch in " + tip.string());
        // Inherit platform BIOS when the title omits bios_identity.
        // Explicit null opts out: "bios_identity": null
        if (!tj.contains("bios_identity")) {
            auto it = platform_bios.find(t.platform);
            if (it != platform_bios.end()) t.bios_identity = it->second;
        }
        cat.titles.push_back(std::move(t));
    }
    return cat;
}

} // namespace retcomm
