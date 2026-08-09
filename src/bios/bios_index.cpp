#include "retcomm/bios_index.hpp"
#include "retcomm/hash.hpp"
#include "retcomm/library_index.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <unordered_set>

namespace retcomm {
namespace {

using nlohmann::json;

std::string norm_path(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    return (ec ? p : c).string();
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string lower_ext(const fs::path& p) { return lower(p.extension().string()); }

bool is_bios_ext(const std::string& ext) {
    return ext == ".bin" || ext == ".rom" || ext == ".bios" || ext == ".img";
}

bool excluded_dir(const std::string& name, const std::vector<std::string>& exclude) {
    const std::string n = lower(name);
    for (const auto& e : exclude) {
        if (n == lower(e)) return true;
    }
    // Emulator-owned trees under ES-DE bios roots — never hash these.
    static const char* kBiosJunk[] = {"ppsspp",     "mupen64plus", "azahar", "ryujinx",
                                      "shadps4",    "yuzu",        "suyu",   "switch",
                                      "firmware",   "cache",       "screenshots", "savestates"};
    for (const char* j : kBiosJunk) {
        if (n == j) return true;
    }
    return false;
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

BiosFile file_from_json(const json& j) {
    BiosFile f;
    f.path = j.value("path", "");
    f.platform = j.value("platform", "");
    f.filename = j.value("filename", "");
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

json file_to_json(const BiosFile& f) {
    return json{{"path", f.path},
                {"platform", f.platform},
                {"filename", f.filename},
                {"size", f.size},
                {"mtime_sec", f.mtime_sec},
                {"crc32", f.crc32},
                {"md5", f.md5},
                {"sha1", f.sha1},
                {"sha256", f.sha256},
                {"title_id", f.title_id},
                {"matched_by", f.matched_by}};
}

struct PlatformBiosNeed {
    std::string platform;
    std::unordered_set<std::uint64_t> sizes;
    bool want_crc = false;
    bool want_md5 = false;
    bool want_sha1 = false;
    bool want_sha256 = false;
    std::vector<const Title*> titles;
};

std::map<std::string, PlatformBiosNeed> collect_needs(const Catalog& cat) {
    std::map<std::string, PlatformBiosNeed> out;
    for (const auto& t : cat.titles) {
        if (!t.has_bios_identity()) continue;
        auto& n = out[t.platform];
        n.platform = t.platform;
        n.titles.push_back(&t);
        if (!t.bios_identity.crc32.empty()) n.want_crc = true;
        if (!t.bios_identity.md5.empty()) n.want_md5 = true;
        if (!t.bios_identity.sha1.empty()) n.want_sha1 = true;
        if (!t.bios_identity.sha256.empty()) n.want_sha256 = true;
        for (auto sz : t.bios_identity.sizes) n.sizes.insert(sz);
    }
    return out;
}

bool size_interesting(const PlatformBiosNeed& need, std::uint64_t size) {
    if (need.sizes.empty()) return true;
    return need.sizes.count(size) > 0;
}

bool filename_hint_match(const Title& t, const std::string& filename) {
    const std::string fn = lower(filename);
    for (const auto& h : t.bios_identity.filenames) {
        if (fn == lower(h)) return true;
    }
    return false;
}

int match_rank(const std::string& by) {
    if (by == "crc32") return 0;
    if (by == "md5") return 1;
    if (by == "sha1") return 2;
    if (by == "sha256") return 3;
    if (by == "filename") return 4;
    if (by == "size") return 5;
    return 9;
}

int path_preference(const std::string& path, const Title& t) {
    const std::string p = lower(path);
    int score = 0;
    // Prefer system subfolders over emulator trees / deep nests.
    const bool in_ps = p.find("/ps/") != std::string::npos ||
                       p.find("/psx/") != std::string::npos ||
                       p.find("\\ps\\") != std::string::npos ||
                       p.find("\\psx\\") != std::string::npos;
    const bool in_gba = p.find("/gba/") != std::string::npos ||
                        p.find("\\gba\\") != std::string::npos;
    if ((t.platform == "psx" && in_ps) || (t.platform == "gba" && in_gba)) score += 3;
    for (const auto& h : t.bios_identity.filenames) {
        if (lower(fs::path(path).filename().string()) == lower(h)) score += 2;
    }
    // Prefer shorter paths (less nested junk).
    score -= static_cast<int>(std::count(p.begin(), p.end(), '/')) / 2;
    return score;
}

bool try_match_title(const Title& t, BiosFile& f) {
    const auto& id = t.bios_identity;
    if (!id.sizes.empty()) {
        bool ok = false;
        for (auto sz : id.sizes)
            if (sz == f.size) {
                ok = true;
                break;
            }
        if (!ok) return false;
    }

    if (!id.crc32.empty() && !f.crc32.empty()) {
        for (const auto& c : id.crc32) {
            if (c == f.crc32) {
                f.title_id = t.id;
                f.matched_by = "crc32";
                return true;
            }
        }
    }
    if (!id.md5.empty() && !f.md5.empty()) {
        for (const auto& s : id.md5) {
            if (s == f.md5) {
                f.title_id = t.id;
                f.matched_by = "md5";
                return true;
            }
        }
    }
    if (!id.sha1.empty() && !f.sha1.empty()) {
        for (const auto& s : id.sha1) {
            if (s == f.sha1) {
                f.title_id = t.id;
                f.matched_by = "sha1";
                return true;
            }
        }
    }
    if (!id.sha256.empty() && !f.sha256.empty()) {
        for (const auto& s : id.sha256) {
            if (s == f.sha256) {
                f.title_id = t.id;
                f.matched_by = "sha256";
                return true;
            }
        }
    }
    if (filename_hint_match(t, f.filename)) {
        f.title_id = t.id;
        f.matched_by = "filename";
        return true;
    }
    // Size-only only when identity has sizes and no stronger hashes configured,
    // or hashes were requested but file wasn't hashed yet — skip size-only if
    // digest lists exist (avoid binding random 512KiB files without hash).
    if (!id.crc32.empty() || !id.md5.empty() || !id.sha1.empty() || !id.sha256.empty())
        return false;
    if (!id.sizes.empty()) {
        f.title_id = t.id;
        f.matched_by = "size";
        return true;
    }
    return false;
}

void emit(const BiosScanOptions& opts, BiosScanProgress p) {
    if (opts.on_progress) opts.on_progress(p);
}

} // namespace

void BiosIndex::rebuild_path_map() {
    by_path.clear();
    by_path.reserve(files.size());
    for (size_t i = 0; i < files.size(); ++i) by_path[files[i].path] = i;
}

const BiosFile* BiosIndex::find_path(const std::string& path) const {
    auto it = by_path.find(path);
    if (it == by_path.end()) return nullptr;
    return &files[it->second];
}

BiosFile* BiosIndex::find_path_mut(const std::string& path) {
    auto it = by_path.find(path);
    if (it == by_path.end()) return nullptr;
    return &files[it->second];
}

bool BiosIndex::is_fresh(const BiosFile& f, std::uint64_t size, std::int64_t mtime_sec) const {
    return f.size == size && f.mtime_sec == mtime_sec &&
           (!f.crc32.empty() || !f.md5.empty() || !f.sha1.empty() || !f.sha256.empty());
}

fs::path BiosIndex::preferred_bios(const std::string& title_id) const {
    const auto* t = find_title(title_id);
    if (!t || t->preferred_path.empty()) return {};
    return t->preferred_path;
}

const BiosTitleBind* BiosIndex::find_title(const std::string& title_id) const {
    for (const auto& t : titles) {
        if (t.title_id == title_id) return &t;
    }
    return nullptr;
}

BiosIndex load_bios_index(const fs::path& path) {
    BiosIndex idx;
    std::ifstream in(path);
    if (!in) return idx;
    try {
        json j;
        in >> j;
        idx.schema_version = j.value("schema_version", 1);
        idx.bios_root = j.value("bios_root", "");
        if (j.contains("files") && j.at("files").is_array()) {
            for (const auto& fj : j.at("files")) idx.files.push_back(file_from_json(fj));
        }
        if (j.contains("titles") && j.at("titles").is_array()) {
            for (const auto& tj : j.at("titles")) {
                BiosTitleBind b;
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
        idx.rebuild_path_map();
    } catch (...) {
        return BiosIndex{};
    }
    return idx;
}

bool save_bios_index(const fs::path& path, const BiosIndex& index) {
    json files = json::array();
    for (const auto& f : index.files) files.push_back(file_to_json(f));
    json titles = json::array();
    for (const auto& t : index.titles) {
        titles.push_back(json{{"title_id", t.title_id},
                              {"preferred_path", t.preferred_path},
                              {"paths", t.paths}});
    }
    json j = {{"schema_version", index.schema_version},
              {"bios_root", index.bios_root},
              {"files", std::move(files)},
              {"titles", std::move(titles)}};
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (!out) return false;
    out << j.dump(2) << "\n";
    return static_cast<bool>(out);
}

BiosScanResult scan_bios_roots(const Catalog& catalog, const AppConfig& cfg,
                               const std::vector<fs::path>& roots, const BiosScanOptions& opts) {
    BiosScanResult result;
    const auto needs = collect_needs(catalog);
    if (needs.empty()) {
        result.errors.push_back("catalog has no titles with bios_identity");
        return result;
    }

    std::unordered_set<std::string> seen_roots;
    for (const auto& r : roots) {
        std::error_code ec;
        if (!fs::is_directory(r, ec)) continue;
        const std::string key = norm_path(r);
        if (!seen_roots.insert(key).second) continue;
        result.scanned_roots.push_back(r);
    }
    if (result.scanned_roots.empty()) {
        result.errors.push_back("no bios roots to scan");
        return result;
    }

    // Build list of (platform, root) walks: each need scans its platform folders
    // under each provided root, plus the root itself for flat dumps.
    struct Walk {
        std::string platform;
        fs::path root;
    };
    auto platform_allowed = [&](const std::string& plat) {
        if (opts.platforms.empty()) return true;
        return std::find(opts.platforms.begin(), opts.platforms.end(), plat) !=
               opts.platforms.end();
    };

    std::vector<Walk> walks;
    std::unordered_set<std::string> walk_keys;
    for (const auto& root : result.scanned_roots) {
        // Flat root only when scanning all platforms (filtered scans stay in folders).
        if (opts.platforms.empty()) {
            const std::string flat_key = norm_path(root) + "|*";
            if (walk_keys.insert(flat_key).second) walks.push_back({"", root});
        }
        for (const auto& [plat, need] : needs) {
            (void)need;
            if (!platform_allowed(plat)) continue;
            for (const auto& folder : cfg.folders_for_platform(plat)) {
                const fs::path sub = root / folder;
                std::error_code ec;
                if (!fs::is_directory(sub, ec)) continue;
                const std::string key = norm_path(sub) + "|" + plat;
                if (!walk_keys.insert(key).second) continue;
                walks.push_back({plat, sub});
            }
        }
    }

    std::vector<BiosFile> candidates;
    for (const auto& w : walks) {
        std::error_code ec;
        std::size_t walked = 0;
        for (auto it = fs::recursive_directory_iterator(
                 w.root, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_directory(ec)) {
                if (excluded_dir(it->path().filename().string(), cfg.exclude_dirs)) {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            const auto& path = it->path();
            const std::string ext = lower_ext(path);
            if (!is_bios_ext(ext)) continue;

            const auto sz = it->file_size(ec);
            if (ec) continue;
            // Flat root: accept if any platform size matches; platform walks
            // use that platform's size set.
            bool interesting = false;
            std::string plat = w.platform;
            if (!plat.empty()) {
                interesting = size_interesting(needs.at(plat), sz);
            } else {
                for (const auto& [p, need] : needs) {
                    if (size_interesting(need, sz)) {
                        interesting = true;
                        // Prefer assigning platform from parent folder name.
                        const std::string parent = lower(path.parent_path().filename().string());
                        for (const auto& folder : cfg.folders_for_platform(p)) {
                            if (parent == lower(folder) || parent == lower(p)) {
                                plat = p;
                                break;
                            }
                        }
                        if (plat.empty()) plat = p;
                        break;
                    }
                }
            }
            if (!interesting) {
                ++result.skipped_hash;
                continue;
            }

            BiosFile f;
            f.path = norm_path(path);
            f.platform = plat;
            f.filename = path.filename().string();
            f.size = sz;
            f.mtime_sec = file_mtime_sec(path);
            candidates.push_back(std::move(f));
            ++walked;
            if (walked % 50 == 0) {
                emit(opts, {"walk", plat.empty() ? "flat" : plat, walked, 0, path});
            }
        }
    }

    emit(opts, {"hash", "", 0, candidates.size(), {}});
    for (size_t i = 0; i < candidates.size(); ++i) {
        auto& f = candidates[i];
        emit(opts, {"hash", f.platform, i + 1, candidates.size(), f.path});

        const PlatformBiosNeed* need = nullptr;
        auto itn = needs.find(f.platform);
        if (itn != needs.end()) need = &itn->second;

        const bool want_crc = !need || need->want_crc;
        const bool want_md5 = need && need->want_md5;
        const bool want_sha1 = need && need->want_sha1;
        const bool want_sha256 = need && need->want_sha256;

        bool use_cache = false;
        if (!opts.full_rescan && opts.index) {
            if (const auto* cached = opts.index->find_path(f.path)) {
                // Same rule as ROM scan: fresh size/mtime and every needed digest present.
                const bool need_ok =
                    (!want_crc || !cached->crc32.empty()) &&
                    (!want_md5 || !cached->md5.empty()) &&
                    (!want_sha1 || !cached->sha1.empty()) &&
                    (!want_sha256 || !cached->sha256.empty());
                if (need_ok && opts.index->is_fresh(*cached, f.size, f.mtime_sec)) {
                    f.crc32 = cached->crc32;
                    f.md5 = cached->md5;
                    f.sha1 = cached->sha1;
                    f.sha256 = cached->sha256;
                    use_cache = true;
                    ++result.cache_hits;
                }
            }
        }
        if (!use_cache) {
            if (want_crc) f.crc32 = file_crc32_hex(f.path);
            if (want_md5) f.md5 = file_md5_hex(f.path);
            if (want_sha1) f.sha1 = file_sha1_hex(f.path);
            if (want_sha256) f.sha256 = file_sha256_hex(f.path);
            if (f.crc32.empty() && f.md5.empty() && f.sha1.empty() && f.sha256.empty() &&
                want_crc)
                f.crc32 = file_crc32_hex(f.path);
            ++result.hashed_files;
        }
        result.files.push_back(f);
    }

    emit(opts, {"match", "", 0, 0, {}});
    // Match each catalog title that needs BIOS — a file may bind to many titles
    // (shared SCPH1001).
    std::map<std::string, BiosTitleBind> binds;
    for (const auto& [plat, need] : needs) {
        for (const Title* t : need.titles) {
            BiosTitleBind bind;
            bind.title_id = t->id;
            std::vector<std::pair<int, std::string>> ranked;
            for (auto& f : result.files) {
                if (!f.platform.empty() && f.platform != t->platform) {
                    // Flat-assigned platform mismatch: still allow if hashes match.
                }
                BiosFile trial = f;
                trial.title_id.clear();
                trial.matched_by.clear();
                if (!try_match_title(*t, trial)) continue;
                f.title_id = trial.title_id; // last writer; OK for display
                f.matched_by = trial.matched_by;
                const int rank =
                    match_rank(trial.matched_by) * 100 - path_preference(f.path, *t);
                ranked.emplace_back(rank, f.path);
                bind.paths.push_back(f.path);
            }
            if (ranked.empty()) continue;
            std::sort(ranked.begin(), ranked.end());
            bind.preferred_path = ranked.front().second;
            // Unique paths
            std::sort(bind.paths.begin(), bind.paths.end());
            bind.paths.erase(std::unique(bind.paths.begin(), bind.paths.end()), bind.paths.end());
            binds[t->id] = std::move(bind);
        }
    }
    for (auto& [id, b] : binds) result.matches.push_back(std::move(b));
    std::sort(result.matches.begin(), result.matches.end(),
              [](const BiosTitleBind& a, const BiosTitleBind& b) {
                  return a.title_id < b.title_id;
              });
    return result;
}

BiosScanResult scan_bios_library(const Catalog& catalog, const AppConfig& cfg,
                                 const BiosScanOptions& opts) {
    std::vector<fs::path> roots;
    if (!cfg.bios_root.empty()) roots.push_back(cfg.bios_root);
    return scan_bios_roots(catalog, cfg, roots, opts);
}

std::size_t rematch_bios_titles(BiosIndex& index, const Catalog& catalog) {
    std::map<std::string, BiosTitleBind> binds;
    for (const auto& t : catalog.titles) {
        if (!t.has_bios_identity()) continue;
        BiosTitleBind b;
        b.title_id = t.id;
        std::vector<std::pair<int, std::string>> ranked;
        for (auto& f : index.files) {
            BiosFile trial = f;
            trial.title_id.clear();
            trial.matched_by.clear();
            if (!try_match_title(t, trial)) continue;
            // Keep a display hint on the file row (last matching title wins).
            f.title_id = trial.title_id;
            f.matched_by = trial.matched_by;
            ranked.emplace_back(match_rank(trial.matched_by) * 100 - path_preference(f.path, t),
                                f.path);
            b.paths.push_back(f.path);
        }
        if (ranked.empty()) continue;
        std::sort(ranked.begin(), ranked.end());
        b.preferred_path = ranked.front().second;
        std::sort(b.paths.begin(), b.paths.end());
        b.paths.erase(std::unique(b.paths.begin(), b.paths.end()), b.paths.end());
        binds[t.id] = std::move(b);
    }

    index.titles.clear();
    for (auto& [id, b] : binds) index.titles.push_back(std::move(b));
    std::sort(index.titles.begin(), index.titles.end(),
              [](const BiosTitleBind& a, const BiosTitleBind& b) {
                  return a.title_id < b.title_id;
              });
    return index.titles.size();
}

void merge_bios_scan_into_index(BiosIndex& index, const Catalog& catalog,
                                const BiosScanResult& scan, const fs::path& bios_root) {
    if (!bios_root.empty()) index.bios_root = bios_root.string();

    std::unordered_set<std::string> seen;
    for (const auto& f : scan.files) seen.insert(f.path);

    // Drop vanished files under scanned roots.
    if (!scan.scanned_roots.empty()) {
        std::vector<BiosFile> keep;
        keep.reserve(index.files.size());
        for (auto& f : index.files) {
            if (under_any_root(f.path, scan.scanned_roots) && !seen.count(f.path)) continue;
            keep.push_back(std::move(f));
        }
        index.files = std::move(keep);
        index.rebuild_path_map();
    }

    for (const auto& f : scan.files) {
        if (auto* existing = index.find_path_mut(f.path)) {
            *existing = f;
        } else {
            index.files.push_back(f);
            index.by_path[f.path] = index.files.size() - 1;
        }
    }
    index.rebuild_path_map();

    // Full catalog rematch from the merged file set (covers new titles that
    // share dumps already hashed in the index).
    rematch_bios_titles(index, catalog);
}

BiosPurgeMissingResult purge_missing_bios_files(BiosIndex& index, const Catalog& catalog,
                                                const std::string& platform) {
    BiosPurgeMissingResult r;
    const size_t titles_before = index.titles.size();
    std::vector<BiosFile> kept;
    kept.reserve(index.files.size());
    for (auto& f : index.files) {
        if (!platform.empty() && f.platform != platform) {
            kept.push_back(std::move(f));
            continue;
        }
        std::error_code ec;
        if (fs::is_regular_file(f.path, ec)) {
            kept.push_back(std::move(f));
            continue;
        }
        ++r.removed_files;
        r.removed_paths.push_back(f.path);
    }
    index.files = std::move(kept);
    index.rebuild_path_map();
    rematch_bios_titles(index, catalog);
    if (titles_before > index.titles.size())
        r.removed_title_binds = titles_before - index.titles.size();
    return r;
}

} // namespace retcomm
