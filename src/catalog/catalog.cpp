#include "retcomm/catalog.hpp"
#include "retcomm/hash.hpp"
#include "retcomm/paths.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>

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

Title parse_title(const json& j) {
    Title t;
    t.id = j.value("id", "");
    t.name = j.value("name", t.id);
    t.kind = j.value("kind", "recomp");
    t.platform = j.value("platform", "");
    t.description = j.value("description", "");
    t.homepage = j.value("homepage", "");
    t.notes = j.value("notes", "");
    t.install_dir_name = j.value("install_dir_name", t.id);
    t.rom_extensions = string_array(j, "rom_extensions");

    if (j.contains("rom_identity") && j.at("rom_identity").is_object()) {
        const auto& id = j.at("rom_identity");
        for (auto& c : string_array(id, "crc32"))
            t.rom_identity.crc32.push_back(to_lower_hex(c));
        for (auto& c : string_array(id, "sha1"))
            t.rom_identity.sha1.push_back(to_lower_hex(c));
        for (auto& c : string_array(id, "sha256"))
            t.rom_identity.sha256.push_back(to_lower_hex(c));
        t.rom_identity.disc_serials = string_array(id, "disc_serials");
    }

    if (j.contains("release") && j.at("release").is_object()) {
        const auto& r = j.at("release");
        t.release.github = r.value("github", "");
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
    return !rom_identity.crc32.empty() || !rom_identity.sha1.empty() ||
           !rom_identity.sha256.empty() || !rom_identity.disc_serials.empty();
}

const std::string& Title::launch_binary_for_host() const {
    const std::string os = host_os_key();
    if (os == "windows") return launch.windows;
    if (os == "macos") return launch.macos;
    return launch.linux;
}

const std::string& Title::asset_glob_for_host() const {
    const std::string os = host_os_key();
    if (os == "windows") return release.asset_glob_windows;
    if (os == "macos") return release.asset_glob_macos;
    return release.asset_glob_linux;
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
        cat.titles.push_back(std::move(t));
    }
    return cat;
}

} // namespace retcomm
