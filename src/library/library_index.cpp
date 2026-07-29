#include "retcomm/library_index.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <unordered_set>

namespace retcomm {
namespace {

using nlohmann::json;

std::string norm_path(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    return (ec ? p : c).string();
}

bool under_any_root(const std::string& path, const std::vector<fs::path>& roots) {
    for (const auto& root : roots) {
        const std::string r = norm_path(root);
        if (path == r) return true;
        if (path.size() > r.size() && path.compare(0, r.size(), r) == 0 &&
            (path[r.size()] == '/' || path[r.size()] == '\\'))
            return true;
    }
    return false;
}

LibraryFile file_from_json(const json& j) {
    LibraryFile f;
    f.path = j.value("path", "");
    f.platform = j.value("platform", "");
    f.ext = j.value("ext", "");
    f.size = j.value("size", 0ull);
    f.mtime_sec = j.value("mtime_sec", 0ll);
    f.crc32 = j.value("crc32", "");
    f.md5 = j.value("md5", "");
    f.sha1 = j.value("sha1", "");
    f.sha256 = j.value("sha256", "");
    f.title_id = j.value("title_id", "");
    f.matched_by = j.value("matched_by", "");
    return f;
}

json file_to_json(const LibraryFile& f) {
    return json{{"path", f.path},
                {"platform", f.platform},
                {"ext", f.ext},
                {"size", f.size},
                {"mtime_sec", f.mtime_sec},
                {"crc32", f.crc32},
                {"md5", f.md5},
                {"sha1", f.sha1},
                {"sha256", f.sha256},
                {"title_id", f.title_id},
                {"matched_by", f.matched_by}};
}

std::string lower_ext_str(const fs::path& p) {
    std::string e = p.extension().string();
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

std::string lower_ext_str(const std::string& path) {
    return lower_ext_str(fs::path(path));
}

bool is_disc_dump_ext(const std::string& ext) {
    return ext == ".bin" || ext == ".iso" || ext == ".img";
}

// Prefer same-stem sibling .cue; else any sheet whose FILE "..." BINARY
// basename matches the dump filename.
fs::path companion_cue_path(const fs::path& dump_path) {
    std::error_code ec;
    const fs::path sibling = dump_path.parent_path() / (dump_path.stem().string() + ".cue");
    if (fs::is_regular_file(sibling, ec)) return sibling;

    const fs::path dir = dump_path.parent_path();
    if (dir.empty() || !fs::is_directory(dir, ec)) return {};

    const std::string dump_name = dump_path.filename().string();
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (lower_ext_str(it->path()) != ".cue") continue;

        std::ifstream in(it->path());
        if (!in) continue;
        std::string line;
        while (std::getline(in, line)) {
            size_t i = 0;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            if (line.size() - i < 4 || line.compare(i, 4, "FILE") != 0) continue;
            i += 4;
            if (i < line.size() && std::isalnum(static_cast<unsigned char>(line[i]))) continue;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            if (i >= line.size()) continue;

            std::string ref;
            if (line[i] == '"' || line[i] == '\'') {
                const char q = line[i++];
                const size_t start = i;
                while (i < line.size() && line[i] != q) ++i;
                ref = line.substr(start, i - start);
            } else {
                const size_t start = i;
                while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
                ref = line.substr(start, i - start);
            }
            if (ref.empty()) continue;

            // Optional BINARY (or other) keyword — basename match is enough.
            if (fs::path(ref).filename().string() == dump_name) return it->path();
        }
    }
    return {};
}

} // namespace

void LibraryIndex::rebuild_path_map() {
    by_path.clear();
    by_path.reserve(files.size());
    for (size_t i = 0; i < files.size(); ++i) by_path[files[i].path] = i;
}

const LibraryFile* LibraryIndex::find_path(const std::string& path) const {
    auto it = by_path.find(path);
    if (it == by_path.end()) return nullptr;
    return &files[it->second];
}

LibraryFile* LibraryIndex::find_path_mut(const std::string& path) {
    auto it = by_path.find(path);
    if (it == by_path.end()) return nullptr;
    return &files[it->second];
}

bool LibraryIndex::is_fresh(const LibraryFile& f, std::uint64_t size,
                            std::int64_t mtime_sec) const {
    return f.size == size && f.mtime_sec == mtime_sec;
}

const LibraryTitleBind* LibraryIndex::find_title(const std::string& title_id) const {
    for (const auto& t : titles) {
        if (t.title_id == title_id) return &t;
    }
    return nullptr;
}

fs::path LibraryIndex::preferred_rom(const std::string& title_id) const {
    if (const auto* t = find_title(title_id)) {
        if (t->preferred_path.empty()) return {};
        fs::path preferred = t->preferred_path;
        if (is_disc_dump_ext(lower_ext_str(preferred))) {
            const fs::path cue = companion_cue_path(preferred);
            if (!cue.empty()) return cue;
        }
        return preferred;
    }
    return {};
}

int rom_path_rank(const std::string& ext) {
    if (ext == ".cue") return 0;
    if (ext == ".sfc" || ext == ".z64" || ext == ".gba" || ext == ".md") return 1;
    if (ext == ".bin" || ext == ".gen" || ext == ".smd") return 2;
    if (ext == ".smc" || ext == ".n64" || ext == ".v64") return 3;
    if (ext == ".chd") return 4;
    if (ext == ".iso") return 5;
    return 9;
}

std::int64_t file_mtime_sec(const fs::path& path) {
    std::error_code ec;
    const auto ft = fs::last_write_time(path, ec);
    if (ec) return 0;
    return std::chrono::duration_cast<std::chrono::seconds>(ft.time_since_epoch()).count();
}

LibraryIndex load_library_index(const fs::path& path) {
    LibraryIndex idx;
    std::ifstream in(path);
    if (!in) return idx;
    try {
        json j;
        in >> j;
        idx.schema_version = j.value("schema_version", 1);
        idx.library_root = j.value("library_root", "");
        if (j.contains("files") && j.at("files").is_array()) {
            for (const auto& fj : j.at("files")) idx.files.push_back(file_from_json(fj));
        }
        if (j.contains("titles") && j.at("titles").is_array()) {
            for (const auto& tj : j.at("titles")) {
                LibraryTitleBind b;
                b.title_id = tj.value("title_id", "");
                b.preferred_path = tj.value("preferred_path", "");
                if (tj.contains("paths") && tj.at("paths").is_array()) {
                    for (const auto& p : tj.at("paths")) {
                        if (p.is_string()) b.paths.push_back(p.get<std::string>());
                    }
                }
                if (!b.title_id.empty()) idx.titles.push_back(std::move(b));
            }
        }
    } catch (...) {
        return LibraryIndex{};
    }
    idx.rebuild_path_map();
    return idx;
}

bool save_library_index(const fs::path& path, const LibraryIndex& index) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    json j;
    j["schema_version"] = index.schema_version;
    j["library_root"] = index.library_root;
    j["files"] = json::array();
    for (const auto& f : index.files) j["files"].push_back(file_to_json(f));
    j["titles"] = json::array();
    for (const auto& t : index.titles) {
        j["titles"].push_back(json{{"title_id", t.title_id},
                                   {"preferred_path", t.preferred_path},
                                   {"paths", t.paths}});
    }

    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) return false;
        out << j.dump(2) << '\n';
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
    }
    return !ec;
}

void merge_scan_into_index(LibraryIndex& index, const Catalog& catalog,
                           const ScanResult& scan, const fs::path& library_root) {
    (void)catalog;
    if (!library_root.empty()) index.library_root = library_root.string();

    std::unordered_set<std::string> seen;
    seen.reserve(scan.files.size());

    // Map path -> match info from preferred TitleMatch list + any file we tag below.
    std::unordered_map<std::string, std::pair<std::string, std::string>> match_by_path;
    for (const auto& m : scan.matches) {
        if (!m.title) continue;
        match_by_path[norm_path(m.rom_path)] = {m.title->id, m.matched_by};
    }

    // Also attach title_id to every scanned file that hashes-match (scan.files may
    // include secondary dumps for the same title).
    for (const auto& rf : scan.files) {
        if (rf.crc32.empty() && rf.md5.empty() && rf.sha1.empty() && rf.sha256.empty()) continue;
        for (const auto& m : scan.matches) {
            if (!m.title || rf.platform != m.title->platform) continue;
            bool hit = false;
            std::string by;
            if (!m.title->rom_identity.crc32.empty() && !rf.crc32.empty()) {
                for (const auto& c : m.title->rom_identity.crc32) {
                    if (c == rf.crc32) {
                        hit = true;
                        by = "crc32";
                        break;
                    }
                }
            }
            if (!hit && !m.title->rom_identity.md5.empty() && !rf.md5.empty()) {
                for (const auto& c : m.title->rom_identity.md5) {
                    if (c == rf.md5) {
                        hit = true;
                        by = "md5";
                        break;
                    }
                }
            }
            if (!hit && !m.title->rom_identity.sha1.empty() && !rf.sha1.empty()) {
                for (const auto& c : m.title->rom_identity.sha1) {
                    if (c == rf.sha1) {
                        hit = true;
                        by = "sha1";
                        break;
                    }
                }
            }
            if (!hit && !m.title->rom_identity.sha256.empty() && !rf.sha256.empty()) {
                for (const auto& c : m.title->rom_identity.sha256) {
                    if (c == rf.sha256) {
                        hit = true;
                        by = "sha256";
                        break;
                    }
                }
            }
            if (hit) {
                match_by_path[norm_path(rf.path)] = {m.title->id, by};
                break;
            }
        }
    }

    for (const auto& rf : scan.files) {
        const std::string path = norm_path(rf.path);
        seen.insert(path);

        std::error_code ec;
        const auto size = rf.size ? rf.size : fs::file_size(rf.path, ec);
        const auto mtime = rf.mtime_sec ? rf.mtime_sec : file_mtime_sec(rf.path);

        LibraryFile* existing = index.find_path_mut(path);
        LibraryFile f = existing ? *existing : LibraryFile{};
        f.path = path;
        f.platform = rf.platform;
        f.ext = rf.ext;
        f.size = size;
        f.mtime_sec = mtime;
        if (!rf.crc32.empty()) f.crc32 = rf.crc32;
        if (!rf.md5.empty()) f.md5 = rf.md5;
        if (!rf.sha1.empty()) f.sha1 = rf.sha1;
        if (!rf.sha256.empty()) f.sha256 = rf.sha256;
        auto mit = match_by_path.find(path);
        if (mit != match_by_path.end()) {
            f.title_id = mit->second.first;
            f.matched_by = mit->second.second;
        } else if (!existing) {
            f.title_id.clear();
            f.matched_by.clear();
        }
        // If file no longer matches (hash changed), clear stale bind when we
        // had hashes this pass and no match.
        if (mit == match_by_path.end() &&
            (!rf.crc32.empty() || !rf.md5.empty() || !rf.sha1.empty() || !rf.sha256.empty())) {
            f.title_id.clear();
            f.matched_by.clear();
        }

        if (existing) {
            *existing = f;
        } else {
            index.by_path[path] = index.files.size();
            index.files.push_back(std::move(f));
        }
    }

    // Drop vanished files that lived under scanned roots.
    if (!scan.scanned_roots.empty()) {
        std::vector<LibraryFile> kept;
        kept.reserve(index.files.size());
        for (auto& f : index.files) {
            if (under_any_root(f.path, scan.scanned_roots) && !seen.count(f.path))
                continue;
            kept.push_back(std::move(f));
        }
        index.files = std::move(kept);
        index.rebuild_path_map();
    }

    // Companion .cue sheets are unbound by hash; promote them when a hashed
    // disc dump (.bin/.iso/.img) already carries title_id.
    {
        const size_t n = index.files.size();
        for (size_t i = 0; i < n; ++i) {
            const LibraryFile& f = index.files[i];
            if (f.title_id.empty()) continue;
            const std::string ext = f.ext.empty() ? lower_ext_str(f.path) : f.ext;
            if (!is_disc_dump_ext(ext)) continue;

            const fs::path cue = companion_cue_path(f.path);
            if (cue.empty()) continue;

            const std::string cue_norm = norm_path(cue);
            std::error_code ec;
            const auto size = fs::file_size(cue, ec);
            const auto mtime = file_mtime_sec(cue);

            LibraryFile* existing = index.find_path_mut(cue_norm);
            LibraryFile cf = existing ? *existing : LibraryFile{};
            cf.path = cue_norm;
            cf.platform = f.platform;
            cf.ext = ".cue";
            cf.size = ec ? 0 : size;
            cf.mtime_sec = mtime;
            cf.title_id = f.title_id;
            cf.matched_by = "companion-cue";
            if (existing) {
                *existing = std::move(cf);
            } else {
                index.by_path[cue_norm] = index.files.size();
                index.files.push_back(std::move(cf));
            }
        }
        index.rebuild_path_map();
    }

    // Rebuild title bindings. Prefer scan.matches so one ROM can bind to
    // multiple titles (e.g. Super Mario World + Co-op share the stock dump).
    // Also fold in file.title_id paths (companion .cue, etc.).
    std::unordered_map<std::string, LibraryTitleBind> binds;
    auto add_path = [](LibraryTitleBind& b, const std::string& path) {
        if (path.empty()) return;
        if (std::find(b.paths.begin(), b.paths.end(), path) == b.paths.end())
            b.paths.push_back(path);
    };
    for (const auto& m : scan.matches) {
        if (!m.title) continue;
        auto& b = binds[m.title->id];
        b.title_id = m.title->id;
        for (const auto& p : m.all_paths) add_path(b, norm_path(p));
        if (!m.rom_path.empty()) b.preferred_path = norm_path(m.rom_path);
    }
    for (const auto& f : index.files) {
        if (f.title_id.empty()) continue;
        auto& b = binds[f.title_id];
        b.title_id = f.title_id;
        add_path(b, f.path);
    }
    for (auto& [id, b] : binds) {
        std::sort(b.paths.begin(), b.paths.end(), [](const std::string& a, const std::string& bpath) {
            auto ext_of = [](const std::string& p) {
                auto pos = p.find_last_of('.');
                if (pos == std::string::npos) return std::string{};
                std::string e = p.substr(pos);
                for (char& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
                return e;
            };
            const int ra = rom_path_rank(ext_of(a));
            const int rb = rom_path_rank(ext_of(bpath));
            if (ra != rb) return ra < rb;
            return a < bpath;
        });
        if (b.preferred_path.empty() && !b.paths.empty()) b.preferred_path = b.paths.front();
        // Keep preferred even if sort would pick another path; ensure it is listed.
        if (!b.preferred_path.empty()) add_path(b, b.preferred_path);
    }
    index.titles.clear();
    for (auto& [id, b] : binds) index.titles.push_back(std::move(b));
    std::sort(index.titles.begin(), index.titles.end(),
              [](const LibraryTitleBind& a, const LibraryTitleBind& b) {
                  return a.title_id < b.title_id;
              });
}

} // namespace retcomm
