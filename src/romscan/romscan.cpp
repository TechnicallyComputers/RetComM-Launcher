#include "retcomm/romscan.hpp"
#include "retcomm/hash.hpp"
#include "retcomm/library_index.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace retcomm {
namespace {

std::string lower_ext(const fs::path& p) {
    std::string e = p.extension().string();
    for (char& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

bool list_has(const std::vector<std::string>& hay, const std::string& needle) {
    for (const auto& h : hay) {
        if (h == needle) return true;
    }
    return false;
}

bool name_excluded(const fs::path& p, const std::vector<std::string>& exclude) {
    const std::string name = p.filename().string();
    for (const auto& ex : exclude) {
        if (name == ex) return true;
    }
    return false;
}

struct PlatformNeed {
    std::unordered_set<std::string> extensions; // ".sfc"
    bool need_crc = false;
    bool need_md5 = false;
    bool need_sha1 = false;
    bool need_sha256 = false;
    // Each title's acceptable hash list — used to stop hashing once all are hit.
    // Titles that list multiple algorithms contribute one group per non-empty list;
    // matching any algorithm for that title is enough (see title_identity_matched).
    std::vector<const Title*> identity_titles;
};

std::unordered_map<std::string, PlatformNeed> needs_by_platform(const Catalog& catalog) {
    std::unordered_map<std::string, PlatformNeed> out;
    for (const auto& t : catalog.titles) {
        auto& n = out[t.platform];
        for (const auto& e : t.rom_extensions) {
            std::string le = e;
            for (char& c : le) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (!le.empty() && le[0] != '.') le.insert(le.begin(), '.');
            if (le == ".iso") continue; // never hash optical ISO images
            n.extensions.insert(le);
        }
        if (!t.rom_identity.crc32.empty()) n.need_crc = true;
        if (!t.rom_identity.md5.empty()) n.need_md5 = true;
        if (!t.rom_identity.sha1.empty()) n.need_sha1 = true;
        if (!t.rom_identity.sha256.empty()) n.need_sha256 = true;
        if (!t.rom_identity.crc32.empty() || !t.rom_identity.md5.empty() ||
            !t.rom_identity.sha1.empty() || !t.rom_identity.sha256.empty()) {
            n.identity_titles.push_back(&t);
        }
    }
    return out;
}

// Size gate is per-title. Hash a candidate when any identity title could use it:
//   - title has empty sizes[] (no gate — typical carts), or
//   - title lists this exact byte length (disc dumps).
// Full rescan ignores sizes and hashes every extension match.
bool size_eligible_for_hash(const PlatformNeed& need, std::uint64_t size, bool full_rescan) {
    if (full_rescan || need.identity_titles.empty()) return true;
    for (const Title* t : need.identity_titles) {
        if (t->rom_identity.sizes.empty()) return true;
        for (auto sz : t->rom_identity.sizes) {
            if (sz == size) return true;
        }
    }
    return false;
}

// SNES copier header: 512 bytes when (size % 1024) == 512. Catalog CRCs are
// headerless (No-Intro); strip before hashing.
std::uint64_t snes_hash_skip(const std::string& platform, std::uint64_t size) {
    if (platform == "snes" && size >= 512 && (size % 1024ull) == 512ull) return 512;
    return 0;
}

bool title_identity_matched(const Title& t, const std::string& platform,
                            const ScanResult& result) {
    for (const auto& rf : result.files) {
        if (rf.platform != platform) continue;
        if (!t.rom_identity.crc32.empty() && !rf.crc32.empty() &&
            list_has(t.rom_identity.crc32, rf.crc32))
            return true;
        if (!t.rom_identity.md5.empty() && !rf.md5.empty() &&
            list_has(t.rom_identity.md5, rf.md5))
            return true;
        if (!t.rom_identity.sha1.empty() && !rf.sha1.empty() &&
            list_has(t.rom_identity.sha1, rf.sha1))
            return true;
        if (!t.rom_identity.sha256.empty() && !rf.sha256.empty() &&
            list_has(t.rom_identity.sha256, rf.sha256))
            return true;
    }
    return false;
}

// Early-out when every hash-identified title on this platform has a hit.
bool all_identity_titles_matched(const PlatformNeed& need, const std::string& platform,
                                 const ScanResult& result) {
    if (need.identity_titles.empty()) return false;
    for (const Title* t : need.identity_titles) {
        if (!title_identity_matched(*t, platform, result)) return false;
    }
    return true;
}

// During the hash loop: skip files that cannot help any still-unmatched title.
bool size_eligible_for_unmatched(const PlatformNeed& need, const std::string& platform,
                                 std::uint64_t size, const ScanResult& result,
                                 bool full_rescan) {
    for (const Title* t : need.identity_titles) {
        if (title_identity_matched(*t, platform, result)) continue;
        if (full_rescan) return true;
        // Empty sizes[] = no size gate (typical carts): any candidate may match.
        // Non-empty sizes[] = only those byte lengths can identify the title.
        if (t->rom_identity.sizes.empty()) return true;
        for (auto sz : t->rom_identity.sizes) {
            if (sz == size) return true;
        }
    }
    return false;
}

std::string platform_for_folder(const AppConfig& config, const std::string& folder) {
    for (const auto& [plat, folders] : config.platform_folders) {
        for (const auto& f : folders) {
            if (f == folder) return plat;
        }
    }
    return folder;
}

void emit(const ScanOptions& opts, ScanProgress p) {
    if (opts.on_progress) opts.on_progress(p);
}

bool platform_allowed(const ScanOptions& opts, const std::string& platform) {
    if (opts.platforms.empty()) return true;
    return std::find(opts.platforms.begin(), opts.platforms.end(), platform) !=
           opts.platforms.end();
}

bool looks_like_library_root(const fs::path& dir, const Catalog& catalog,
                             const AppConfig& config) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    for (const auto& t : catalog.titles) {
        for (const auto& folder : config.folders_for_platform(t.platform)) {
            if (fs::is_directory(dir / folder, ec)) return true;
        }
    }
    return false;
}

std::string join_folders(const std::vector<std::string>& folders) {
    std::string s;
    for (const auto& f : folders) {
        if (!s.empty()) s += ", ";
        s += f;
    }
    return s;
}

void walk_platform_root(const fs::path& root, const std::string& platform,
                        const PlatformNeed& need, const AppConfig& config,
                        const ScanOptions& opts, ScanResult& result) {
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        result.errors.push_back("rom dir missing: " + root.string());
        return;
    }

    result.scanned_roots.push_back(root);

    std::vector<fs::path> candidates;
    auto it = fs::recursive_directory_iterator(
        root, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_directory(ec)) {
            if (name_excluded(it->path(), config.exclude_dirs)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        const auto& path = it->path();
        const std::string ext = lower_ext(path);
        // Never index or hash optical .iso images (use .cue+.bin for disc titles).
        if (ext == ".iso") continue;
        if (need.extensions.find(ext) == need.extensions.end()) continue;
        candidates.push_back(path);
        emit(opts, {"walk", platform, path, candidates.size(), 0});
    }
    if (ec) {
        result.errors.push_back("scan error under " + root.string() + ": " + ec.message());
    }

    const bool want_crc = opts.compute_crc_when_needed && need.need_crc;
    const bool want_md5 = opts.compute_md5_when_needed && need.need_md5;
    const bool want_sha1 = opts.compute_sha1_when_needed && need.need_sha1;
    const bool want_sha256 = opts.compute_sha256_when_needed && need.need_sha256;
    const bool want_any_hash = want_crc || want_md5 || want_sha1 || want_sha256;

    auto fill_meta = [](RomFile& rf, const fs::path& path) {
        std::error_code ec;
        rf.size = fs::file_size(path, ec);
        if (ec) rf.size = 0;
        rf.mtime_sec = file_mtime_sec(path);
    };

    // Pre-filter to the files we will actually hash (size gate for disc dumps).
    std::vector<fs::path> to_hash;
    to_hash.reserve(candidates.size());
    for (const auto& path : candidates) {
        RomFile rf;
        rf.path = path;
        rf.platform = platform;
        rf.ext = lower_ext(path);
        fill_meta(rf, path);

        if (!want_any_hash) {
            ++result.skipped_hash;
            result.files.push_back(std::move(rf));
            continue;
        }
        if (!size_eligible_for_hash(need, rf.size, opts.full_rescan)) {
            ++result.skipped_hash;
            result.files.push_back(std::move(rf));
            continue;
        }

        // Incremental: reuse hashes when path+size+mtime still match the index.
        // Rehash SNES dumps with a copier header — older indexes stored
        // header-inclusive CRCs that never match No-Intro identities.
        const bool smc_header = snes_hash_skip(platform, rf.size) != 0;
        if (!opts.full_rescan && opts.index && !smc_header) {
            std::error_code cec;
            const fs::path canon = fs::weakly_canonical(path, cec);
            const std::string key = (cec ? path : canon).string();
            if (const LibraryFile* cached = opts.index->find_path(key)) {
                const bool need_ok =
                    (!want_crc || !cached->crc32.empty()) &&
                    (!want_md5 || !cached->md5.empty()) &&
                    (!want_sha1 || !cached->sha1.empty()) &&
                    (!want_sha256 || !cached->sha256.empty());
                if (need_ok && opts.index->is_fresh(*cached, rf.size, rf.mtime_sec)) {
                    rf.crc32 = cached->crc32;
                    rf.md5 = cached->md5;
                    rf.sha1 = cached->sha1;
                    rf.sha256 = cached->sha256;
                    rf.from_cache = true;
                    ++result.cache_hits;
                    emit(opts, {"cache", platform, path, result.cache_hits, 0});
                    result.files.push_back(std::move(rf));
                    continue;
                }
            }
        }
        to_hash.push_back(path);
    }

    if (!want_any_hash) return;

    // Incremental: if cache hits already identified every title on this platform,
    // do not re-hash remaining candidates (Full Rescan still hashes everything).
    if (!opts.full_rescan && all_identity_titles_matched(need, platform, result)) {
        for (const auto& path : to_hash) {
            ++result.skipped_hash;
            RomFile rest;
            rest.path = path;
            rest.platform = platform;
            rest.ext = lower_ext(path);
            fill_meta(rest, path);
            result.files.push_back(std::move(rest));
        }
        return;
    }

    // Smaller dumps first (.bin before large leftovers) so we can early-out.
    std::sort(to_hash.begin(), to_hash.end(), [](const fs::path& a, const fs::path& b) {
        std::error_code ea, eb;
        const auto sa = fs::file_size(a, ea);
        const auto sb = fs::file_size(b, eb);
        if (ea || eb) return a.string() < b.string();
        if (sa != sb) return sa < sb;
        return a.string() < b.string();
    });

    for (size_t i = 0; i < to_hash.size(); ++i) {
        const auto& path = to_hash[i];
        RomFile rf;
        rf.path = path;
        rf.platform = platform;
        rf.ext = lower_ext(path);
        fill_meta(rf, path);

        if (!size_eligible_for_unmatched(need, platform, rf.size, result, opts.full_rescan)) {
            ++result.skipped_hash;
            result.files.push_back(std::move(rf));
            continue;
        }

        emit(opts, {"hash", platform, path, i + 1, to_hash.size()});
        const std::uint64_t skip = snes_hash_skip(platform, rf.size);
        if (want_crc) rf.crc32 = file_crc32_hex(path, skip);
        if (want_md5) rf.md5 = file_md5_hex(path, skip);
        if (want_sha1) rf.sha1 = file_sha1_hex(path, skip);
        if (want_sha256) rf.sha256 = file_sha256_hex(path, skip);
        ++result.hashed_files;
        result.files.push_back(std::move(rf));

        if (all_identity_titles_matched(need, platform, result)) {
            for (size_t j = i + 1; j < to_hash.size(); ++j) {
                ++result.skipped_hash;
                RomFile rest;
                rest.path = to_hash[j];
                rest.platform = platform;
                rest.ext = lower_ext(to_hash[j]);
                fill_meta(rest, to_hash[j]);
                result.files.push_back(std::move(rest));
            }
            break;
        }
    }
}

void match_titles(const Catalog& catalog, ScanResult& result) {
    for (const auto& t : catalog.titles) {
        if (!t.has_rom_identity()) continue;
        TitleMatch m;
        m.title = &t;
        for (const auto& rf : result.files) {
            if (rf.platform != t.platform) continue;
            bool hit = false;
            std::string by;
            if (!t.rom_identity.crc32.empty() && !rf.crc32.empty() &&
                list_has(t.rom_identity.crc32, rf.crc32)) {
                hit = true;
                by = "crc32";
            } else if (!t.rom_identity.md5.empty() && !rf.md5.empty() &&
                       list_has(t.rom_identity.md5, rf.md5)) {
                hit = true;
                by = "md5";
            } else if (!t.rom_identity.sha1.empty() && !rf.sha1.empty() &&
                       list_has(t.rom_identity.sha1, rf.sha1)) {
                hit = true;
                by = "sha1";
            } else if (!t.rom_identity.sha256.empty() && !rf.sha256.empty() &&
                       list_has(t.rom_identity.sha256, rf.sha256)) {
                hit = true;
                by = "sha256";
            }
            if (!hit) continue;
            if (!rom_identity_toc_ok(t.rom_identity, rf.path)) continue;
            m.all_paths.push_back(rf.path);
            if (m.matched_by.empty()) m.matched_by = by;
        }
        if (m.all_paths.empty()) continue;
        std::sort(m.all_paths.begin(), m.all_paths.end(),
                  [](const fs::path& a, const fs::path& b) {
                      const int ra = rom_path_rank(lower_ext(a));
                      const int rb = rom_path_rank(lower_ext(b));
                      if (ra != rb) return ra < rb;
                      return a.string() < b.string();
                  });
        m.rom_path = m.all_paths.front();
        result.matches.push_back(std::move(m));
    }
    std::sort(result.matches.begin(), result.matches.end(),
              [](const TitleMatch& a, const TitleMatch& b) {
                  return a.title->name < b.title->name;
              });
}

std::vector<std::pair<std::string, fs::path>> roots_for_library(const Catalog& catalog,
                                                                const AppConfig& config,
                                                                const ScanOptions& opts,
                                                                ScanResult& errors_out) {
    std::vector<std::pair<std::string, fs::path>> roots;
    std::unordered_set<std::string> platforms;
    for (const auto& t : catalog.titles) {
        if (platform_allowed(opts, t.platform)) platforms.insert(t.platform);
    }

    for (const auto& platform : platforms) {
        auto pr = config.platform_roots(platform);
        if (pr.empty()) {
            errors_out.errors.push_back(
                "no folder for platform '" + platform + "' under " +
                config.library_root.string() +
                " (tried: " + join_folders(config.folders_for_platform(platform)) + ")");
            continue;
        }
        for (auto& p : pr) roots.emplace_back(platform, std::move(p));
    }
    return roots;
}

ScanResult scan_tagged_roots(const Catalog& catalog, const AppConfig& config,
                             const std::vector<std::pair<std::string, fs::path>>& roots,
                             const ScanOptions& opts) {
    ScanResult result;
    const auto needs = needs_by_platform(catalog);

    for (const auto& [platform, root] : roots) {
        if (!platform_allowed(opts, platform)) continue;
        auto it = needs.find(platform);
        if (it == needs.end()) {
            result.errors.push_back("no catalog titles for platform '" + platform +
                                    "' (skipping " + root.string() + ")");
            continue;
        }
        walk_platform_root(root, platform, it->second, config, opts, result);
    }

    emit(opts, {"match", "", {}, result.files.size(), result.files.size()});
    match_titles(catalog, result);
    return result;
}

void merge_results(ScanResult& into, ScanResult&& from) {
    into.files.insert(into.files.end(), std::make_move_iterator(from.files.begin()),
                      std::make_move_iterator(from.files.end()));
    into.matches.insert(into.matches.end(), std::make_move_iterator(from.matches.begin()),
                        std::make_move_iterator(from.matches.end()));
    into.errors.insert(into.errors.end(), std::make_move_iterator(from.errors.begin()),
                       std::make_move_iterator(from.errors.end()));
    into.scanned_roots.insert(into.scanned_roots.end(),
                              std::make_move_iterator(from.scanned_roots.begin()),
                              std::make_move_iterator(from.scanned_roots.end()));
    into.hashed_files += from.hashed_files;
    into.skipped_hash += from.skipped_hash;
    into.cache_hits += from.cache_hits;
}

} // namespace

ScanResult scan_rom_library(const Catalog& catalog, const AppConfig& config,
                            const ScanOptions& opts) {
    ScanResult result;
    if (config.library_root.empty()) {
        result.errors.push_back(
            "no library_root configured — set it in config.json or pass --rom-dir");
        return result;
    }

    std::error_code ec;
    if (!fs::is_directory(config.library_root, ec)) {
        result.errors.push_back("library_root missing: " + config.library_root.string());
        return result;
    }

    auto roots = roots_for_library(catalog, config, opts, result);
    auto scanned = scan_tagged_roots(catalog, config, roots, opts);
    scanned.errors.insert(scanned.errors.begin(), result.errors.begin(), result.errors.end());
    return scanned;
}

ScanResult scan_rom_roots(const Catalog& catalog, const AppConfig& config,
                          const std::vector<fs::path>& roots, const ScanOptions& opts) {
    ScanResult combined;
    std::vector<std::pair<std::string, fs::path>> platform_roots;

    for (const auto& root : roots) {
        std::error_code ec;
        fs::path use = fs::weakly_canonical(root, ec);
        if (ec) use = root;

        if (looks_like_library_root(use, catalog, config)) {
            AppConfig tmp = config;
            tmp.library_root = use;
            merge_results(combined, scan_rom_library(catalog, tmp, opts));
            continue;
        }

        const std::string platform = platform_for_folder(config, use.filename().string());
        platform_roots.emplace_back(platform, use);
    }

    if (!platform_roots.empty())
        merge_results(combined, scan_tagged_roots(catalog, config, platform_roots, opts));

    // Rematch once across merged files so duplicates across roots collapse cleanly.
    combined.matches.clear();
    match_titles(catalog, combined);
    return combined;
}

} // namespace retcomm
