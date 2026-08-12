#include "retcomm/cache_gc.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace retcomm {
namespace {

using clock = std::chrono::system_clock;

int version_cmp_tag(std::string a, std::string b) {
    auto strip_v = [](std::string& s) {
        if (s.size() >= 2 && (s[0] == 'v' || s[0] == 'V') &&
            std::isdigit(static_cast<unsigned char>(s[1])))
            s.erase(s.begin());
    };
    strip_v(a);
    strip_v(b);
    size_t ia = 0, ib = 0;
    while (ia < a.size() || ib < b.size()) {
        long va = 0, vb = 0;
        bool ha = false, hb = false;
        while (ia < a.size() && std::isdigit(static_cast<unsigned char>(a[ia]))) {
            va = va * 10 + (a[ia] - '0');
            ++ia;
            ha = true;
        }
        while (ib < b.size() && std::isdigit(static_cast<unsigned char>(b[ib]))) {
            vb = vb * 10 + (b[ib] - '0');
            ++ib;
            hb = true;
        }
        if (!ha && !hb) {
            // Fall back to lexicographic on remaining non-numeric tails.
            return a.compare(ia, std::string::npos, b, ib) < 0
                       ? -1
                       : (a.compare(ia, std::string::npos, b, ib) > 0 ? 1 : 0);
        }
        if (va != vb) return va < vb ? -1 : 1;
        if (ia < a.size() && a[ia] == '.') ++ia;
        if (ib < b.size() && b[ib] == '.') ++ib;
        if (!ha || !hb) return ha ? 1 : (hb ? -1 : 0);
    }
    return 0;
}

std::uint64_t dir_byte_size(const fs::path& root) {
    std::error_code ec;
    std::uint64_t total = 0;
    if (!fs::exists(root, ec)) return 0;
    if (fs::is_regular_file(root, ec)) {
        const auto sz = fs::file_size(root, ec);
        return ec ? 0 : static_cast<std::uint64_t>(sz);
    }
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto sz = it->file_size(ec);
        if (!ec) total += static_cast<std::uint64_t>(sz);
    }
    return total;
}

bool remove_path_accounting(const fs::path& p, CacheGcResult& r, const char* kind) {
    std::error_code ec;
    if (!fs::exists(p, ec) && !fs::is_symlink(p, ec)) return false;
    const std::uint64_t bytes = dir_byte_size(p);
    fs::remove_all(p, ec);
    if (ec) {
        r.messages.push_back(std::string("failed to remove ") + kind + " " + p.string() +
                             ": " + ec.message());
        r.ok = false;
        return false;
    }
    r.bytes_freed += bytes;
    r.messages.push_back(std::string("removed ") + kind + " " + p.string());
    return true;
}

fs::path resolve_link_target(const fs::path& link) {
    std::error_code ec;
    if (!fs::exists(link, ec) && !fs::is_symlink(link, ec)) return {};
    if (fs::is_symlink(link, ec)) {
        fs::path t = fs::read_symlink(link, ec);
        if (ec) return {};
        if (t.is_relative()) t = link.parent_path() / t;
        return fs::weakly_canonical(t, ec);
    }
#if defined(_WIN32)
    // Junctions look like directories; weakly_canonical still resolves them.
#endif
    return fs::weakly_canonical(link, ec);
}

bool path_under(const fs::path& path, const fs::path& prefix) {
    std::error_code ec;
    const fs::path a = fs::weakly_canonical(path, ec);
    const fs::path b = fs::weakly_canonical(prefix, ec);
    if (a.empty() || b.empty()) return false;
    auto ait = a.begin(), bit = b.begin();
    for (; ait != a.end() && bit != b.end(); ++ait, ++bit) {
        if (*ait != *bit) return false;
    }
    return bit == b.end();
}

// Collect engines/<name>/<pin> paths referenced by any title's src/current link.
std::set<std::string> referenced_engine_keys(const Paths& paths, const AppConfig& cfg) {
    std::set<std::string> out;
    std::error_code ec;
    const fs::path engines = paths.engines_dir;
    if (!fs::is_directory(engines, ec)) return out;

    auto note_if_engine = [&](const fs::path& link) {
        std::error_code lec;
        if (!fs::exists(link, lec)) return;
        // Symlink, junction, or real path — canonical form under engines/ counts.
        const fs::path target = fs::weakly_canonical(link, lec);
        if (lec || target.empty() || !path_under(target, engines)) return;
        const fs::path rel = fs::relative(target, engines, lec);
        if (lec || rel.empty()) return;
        // Expect <name>/<pin>[/…]
        auto it = rel.begin();
        if (it == rel.end()) return;
        const std::string name = it->string();
        ++it;
        if (it == rel.end()) return;
        const std::string pin = it->string();
        if (name.empty() || pin.empty() || pin[0] == '.') return;
        out.insert(name + "/" + pin);
    };

    static const char* kEngineNames[] = {"psxrecomp", "recomp-ui", "gbarecomp", "snesrecomp"};
    for (const auto& root : scan_install_roots(cfg, paths)) {
        if (root.path.empty() || !fs::is_directory(root.path, ec)) continue;
        for (auto it = fs::directory_iterator(root.path, ec); !ec && it != fs::directory_iterator();
             it.increment(ec)) {
            if (!it->is_directory(ec)) continue;
            const fs::path current = it->path() / "src" / "current";
            if (!fs::is_directory(current, ec)) continue;
            for (const char* name : kEngineNames) note_if_engine(current / name);
        }
    }
    return out;
}

void gc_versioned_pack_dir(const fs::path& pack_base, int keep_n, const char* kind,
                           CacheGcResult& r, std::size_t* counter) {
    std::error_code ec;
    if (keep_n < 1) keep_n = 1;
    if (!fs::is_directory(pack_base, ec)) return;

    fs::path latest_target;
    const fs::path latest = pack_base / "latest";
    if (fs::exists(latest, ec) || fs::is_symlink(latest, ec))
        latest_target = resolve_link_target(latest);

    std::vector<fs::path> versions;
    for (auto it = fs::directory_iterator(pack_base, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        if (!it->is_directory(ec) && !fs::is_symlink(it->path(), ec)) continue;
        const auto name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        if (name == "latest" || name.rfind("latest.old-", 0) == 0) continue;
        if (name == "path.sh" || name == "latest.path") continue;
        versions.push_back(it->path());
    }

    std::sort(versions.begin(), versions.end(), [](const fs::path& a, const fs::path& b) {
        return version_cmp_tag(a.filename().string(), b.filename().string()) < 0;
    });

    std::set<fs::path> keep;
    for (int i = 0; i < keep_n && !versions.empty(); ++i) {
        keep.insert(versions[versions.size() - 1 - static_cast<size_t>(i)]);
    }
    if (!latest_target.empty()) {
        for (const auto& v : versions) {
            if (fs::equivalent(v, latest_target, ec) || path_under(latest_target, v)) {
                keep.insert(v);
                break;
            }
            ec.clear();
        }
    }

    for (const auto& v : versions) {
        if (keep.count(v)) continue;
        if (remove_path_accounting(v, r, kind)) ++(*counter);
    }
}

void gc_toolchains_and_sdks(const Paths& paths, const AppConfig& cfg, CacheGcResult& r) {
    std::error_code ec;
    auto walk = [&](const fs::path& base, int keep_n, const char* kind, std::size_t* counter) {
        if (!fs::is_directory(base, ec)) return;
        for (auto it = fs::directory_iterator(base, ec); !ec && it != fs::directory_iterator();
             it.increment(ec)) {
            if (!it->is_directory(ec)) continue;
            const auto name = it->path().filename().string();
            if (name.empty() || name[0] == '.') continue;
            // python-standalone is small; still GC old tags the same way.
            gc_versioned_pack_dir(it->path(), keep_n, kind, r, counter);
        }
    };
    walk(paths.toolchains_dir, cfg.keep_toolchain_versions, "toolchain", &r.removed_toolchains);
    walk(paths.sdks_dir, cfg.keep_sdk_versions, "sdk", &r.removed_sdks);
}

void gc_engines(const Paths& paths, const AppConfig& cfg, CacheGcResult& r) {
    std::error_code ec;
    if (!fs::is_directory(paths.engines_dir, ec)) return;
    const auto referenced = referenced_engine_keys(paths, cfg);

    for (auto nit = fs::directory_iterator(paths.engines_dir, ec);
         !ec && nit != fs::directory_iterator(); nit.increment(ec)) {
        if (!nit->is_directory(ec)) continue;
        const std::string name = nit->path().filename().string();
        if (name.empty() || name[0] == '.') continue;

        struct PinEntry {
            fs::path path;
            clock::time_point mtime{};
            bool referenced = false;
        };
        std::vector<PinEntry> pins;
        for (auto pit = fs::directory_iterator(nit->path(), ec);
             !ec && pit != fs::directory_iterator(); pit.increment(ec)) {
            if (!pit->is_directory(ec)) continue;
            const std::string pin = pit->path().filename().string();
            if (pin.empty() || pin[0] == '.') continue;
            PinEntry e;
            e.path = pit->path();
            e.referenced = referenced.count(name + "/" + pin) > 0;
            auto ftime = fs::last_write_time(e.path, ec);
            if (!ec) {
                const auto sctp = std::chrono::time_point_cast<clock::duration>(
                    ftime - fs::file_time_type::clock::now() + clock::now());
                e.mtime = sctp;
            }
            pins.push_back(std::move(e));
        }

        std::vector<PinEntry*> orphans;
        for (auto& p : pins) {
            if (!p.referenced) orphans.push_back(&p);
        }
        std::sort(orphans.begin(), orphans.end(), [](const PinEntry* a, const PinEntry* b) {
            return a->mtime < b->mtime;
        });

        const int keep_orphans = std::max(0, cfg.keep_orphan_engine_pins);
        const int drop = static_cast<int>(orphans.size()) - keep_orphans;
        for (int i = 0; i < drop; ++i) {
            if (remove_path_accounting(orphans[static_cast<size_t>(i)]->path, r, "engine"))
                ++r.removed_engines;
        }
    }
}

void gc_release_zips(const Paths& paths, const AppConfig& cfg, CacheGcResult& r) {
    std::error_code ec;
    const fs::path base = paths.data_dir / "cache" / "releases";
    if (!fs::is_directory(base, ec)) return;
    const int keep_n = std::max(1, cfg.keep_release_zips_per_repo);

    for (auto rit = fs::directory_iterator(base, ec); !ec && rit != fs::directory_iterator();
         rit.increment(ec)) {
        if (!rit->is_directory(ec)) continue;
        struct TagDir {
            fs::path path;
            clock::time_point mtime{};
        };
        std::vector<TagDir> tags;
        for (auto tit = fs::directory_iterator(rit->path(), ec);
             !ec && tit != fs::directory_iterator(); tit.increment(ec)) {
            if (!tit->is_directory(ec)) continue;
            TagDir t;
            t.path = tit->path();
            auto ftime = fs::last_write_time(t.path, ec);
            if (!ec) {
                t.mtime = std::chrono::time_point_cast<clock::duration>(
                    ftime - fs::file_time_type::clock::now() + clock::now());
            }
            tags.push_back(std::move(t));
        }
        if (static_cast<int>(tags.size()) <= keep_n) continue;
        std::sort(tags.begin(), tags.end(), [](const TagDir& a, const TagDir& b) {
            if (a.mtime != b.mtime) return a.mtime < b.mtime;
            return version_cmp_tag(a.path.filename().string(), b.path.filename().string()) < 0;
        });
        const int drop = static_cast<int>(tags.size()) - keep_n;
        for (int i = 0; i < drop; ++i) {
            if (remove_path_accounting(tags[static_cast<size_t>(i)].path, r, "release-zip"))
                ++r.removed_release_zips;
        }
    }
}

void gc_idle_build_dirs(const Paths& paths, const AppConfig& cfg, CacheGcResult& r) {
    const int days = cfg.idle_build_keep_days;
    if (days <= 0) return;
    // Nuclear option already wipes after every build — idle GC is redundant.
    if (cfg.auto_clean_build_dirs) return;

    std::error_code ec;
    const auto cutoff = clock::now() - std::chrono::hours(24 * days);

    for (const auto& root : scan_install_roots(cfg, paths)) {
        if (root.path.empty() || !fs::is_directory(root.path, ec)) continue;
        for (auto it = fs::directory_iterator(root.path, ec); !ec && it != fs::directory_iterator();
             it.increment(ec)) {
            if (!it->is_directory(ec)) continue;
            const fs::path current = it->path() / "src" / "current";
            if (!fs::is_directory(current, ec)) continue;

            // Prefer catalog build_dir name; also check plain "build".
            std::vector<fs::path> candidates = {current / "build", current / "build-release"};
            for (const fs::path& build_dir : candidates) {
                if (!fs::is_directory(build_dir, ec)) continue;
                auto ftime = fs::last_write_time(build_dir, ec);
                if (ec) continue;
                const auto mtime = std::chrono::time_point_cast<clock::duration>(
                    ftime - fs::file_time_type::clock::now() + clock::now());
                if (mtime > cutoff) continue;
                if (remove_path_accounting(build_dir, r, "idle-build")) ++r.removed_idle_builds;
            }
        }
    }
}

} // namespace

fs::path shared_ccache_dir(const Paths& paths) {
    return paths.data_dir / "ccache";
}

std::vector<std::pair<std::string, std::string>> shared_ccache_env(const Paths& paths,
                                                                   const AppConfig& cfg) {
    std::vector<std::pair<std::string, std::string>> out;
    if (cfg.ccache_max_gb <= 0) return out;
    std::error_code ec;
    const fs::path dir = shared_ccache_dir(paths);
    fs::create_directories(dir, ec);
    out.emplace_back("CCACHE_DIR", dir.string());
    out.emplace_back("CCACHE_MAXSIZE", std::to_string(cfg.ccache_max_gb) + "G");
    // Prefer compressing objects — disk bound for huge generated-C TUs.
    out.emplace_back("CCACHE_COMPRESS", "1");
    return out;
}

CacheGcResult run_cache_gc(const Paths& paths, const AppConfig& cfg) {
    CacheGcResult r;
    if (!cfg.auto_gc_caches) {
        r.message = "cache GC disabled (auto_gc_caches=false)";
        return r;
    }

    ensure_dirs(paths);
    gc_toolchains_and_sdks(paths, cfg, r);
    gc_engines(paths, cfg, r);
    gc_release_zips(paths, cfg, r);
    gc_idle_build_dirs(paths, cfg, r);

    // Best-effort: apply ccache max size via CLI when present.
    if (cfg.ccache_max_gb > 0) {
        std::error_code ec;
        fs::create_directories(shared_ccache_dir(paths), ec);
        // Env alone is enough for future runs; try `ccache -M` when on PATH.
        const std::string max = std::to_string(cfg.ccache_max_gb) + "G";
#if defined(_WIN32)
        const std::string cmd = "ccache.exe -M " + max;
#else
        const std::string cmd = "CCACHE_DIR=\"" + shared_ccache_dir(paths).string() +
                                "\" ccache -M " + max + " >/dev/null 2>&1";
#endif
        (void)std::system(cmd.c_str());
    }

    std::ostringstream oss;
    oss << "Cache GC: removed " << r.removed_toolchains << " toolchain(s), " << r.removed_sdks
        << " sdk(s), " << r.removed_engines << " engine pin(s), " << r.removed_release_zips
        << " release zip folder(s), " << r.removed_idle_builds << " idle build dir(s)";
    if (r.bytes_freed > 0) {
        const double gib = static_cast<double>(r.bytes_freed) / (1024.0 * 1024.0 * 1024.0);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.2f", gib);
        oss << " (~" << buf << " GiB)";
    }
    r.message = oss.str();
    return r;
}

} // namespace retcomm
