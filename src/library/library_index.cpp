#include "retcomm/library_index.hpp"
#include "retcomm/hash.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>
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
    // Track images only — .iso/.chd are not accepted for multi-track PSX
    // (cannot reliably expand to a Redump cue + bins).
    return ext == ".bin" || ext == ".img";
}

std::string ascii_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool iequals_ascii(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

// Redump: "Game (USA) (Track 01).bin" → "Game (USA).cue"
std::string strip_track_suffix(std::string stem) {
    // Match " (Track N)" / " (Track NN)" / " (Track NNN)" at end (case-insensitive).
    if (stem.size() < 10) return stem;
    const std::string lower = ascii_lower(stem);
    const std::string key = " (track ";
    const auto pos = lower.rfind(key);
    if (pos == std::string::npos) return stem;
    size_t i = pos + key.size();
    if (i >= lower.size() || !std::isdigit(static_cast<unsigned char>(lower[i]))) return stem;
    while (i < lower.size() && std::isdigit(static_cast<unsigned char>(lower[i]))) ++i;
    if (i >= lower.size() || lower[i] != ')') return stem;
    if (i + 1 != lower.size()) return stem;
    return stem.substr(0, pos);
}

bool list_has_ci(const std::vector<std::string>& hay, const std::string& needle) {
    for (const auto& h : hay) {
        if (iequals_ascii(h, needle)) return true;
    }
    return false;
}

bool digest_matches_identity(const RomIdentity& id, const std::string& crc32,
                             const std::string& md5, const std::string& sha1,
                             const std::string& sha256, std::string* matched_by) {
    if (!id.crc32.empty() && !crc32.empty() && list_has_ci(id.crc32, crc32)) {
        if (matched_by) *matched_by = "crc32";
        return true;
    }
    if (!id.md5.empty() && !md5.empty() && list_has_ci(id.md5, md5)) {
        if (matched_by) *matched_by = "md5";
        return true;
    }
    if (!id.sha1.empty() && !sha1.empty() && list_has_ci(id.sha1, sha1)) {
        if (matched_by) *matched_by = "sha1";
        return true;
    }
    if (!id.sha256.empty() && !sha256.empty() && list_has_ci(id.sha256, sha256)) {
        if (matched_by) *matched_by = "sha256";
        return true;
    }
    return false;
}

bool size_in_identity(const RomIdentity& id, std::uint64_t size) {
    if (id.sizes.empty()) return true;
    for (auto sz : id.sizes) {
        if (sz == size) return true;
    }
    return false;
}

void upsert_library_file(LibraryIndex& index, LibraryFile f) {
    const std::string path = f.path;
    if (LibraryFile* existing = index.find_path_mut(path)) {
        *existing = std::move(f);
        return;
    }
    index.by_path[path] = index.files.size();
    index.files.push_back(std::move(f));
}

void upsert_title_bind(LibraryIndex& index, const std::string& title_id,
                       const fs::path& preferred, const std::vector<fs::path>& extra_paths) {
    LibraryTitleBind* bind = nullptr;
    for (auto& t : index.titles) {
        if (t.title_id == title_id) {
            bind = &t;
            break;
        }
    }
    if (!bind) {
        index.titles.push_back(LibraryTitleBind{});
        bind = &index.titles.back();
        bind->title_id = title_id;
    }

    auto add = [&](const fs::path& p) {
        if (p.empty()) return;
        const std::string s = norm_path(p);
        if (std::find(bind->paths.begin(), bind->paths.end(), s) == bind->paths.end())
            bind->paths.push_back(s);
    };
    add(preferred);
    for (const auto& p : extra_paths) add(p);

    std::sort(bind->paths.begin(), bind->paths.end(), [](const std::string& a, const std::string& b) {
        const int ra = rom_path_rank(lower_ext_str(a));
        const int rb = rom_path_rank(lower_ext_str(b));
        if (ra != rb) return ra < rb;
        return a < b;
    });
    if (!preferred.empty())
        bind->preferred_path = norm_path(preferred);
    else if (!bind->paths.empty())
        bind->preferred_path = bind->paths.front();
}

// Prefer same-stem sibling .cue; Redump set-name .cue (strip " (Track NN)");
// else any sheet whose FILE "..." basename matches the dump (case-insensitive).
fs::path companion_cue_path(const fs::path& dump_path) {
    std::error_code ec;
    const fs::path dir = dump_path.parent_path();
    const std::string stem = dump_path.stem().string();

    const fs::path sibling = dir / (stem + ".cue");
    if (fs::is_regular_file(sibling, ec)) return sibling;

    const std::string set_stem = strip_track_suffix(stem);
    if (set_stem != stem) {
        const fs::path set_cue = dir / (set_stem + ".cue");
        if (fs::is_regular_file(set_cue, ec)) return set_cue;
    }

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
            if (iequals_ascii(fs::path(ref).filename().string(), dump_name)) return it->path();
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
    // Require at least one digest so empty stub rows never count as cache hits.
    return f.size == size && f.mtime_sec == mtime_sec &&
           (!f.crc32.empty() || !f.md5.empty() || !f.sha1.empty() || !f.sha256.empty());
}

const LibraryTitleBind* LibraryIndex::find_title(const std::string& title_id) const {
    for (const auto& t : titles) {
        if (t.title_id == title_id) return &t;
    }
    return nullptr;
}

fs::path LibraryIndex::preferred_rom(const std::string& title_id) const {
    if (const auto* t = find_title(title_id)) {
        // Prefer best-ranked bound path (.cue first for discs).
        std::string best = t->preferred_path;
        int best_rank = best.empty() ? 100 : rom_path_rank(lower_ext_str(best));
        for (const auto& p : t->paths) {
            const int r = rom_path_rank(lower_ext_str(p));
            if (best.empty() || r < best_rank || (r == best_rank && p < best)) {
                best = p;
                best_rank = r;
            }
        }
        if (best.empty()) return {};
        fs::path preferred = best;
        if (is_disc_dump_ext(lower_ext_str(preferred))) {
            const fs::path cue = companion_cue_path(preferred);
            // Disc Play stages .cue only — never surface a naked .bin/.img.
            if (!cue.empty()) return cue;
            return {};
        }
        return preferred;
    }
    return {};
}

fs::path companion_cue_for_disc_dump(const fs::path& dump_path) {
    return companion_cue_path(dump_path);
}

int count_cue_tracks(const fs::path& cue_path) {
    std::error_code ec;
    if (cue_path.empty() || !fs::is_regular_file(cue_path, ec)) return 0;
    if (lower_ext_str(cue_path) != ".cue") return 0;
    std::ifstream in(cue_path);
    if (!in) return 0;
    int tracks = 0;
    std::string line;
    while (std::getline(in, line)) {
        size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (line.size() - i < 5) continue;
        if (line.compare(i, 5, "TRACK") != 0) continue;
        const size_t after = i + 5;
        if (after < line.size() && std::isalnum(static_cast<unsigned char>(line[after])))
            continue;
        ++tracks;
    }
    return tracks;
}

bool rom_identity_toc_ok(const RomIdentity& id, const fs::path& matched_path) {
    const std::string ext = lower_ext_str(matched_path);
    // Never accept cooked ISO / CHD for disc identity (no reliable multi-track).
    if (ext == ".iso" || ext == ".chd") return false;

    if (id.track_counts.empty() && !id.require_cue) return true;

    fs::path cue = matched_path;
    if (ext != ".cue") {
        if (is_disc_dump_ext(ext))
            cue = companion_cue_path(matched_path);
        else
            cue.clear();
    }
    if (cue.empty()) {
        // No cue available — fail when policy requires one or an exact track count.
        return id.track_counts.empty() && !id.require_cue;
    }
    if (id.track_counts.empty()) return true; // require_cue alone: cue present is enough

    const int n = count_cue_tracks(cue);
    if (n < 1) return false;
    for (int want : id.track_counts) {
        if (want == n) return true;
    }
    return false;
}

int rom_path_rank(const std::string& ext) {
    if (ext == ".cue") return 0;
    if (ext == ".sfc" || ext == ".z64" || ext == ".gba" || ext == ".md") return 1;
    if (ext == ".bin" || ext == ".gen" || ext == ".smd") return 2;
    if (ext == ".smc" || ext == ".n64" || ext == ".v64") return 3;
    // .iso/.chd intentionally ranked last and rejected by rom_identity_toc_ok.
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
        // Pull companion .cue sheets for any bound disc dumps.
        {
            std::vector<std::string> dumps = b.paths;
            if (!b.preferred_path.empty()) dumps.push_back(b.preferred_path);
            for (const auto& p : dumps) {
                if (!is_disc_dump_ext(lower_ext_str(p))) continue;
                const fs::path cue = companion_cue_path(p);
                if (!cue.empty()) add_path(b, norm_path(cue));
            }
        }

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
        // Always prefer best-ranked path (.cue over .bin for discs).
        if (!b.paths.empty()) b.preferred_path = b.paths.front();
    }
    index.titles.clear();
    for (auto& [id, b] : binds) index.titles.push_back(std::move(b));
    std::sort(index.titles.begin(), index.titles.end(),
              [](const LibraryTitleBind& a, const LibraryTitleBind& b) {
                  return a.title_id < b.title_id;
              });
}

BindDownloadResult bind_downloaded_rom_to_index(LibraryIndex& index, const Title& title,
                                                const fs::path& saved_path,
                                                const fs::path& library_root) {
    BindDownloadResult r;
    if (!title.has_rom_identity()) {
        r.message = "catalog title has no rom_identity";
        return r;
    }
    std::error_code ec;
    if (saved_path.empty() || (!fs::is_regular_file(saved_path, ec) && !fs::is_directory(saved_path, ec))) {
        r.message = "download path missing: " + saved_path.string();
        return r;
    }

    if (!library_root.empty()) index.library_root = library_root.string();

    const fs::path dir =
        fs::is_directory(saved_path, ec) ? saved_path : saved_path.parent_path();
    fs::path cue;
    if (lower_ext_str(saved_path) == ".cue" && fs::is_regular_file(saved_path, ec))
        cue = saved_path;
    else if (fs::is_regular_file(saved_path, ec) && is_disc_dump_ext(lower_ext_str(saved_path)))
        cue = companion_cue_path(saved_path);

    if (cue.empty() && fs::is_directory(dir, ec)) {
        // Prefer set-named cue next to Redump tracks; else first .cue in the folder.
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            if (lower_ext_str(it->path()) != ".cue") continue;
            cue = it->path();
            break;
        }
    }

    std::unordered_set<std::string> allowed_ext;
    for (auto e : title.rom_extensions) {
        for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!e.empty() && e[0] != '.') e.insert(e.begin(), '.');
        if (e == ".iso" || e == ".chd" || e == ".cue") continue;
        allowed_ext.insert(e);
    }
    // Disc dumps are always candidates when identity uses track images.
    allowed_ext.insert(".bin");
    allowed_ext.insert(".img");

    std::vector<fs::path> candidates;
    auto consider = [&](const fs::path& p) {
        if (p.empty() || !fs::is_regular_file(p, ec)) return;
        const std::string ext = lower_ext_str(p);
        if (!allowed_ext.count(ext)) return;
        candidates.push_back(p);
    };

    if (fs::is_regular_file(saved_path, ec)) consider(saved_path);
    if (fs::is_directory(dir, ec)) {
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            consider(it->path());
        }
    }

    // Prefer identity sizes (Track 01 for multi-track PSX), then smaller files.
    std::sort(candidates.begin(), candidates.end(), [&](const fs::path& a, const fs::path& b) {
        const auto sa = fs::file_size(a, ec);
        const auto sb = fs::file_size(b, ec);
        const bool a_sz = size_in_identity(title.rom_identity, sa);
        const bool b_sz = size_in_identity(title.rom_identity, sb);
        if (a_sz != b_sz) return a_sz;
        if (sa != sb) return sa < sb;
        return a.string() < b.string();
    });
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    if (candidates.empty()) {
        r.message = "no dump candidates next to " + saved_path.string();
        return r;
    }

    fs::path matched_dump;
    std::string matched_by;
    std::string crc32, md5, sha1, sha256;
    std::uint64_t matched_size = 0;
    std::int64_t matched_mtime = 0;

    for (const auto& path : candidates) {
        const auto size = fs::file_size(path, ec);
        if (ec) continue;
        if (!title.rom_identity.sizes.empty() && !size_in_identity(title.rom_identity, size))
            continue;

        const std::uint64_t skip =
            (title.platform == "snes" && size >= 512 && (size % 1024ull) == 512ull) ? 512ull : 0ull;

        std::string c, m, s1, s256;
        if (!title.rom_identity.crc32.empty()) c = file_crc32_hex(path, skip);
        if (!title.rom_identity.md5.empty()) m = file_md5_hex(path, skip);
        if (!title.rom_identity.sha1.empty()) s1 = file_sha1_hex(path, skip);
        if (!title.rom_identity.sha256.empty()) s256 = file_sha256_hex(path, skip);

        std::string by;
        if (!digest_matches_identity(title.rom_identity, c, m, s1, s256, &by)) continue;

        // Resolve cue for this dump when we still lack one.
        if (cue.empty() && is_disc_dump_ext(lower_ext_str(path))) cue = companion_cue_path(path);

        if (!rom_identity_toc_ok(title.rom_identity, path)) {
            const int n = cue.empty() ? 0 : count_cue_tracks(cue.empty() ? companion_cue_path(path) : cue);
            std::ostringstream oss;
            oss << "digest matched via " << by << " (" << path.filename().string()
                << ") but TOC check failed";
            if (title.rom_identity.require_cue || !title.rom_identity.track_counts.empty()) {
                oss << " — cue tracks=" << n << " want=";
                if (title.rom_identity.track_counts.empty())
                    oss << "any (require_cue)";
                else {
                    for (size_t i = 0; i < title.rom_identity.track_counts.size(); ++i) {
                        if (i) oss << '|';
                        oss << title.rom_identity.track_counts[i];
                    }
                }
            }
            r.message = oss.str();
            // Keep looking; another candidate might pass TOC (unlikely).
            continue;
        }

        matched_dump = path;
        matched_by = by;
        crc32 = std::move(c);
        md5 = std::move(m);
        sha1 = std::move(s1);
        sha256 = std::move(s256);
        matched_size = size;
        matched_mtime = file_mtime_sec(path);
        break;
    }

    if (matched_dump.empty()) {
        if (r.message.empty())
            r.message = "no file matched rom_identity digests under " + dir.string();
        return r;
    }

    if (cue.empty() && is_disc_dump_ext(lower_ext_str(matched_dump)))
        cue = companion_cue_path(matched_dump);

    // Upsert hashed dump.
    {
        LibraryFile f;
        f.path = norm_path(matched_dump);
        f.platform = title.platform;
        f.ext = lower_ext_str(matched_dump);
        f.size = matched_size;
        f.mtime_sec = matched_mtime;
        f.crc32 = crc32;
        f.md5 = md5;
        f.sha1 = sha1;
        f.sha256 = sha256;
        f.title_id = title.id;
        f.matched_by = matched_by;
        upsert_library_file(index, std::move(f));
    }

    // Upsert companion cue (unhashed; bind by association).
    if (!cue.empty() && fs::is_regular_file(cue, ec)) {
        LibraryFile cf;
        cf.path = norm_path(cue);
        cf.platform = title.platform;
        cf.ext = ".cue";
        cf.size = fs::file_size(cue, ec);
        if (ec) cf.size = 0;
        cf.mtime_sec = file_mtime_sec(cue);
        cf.title_id = title.id;
        cf.matched_by = "companion-cue";
        upsert_library_file(index, std::move(cf));
    }

    const fs::path preferred = !cue.empty() ? cue : matched_dump;
    upsert_title_bind(index, title.id, preferred, {matched_dump, cue});

    // preferred_rom refuses naked disc dumps — require cue when policy says so.
    const fs::path bound = index.preferred_rom(title.id);
    if (bound.empty()) {
        r.message = "indexed " + matched_dump.filename().string() + " via " + matched_by +
                    " but preferred_rom is empty (companion .cue missing?)";
        return r;
    }

    r.ok = true;
    r.preferred_path = bound;
    r.matched_by = matched_by;
    r.matched_dump = matched_dump;
    std::ostringstream oss;
    oss << "Bound " << title.id << " via " << matched_by << " → " << bound.string();
    if (is_disc_dump_ext(lower_ext_str(matched_dump)))
        oss << " (dump " << matched_dump.filename().string() << ")";
    r.message = oss.str();
    return r;
}

} // namespace retcomm
