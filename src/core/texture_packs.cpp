#include "retcomm/texture_packs.hpp"

#include "retcomm/install.hpp"   // extract_archive_to

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>

namespace retcomm {
namespace {

using nlohmann::json;

bool is_hex_run(const std::string& s) {
    if (s.empty() || s.size() > 8) return false;
    for (const char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

// Sanitise an arbitrary archive/folder stem into a directory-safe pack id.
std::string make_pack_id(std::string stem) {
    // Beetle packs are usually distributed as "<something>-texture-replacements";
    // that suffix names the format, not the pack, so it makes a poor id.
    const std::string suffix = "-texture-replacements";
    if (stem.size() > suffix.size() &&
        stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0)
        stem.erase(stem.size() - suffix.size());

    std::string out;
    for (const char c : stem) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u))          out.push_back(static_cast<char>(std::tolower(u)));
        else if (!out.empty() && out.back() != '-') out.push_back('-');
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.empty()) out = "pack";
    return out;
}

// Count the .png files in a directory whose names are <hex>-<hex>.png.
int count_textures(const fs::path& dir, long long* bytes_out) {
    int n = 0;
    long long bytes = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        if (!is_texture_pack_file(e.path().filename().string())) continue;
        n++;
        bytes += static_cast<long long>(e.file_size(ec));
    }
    if (bytes_out) *bytes_out = bytes;
    return n;
}

// The directory inside `root` that actually holds the textures. Packs arrive
// wrapped in one or two levels (a GitHub zip adds "<repo>-<branch>/", and the
// Beetle convention adds "<cue name>-texture-replacements/"), so descend
// through single-child directories until textures appear.
fs::path find_texture_root(const fs::path& root) {
    fs::path cur = root;
    for (int depth = 0; depth < 4; ++depth) {
        long long unused = 0;
        if (count_textures(cur, &unused) > 0) return cur;

        std::error_code ec;
        fs::path only_child;
        int dirs = 0;
        for (const auto& e : fs::directory_iterator(cur, ec)) {
            if (ec) break;
            if (!e.is_directory(ec)) continue;
            only_child = e.path();
            if (++dirs > 1) break;
        }
        if (dirs != 1) break;
        cur = only_child;
    }
    return cur;
}

void read_pack_json(const fs::path& dir, TexturePack& p) {
    std::ifstream in(dir / "pack.json");
    if (!in) return;
    try {
        json j;
        in >> j;
        p.name        = j.value("name", p.name);
        p.author      = j.value("author", std::string{});
        p.version     = j.value("version", std::string{});
        p.description = j.value("description", std::string{});
        p.source_url  = j.value("source_url", std::string{});
    } catch (...) {
        // A malformed pack.json costs metadata, not the pack.
    }
}

void read_coverage_json(const fs::path& dir, TexturePack& p) {
    std::ifstream in(dir / "coverage.json");
    if (!in) return;
    try {
        json j;
        in >> j;
        p.coverage_entries = j.value("pack_entries", 0);
        p.coverage_matched = j.value("matched", 0);
        p.coverage_seconds = j.value("session_seconds", 0LL);
        p.has_coverage = true;
    } catch (...) {
    }
}

} // namespace

bool is_texture_pack_file(const std::string& name) {
    const auto dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (ext != "png") return false;   // psxrecomp decodes PNG only today

    const std::string stem = name.substr(0, dot);
    const auto dash = stem.find('-');
    if (dash == std::string::npos) return false;
    return is_hex_run(stem.substr(0, dash)) && is_hex_run(stem.substr(dash + 1));
}

fs::path texture_packs_dir(const Paths& paths, const std::string& title_id) {
    return paths.data_dir / "texturepacks" / title_id;
}

std::vector<TexturePack> scan_texture_packs(const Paths& paths,
                                            const std::string& title_id) {
    std::vector<TexturePack> out;
    if (title_id.empty()) return out;

    const fs::path root = texture_packs_dir(paths, title_id);
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root, ec)) {
        if (ec) break;
        if (!e.is_directory(ec)) continue;
        // Skip dot-directories: .import-tmp is staged in here, and a crash
        // mid-import must not leave a phantom pack in the list.
        const std::string dir_name = e.path().filename().string();
        if (dir_name.empty() || dir_name.front() == '.') continue;

        TexturePack p;
        p.id   = dir_name;
        p.name = p.id;
        p.dir  = e.path();
        p.texture_count = count_textures(p.dir, &p.bytes);
        read_pack_json(p.dir, p);
        read_coverage_json(p.dir, p);
        out.push_back(std::move(p));
    }

    std::sort(out.begin(), out.end(), [](const TexturePack& a, const TexturePack& b) {
        if (a.name != b.name) return a.name < b.name;
        return a.id < b.id;
    });
    return out;
}

bool install_texture_pack(const Paths& paths, const std::string& title_id,
                          const fs::path& source, std::string* out_pack_id,
                          std::string* error) {
    const auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    if (title_id.empty()) return fail("no title selected");

    std::error_code ec;
    if (!fs::exists(source, ec)) return fail("no such file or folder: " + source.string());

    // Unpack archives into a scratch dir; a folder source is used in place.
    const fs::path staging = texture_packs_dir(paths, title_id) / ".import-tmp";
    fs::remove_all(staging, ec);

    fs::path content = source;
    const bool is_archive = fs::is_regular_file(source, ec);
    if (is_archive) {
        fs::create_directories(staging, ec);
        if (ec) return fail("cannot create " + staging.string() + ": " + ec.message());
        std::string err;
        if (!extract_archive_to(source, staging, &err)) {
            fs::remove_all(staging, ec);
            return fail("cannot extract pack: " + err);
        }
        content = staging;
    }

    const fs::path texture_root = find_texture_root(content);
    long long bytes = 0;
    if (count_textures(texture_root, &bytes) == 0) {
        fs::remove_all(staging, ec);
        return fail("no <hash>-<hash>.png textures found in " + source.string() +
                    " — this does not look like a Beetle-format texture pack");
    }

    // Name the pack after what the user actually chose, not the wrapper folder
    // the format happens to use.
    std::string id = make_pack_id(is_archive ? source.stem().string()
                                             : source.filename().string());
    const fs::path root = texture_packs_dir(paths, title_id);
    fs::path dest = root / id;
    for (int n = 2; fs::exists(dest, ec) && n < 100; ++n)
        dest = root / (id + "-" + std::to_string(n));
    id = dest.filename().string();

    fs::create_directories(dest, ec);
    if (ec) {
        fs::remove_all(staging, ec);
        return fail("cannot create " + dest.string() + ": " + ec.message());
    }

    // Copy the textures themselves — flattened, so a wrapper directory in the
    // source never reaches the store and the game needs no path convention.
    int copied = 0;
    for (const auto& e : fs::directory_iterator(texture_root, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        const std::string fn = e.path().filename().string();
        if (!is_texture_pack_file(fn)) continue;
        fs::copy_file(e.path(), dest / fn, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            fs::remove_all(dest, ec);
            fs::remove_all(staging, ec);
            return fail("cannot copy " + fn + ": " + ec.message());
        }
        copied++;
    }

    // Carry the pack's own metadata when it has any, else synthesise it so the
    // manager always has a name to show.
    const fs::path src_meta = texture_root / "pack.json";
    if (fs::exists(src_meta, ec)) {
        fs::copy_file(src_meta, dest / "pack.json",
                      fs::copy_options::overwrite_existing, ec);
    } else {
        json j;
        j["id"] = id;
        j["name"] = is_archive ? source.stem().string() : source.filename().string();
        j["version"] = "";
        j["author"] = "";
        j["description"] = "";
        j["source_url"] = "";
        std::ofstream out(dest / "pack.json");
        if (out) out << j.dump(2) << '\n';
    }

    fs::remove_all(staging, ec);
    if (out_pack_id) *out_pack_id = id;
    (void)copied;
    return true;
}

bool remove_texture_pack(const Paths& paths, const std::string& title_id,
                         const std::string& pack_id, std::string* error) {
    if (title_id.empty() || pack_id.empty()) {
        if (error) *error = "no pack selected";
        return false;
    }
    // Refuse anything that could escape the store.
    if (pack_id.find('/') != std::string::npos ||
        pack_id.find('\\') != std::string::npos || pack_id == "..") {
        if (error) *error = "invalid pack id";
        return false;
    }

    const fs::path dir = texture_packs_dir(paths, title_id) / pack_id;
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (ec) {
        if (error) *error = "cannot remove " + dir.string() + ": " + ec.message();
        return false;
    }
    return true;
}

} // namespace retcomm
