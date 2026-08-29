#include <algorithm>
#include "retcomm/catalog.hpp"
#include "retcomm/hash.hpp"
#include "retcomm/paths.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
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
        if (id.contains("track_counts") && id.at("track_counts").is_array()) {
            for (const auto& v : id.at("track_counts")) {
                if (v.is_number_unsigned() || v.is_number_integer()) {
                    const int n = v.get<int>();
                    if (n >= 1) t.rom_identity.track_counts.push_back(n);
                }
            }
        }
        // Per-disc identity for multi-disc sets. Ignored when fewer than two
        // entries — a lone disc is just the flat identity written the long way.
        if (id.contains("discs") && id.at("discs").is_array()) {
            for (const auto& dj : id.at("discs")) {
                if (!dj.is_object()) continue;
                DiscIdentity d;
                d.index = dj.value("index", 0);
                d.serial = dj.value("serial", "");
                d.cue_name = dj.value("cue_name", "");
                d.bin_name = dj.value("bin_name", "");
                for (auto& c : string_array(dj, "crc32")) d.crc32.push_back(to_lower_hex(c));
                for (auto& c : string_array(dj, "md5")) d.md5.push_back(to_lower_hex(c));
                for (auto& c : string_array(dj, "sha1")) d.sha1.push_back(to_lower_hex(c));
                for (auto& c : string_array(dj, "sha256")) d.sha256.push_back(to_lower_hex(c));
                if (dj.contains("sizes") && dj.at("sizes").is_array()) {
                    for (const auto& v : dj.at("sizes")) {
                        if (v.is_number_unsigned() || v.is_number_integer())
                            d.sizes.push_back(v.get<std::uint64_t>());
                    }
                }
                if (dj.contains("track_counts") && dj.at("track_counts").is_array()) {
                    for (const auto& v : dj.at("track_counts")) {
                        if (v.is_number_unsigned() || v.is_number_integer()) {
                            const int n = v.get<int>();
                            if (n >= 1) d.track_counts.push_back(n);
                        }
                    }
                }
                if (d.index <= 0) d.index = static_cast<int>(t.rom_identity.discs.size()) + 1;
                t.rom_identity.discs.push_back(std::move(d));
            }
            std::sort(t.rom_identity.discs.begin(), t.rom_identity.discs.end(),
                      [](const DiscIdentity& a, const DiscIdentity& b) {
                          return a.index < b.index;
                      });
            if (t.rom_identity.discs.size() < 2) t.rom_identity.discs.clear();
        }
        t.rom_identity.require_cue = id.value("require_cue", false);
        if (!t.rom_identity.require_cue) {
            for (int n : t.rom_identity.track_counts) {
                if (n > 1) {
                    t.rom_identity.require_cue = true;
                    break;
                }
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

    if (j.contains("netplay") && j.at("netplay").is_object()) {
        const auto& n = j.at("netplay");
        t.netplay.supported = n.value("supported", false);
        t.netplay.stack = n.value("stack", "");
        t.netplay.game_name = n.value("game_name", "");
        t.netplay.game_version = n.value("game_version", "");
        t.netplay.max_slots = n.value("max_slots", 2);
        if (t.netplay.max_slots < 2) t.netplay.max_slots = 2;
        t.netplay.lobby_url = n.value("lobby_url", "");
        t.netplay.transports = string_array(n, "transports");
        t.netplay.match_caps_schema = n.value("match_caps_schema", "");
        // Incomplete / unknown stack → treat as unsupported.
        if (!t.netplay.supported || t.netplay.stack != "recomp-net" ||
            t.netplay.game_name.empty()) {
            t.netplay.supported = false;
        }
    }

    if (j.contains("build") && j.at("build").is_object()) {
        const auto& b = j.at("build");
        t.build.enabled = b.value("enabled", false);
        if (b.contains("source") && b.at("source").is_object()) {
            const auto& s = b.at("source");
            t.build.source.github = s.value("github", "");
            t.build.source.ref = s.value("ref", "");
        }
        auto parse_pack = [](const json& p, TitleBuildPack& out) {
            out.id = p.value("id", "");
            out.github = p.value("github", "");
            out.min_version = p.value("min_version", "");
            if (p.contains("asset_glob") && p.at("asset_glob").is_object()) {
                const auto& g = p.at("asset_glob");
                out.asset_glob_linux = g.value("linux", "");
                out.asset_glob_windows = g.value("windows", "");
                out.asset_glob_macos = g.value("macos", "");
            }
        };
        if (b.contains("sdk") && b.at("sdk").is_object())
            parse_pack(b.at("sdk"), t.build.sdk);
        if (b.contains("toolchain") && b.at("toolchain").is_object())
            parse_pack(b.at("toolchain"), t.build.toolchain);
        if (b.contains("generate") && b.at("generate").is_object()) {
            const auto& g = b.at("generate");
            t.build.generate.engine = g.value("engine", t.build.generate.engine);
            t.build.generate.cfg_dir = g.value("cfg_dir", t.build.generate.cfg_dir);
            t.build.generate.out_dir = g.value("out_dir", t.build.generate.out_dir);
            t.build.generate.funcs_h = g.value("funcs_h", t.build.generate.funcs_h);
            t.build.generate.cfg_roots = g.value("cfg_roots", t.build.generate.cfg_roots);
            t.build.generate.config = g.value("config", t.build.generate.config);
        }
        if (b.contains("cmake") && b.at("cmake").is_object()) {
            const auto& c = b.at("cmake");
            t.build.cmake.build_dir = c.value("build_dir", t.build.cmake.build_dir);
            t.build.cmake.target = c.value("target", "");
            t.build.cmake.config = c.value("config", t.build.cmake.config);
        }
        if (t.build.source.github.empty())
            t.build.source.github = t.release.github;
        if (t.build.cmake.target.empty())
            t.build.cmake.target = t.launch_binary_for_host();
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

bool Title::supports_netplay() const {
    return netplay.supported && netplay.stack == "recomp-net" && !netplay.game_name.empty();
}

const std::string& TitleBuildPack::asset_glob_for_os(const std::string& os) const {
    if (os == "windows") return asset_glob_windows;
    if (os == "macos") return asset_glob_macos;
    return asset_glob_linux;
}

const std::string& TitleBuildPack::asset_glob_for_host() const {
    return asset_glob_for_os(host_os_key());
}

bool Title::supports_local_build() const {
    if (!build.enabled) return false;
    if (build.source.ref.empty()) return false;
    const std::string src_gh =
        build.source.github.empty() ? release.github : build.source.github;
    if (src_gh.empty()) return false;
    // SDK tools may come from a separate pack *or* be embedded in the host
    // release zip (one-zip titles like BPE). Require one of the two.
    const bool has_sdk_pack = !build.sdk.id.empty() && !build.sdk.github.empty() &&
                              !build.sdk.asset_glob_for_host().empty();
    const bool has_embedded_zip =
        !release.github.empty() && !asset_glob_for_host().empty();
    if (!has_sdk_pack && !has_embedded_zip) return false;
    // Toolchain: downloadable pack and/or embedded under release zip toolchain/.
    if (build.toolchain.id.empty()) return false;
    const bool has_tc_pack = !build.toolchain.github.empty() &&
                             !build.toolchain.asset_glob_for_host().empty();
    if (!has_tc_pack && !has_embedded_zip) return false;
    if (build.cmake.target.empty() && launch_binary_for_host().empty()) return false;
    return true;
}

bool Title::supports_prebuilt_install() const {
    return !release.github.empty() && !asset_glob_for_host().empty() &&
           !launch_binary_for_host().empty();
}

bool Title::prefers_local_build_install(bool prefer_prebuilt) const {
    return !prefer_prebuilt && supports_local_build();
}

std::string normalize_netplay_version(std::string version) {
    while (!version.empty() &&
           std::isspace(static_cast<unsigned char>(version.front())))
        version.erase(version.begin());
    while (!version.empty() &&
           std::isspace(static_cast<unsigned char>(version.back())))
        version.pop_back();
    if (version.empty()) return "dev";
    // Strip a single leading 'v' when the rest looks like a version (digit / digit.).
    if ((version[0] == 'v' || version[0] == 'V') && version.size() > 1) {
        const unsigned char c = static_cast<unsigned char>(version[1]);
        if (std::isdigit(c)) version.erase(version.begin());
    }
    return version;
}

bool netplay_versions_equal(const std::string& a, const std::string& b) {
    return normalize_netplay_version(a) == normalize_netplay_version(b);
}

const Title* Catalog::find(const std::string& id) const {
    for (const auto& t : titles) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

const Title* Catalog::find_by_netplay_game_name(const std::string& game_name) const {
    if (game_name.empty()) return nullptr;
    for (const auto& t : titles) {
        if (t.supports_netplay() && t.netplay.game_name == game_name) return &t;
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
    cat.catalog_date = index.value("catalog_date", "");
    cat.release_tag = index.value("release_tag", "");
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
