#include "retcomm/romscan.hpp"
#include "retcomm/hash.hpp"

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
    bool need_sha1 = false;
};

std::unordered_map<std::string, PlatformNeed> needs_by_platform(const Catalog& catalog) {
    std::unordered_map<std::string, PlatformNeed> out;
    for (const auto& t : catalog.titles) {
        auto& n = out[t.platform];
        for (const auto& e : t.rom_extensions) {
            std::string le = e;
            for (char& c : le) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (!le.empty() && le[0] != '.') le.insert(le.begin(), '.');
            n.extensions.insert(le);
        }
        if (!t.rom_identity.crc32.empty()) n.need_crc = true;
        if (!t.rom_identity.sha1.empty()) n.need_sha1 = true;
    }
    return out;
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
        if (need.extensions.find(ext) == need.extensions.end()) continue;
        candidates.push_back(path);
        emit(opts, {"walk", platform, path, candidates.size(), 0});
    }
    if (ec) {
        result.errors.push_back("scan error under " + root.string() + ": " + ec.message());
    }

    const bool do_crc = opts.compute_crc_when_needed && need.need_crc;
    const bool do_sha1 = opts.compute_sha1_when_needed && need.need_sha1;

    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto& path = candidates[i];
        RomFile rf;
        rf.path = path;
        rf.platform = platform;
        rf.ext = lower_ext(path);

        if (do_crc || do_sha1) {
            emit(opts, {"hash", platform, path, i + 1, candidates.size()});
            if (do_crc) {
                rf.crc32 = file_crc32_hex(path);
                ++result.hashed_files;
            }
            if (do_sha1) {
                rf.sha1 = file_sha1_hex(path);
                if (!do_crc) ++result.hashed_files;
            }
        } else {
            ++result.skipped_hash;
        }
        result.files.push_back(std::move(rf));
    }
}

void match_titles(const Catalog& catalog, ScanResult& result) {
    std::unordered_set<std::string> matched_title_ids;
    for (const auto& t : catalog.titles) {
        if (!t.has_rom_identity()) continue;
        for (const auto& rf : result.files) {
            if (rf.platform != t.platform) continue;
            bool hit = false;
            std::string by;
            if (!t.rom_identity.crc32.empty() && !rf.crc32.empty() &&
                list_has(t.rom_identity.crc32, rf.crc32)) {
                hit = true;
                by = "crc32";
            } else if (!t.rom_identity.sha1.empty()) {
                std::string sha = rf.sha1;
                if (sha.empty()) sha = file_sha1_hex(rf.path);
                if (!sha.empty() && list_has(t.rom_identity.sha1, sha)) {
                    hit = true;
                    by = "sha1";
                }
            }
            if (!hit) continue;
            if (matched_title_ids.count(t.id)) continue;
            matched_title_ids.insert(t.id);
            TitleMatch m;
            m.title = &t;
            m.rom_path = rf.path;
            m.matched_by = by;
            result.matches.push_back(m);
            break;
        }
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
