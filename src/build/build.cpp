#include "retcomm/build.hpp"
#include "retcomm/http.hpp"
#include "retcomm/library_index.hpp"
#include "retcomm/process_env.hpp"
#include "retcomm/toolchain_env.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace retcomm {
namespace {

using nlohmann::json;

void progress(const BuildProgressFn& fn, const std::string& msg, float frac = -1.0f) {
    if (fn) fn(msg, frac);
    std::cerr << msg << "\n";
}

// Stream a file with throttled status updates (same cadence as RomM downloads).
bool http_download_with_progress(const std::string& url, const fs::path& dest, std::string* error,
                                 const std::vector<std::pair<std::string, std::string>>& headers,
                                 const BuildProgressFn& on_progress, const std::string& label) {
    int last_pct = -1;
    std::uint64_t last_mib_bucket = static_cast<std::uint64_t>(-1);
    return http_download(url, dest, error, headers,
                         [&](std::uint64_t got, std::uint64_t total) {
                             if (total == 0) {
                                 const std::uint64_t mib = got >> 20;
                                 if (mib == 0) return; // wait for Content-Length or ≥1 MiB
                                 const std::uint64_t bucket = mib / 8; // every ~8 MiB
                                 if (bucket == last_mib_bucket) return;
                                 last_mib_bucket = bucket;
                                 progress(on_progress,
                                          label + "… " + std::to_string(mib) + " MiB");
                                 return;
                             }
                             const int pct = static_cast<int>((got * 100) / total);
                             if (pct == last_pct || (pct != 100 && pct % 5 != 0)) return;
                             last_pct = pct;
                             const float frac =
                                 static_cast<float>(got) / static_cast<float>(total);
                             progress(on_progress, label + "… " + std::to_string(pct) + "%", frac);
                         });
}

std::string unique_pack_suffix() {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
#if defined(_WIN32)
    return std::to_string(GetCurrentProcessId()) + "-" + std::to_string(ticks);
#else
    return std::to_string(static_cast<unsigned>(::getpid())) + "-" + std::to_string(ticks);
#endif
}

// Move extracted staging into dest. Prefer rename-aside of any previous dest so
// activation stays fast; slow unlink of the old tree happens afterward.
bool activate_pack_tree(const fs::path& staging, const fs::path& dest, fs::path* outgoing_for_cleanup,
                        std::string* error, const BuildProgressFn& on_progress,
                        const std::string& pack_label) {
    std::error_code ec;
    const fs::path parent = dest.parent_path();
    fs::create_directories(parent, ec);

    const fs::path incoming = parent / (dest.filename().string() + ".new");
    fs::remove_all(incoming, ec);
    progress(on_progress, "Activating " + pack_label + "…");
    fs::rename(staging, incoming, ec);
    if (ec) {
        fs::copy(staging, incoming,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        fs::remove_all(staging, ec);
        if (ec) {
            if (error) *error = "pack stage failed: " + ec.message();
            return false;
        }
    }

    if (fs::exists(dest, ec)) {
        const fs::path outgoing = parent / (dest.filename().string() + ".old-" + unique_pack_suffix());
        ec.clear();
        fs::rename(dest, outgoing, ec);
        if (ec) {
            progress(on_progress, "Removing previous " + pack_label + "…");
            fs::remove_all(dest, ec);
            if (ec) {
                fs::remove_all(incoming, ec);
                if (error) *error = "cannot replace existing pack: " + ec.message();
                return false;
            }
        } else if (outgoing_for_cleanup) {
            *outgoing_for_cleanup = outgoing;
        }
    }

    ec.clear();
    fs::rename(incoming, dest, ec);
    if (ec) {
        fs::copy(incoming, dest,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        fs::remove_all(incoming, ec);
        if (ec) {
            if (error) *error = "pack install failed: " + ec.message();
            return false;
        }
    }
    return true;
}

// Mirror retcomm-toolchains install.sh: latest pointer + idempotent user PATH.
void maybe_publish_toolchain_path(const Paths& paths, const std::string& pack_id,
                                  bool toolchain, PackEnsureResult& r) {
    if (!toolchain || !r.ok || r.root.empty()) return;
    const std::string id = pack_id.empty() ? "cmake-clang-v1" : pack_id;
    std::string note;
    if (publish_toolchain_user_env(paths, id, r.root, &note) && !note.empty()) {
        if (!r.message.empty()) r.message += "; ";
        r.message += note;
    }
}

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string sanitize_tag(std::string tag) {
    for (char& c : tag) {
        if (c == '/' || c == '\\' || c == ':' || c == '<' || c == '>' || c == '|' || c == '*' ||
            c == '?' || c == '"')
            c = '_';
    }
    return tag;
}

// Compare dotted numeric versions (optional leading 'v'). Returns <0, 0, >0.
int version_cmp(std::string a, std::string b) {
    auto strip_v = [](std::string& s) {
        if (s.size() >= 2 && (s[0] == 'v' || s[0] == 'V') && std::isdigit(static_cast<unsigned char>(s[1])))
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
        if (!ha && !hb) return 0;
        if (va != vb) return va < vb ? -1 : 1;
        if (ia < a.size() && a[ia] == '.') ++ia;
        if (ib < b.size() && b[ib] == '.') ++ib;
        if (!ha || !hb) return ha ? 1 : (hb ? -1 : 0);
    }
    return 0;
}

bool version_satisfies(const std::string& have, const std::string& need) {
    if (need.empty()) return true;
    if (have.empty()) return false;
    return version_cmp(have, need) >= 0;
}

std::string iso8601_now() {
    using clock = std::chrono::system_clock;
    const auto t = clock::to_time_t(clock::now());
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

bool match_glob(const std::string& pattern, const std::string& name) {
    if (pattern.empty()) return false;
    const std::string p = to_lower(pattern);
    const std::string n = to_lower(name);
    size_t pi = 0, ni = 0, star = std::string::npos, match = 0;
    while (ni < n.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == n[ni])) {
            ++pi;
            ++ni;
        } else if (pi < p.size() && p[pi] == '*') {
            star = pi++;
            match = ni;
        } else if (star != std::string::npos) {
            pi = star + 1;
            ni = ++match;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

struct GhAsset {
    std::string name;
    std::string browser_download_url;
};

struct GhRelease {
    std::string tag;
    std::string html_url;
    std::vector<GhAsset> assets;
};

bool parse_release(const json& j, GhRelease& out, std::string* error) {
    if (!j.is_object()) {
        if (error) *error = "invalid release JSON";
        return false;
    }
    out.tag = j.value("tag_name", "");
    out.html_url = j.value("html_url", "");
    out.assets.clear();
    if (j.contains("assets") && j.at("assets").is_array()) {
        for (const auto& a : j.at("assets")) {
            if (!a.is_object()) continue;
            GhAsset asset;
            asset.name = a.value("name", "");
            asset.browser_download_url = a.value("browser_download_url", "");
            if (!asset.name.empty() && !asset.browser_download_url.empty())
                out.assets.push_back(std::move(asset));
        }
    }
    if (out.tag.empty()) {
        if (error) *error = "release missing tag_name";
        return false;
    }
    return true;
}

bool fetch_latest_release(const std::string& github_slug, GhRelease& out, std::string* error,
                          bool allow_prerelease) {
    if (github_slug.empty() || github_slug.find('/') == std::string::npos) {
        if (error) *error = "invalid github slug (want owner/repo)";
        return false;
    }
    const auto headers = github_http_headers();
    if (!allow_prerelease) {
        const std::string url =
            "https://api.github.com/repos/" + github_slug + "/releases/latest";
        auto res = http_get(url, headers);
        if (!res.ok()) {
            if (error) *error = "GitHub latest: " + (res.error.empty() ? res.body : res.error);
            return false;
        }
        try {
            return parse_release(json::parse(res.body), out, error);
        } catch (const std::exception& e) {
            if (error) *error = e.what();
            return false;
        }
    }
    const std::string url = "https://api.github.com/repos/" + github_slug + "/releases?per_page=15";
    auto res = http_get(url, headers);
    if (!res.ok()) {
        if (error) *error = "GitHub releases: " + (res.error.empty() ? res.body : res.error);
        return false;
    }
    try {
        const json arr = json::parse(res.body);
        if (!arr.is_array() || arr.empty()) {
            if (error) *error = "no releases";
            return false;
        }
        for (const auto& item : arr) {
            if (item.value("draft", false)) continue;
            if (!allow_prerelease && item.value("prerelease", false)) continue;
            return parse_release(item, out, error);
        }
        if (error) *error = "no suitable release";
        return false;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool ends_with_ci(const std::string& s, const std::string& suf) {
    if (s.size() < suf.size()) return false;
    for (size_t i = 0; i < suf.size(); ++i) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[s.size() - suf.size() + i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suf[i])));
        if (a != b) return false;
    }
    return true;
}

const GhAsset* pick_asset(const GhRelease& rel, const std::string& glob) {
    if (glob.empty()) return nullptr;
    const GhAsset* best = nullptr;
    int best_score = -1;
    const std::string glob_l = to_lower(glob);
    const bool glob_wants_tools = glob_l.find("tools") != std::string::npos;
    for (const auto& a : rel.assets) {
        if (!match_glob(glob, a.name)) continue;
        int score = 0;
        const std::string n = to_lower(a.name);
        if (ends_with_ci(n, ".zip")) score += 3;
        if (n.find("x64") != std::string::npos || n.find("amd64") != std::string::npos ||
            n.find("x86_64") != std::string::npos || n.find("win64") != std::string::npos)
            score += 2;
        if (n.find("arm64") != std::string::npos || n.find("aarch64") != std::string::npos)
            score += 1;
        if (!glob_wants_tools && n.find("tools") != std::string::npos) score -= 10;
        if (score > best_score) {
            best_score = score;
            best = &a;
        }
    }
    return best;
}

fs::path unwrap_single_subdir(const fs::path& root) {
    std::error_code ec;
    std::vector<fs::path> kids;
    for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        kids.push_back(it->path());
    }
    if (kids.size() == 1 && fs::is_directory(kids[0], ec)) return kids[0];
    return root;
}

// True when an extracted tree can cmake-configure (release zips that only ship a
// binary fail this and we fall back to the GitHub source zipball).
bool source_tree_buildable(const Title& title, const fs::path& root) {
    std::error_code ec;
    if (!fs::is_regular_file(root / "CMakeLists.txt", ec)) return false;
    const std::string eng = to_lower(title.build.generate.engine);
    const std::string plat = to_lower(title.platform);
    if (eng == "psxrecomp" || (eng.empty() && plat == "psx")) {
        return fs::is_regular_file(root / "psxrecomp" / "runtime" / "runtime.cmake", ec) &&
               fs::is_directory(root / "recomp-ui", ec);
    }
    if (eng == "gbarecomp" || (eng.empty() && plat == "gba")) {
        return fs::is_directory(root / "gbarecomp", ec) &&
               fs::is_directory(root / "recomp-ui", ec);
    }
    return true;
}

bool install_extracted_tree(const fs::path& staging, const fs::path& dest, std::string* error) {
    std::error_code ec;
    fs::remove_all(dest, ec);
    fs::create_directories(dest.parent_path(), ec);
    const fs::path inner = unwrap_single_subdir(staging);
    if (inner == staging) {
        fs::rename(staging, dest, ec);
        if (ec) {
            fs::copy(staging, dest,
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            fs::remove_all(staging, ec);
        }
    } else {
        fs::create_directories(dest, ec);
        for (auto it = fs::directory_iterator(inner, ec); !ec && it != fs::directory_iterator();
             it.increment(ec)) {
            const fs::path to = dest / it->path().filename();
            fs::rename(it->path(), to, ec);
            if (ec) {
                fs::copy(it->path(), to,
                         fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            }
        }
        fs::remove_all(staging, ec);
    }
    if (!fs::is_directory(dest, ec)) {
        if (error) *error = ec ? ec.message() : "destination missing after extract";
        return false;
    }
    return true;
}

std::string shell_quote(const std::string& s) {
#if defined(_WIN32)
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += '"';
    return out;
#else
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
#endif
}

fs::path find_named_file(const fs::path& root, const std::string& name) {
    if (name.empty()) return {};
    std::error_code ec;
    const fs::path direct = root / name;
    if (fs::is_regular_file(direct, ec)) return direct;
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (it->path().filename() == name) return it->path();
    }
    return {};
}

void make_executable(const fs::path& p) {
#if !defined(_WIN32)
    std::error_code ec;
    fs::permissions(p, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
#else
    (void)p;
#endif
}

void set_current_symlink(const fs::path& install_root, const std::string& tag) {
    const fs::path link = install_root / "current";
    const fs::path target = fs::path("releases") / tag;
    std::error_code ec;
    if (fs::exists(link, ec) || fs::is_symlink(link, ec)) fs::remove(link, ec);
#if defined(_WIN32)
    fs::create_directory_symlink(install_root / target, link, ec);
    if (ec) {
        std::ofstream ptr(install_root / "current.path");
        ptr << target.string();
    }
#else
    fs::create_directory_symlink(target, link, ec);
    if (ec) throw std::runtime_error("cannot create current symlink: " + ec.message());
#endif
}

fs::path find_sdk_cli(const fs::path& sdk_root) {
    std::error_code ec;
    const fs::path marker = sdk_root / "retcomm-sdk.json";
    if (fs::is_regular_file(marker, ec)) {
        try {
            std::ifstream in(marker);
            json j;
            in >> j;
            const std::string rel = j.value("cli", "");
            if (!rel.empty()) {
                const fs::path p = sdk_root / rel;
                if (fs::is_regular_file(p, ec)) return p;
            }
        } catch (...) {
        }
    }
    for (const char* name :
         {"snesrecomp_cli.py", "psxrecomp_cli.py", "gbarecomp_cli.py"}) {
        const fs::path flat = sdk_root / name;
        if (fs::is_regular_file(flat, ec)) return flat;
        const fs::path nested = find_named_file(sdk_root, name);
        if (!nested.empty()) return nested;
    }
    return {};
}

std::string resolve_generate_engine(const Title& title) {
    const std::string eng = to_lower(title.build.generate.engine);
    if (!eng.empty()) return eng;
    const std::string plat = to_lower(title.platform);
    if (plat == "psx" || plat == "ps1" || plat == "ps") return "psxrecomp";
    if (plat == "gba") return "gbarecomp";
    return "snesrecomp";
}

bool copy_rel_file(const fs::path& from_root, const fs::path& to_root, const fs::path& rel,
                   std::error_code& ec) {
    const fs::path from = from_root / rel;
    if (!fs::is_regular_file(from, ec)) return false;
    const fs::path to = to_root / rel;
    fs::create_directories(to.parent_path(), ec);
    if (ec) return false;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

bool copy_rel_tree(const fs::path& from_root, const fs::path& to_root, const fs::path& rel,
                   std::error_code& ec) {
    const fs::path from = from_root / rel;
    if (!fs::is_directory(from, ec)) return false;
    const fs::path to = to_root / rel;
    fs::create_directories(to.parent_path(), ec);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    return !ec;
}

bool copy_tree_overwrite(const fs::path& from, const fs::path& to, std::string* error) {
    std::error_code ec;
    if (!fs::is_directory(from, ec)) {
        if (error) *error = "missing directory: " + from.string();
        return false;
    }
    fs::create_directories(to.parent_path(), ec);
    fs::remove_all(to, ec);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error) *error = "copy " + from.string() + ": " + ec.message();
        return false;
    }
    return true;
}

std::string file_stat_sig(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return {};
    const auto sz = fs::file_size(p, ec);
    if (ec) return {};
    return std::to_string(static_cast<unsigned long long>(sz)) + ":" +
           std::to_string(file_mtime_sec(p));
}

fs::path codegen_cache_dir(const Paths& paths, const Title& title) {
    return paths.apps_dir / title.install_dir_name / "codegen-cache";
}

fs::path snes_out_dir(const Title& title, const fs::path& src_root) {
    const std::string rel =
        title.build.generate.out_dir.empty() ? "src/gen" : title.build.generate.out_dir;
    return src_root / rel;
}

fs::path gba_out_dir(const Title& title, const fs::path& src_root) {
    const std::string rel = title.build.generate.out_dir.empty()
                                ? "variants/emerald/generated"
                                : title.build.generate.out_dir;
    return src_root / rel;
}

bool psx_generated_ready(const fs::path& src_root) {
    std::error_code ec;
    const fs::path game_gen = src_root / "generated";
    if (!fs::is_directory(game_gen, ec)) return false;
    bool game_ok = false;
    for (auto it = fs::directory_iterator(game_gen, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto name = it->path().filename().string();
        if (name.size() > 12 && name.find("_dispatch.c") != std::string::npos) {
            game_ok = true;
            break;
        }
    }
    if (!game_ok) return false;
    const fs::path bios_gen = src_root / "psxrecomp" / "generated";
    return fs::is_regular_file(bios_gen / "OpenBIOS_dispatch.c", ec) ||
           fs::is_regular_file(bios_gen / "SCPH1001_dispatch.c", ec);
}

bool snes_generated_ready(const Title& title, const fs::path& src_root) {
    std::error_code ec;
    const fs::path out = snes_out_dir(title, src_root);
    if (!fs::is_directory(out, ec)) return false;
    for (auto it = fs::directory_iterator(out, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto ext = it->path().extension().string();
        if (ext == ".c" || ext == ".h" || ext == ".cpp") return true;
    }
    return false;
}

bool gba_generated_ready(const Title& title, const fs::path& src_root) {
    std::error_code ec;
    const fs::path out = gba_out_dir(title, src_root);
    if (!fs::is_regular_file(out / "dispatch_table.cpp", ec)) return false;
    int shards = 0;
    for (auto it = fs::directory_iterator(out, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto name = it->path().filename().string();
        if (name.rfind("recompiled_", 0) == 0 && it->path().extension() == ".cpp")
            ++shards;
    }
    return shards >= 2;
}

bool generated_ready(const Title& title, const std::string& engine, const fs::path& src_root) {
    if (engine == "psxrecomp") return psx_generated_ready(src_root);
    if (engine == "gbarecomp") return gba_generated_ready(title, src_root);
    return snes_generated_ready(title, src_root);
}

std::string rom_fingerprint(const Paths& paths, const fs::path& rom) {
    std::error_code ec;
    if (!fs::is_regular_file(rom, ec)) return {};
    const auto idx = load_library_index(paths.library_index_path);
    if (const LibraryFile* f = idx.find_path(rom.string())) {
        if (!f->sha256.empty()) return "sha256:" + f->sha256;
        if (!f->sha1.empty()) return "sha1:" + f->sha1;
        if (!f->md5.empty()) return "md5:" + f->md5;
        if (!f->crc32.empty()) return "crc32:" + f->crc32;
    }
    return "stat:" + file_stat_sig(rom) + ":" + rom.generic_string();
}

std::string bios_fingerprint(const BuildOptions& opts) {
    if (opts.use_openbios || opts.bios_path.empty()) return "openbios";
    std::error_code ec;
    if (!fs::is_regular_file(opts.bios_path, ec)) return "bios-missing";
    return "stat:" + file_stat_sig(opts.bios_path) + ":" + opts.bios_path.generic_string();
}

std::string tool_fingerprint(const fs::path& game_bin, const fs::path& bios_bin) {
    return file_stat_sig(game_bin) + "|" + file_stat_sig(bios_bin);
}

json make_codegen_meta(const std::string& engine, const std::string& rom_fp,
                       const std::string& bios_fp, const std::string& sdk_tag,
                       const std::string& tool_fp) {
    return json{{"engine", engine},
                {"rom", rom_fp},
                {"bios", bios_fp},
                {"sdk_tag", sdk_tag},
                {"tools", tool_fp}};
}

bool codegen_meta_matches(const json& meta, const json& want) {
    return meta.value("engine", "") == want.value("engine", "") &&
           meta.value("rom", "") == want.value("rom", "") &&
           meta.value("bios", "") == want.value("bios", "") &&
           meta.value("sdk_tag", "") == want.value("sdk_tag", "") &&
           meta.value("tools", "") == want.value("tools", "");
}

bool install_generated_from(const Title& title, const std::string& engine,
                            const fs::path& from_root, const fs::path& to_root,
                            std::string* error) {
    if (engine == "psxrecomp") {
        if (!copy_tree_overwrite(from_root / "generated", to_root / "generated", error))
            return false;
        const fs::path bios_from = from_root / "psxrecomp" / "generated";
        const fs::path bios_to = to_root / "psxrecomp" / "generated";
        std::error_code ec;
        if (fs::is_directory(bios_from, ec)) {
            if (!copy_tree_overwrite(bios_from, bios_to, error)) return false;
        }
        return psx_generated_ready(to_root);
    }
    if (engine == "gbarecomp") {
        if (!copy_tree_overwrite(gba_out_dir(title, from_root), gba_out_dir(title, to_root),
                                 error))
            return false;
        const fs::path bios_from =
            from_root / "gbarecomp" / "src" / "runtime" / "generated_bios";
        const fs::path bios_to =
            to_root / "gbarecomp" / "src" / "runtime" / "generated_bios";
        std::error_code ec;
        if (fs::is_directory(bios_from, ec) &&
            fs::is_regular_file(bios_from / "bios_recompiled.cpp", ec)) {
            if (!copy_tree_overwrite(bios_from, bios_to, error)) return false;
        }
        return gba_generated_ready(title, to_root);
    }
    const fs::path from_out = snes_out_dir(title, from_root);
    const fs::path to_out = snes_out_dir(title, to_root);
    if (!copy_tree_overwrite(from_out, to_out, error)) return false;
    return snes_generated_ready(title, to_root);
}

bool try_restore_codegen_cache(const Paths& paths, const Title& title, const std::string& engine,
                               const fs::path& src_root, const json& want, std::string* note) {
    const fs::path cache = codegen_cache_dir(paths, title);
    std::error_code ec;
    const fs::path meta_path = cache / ".retcomm-codegen.json";
    if (!fs::is_regular_file(meta_path, ec)) return false;
    try {
        std::ifstream in(meta_path);
        const json meta = json::parse(in);
        if (!codegen_meta_matches(meta, want)) return false;
    } catch (...) {
        return false;
    }
    if (!generated_ready(title, engine, cache)) return false;
    std::string err;
    if (!install_generated_from(title, engine, cache, src_root, &err)) return false;
    if (note) *note = "restored codegen-cache";
    return true;
}

bool try_adopt_previous_src_generated(const Paths& paths, const Title& title,
                                      const std::string& engine, const fs::path& src_root,
                                      const json& want, std::string* note) {
    std::error_code ec;
    const fs::path src_base = paths.apps_dir / title.install_dir_name / "src";
    if (!fs::is_directory(src_base, ec)) return false;

    std::vector<fs::path> candidates;
    for (auto it = fs::directory_iterator(src_base, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const auto name = it->path().filename().string();
        if (!name.empty() && name[0] == '.') continue;
        if (it->path() == src_root) continue;
        if (!generated_ready(title, engine, it->path())) continue;
        candidates.push_back(it->path());
    }
    if (candidates.empty()) return false;
    std::sort(candidates.begin(), candidates.end());
    // Newest tag directory first (lexicographic works for v0.1.x / build- tags mostly).
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
        const fs::path stamp = *it / ".retcomm-codegen.json";
        if (!fs::is_regular_file(stamp, ec)) continue;
        try {
            std::ifstream in(stamp);
            const json meta = json::parse(in);
            if (!codegen_meta_matches(meta, want)) continue;
        } catch (...) {
            continue;
        }
        std::string err;
        if (!install_generated_from(title, engine, *it, src_root, &err)) continue;
        if (note) *note = "carried forward from " + it->filename().string();
        return true;
    }
    return false;
}

bool save_codegen_cache(const Paths& paths, const Title& title, const std::string& engine,
                        const fs::path& src_root, const json& meta, std::string* note) {
    if (!generated_ready(title, engine, src_root)) return false;
    const fs::path cache = codegen_cache_dir(paths, title);
    std::string err;
    if (engine == "psxrecomp") {
        if (!copy_tree_overwrite(src_root / "generated", cache / "generated", &err)) return false;
        const fs::path bios = src_root / "psxrecomp" / "generated";
        std::error_code ec;
        if (fs::is_directory(bios, ec)) {
            if (!copy_tree_overwrite(bios, cache / "psxrecomp" / "generated", &err)) return false;
        }
    } else if (engine == "gbarecomp") {
        if (!copy_tree_overwrite(gba_out_dir(title, src_root), gba_out_dir(title, cache), &err))
            return false;
        const fs::path bios =
            src_root / "gbarecomp" / "src" / "runtime" / "generated_bios";
        std::error_code ec;
        if (fs::is_directory(bios, ec) &&
            fs::is_regular_file(bios / "bios_recompiled.cpp", ec)) {
            if (!copy_tree_overwrite(
                    bios, cache / "gbarecomp" / "src" / "runtime" / "generated_bios", &err))
                return false;
        }
    } else {
        if (!copy_tree_overwrite(snes_out_dir(title, src_root), snes_out_dir(title, cache), &err))
            return false;
    }
    try {
        std::ofstream out(cache / ".retcomm-codegen.json");
        out << meta.dump(2) << "\n";
        std::ofstream stamp(src_root / ".retcomm-codegen.json");
        stamp << meta.dump(2) << "\n";
    } catch (...) {
        return false;
    }
    if (note) *note = "saved codegen-cache";
    return true;
}

// Promote CLI + emitter binaries from a vendored game zip into the shared SDK
// cache, then callers may prune duplicates from the source tree.
PackEnsureResult harvest_embedded_sdk(const Paths& paths, const Title& title,
                                      const fs::path& src_root, const std::string& tag) {
    PackEnsureResult r;
    std::error_code ec;
    const std::string engine = resolve_generate_engine(title);
    fs::path eng = src_root;
    std::string pack_id = title.build.sdk.id;
    if (engine == "psxrecomp") {
        eng = src_root / "psxrecomp";
        if (pack_id.empty()) pack_id = "psxrecomp-tools";
    } else if (engine == "gbarecomp") {
        eng = src_root / "gbarecomp";
        if (pack_id.empty()) pack_id = "gbarecomp-tools";
    } else {
        if (fs::is_directory(src_root / "snesrecomp", ec)) eng = src_root / "snesrecomp";
        if (pack_id.empty()) pack_id = "snesrecomp-tools";
    }
    if (find_sdk_cli(eng).empty()) {
        r.message = "no embedded SDK CLI under " + eng.string();
        return r;
    }
    auto has_tool = [&](const char* name, const char* name_exe) {
        return fs::is_regular_file(eng / "recompiler/build" / name, ec) ||
               fs::is_regular_file(eng / "recompiler/build" / name_exe, ec) ||
               fs::is_regular_file(eng / "build" / name, ec) ||
               fs::is_regular_file(eng / "build" / name_exe, ec) ||
               fs::is_regular_file(eng / name, ec) ||
               fs::is_regular_file(eng / name_exe, ec) ||
               !find_named_file(eng, name).empty() || !find_named_file(eng, name_exe).empty();
    };
    if (engine == "psxrecomp") {
        // Both emitters are required: game C + OpenBIOS regen.
        if (!has_tool("psxrecomp-game", "psxrecomp-game.exe")) {
            r.message = "embedded psxrecomp tree missing psxrecomp-game";
            return r;
        }
        if (!has_tool("psxrecomp-bios", "psxrecomp-bios.exe")) {
            r.message = "embedded psxrecomp tree missing psxrecomp-bios";
            return r;
        }
    }
    if (engine == "gbarecomp") {
        if (!has_tool("gba_recompile", "gba_recompile.exe")) {
            r.message = "embedded gbarecomp tree missing gba_recompile";
            return r;
        }
    }

    ensure_dirs(paths);
    const std::string safe = sanitize_tag(tag.empty() ? "embedded" : tag);
    const fs::path dest = paths.sdks_dir / pack_id / safe;
    fs::remove_all(dest, ec);
    fs::create_directories(dest, ec);
    if (ec) {
        r.message = "create sdk cache: " + ec.message();
        return r;
    }

    // Slim tools layout (mirrors package_*_tools.sh packs).
    copy_rel_file(eng, dest, "psxrecomp_cli.py", ec);
    copy_rel_file(eng, dest, "snesrecomp_cli.py", ec);
    copy_rel_file(eng, dest, "gbarecomp_cli.py", ec);
    copy_rel_file(eng, dest, "retcomm-sdk.json", ec);
    copy_rel_tree(eng, dest, "tools", ec);
    copy_rel_tree(eng, dest, "docs", ec);
    if (engine == "psxrecomp") {
        for (const char* name : {"psxrecomp-game", "psxrecomp-game.exe", "psxrecomp-bios",
                                 "psxrecomp-bios.exe"}) {
            copy_rel_file(eng, dest, fs::path("recompiler/build") / name, ec);
        }
        for (const char* name : {"OpenBIOS.toml", "openbios.bin", "OpenBIOS.LICENSE",
                                 "SCPH1001.toml"}) {
            copy_rel_file(eng, dest, fs::path("bios") / name, ec);
        }
        for (const char* name : {"openbios_elf_seeds.json", "openbios_dispatch_miss.json",
                                 "phase2_ghidra_seeds.json"}) {
            copy_rel_file(eng, dest, fs::path("recompiler/seeds") / name, ec);
        }
    }
    if (engine == "gbarecomp") {
        for (const char* name : {"gba_recompile", "gba_recompile.exe"}) {
            copy_rel_file(eng, dest, name, ec);
            copy_rel_file(eng, dest, fs::path("build") / name, ec);
        }
        copy_rel_file(eng, dest, "bios/gba_bios.toml", ec);
    }

    if (find_sdk_cli(dest).empty()) {
        r.message = "harvested SDK missing CLI";
        fs::remove_all(dest, ec);
        return r;
    }
    if (engine == "psxrecomp") {
        const bool dest_game =
            fs::is_regular_file(dest / "recompiler/build/psxrecomp-game", ec) ||
            fs::is_regular_file(dest / "recompiler/build/psxrecomp-game.exe", ec);
        const bool dest_bios =
            fs::is_regular_file(dest / "recompiler/build/psxrecomp-bios", ec) ||
            fs::is_regular_file(dest / "recompiler/build/psxrecomp-bios.exe", ec);
        if (!dest_game || !dest_bios) {
            r.message = "harvested SDK incomplete (need psxrecomp-game and psxrecomp-bios)";
            fs::remove_all(dest, ec);
            return r;
        }
    }
    {
        json meta = {{"id", pack_id},
                     {"tag", tag},
                     {"source", "harvested-from-game-zip"},
                     {"github", title.release.github}};
        std::ofstream out(dest / ".retcomm-pack.json");
        out << meta.dump(2) << "\n";
    }
    r.ok = true;
    r.root = dest;
    r.tag = tag;
    r.message = "harvested SDK into " + dest.string();
    return r;
}

void prune_embedded_tool_bins(const fs::path& src_root, const std::string& engine) {
    if (engine != "psxrecomp") return;
    std::error_code ec;
    const fs::path build = src_root / "psxrecomp" / "recompiler" / "build";
    if (!fs::is_directory(build, ec)) return;
    for (const char* name : {"psxrecomp-game", "psxrecomp-game.exe", "psxrecomp-bios",
                             "psxrecomp-bios.exe"}) {
        fs::remove(build / name, ec);
    }
}

fs::path toolchain_bin_dir(const fs::path& toolchain_root) {
    std::error_code ec;
    const fs::path bin = toolchain_root / "bin";
    if (fs::is_directory(bin, ec)) return bin;
    // Some packs nest one folder.
    const fs::path unwrapped = unwrap_single_subdir(toolchain_root);
    if (fs::is_directory(unwrapped / "bin", ec)) return unwrapped / "bin";
    return {};
}

bool toolchain_looks_usable(const fs::path& root) {
    std::error_code ec;
    const fs::path bin = toolchain_bin_dir(root);
    if (bin.empty()) return false;
    return fs::is_regular_file(bin / "cmake", ec) || fs::is_regular_file(bin / "cmake.exe", ec);
}

std::string read_toolchain_version(const fs::path& root) {
    std::error_code ec;
    const fs::path meta = root / "retcomm-toolchain.json";
    if (!fs::is_regular_file(meta, ec)) return {};
    try {
        std::ifstream in(meta);
        json j = json::parse(in);
        return j.value("version", "");
    } catch (...) {
        return {};
    }
}

fs::path find_cached_toolchain(const Paths& paths, const std::string& pack_id,
                               const std::string& min_version = {}) {
    std::error_code ec;
    const fs::path base = paths.toolchains_dir / pack_id;
    if (!fs::is_directory(base, ec)) return {};
    // Prefer highest semver (then name) among usable packs that meet min_version.
    std::vector<fs::path> candidates;
    for (auto it = fs::directory_iterator(base, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const auto name = it->path().filename().string();
        if (!name.empty() && name[0] == '.') continue;
        // Skip symlink/junction pointer and renamed leftover full copies.
        if (name == "latest" || name.rfind("latest.old-", 0) == 0) continue;
        if (!toolchain_looks_usable(it->path())) continue;
        const fs::path root = unwrap_single_subdir(it->path());
        std::string ver = read_toolchain_version(root);
        if (ver.empty()) ver = name;
        if (!version_satisfies(ver, min_version)) continue;
        candidates.push_back(it->path());
    }
    if (candidates.empty()) return {};
    std::sort(candidates.begin(), candidates.end(),
              [](const fs::path& a, const fs::path& b) {
                  const fs::path ra = unwrap_single_subdir(a);
                  const fs::path rb = unwrap_single_subdir(b);
                  std::string va = read_toolchain_version(ra);
                  std::string vb = read_toolchain_version(rb);
                  if (va.empty()) va = a.filename().string();
                  if (vb.empty()) vb = b.filename().string();
                  const int c = version_cmp(va, vb);
                  if (c != 0) return c < 0;
                  return a.filename().string() < b.filename().string();
              });
    return unwrap_single_subdir(candidates.back());
}

void prune_embedded_toolchain(const fs::path& src_root) {
    std::error_code ec;
    fs::remove_all(src_root / "toolchain", ec);
}

// Promote game-zip toolchain/ into the shared cache (or reuse an existing cache
// entry), then prune the per-title copy to save disk.
PackEnsureResult harvest_embedded_toolchain(const Paths& paths, const Title& title,
                                            const fs::path& src_root,
                                            const std::string& src_tag) {
    PackEnsureResult r;
    std::error_code ec;
    const std::string pack_id =
        title.build.toolchain.id.empty() ? "cmake-clang-v1" : title.build.toolchain.id;

    const fs::path emb_raw = src_root / "toolchain";
    const bool have_embedded =
        fs::is_directory(emb_raw, ec) && toolchain_looks_usable(unwrap_single_subdir(emb_raw));

    // Reuse any already-cached pack for this id (shared across titles).
    const fs::path cached =
        find_cached_toolchain(paths, pack_id, title.build.toolchain.min_version);
    if (!cached.empty()) {
        if (have_embedded) prune_embedded_toolchain(src_root);
        r.ok = true;
        r.root = cached;
        r.tag = cached.filename().string();
        r.message = "using cached toolchain " + r.root.string();
        maybe_publish_toolchain_path(paths, pack_id, /*toolchain=*/true, r);
        return r;
    }

    if (!have_embedded) {
        r.message = "no embedded toolchain/ under " + src_root.string();
        return r;
    }

    const fs::path emb = unwrap_single_subdir(emb_raw);
    std::string ver = read_toolchain_version(emb);
    if (!version_satisfies(ver.empty() ? src_tag : ver, title.build.toolchain.min_version)) {
        r.message = "embedded toolchain version too old (need >= " +
                    title.build.toolchain.min_version + ")";
        return r;
    }
    if (ver.empty()) ver = src_tag.empty() ? "embedded" : src_tag;
    const std::string safe = sanitize_tag(ver);
    ensure_dirs(paths);
    const fs::path dest = paths.toolchains_dir / pack_id / safe;
    fs::remove_all(dest, ec);
    fs::create_directories(dest.parent_path(), ec);

    // Prefer rename (no 2GB copy); fall back to copy + delete.
    fs::rename(emb, dest, ec);
    if (ec) {
        ec.clear();
        fs::create_directories(dest, ec);
        fs::copy(emb, dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing,
                 ec);
        if (ec) {
            r.message = "promote toolchain failed: " + ec.message();
            fs::remove_all(dest, ec);
            return r;
        }
        fs::remove_all(emb_raw, ec);
    } else {
        // Renamed inner tree; drop empty wrapper if emb was nested.
        fs::remove_all(emb_raw, ec);
    }

    if (!toolchain_looks_usable(dest)) {
        r.message = "harvested toolchain missing bin/cmake";
        fs::remove_all(dest, ec);
        return r;
    }
    {
        json meta = {{"id", pack_id},
                     {"tag", ver},
                     {"source", "harvested-from-game-zip"},
                     {"github", title.release.github}};
        std::ofstream out(dest / ".retcomm-pack.json");
        out << meta.dump(2) << "\n";
    }
    r.ok = true;
    r.root = unwrap_single_subdir(dest);
    r.tag = ver;
    r.message = "harvested toolchain into " + r.root.string();
    maybe_publish_toolchain_path(paths, pack_id, /*toolchain=*/true, r);
    return r;
}

void prune_build_tree_after_success(const fs::path& src_root, const fs::path& build_dir) {
    std::error_code ec;
    // Drop cmake intermediates / whole build dir (staged binary lives under releases/).
    if (!build_dir.empty()) fs::remove_all(build_dir, ec);
    prune_embedded_toolchain(src_root);
}

// Pins must match retcomm-toolchains/pins.env (PYTHON_VERSION / PYTHON_PBS_TAG).
constexpr const char* kPythonVersion = "3.12.13";
constexpr const char* kPythonPbsTag = "20260807";
constexpr const char* kPythonStandalonePackId = "python-standalone";

bool path_looks_like_windows_store_python(const fs::path& p) {
    const std::string s = p.string();
    // Microsoft Store alias stubs live under WindowsApps and break non-interactive use.
    return s.find("WindowsApps") != std::string::npos ||
           s.find("windowsapps") != std::string::npos;
}

bool python_binary_usable(const fs::path& p) {
    std::error_code ec;
    if (p.empty() || !fs::is_regular_file(p, ec)) return false;
#if defined(_WIN32)
    if (path_looks_like_windows_store_python(p)) return false;
#endif
    return true;
}

// Locate CPython under a PBS / toolchain python/ root.
fs::path python_exe_in_root(const fs::path& py_root) {
    std::error_code ec;
    if (py_root.empty() || !fs::is_directory(py_root, ec)) return {};
#if defined(_WIN32)
    for (const char* rel : {"python.exe", "python3.exe"}) {
        const fs::path cand = py_root / rel;
        if (python_binary_usable(cand)) return cand;
    }
#endif
    for (const char* rel : {"bin/python3", "bin/python"}) {
        const fs::path cand = py_root / rel;
        if (python_binary_usable(cand)) return cand;
    }
#if defined(__APPLE__)
    // macOS universal pack: arch-specific trees + dispatcher under bin/.
#if defined(__aarch64__)
    for (const char* rel : {"aarch64-apple-darwin/bin/python3", "aarch64-apple-darwin/bin/python"}) {
        const fs::path cand = py_root / rel;
        if (python_binary_usable(cand)) return cand;
    }
#else
    for (const char* rel : {"x86_64-apple-darwin/bin/python3", "x86_64-apple-darwin/bin/python"}) {
        const fs::path cand = py_root / rel;
        if (python_binary_usable(cand)) return cand;
    }
#endif
#endif
    return {};
}

std::string pbs_target_triple() {
#if defined(_WIN32)
#if defined(_M_ARM64) || defined(__aarch64__)
    return "aarch64-pc-windows-msvc";
#else
    return "x86_64-pc-windows-msvc";
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__)
    return "aarch64-apple-darwin";
#else
    return "x86_64-apple-darwin";
#endif
#else
#if defined(__aarch64__)
    return "aarch64-unknown-linux-gnu";
#else
    return "x86_64-unknown-linux-gnu";
#endif
#endif
}

fs::path find_bundled_python(const Paths& paths, const fs::path& toolchain_root) {
    std::error_code ec;
    if (!toolchain_root.empty()) {
        if (auto p = python_exe_in_root(toolchain_root / "python"); !p.empty()) return p;
    }
    // Shared cache: toolchains/python-standalone/<tag>/…
    const fs::path base = paths.toolchains_dir / kPythonStandalonePackId;
    if (!fs::is_directory(base, ec)) return {};
    std::vector<fs::path> tags;
    for (auto it = fs::directory_iterator(base, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        if (it->path().filename() == "latest") continue;
        tags.push_back(it->path());
    }
    std::sort(tags.begin(), tags.end());
    for (auto it = tags.rbegin(); it != tags.rend(); ++it) {
        const fs::path root = unwrap_single_subdir(*it);
        if (auto p = python_exe_in_root(root); !p.empty()) return p;
        if (auto p = python_exe_in_root(root / "python"); !p.empty()) return p;
    }
    const fs::path latest = base / "latest";
    if (fs::exists(latest, ec)) {
        const fs::path root = unwrap_single_subdir(latest);
        if (auto p = python_exe_in_root(root); !p.empty()) return p;
        if (auto p = python_exe_in_root(root / "python"); !p.empty()) return p;
    }
    return {};
}

fs::path resolve_python(const Paths& paths, const fs::path& toolchain_root) {
    if (const char* env = std::getenv("RETCOMM_PYTHON")) {
        const fs::path p = env;
        if (python_binary_usable(p)) return p;
        // Allow RETCOMM_PYTHON=python3 style names when they are not Store stubs.
        if (!p.empty() && !p.has_parent_path()) {
#if defined(_WIN32)
            if (path_looks_like_windows_store_python(p)) {
                // fall through
            } else {
                return p;
            }
#else
            return p;
#endif
        }
    }
    if (auto bundled = find_bundled_python(paths, toolchain_root); !bundled.empty())
        return bundled;
#if defined(_WIN32)
    // Do not fall back to bare "python" — often the Windows Store alias stub.
    return {};
#else
    return "python3";
#endif
}

// Download python-build-standalone into toolchains/python-standalone/<tag>/ when
// neither the toolchain pack nor the shared cache has a usable interpreter.
PackEnsureResult ensure_bundled_python(const Paths& paths, const fs::path& toolchain_root,
                                       BuildProgressFn on_progress) {
    PackEnsureResult r;
    if (auto existing = find_bundled_python(paths, toolchain_root); !existing.empty()) {
        r.ok = true;
        r.root = existing.parent_path();
        if (r.root.filename() == "bin") r.root = r.root.parent_path();
        r.tag = kPythonPbsTag;
        r.message = "using bundled Python " + existing.string();
        return r;
    }

    const std::string triple = pbs_target_triple();
    const std::string asset = std::string("cpython-") + kPythonVersion + "+" + kPythonPbsTag +
                              "-" + triple + "-install_only_stripped.tar.gz";
    const std::string url =
        std::string("https://github.com/astral-sh/python-build-standalone/releases/download/") +
        kPythonPbsTag + "/" + asset;

    progress(on_progress, "Downloading embeddable Python " + std::string(kPythonVersion) + "…",
             0.02f);

    const fs::path dest = paths.toolchains_dir / kPythonStandalonePackId / kPythonPbsTag;
    const fs::path downloads = paths.data_dir / "downloads";
    const fs::path archive = downloads / asset;
    const fs::path staging =
        downloads / ("extract-" + std::string(kPythonStandalonePackId) + "-" + kPythonPbsTag);

    std::error_code ec;
    fs::create_directories(downloads, ec);
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);

    std::string err;
    if (!http_download_with_progress(url, archive, &err, {}, on_progress, asset)) {
        r.message = "Python download failed: " + err;
        return r;
    }
    if (!extract_archive_to(archive, staging, &err)) {
        r.message = "Python extract failed: " + err;
        return r;
    }

    fs::path py_src = staging / "python";
    if (!fs::is_directory(py_src, ec)) {
        const fs::path inner = unwrap_single_subdir(staging);
        if (fs::is_directory(inner / "python", ec))
            py_src = inner / "python";
        else if (!python_exe_in_root(inner).empty())
            py_src = inner;
        else {
            r.message = "Python archive layout unexpected (no python/)";
            fs::remove_all(staging, ec);
            return r;
        }
    }

    fs::remove_all(dest, ec);
    fs::create_directories(dest, ec);
    fs::copy(py_src, dest / "python",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    fs::remove_all(staging, ec);
    if (ec) {
        r.message = "Python install failed: " + ec.message();
        return r;
    }

    const fs::path exe = find_bundled_python(paths, {});
    if (exe.empty()) {
        r.message = "Python installed but interpreter not found under " + dest.string();
        return r;
    }

    const fs::path latest = paths.toolchains_dir / kPythonStandalonePackId / "latest";
    fs::remove_all(latest, ec);
    fs::create_directory_symlink(dest, latest, ec); // best-effort

    r.ok = true;
    r.root = dest;
    r.tag = kPythonPbsTag;
    r.message = "installed embeddable Python " + exe.string();
    progress(on_progress, "Embeddable Python ready", 0.03f);
    return r;
}

#if defined(_WIN32)
std::wstring narrow_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                                nullptr, 0);
    if (n > 0) {
        std::wstring out(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                            out.data(), n);
        return out;
    }
    n = MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring(s.begin(), s.end());
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string path_to_utf8(const fs::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::wstring win_quote_arg(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";
    bool need = false;
    for (wchar_t c : arg) {
        if (c == L' ' || c == L'\t' || c == L'"') {
            need = true;
            break;
        }
    }
    if (!need) return arg;
    std::wstring out = L"\"";
    for (wchar_t c : arg) {
        if (c == L'"') out += L"\\\"";
        else out += c;
    }
    out += L'"';
    return out;
}

std::wstring win_lower(std::wstring s) {
    for (wchar_t& c : s) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return s;
}

// Prefer toolchain bin\<name>.exe, else leave bare name for PATH search.
fs::path resolve_tool_exe(const std::string& name, const fs::path& path_prefix) {
    std::error_code ec;
    const fs::path as_path(name);
    if (as_path.is_absolute() && fs::is_regular_file(as_path, ec)) return as_path;
    if (!path_prefix.empty()) {
        std::string base = as_path.filename().string();
        if (base.empty()) base = name;
        const fs::path cand = path_prefix / base;
        if (fs::is_regular_file(cand, ec)) return cand;
        const bool has_exe =
            base.size() >= 4 &&
            (base.compare(base.size() - 4, 4, ".exe") == 0 ||
             base.compare(base.size() - 4, 4, ".EXE") == 0);
        if (!has_exe) {
            const fs::path cand_exe = path_prefix / (base + ".exe");
            if (fs::is_regular_file(cand_exe, ec)) return cand_exe;
        }
    }
    return as_path;
}

// Unicode environment block with PATH prepended and optional overrides.
std::wstring build_env_block(const fs::path& path_prefix,
                             const std::vector<std::pair<std::string, std::string>>& env_extra) {
    std::map<std::wstring, std::wstring, std::less<>> vars;
    LPWCH strings = GetEnvironmentStringsW();
    if (strings) {
        for (LPCWSTR p = strings; *p;) {
            const std::wstring entry(p);
            p += entry.size() + 1;
            const size_t eq = entry.find(L'=');
            if (eq == std::wstring::npos || eq == 0) continue;
            vars[win_lower(entry.substr(0, eq))] = entry; // store "KEY=value"
        }
        FreeEnvironmentStringsW(strings);
    }

    auto put = [&](const std::wstring& key, const std::wstring& value) {
        vars[win_lower(key)] = key + L"=" + value;
    };

    if (!path_prefix.empty()) {
        std::wstring path = path_prefix.wstring();
        auto it = vars.find(L"path");
        if (it != vars.end()) {
            const std::wstring& full = it->second;
            const size_t eq = full.find(L'=');
            if (eq != std::wstring::npos && eq + 1 < full.size()) {
                path.push_back(L';');
                path += full.substr(eq + 1);
            }
        }
        put(L"PATH", path);
    }
    for (const auto& [k, v] : env_extra) {
        if (k.empty()) continue;
        put(narrow_to_wide(k), narrow_to_wide(v));
    }

    std::wstring block;
    for (const auto& [_, entry] : vars) {
        block += entry;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

int run_capture_argv(const std::vector<std::string>& args, const fs::path& cwd,
                     const fs::path& path_prefix,
                     const std::vector<std::pair<std::string, std::string>>& env_extra,
                     const std::function<void(const std::string&)>& on_line,
                     std::string* combined_err) {
    if (args.empty()) {
        if (combined_err) *combined_err = "empty command";
        return 127;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr;
    HANDLE wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        if (combined_err) *combined_err = "CreatePipe failed";
        return 127;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    const fs::path exe = resolve_tool_exe(args[0], path_prefix);
    std::wstring cmdline = win_quote_arg(exe.wstring());
    for (size_t i = 1; i < args.size(); ++i) {
        cmdline.push_back(L' ');
        cmdline += win_quote_arg(narrow_to_wide(args[i]));
    }
    std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back(L'\0');

    std::wstring env_block = build_env_block(path_prefix, env_extra);
    std::wstring wcwd = cwd.empty() ? std::wstring() : cwd.wstring();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(
        exe.is_absolute() ? exe.wstring().c_str() : nullptr, mutable_cmd.data(), nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, env_block.data(),
        wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        if (combined_err)
            *combined_err = "CreateProcess failed (" + std::to_string(GetLastError()) + "): " +
                            path_to_utf8(exe);
        return 127;
    }
    CloseHandle(pi.hThread);

    std::string line;
    char buf[512];
    DWORD nread = 0;
    while (ReadFile(rd, buf, sizeof(buf), &nread, nullptr) && nread > 0) {
        for (DWORD i = 0; i < nread; ++i) {
            const char c = buf[i];
            if (c == '\n') {
                while (!line.empty() && line.back() == '\r') line.pop_back();
                if (on_line) on_line(line);
                if (combined_err) {
                    *combined_err += line;
                    *combined_err += '\n';
                }
                line.clear();
            } else {
                line.push_back(c);
            }
        }
    }
    if (!line.empty()) {
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (on_line) on_line(line);
        if (combined_err) {
            *combined_err += line;
            *combined_err += '\n';
        }
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}
#endif

std::string path_with_prefix(const fs::path& path_prefix) {
    const char* cur = std::getenv("PATH");
#if defined(_WIN32)
    const char sep = ';';
#else
    const char sep = ':';
#endif
    if (path_prefix.empty()) return cur ? cur : "";
#if defined(_WIN32)
    std::string out = path_to_utf8(path_prefix);
#else
    std::string out = path_prefix.string();
#endif
    if (cur && *cur) {
        out += sep;
        out += cur;
    }
    return out;
}

static bool path_eq_ci(const std::string& a, const std::string& b) {
#if defined(_WIN32)
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
#else
    return a == b;
#endif
}

// Drop pack-root entries from CMAKE_PREFIX_PATH. llvm-mingw's top-level
// include/math.h (etc.) must not precede libc++'s wrapping headers.
static std::string filter_pack_from_prefix_path(const std::string& cur,
                                                const std::string& pack_s, char sep) {
    if (cur.empty() || pack_s.empty()) return cur;
    std::string out;
    std::string part;
    auto flush = [&]() {
        if (part.empty()) return;
        if (!path_eq_ci(part, pack_s)) {
            if (!out.empty()) out.push_back(sep);
            out += part;
        }
        part.clear();
    };
    for (char c : cur) {
        if (c == sep)
            flush();
        else
            part.push_back(c);
    }
    flush();
    return out;
}

// Pack root (parent of bin/) for find_package(ZLIB/SDL3) without CMAKE_PREFIX_PATH.
// Host deps live under pack/deps/ (1.0.9+). Never put the llvm-mingw pack root
// on CMAKE_PREFIX_PATH / ZLIB_ROOT when that would -isystem mingw include/
// ahead of libc++ (<cmath>/<cwchar> breaks on Windows game builds).
std::vector<std::pair<std::string, std::string>> toolchain_cmake_env(
    const fs::path& path_prefix) {
    std::vector<std::pair<std::string, std::string>> out;
    if (path_prefix.empty()) return out;
    const fs::path pack = path_prefix.parent_path();
    if (pack.empty()) return out;
#if defined(_WIN32)
    const char sep = ';';
    const std::string pack_s = path_to_utf8(pack);
#else
    const char sep = ':';
    const std::string pack_s = pack.string();
#endif
    out.emplace_back("RETCOMM_TOOLCHAIN_DIR", pack_s);

    std::error_code ec;
    const fs::path deps = pack / "deps";
    const bool deps_zlib = fs::is_regular_file(deps / "include" / "zlib.h", ec);
    const bool pack_zlib = fs::is_regular_file(pack / "include" / "zlib.h", ec);
    if (deps_zlib) {
#if defined(_WIN32)
        out.emplace_back("ZLIB_ROOT", path_to_utf8(deps));
#else
        out.emplace_back("ZLIB_ROOT", deps.string());
#endif
    } else if (pack_zlib) {
        // Legacy 1.0.3–1.0.8 layout (Windows: can still poison libc++).
        out.emplace_back("ZLIB_ROOT", pack_s);
    }

    const fs::path sdl3_deps = deps / "lib" / "cmake" / "SDL3";
    const fs::path sdl3_pack = pack / "lib" / "cmake" / "SDL3";
    fs::path sdl3_dir;
    if (fs::is_regular_file(sdl3_deps / "SDL3Config.cmake", ec) ||
        fs::is_regular_file(sdl3_deps / "SDL3-config.cmake", ec)) {
        sdl3_dir = sdl3_deps;
    } else if (fs::is_regular_file(sdl3_pack / "SDL3Config.cmake", ec) ||
               fs::is_regular_file(sdl3_pack / "SDL3-config.cmake", ec)) {
        sdl3_dir = sdl3_pack;
    }
    if (!sdl3_dir.empty()) {
#if defined(_WIN32)
        out.emplace_back("SDL3_DIR", path_to_utf8(sdl3_dir));
#else
        out.emplace_back("SDL3_DIR", sdl3_dir.string());
#endif
    }

    // Strip ambient pack-root prefixes from env.bat / user Path activation so
    // cmake children never inherit the libc++ poison path.
    const char* cur = std::getenv("CMAKE_PREFIX_PATH");
    const std::string filtered =
        filter_pack_from_prefix_path(cur ? cur : "", pack_s, sep);
    out.emplace_back("CMAKE_PREFIX_PATH", filtered);
    return out;
}

// CMAKE_GENERATOR:STRING=Ninja (empty if missing / unreadable).
std::string read_cmake_cache_generator(const fs::path& cache_file) {
    std::error_code ec;
    if (!fs::is_regular_file(cache_file, ec)) return {};
    std::ifstream in(cache_file);
    if (!in) return {};
    std::string line;
    while (std::getline(in, line)) {
        constexpr const char* kKey = "CMAKE_GENERATOR:INTERNAL=";
        constexpr const char* kKey2 = "CMAKE_GENERATOR:STRING=";
        if (line.rfind(kKey, 0) == 0) return line.substr(std::strlen(kKey));
        if (line.rfind(kKey2, 0) == 0) return line.substr(std::strlen(kKey2));
    }
    return {};
}

int run_capture_lines(const std::string& cmd, const fs::path& cwd,
                      const std::function<void(const std::string&)>& on_line,
                      std::string* combined_err) {
    // Drop AppImage LD_LIBRARY_PATH for cmake/git/python children so system
    // git-remote-https is not poisoned by the mount's libcurl.
    AppImageEnvGuard env_guard;
#if defined(_WIN32)
    (void)cmd;
    (void)cwd;
    (void)on_line;
    if (combined_err)
        *combined_err = "internal error: Windows builds must use run_with_path/argv spawn";
    return 127;
#else
    std::string full = "cd " + shell_quote(cwd.string()) + " && " + cmd + " 2>&1";
    FILE* pipe = popen(full.c_str(), "r");
    if (!pipe) {
        if (combined_err) *combined_err = "failed to spawn: " + cmd;
        return 127;
    }
    char buf[512];
    std::string line;
    while (std::fgets(buf, sizeof(buf), pipe)) {
        line = buf;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (on_line) on_line(line);
        if (combined_err) {
            *combined_err += line;
            *combined_err += '\n';
        }
    }
    const int st = pclose(pipe);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
#endif
}

int run_with_path(const std::vector<std::string>& args, const fs::path& cwd,
                  const fs::path& path_prefix, std::string* err_out,
                  const std::function<void(const std::string&)>& on_line = {},
                  const std::vector<std::pair<std::string, std::string>>& env_extra = {}) {
    AppImageEnvGuard env_guard;
#if defined(_WIN32)
    return run_capture_argv(args, cwd, path_prefix, env_extra, on_line, err_out);
#else
    std::ostringstream cmd;
    if (!path_prefix.empty()) {
        cmd << "PATH=" << shell_quote(path_with_prefix(path_prefix)) << " ";
    }
    for (const auto& [k, v] : env_extra) {
        if (k.empty()) continue;
        cmd << k << "=" << shell_quote(v) << " ";
    }
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) cmd << ' ';
        cmd << shell_quote(args[i]);
    }
    return run_capture_lines(cmd.str(), cwd, on_line, err_out);
#endif
}

// Prefer a short summary when the caller already streamed CLI via on_output.
std::string fail_with_log(const std::string& head, const std::string& log,
                          const BuildOutputFn& on_output) {
    if (on_output) return head;
    if (log.empty()) return head;
    return head + "\n" + log;
}

bool copy_tree_if_exists(const fs::path& src, const fs::path& dest, std::string* error) {
    std::error_code ec;
    if (!fs::exists(src, ec)) return true;
    fs::create_directories(dest.parent_path(), ec);
    fs::copy(src, dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error) *error = "copy " + src.string() + ": " + ec.message();
        return false;
    }
    return true;
}

bool stage_build_output(const fs::path& src_root, const fs::path& build_dir,
                        const std::string& launch_name, const fs::path& release_dir,
                        const std::string& game_config_rel, std::string* error) {
    std::error_code ec;
    fs::remove_all(release_dir, ec);
    fs::create_directories(release_dir, ec);
    if (ec) {
        if (error) *error = "create release dir: " + ec.message();
        return false;
    }

    fs::path binary = find_named_file(build_dir, launch_name);
    if (binary.empty()) {
        // Multi-config generators.
        binary = find_named_file(build_dir / "Release", launch_name);
    }
    if (binary.empty()) {
        if (error) *error = "launch binary not found after build: " + launch_name;
        return false;
    }

    const fs::path dest_bin = release_dir / binary.filename();
    fs::copy_file(binary, dest_bin, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error) *error = "copy binary: " + ec.message();
        return false;
    }
    make_executable(dest_bin);

    const fs::path exe_dir = binary.parent_path();
    if (!copy_tree_if_exists(exe_dir / "assets", release_dir / "assets", error)) return false;
    if (!copy_tree_if_exists(src_root / "VERSION", release_dir / "VERSION", error)) return false;
    // game.toml (or catalog generate.config) drives [game] players=N and disc paths.
    // Without it the host defaults to 1 player and hides local/netplay pad UI.
    {
        const fs::path cfg_rel =
            game_config_rel.empty() ? fs::path("game.toml") : fs::path(game_config_rel);
        const fs::path cfg_name = cfg_rel.filename();
        if (!copy_tree_if_exists(src_root / cfg_rel, release_dir / cfg_name, error)) return false;
        if (!fs::exists(release_dir / cfg_name, ec)) {
            // Also try next to the built binary (some projects copy it POST_BUILD).
            copy_tree_if_exists(exe_dir / cfg_name, release_dir / cfg_name, error);
        }
    }
    // Fallback assets from source tree (recomp-ui POST_BUILD may not have run yet).
    if (!fs::exists(release_dir / "assets" / "fonts", ec)) {
        copy_tree_if_exists(src_root / "recomp-ui" / "assets" / "fonts",
                            release_dir / "assets" / "fonts", error);
    }
    if (!fs::exists(release_dir / "assets" / "img", ec)) {
        copy_tree_if_exists(src_root / "recomp" / "launcher", release_dir / "assets" / "img",
                            error);
    }
    return true;
}

std::string first_digest(const std::vector<std::string>& v) {
    return v.empty() ? std::string() : v.front();
}

} // namespace

PackEnsureResult ensure_pack(const Paths& paths, const TitleBuildPack& pack, bool toolchain,
                             const fs::path& override_dir, BuildProgressFn on_progress,
                             bool force) {
    PackEnsureResult r;
    std::error_code ec;

    fs::path ov = override_dir;
    if (ov.empty()) {
        if (toolchain) {
            if (const char* e = std::getenv("RETCOMM_TOOLCHAIN_DIR")) ov = e;
        } else {
            if (const char* e = std::getenv("RETCOMM_SDK_DIR")) ov = e;
        }
    }
    if (!ov.empty() && !force) {
        if (fs::is_directory(ov, ec)) {
            r.ok = true;
            r.root = unwrap_single_subdir(ov);
            r.tag = "override";
            r.message = "using override pack at " + r.root.string();
            maybe_publish_toolchain_path(paths, pack.id, toolchain, r);
            return r;
        }
        // Stale RETCOMM_TOOLCHAIN_DIR / RETCOMM_SDK_DIR (e.g. missing latest/
        // junction) must not block downloads — fall through to cache/GitHub.
        progress(on_progress,
                 "override pack dir missing (" + ov.string() + "); fetching pack…");
        ov.clear();
    }

    if (pack.id.empty() || pack.github.empty()) {
        r.message = "pack missing id/github";
        return r;
    }
    const std::string glob = pack.asset_glob_for_host();
    if (glob.empty()) {
        r.message = "pack missing asset_glob for host OS";
        return r;
    }

    // Prefer any already-cached pack that meets min_version (shared across titles).
    if (!force && toolchain && !pack.min_version.empty()) {
        const fs::path cached = find_cached_toolchain(paths, pack.id, pack.min_version);
        if (!cached.empty()) {
            r.ok = true;
            r.root = cached;
            r.tag = cached.filename().string();
            r.message = "pack cached: " + r.root.string();
            maybe_publish_toolchain_path(paths, pack.id, toolchain, r);
            return r;
        }
    }

    progress(on_progress, std::string("Fetching ") + pack.id + " pack…");
    GhRelease rel;
    std::string err;
    if (!fetch_latest_release(pack.github, rel, &err, /*allow_prerelease=*/true)) {
        r.message = "pack release: " + err +
                    " (set RETCOMM_TOOLCHAIN_DIR / RETCOMM_SDK_DIR for offline packs)";
        return r;
    }
    if (toolchain && !pack.min_version.empty() &&
        !version_satisfies(rel.tag, pack.min_version)) {
        r.message = "latest " + pack.id + " release " + rel.tag +
                    " is older than min_version " + pack.min_version;
        return r;
    }
    const GhAsset* asset = pick_asset(rel, glob);
    if (!asset) {
        r.message = "no pack asset matching '" + glob + "' on " + rel.tag;
        return r;
    }

    const fs::path base = toolchain ? paths.toolchains_dir : paths.sdks_dir;
    const std::string tag = sanitize_tag(rel.tag);
    const fs::path dest = base / pack.id / tag;
    const fs::path stamp = dest / ".retcomm-pack.json";
    if (!force && fs::is_regular_file(stamp, ec) && fs::is_directory(dest, ec)) {
        const fs::path root = unwrap_single_subdir(dest);
        const bool usable = !toolchain || toolchain_looks_usable(root);
        if (usable) {
            if (toolchain) {
                const std::string ver = read_toolchain_version(root);
                if (!version_satisfies(ver.empty() ? rel.tag : ver, pack.min_version)) {
                    // Stale cache entry — re-download below.
                } else {
                    r.ok = true;
                    r.root = root;
                    r.tag = rel.tag;
                    r.message = "pack cached: " + r.root.string();
                    maybe_publish_toolchain_path(paths, pack.id, toolchain, r);
                    return r;
                }
            } else {
                r.ok = true;
                r.root = root;
                r.tag = rel.tag;
                r.message = "pack cached: " + r.root.string();
                return r;
            }
        }
    }

    ensure_dirs(paths);
    const fs::path staging = base / pack.id / ".staging";
    const fs::path download = base / pack.id / ".download" / asset->name;
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    fs::create_directories(download.parent_path(), ec);

    const std::string dl_label = "Downloading " + asset->name;
    progress(on_progress, dl_label + "…");
    auto headers = github_http_headers();
    headers.erase(std::remove_if(headers.begin(), headers.end(),
                                 [](const auto& h) { return h.first == "Accept"; }),
                  headers.end());
    headers.emplace_back("Accept", "application/octet-stream");
    if (!http_download_with_progress(asset->browser_download_url, download, &err, headers,
                                     on_progress, dl_label)) {
        r.message = "pack download failed: " + err;
        return r;
    }
    progress(on_progress, "Extracting " + asset->name + "…");
    if (!extract_archive_to(download, staging, &err)) {
        r.message = "pack extract failed: " + err;
        return r;
    }

    const std::string pack_label = pack.id + " " + rel.tag;
    fs::path outgoing;
    if (!activate_pack_tree(staging, dest, &outgoing, &err, on_progress, pack_label)) {
        r.message = err;
        return r;
    }
    {
        json meta = {{"id", pack.id},
                     {"tag", rel.tag},
                     {"asset", asset->name},
                     {"github", pack.github}};
        std::ofstream out(stamp);
        out << meta.dump(2) << "\n";
    }
    progress(on_progress, "Cleaning download cache…");
    fs::remove(download, ec);

    r.root = unwrap_single_subdir(dest);
    if (toolchain && !pack.min_version.empty()) {
        const std::string ver = read_toolchain_version(r.root);
        if (!version_satisfies(ver.empty() ? rel.tag : ver, pack.min_version)) {
            r.message = "installed " + pack.id + " " + rel.tag +
                        " does not meet min_version " + pack.min_version;
            return r;
        }
    }
    r.ok = true;
    r.tag = rel.tag;
    r.message = "installed pack " + pack.id + " " + rel.tag;
    // Point latest/PATH at the new tree before the slow unlink of the previous copy.
    maybe_publish_toolchain_path(paths, pack.id, toolchain, r);
    if (!outgoing.empty()) {
        progress(on_progress, "Cleaning up previous " + pack_label + "…");
        fs::remove_all(outgoing, ec);
        if (ec) {
            // Non-fatal: new pack is already active; leftover dir can be removed later.
            if (!r.message.empty()) r.message += "; ";
            r.message += "left previous pack at " + outgoing.string() + " (" + ec.message() + ")";
        }
    }
    return r;
}

PackEnsureResult ensure_source_tree(const Paths& paths, const Title& title,
                                    const fs::path& override_dir, bool force,
                                    BuildProgressFn on_progress) {
    PackEnsureResult r;
    std::error_code ec;

    fs::path ov = override_dir;
    if (ov.empty()) {
        if (const char* e = std::getenv("RETCOMM_SOURCE_DIR")) ov = e;
    }
    if (!ov.empty()) {
        if (!fs::is_directory(ov, ec)) {
            r.message = "override source dir missing: " + ov.string();
            return r;
        }
        r.root = unwrap_single_subdir(ov);
        if (!source_tree_buildable(title, r.root)) {
            r.message = "override source dir is not cmake-buildable (missing engine/UI tree): " +
                        r.root.string();
            return r;
        }
        r.ok = true;
        r.tag = title.build.source.ref.empty() ? "override" : title.build.source.ref;
        r.message = "using override source at " + r.root.string();
        return r;
    }

    const std::string gh = title.build.source.github.empty() ? title.release.github
                                                             : title.build.source.github;
    const std::string ref = title.build.source.ref;
    if (gh.empty()) {
        r.message = "build.source.github / release.github required";
        return r;
    }

    const fs::path install_root = paths.apps_dir / title.install_dir_name;
    ensure_dirs(paths);
    const fs::path staging = install_root / "src" / ".staging";
    const fs::path download_dir = install_root / "src" / ".download";
    fs::create_directories(download_dir, ec);

    auto finish_ok = [&](const fs::path& dest, const std::string& tag, const std::string& asset,
                         const char* kind) {
        json meta = {{"github", gh},
                     {"ref", tag},
                     {"asset", asset},
                     {"source", kind}};
        std::ofstream out(dest / ".retcomm-source.json");
        out << meta.dump(2) << "\n";
        r.ok = true;
        r.root = dest;
        r.tag = tag;
        r.message = std::string("source ready (") + kind + ") at " + dest.string();
    };

    // 1) Prefer the host OS game release zip (vendors engine + UI at release pins).
    //    Falls through to zipball when the asset is binary-only or missing.
    const std::string release_gh =
        title.release.github.empty() ? gh : title.release.github;
    const std::string asset_glob = title.asset_glob_for_host();
    if (!asset_glob.empty() && !release_gh.empty()) {
        GhRelease rel;
        std::string err;
        const bool allow_pre = title.release.allow_prerelease;
        if (fetch_latest_release(release_gh, rel, &err, allow_pre)) {
            const GhAsset* asset = pick_asset(rel, asset_glob);
            if (asset) {
                const std::string tag = rel.tag.empty() ? ref : rel.tag;
                const std::string safe = sanitize_tag(tag.empty() ? asset->name : tag);
                const fs::path dest = install_root / "src" / safe;
                const fs::path marker = dest / ".retcomm-source.json";
                if (!force && fs::is_regular_file(marker, ec) && source_tree_buildable(title, dest)) {
                    r.ok = true;
                    r.root = dest;
                    r.tag = tag;
                    r.message = "source cached (release): " + dest.string();
                    return r;
                }

                const std::string dl_label = "Downloading release source " + asset->name;
                progress(on_progress, dl_label + "…");
                fs::remove_all(staging, ec);
                fs::create_directories(staging, ec);
                const fs::path download = download_dir / asset->name;
                auto headers = github_http_headers();
                headers.erase(std::remove_if(headers.begin(), headers.end(),
                                             [](const auto& h) { return h.first == "Accept"; }),
                              headers.end());
                headers.emplace_back("Accept", "application/octet-stream");
                const bool downloaded = http_download_with_progress(
                    asset->browser_download_url, download, &err, headers, on_progress, dl_label);
                if (downloaded) progress(on_progress, "Extracting " + asset->name + "…");
                if (downloaded && extract_archive_to(download, staging, &err) &&
                    install_extracted_tree(staging, dest, &err)) {
                    fs::remove(download, ec);
                    if (source_tree_buildable(title, dest)) {
                        finish_ok(dest, tag, asset->name, "release");
                        return r;
                    }
                    progress(on_progress,
                             "Release zip lacks buildable engine/UI — falling back to zipball…");
                    fs::remove_all(dest, ec);
                } else {
                    progress(on_progress,
                             "Release source fetch failed (" + err + ") — trying zipball…");
                }
            }
        }
    }

    // 2) GitHub source zipball (no git submodules — may be incomplete for psx titles).
    if (ref.empty()) {
        r.message =
            "no buildable release zip and build.source.ref empty — cannot fetch source zipball";
        return r;
    }
    const std::string safe_ref = sanitize_tag(ref);
    const fs::path dest = install_root / "src" / safe_ref;
    const fs::path marker = dest / ".retcomm-source.json";
    if (!force && fs::is_regular_file(marker, ec) && source_tree_buildable(title, dest)) {
        r.ok = true;
        r.root = dest;
        r.tag = ref;
        r.message = "source cached (zipball): " + dest.string();
        return r;
    }

    const std::string zip_label = "Downloading source zipball " + gh + "@" + ref;
    progress(on_progress, zip_label + "…");
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    const fs::path download = download_dir / (safe_ref + "-zipball.zip");
    const std::string url = "https://api.github.com/repos/" + gh + "/zipball/" + ref;
    auto headers = github_http_headers();
    headers.erase(std::remove_if(headers.begin(), headers.end(),
                                 [](const auto& h) { return h.first == "Accept"; }),
                  headers.end());
    headers.emplace_back("Accept", "application/vnd.github+json");
    std::string err;
    if (!http_download_with_progress(url, download, &err, headers, on_progress, zip_label)) {
        r.message = "source zipball download failed: " + err;
        return r;
    }
    progress(on_progress, "Extracting source zipball…");
    if (!extract_archive_to(download, staging, &err)) {
        r.message = "source extract failed: " + err;
        return r;
    }
    if (!install_extracted_tree(staging, dest, &err)) {
        r.message = "source install failed: " + err;
        return r;
    }
    fs::remove(download, ec);
    if (!source_tree_buildable(title, dest)) {
        r.message =
            "source zipball is not cmake-buildable (git submodules omitted). "
            "Publish a setup/release zip that vendors psxrecomp+recomp-ui, or set "
            "RETCOMM_SOURCE_DIR to a full checkout.";
        return r;
    }
    finish_ok(dest, ref, safe_ref + "-zipball.zip", "zipball");
    return r;
}

InstallResult build_title(const Paths& paths, const Title& title, const BuildOptions& opts) {
    InstallResult result;
    result.plan = inspect_install(paths, title);

    if (!title.supports_local_build()) {
        result.message = "catalog build recipe incomplete for " + title.id;
        return result;
    }
    if (opts.rom_path.empty()) {
        result.message =
            "verified ROM required for local build — scan your library and match " + title.id;
        return result;
    }
    std::error_code ec;
    if (!fs::is_regular_file(opts.rom_path, ec)) {
        result.message = "ROM path not found: " + opts.rom_path.string();
        return result;
    }

    if (result.plan.installed && !opts.force && result.plan.record &&
        result.plan.record->method == "build" &&
        result.plan.record->source_ref == title.build.source.ref &&
        title.asset_glob_for_host().empty()) {
        // Floating release-zip titles are skipped in update_title_auto via
        // GitHub release tags; catalog-ref equality is enough for zipball builds.
        result.ok = true;
        result.skipped = true;
        result.message = "already built at source ref " + title.build.source.ref +
                         " — use --force to rebuild\n";
        return result;
    }

    auto src = ensure_source_tree(paths, title, opts.source_dir, opts.force, opts.on_progress);
    if (!src.ok) {
        result.message = src.message;
        return result;
    }

    // Toolchain: RETCOMM_TOOLCHAIN_DIR / opts override, then catalog download
    // into the shared cache, then optional harvest of legacy zip toolchain/.
    PackEnsureResult tc;
    if (!opts.toolchain_dir.empty()) {
        tc = ensure_pack(paths, title.build.toolchain, true, opts.toolchain_dir, opts.on_progress);
    } else {
        const bool can_download =
            !title.build.toolchain.id.empty() && !title.build.toolchain.github.empty() &&
            !title.build.toolchain.asset_glob_for_host().empty();
        if (can_download) {
            progress(opts.on_progress, "Ensuring portable toolchain pack…", 0.015f);
            tc = ensure_pack(paths, title.build.toolchain, true, {}, opts.on_progress);
            if (tc.ok) {
                // Drop any leftover per-title copy from older embedded zips.
                prune_embedded_toolchain(src.root);
            }
        }
        if (!tc.ok) {
            progress(opts.on_progress,
                     can_download ? ("Toolchain download unavailable (" + tc.message +
                                     ") — trying embedded toolchain/…")
                                  : "Resolving embedded toolchain/…",
                     0.018f);
            const PackEnsureResult harvested =
                harvest_embedded_toolchain(paths, title, src.root, src.tag);
            if (harvested.ok) {
                tc = harvested;
                progress(opts.on_progress, tc.message, 0.02f);
            } else if (tc.message.empty()) {
                tc = harvested;
            } else {
                tc.message += "; harvest: " + harvested.message;
            }
        } else {
            progress(opts.on_progress, tc.message, 0.02f);
        }
    }
    if (!tc.ok) {
        result.message = tc.message.empty()
                             ? "toolchain missing (set build.toolchain to download "
                               "cmake-clang-v1, or RETCOMM_TOOLCHAIN_DIR / offline zip)"
                             : tc.message;
        return result;
    }

    // Prefer tools embedded in the game release zip (shared SDK cache + prune).
    PackEnsureResult sdk;
    if (!opts.sdk_dir.empty()) {
        sdk = ensure_pack(paths, title.build.sdk, false, opts.sdk_dir, opts.on_progress);
    } else {
        const std::string pack_id =
            title.build.sdk.id.empty() ? "psxrecomp-tools" : title.build.sdk.id;
        const fs::path cached_sdk =
            paths.sdks_dir / pack_id / sanitize_tag(src.tag.empty() ? "embedded" : src.tag);
        auto cached_sdk_complete = [&](const fs::path& root) -> bool {
            if (find_sdk_cli(root).empty()) return false;
            if (resolve_generate_engine(title) != "psxrecomp") return true;
            std::error_code cec;
            const bool game =
                fs::is_regular_file(root / "recompiler/build/psxrecomp-game", cec) ||
                fs::is_regular_file(root / "recompiler/build/psxrecomp-game.exe", cec);
            const bool bios =
                fs::is_regular_file(root / "recompiler/build/psxrecomp-bios", cec) ||
                fs::is_regular_file(root / "recompiler/build/psxrecomp-bios.exe", cec);
            return game && bios;
        };

        // After a prior successful harvest the emitters are pruned from src/.
        // Reuse the shared SDK cache instead of failing harvest + wiping it.
        if (cached_sdk_complete(cached_sdk)) {
            sdk.ok = true;
            sdk.root = cached_sdk;
            sdk.tag = src.tag;
            sdk.message = "reusing harvested SDK " + cached_sdk.string();
            progress(opts.on_progress, sdk.message, 0.03f);
        } else {
            progress(opts.on_progress, "Harvesting tools from game package…", 0.025f);
            sdk = harvest_embedded_sdk(paths, title, src.root, src.tag);
            if (sdk.ok) {
                progress(opts.on_progress, sdk.message, 0.03f);
                prune_embedded_tool_bins(src.root, resolve_generate_engine(title));
            } else if (!title.build.sdk.id.empty() && !title.build.sdk.github.empty()) {
                // Legacy fallback: catalog still points at a separate tools zip.
                if (!cached_sdk_complete(cached_sdk)) {
                    std::error_code rm_ec;
                    fs::remove_all(cached_sdk, rm_ec);
                }
                progress(opts.on_progress,
                         "No complete embedded tools (" + sdk.message +
                             ") — fetching SDK pack…",
                         0.03f);
                sdk = ensure_pack(paths, title.build.sdk, false, {}, opts.on_progress);
            } else if (!cached_sdk_complete(cached_sdk)) {
                // Drop incomplete harvests so a broken SDK pin is not reused.
                std::error_code rm_ec;
                fs::remove_all(cached_sdk, rm_ec);
            }
        }
    }
    if (!sdk.ok) {
        result.message = sdk.message.empty()
                             ? "SDK tools missing (game release zip must embed "
                               "psxrecomp-game + psxrecomp-bios)"
                             : sdk.message;
        return result;
    }

    const std::string pin_tag =
        "build-" + sanitize_tag(src.tag.empty() ? title.build.source.ref : src.tag);

    const fs::path cli = find_sdk_cli(sdk.root);
    if (cli.empty()) {
        result.message = "SDK CLI not found under " + sdk.root.string() +
                         " (expected snesrecomp_cli.py / psxrecomp_cli.py / "
                         "gbarecomp_cli.py or retcomm-sdk.json)";
        return result;
    }

    const fs::path bin_dir = toolchain_bin_dir(tc.root);
    // Allow override packs that are just "use system tools" with empty bin/.
    const fs::path path_prefix = bin_dir;
    const std::string engine = resolve_generate_engine(title);

    fs::path psxrecomp_game;
    fs::path psxrecomp_bios;
    if (engine == "psxrecomp") {
        for (const char* rel : {"recompiler/build/psxrecomp-game",
                                "recompiler/build/psxrecomp-game.exe",
                                "psxrecomp-game", "psxrecomp-game.exe"}) {
            const fs::path cand = sdk.root / rel;
            if (fs::is_regular_file(cand, ec)) {
                psxrecomp_game = cand;
                break;
            }
        }
        if (psxrecomp_game.empty())
            psxrecomp_game = find_named_file(sdk.root, "psxrecomp-game");
        if (psxrecomp_game.empty())
            psxrecomp_game = find_named_file(sdk.root, "psxrecomp-game.exe");
        if (psxrecomp_game.empty()) {
            result.message =
                "psxrecomp-game not found under SDK " + sdk.root.string() +
                " (tools pack must ship recompiler/build/psxrecomp-game)";
            return result;
        }
        for (const char* rel : {"recompiler/build/psxrecomp-bios",
                                "recompiler/build/psxrecomp-bios.exe",
                                "psxrecomp-bios", "psxrecomp-bios.exe"}) {
            const fs::path cand = sdk.root / rel;
            if (fs::is_regular_file(cand, ec)) {
                psxrecomp_bios = cand;
                break;
            }
        }
        if (psxrecomp_bios.empty())
            psxrecomp_bios = find_named_file(sdk.root, "psxrecomp-bios");
        if (psxrecomp_bios.empty())
            psxrecomp_bios = find_named_file(sdk.root, "psxrecomp-bios.exe");
        if (psxrecomp_bios.empty()) {
            result.message =
                "psxrecomp-bios not found under SDK " + sdk.root.string() +
                " (OpenBIOS regen requires recompiler/build/psxrecomp-bios; "
                "use a complete game zip or psxrecomp-tools pack)";
            return result;
        }
    }

    const json codegen_want = make_codegen_meta(
        engine, rom_fingerprint(paths, opts.rom_path), bios_fingerprint(opts), sdk.tag,
        engine == "psxrecomp" ? tool_fingerprint(psxrecomp_game, psxrecomp_bios) : sdk.tag);

    bool skip_generate = false;
    std::string reuse_note;
    if (!opts.force_generate) {
        if (generated_ready(title, engine, src.root)) {
            // Refresh stamp when inputs still match an existing stamp.
            const fs::path stamp = src.root / ".retcomm-codegen.json";
            bool stamp_ok = false;
            if (fs::is_regular_file(stamp, ec)) {
                try {
                    std::ifstream in(stamp);
                    stamp_ok = codegen_meta_matches(json::parse(in), codegen_want);
                } catch (...) {
                }
            }
            if (stamp_ok || !fs::is_regular_file(stamp, ec)) {
                skip_generate = true;
                reuse_note = stamp_ok ? "sources already present (fingerprint match)"
                                      : "sources already present";
                if (!stamp_ok) {
                    try {
                        std::ofstream out(stamp);
                        out << codegen_want.dump(2) << "\n";
                    } catch (...) {
                    }
                }
            }
        }
        if (!skip_generate &&
            try_restore_codegen_cache(paths, title, engine, src.root, codegen_want, &reuse_note)) {
            skip_generate = true;
        }
        if (!skip_generate && try_adopt_previous_src_generated(paths, title, engine, src.root,
                                                              codegen_want, &reuse_note)) {
            skip_generate = true;
        }
    }

    if (skip_generate) {
        progress(opts.on_progress, "Reusing generated C (" + reuse_note + ")…", 0.45f);
        // Keep stable cache in sync for the next update.
        save_codegen_cache(paths, title, engine, src.root, codegen_want, nullptr);
    } else {
        {
            const auto py_ens = ensure_bundled_python(paths, tc.root, opts.on_progress);
            if (!py_ens.ok) {
                result.message = py_ens.message;
                return result;
            }
        }
        const fs::path python_exe = resolve_python(paths, tc.root);
        if (python_exe.empty()) {
            result.message =
                "No usable Python interpreter (toolchain python/ missing and "
                "embeddable download failed). Set RETCOMM_PYTHON to a real "
                "python.exe / python3, or update cmake-clang-v1 to 1.0.6+.";
            return result;
        }
        progress(opts.on_progress, "Generating C sources…", 0.05f);
        std::vector<std::string> gen_args = {
            python_exe.string(),
            cli.string(),
            "generate",
        };
        if (engine == "psxrecomp") {
            const std::string cfg = title.build.generate.config.empty()
                                        ? "game.toml"
                                        : title.build.generate.config;
            gen_args.push_back("--config");
            gen_args.push_back(cfg);
            gen_args.push_back("--project-root");
            gen_args.push_back(src.root.string());
            gen_args.push_back("--disc");
            gen_args.push_back(opts.rom_path.string());
            gen_args.push_back("--json-progress");
            // Prefer a retail dump from the BIOS index / hub dropdown; OpenBIOS is
            // the fallback when use_openbios is set or no dump was provided.
            if (!opts.use_openbios && !opts.bios_path.empty()) {
                gen_args.push_back("--bios");
                gen_args.push_back(opts.bios_path.string());
            }
        } else if (engine == "gbarecomp") {
            const std::string cfg =
                title.build.generate.config.empty()
                    ? "variants/emerald/symbols/emerald_usa.toml"
                    : title.build.generate.config;
            const std::string out =
                title.build.generate.out_dir.empty()
                    ? "variants/emerald/generated"
                    : title.build.generate.out_dir;
            gen_args.push_back("--config");
            gen_args.push_back(cfg);
            gen_args.push_back("--project-root");
            gen_args.push_back(src.root.string());
            gen_args.push_back("--rom");
            gen_args.push_back(opts.rom_path.string());
            gen_args.push_back("--out-dir");
            gen_args.push_back(out);
            gen_args.push_back("--json-progress");
            if (!opts.bios_path.empty()) {
                gen_args.push_back("--bios");
                gen_args.push_back(opts.bios_path.string());
            }
            const std::string sha1 = first_digest(title.rom_identity.sha1);
            if (!sha1.empty()) {
                gen_args.push_back("--expected-sha1");
                gen_args.push_back(sha1);
            }
        } else {
            gen_args.push_back("--rom");
            gen_args.push_back(opts.rom_path.string());
            gen_args.push_back("--cfg-dir");
            gen_args.push_back(title.build.generate.cfg_dir);
            gen_args.push_back("--out-dir");
            gen_args.push_back(title.build.generate.out_dir);
            gen_args.push_back("--funcs-h");
            gen_args.push_back(title.build.generate.funcs_h);
            gen_args.push_back("--project-root");
            gen_args.push_back(src.root.string());
            gen_args.push_back("--json-progress");
            if (title.build.generate.cfg_roots) gen_args.push_back("--cfg-roots");
            const std::string crc = first_digest(title.rom_identity.crc32);
            const std::string sha256 = first_digest(title.rom_identity.sha256);
            if (!crc.empty()) {
                gen_args.push_back("--expected-crc32");
                gen_args.push_back(crc);
            }
            if (!sha256.empty()) {
                gen_args.push_back("--expected-sha256");
                gen_args.push_back(sha256);
            }
        }

        std::ostringstream gen_preview;
        for (size_t i = 0; i < gen_args.size(); ++i) {
            if (i) gen_preview << ' ';
            gen_preview << shell_quote(gen_args[i]);
        }
        std::vector<std::pair<std::string, std::string>> gen_env;
#if defined(_WIN32)
        if (!psxrecomp_game.empty())
            gen_env.emplace_back("PSXRECOMP_GAME", path_to_utf8(psxrecomp_game));
        if (!psxrecomp_bios.empty())
            gen_env.emplace_back("PSXRECOMP_BIOS", path_to_utf8(psxrecomp_bios));
#else
        if (!psxrecomp_game.empty())
            gen_env.emplace_back("PSXRECOMP_GAME", psxrecomp_game.string());
        if (!psxrecomp_bios.empty())
            gen_env.emplace_back("PSXRECOMP_BIOS", psxrecomp_bios.string());
#endif
        {
            const auto tc_env = toolchain_cmake_env(path_prefix);
            gen_env.insert(gen_env.end(), tc_env.begin(), tc_env.end());
        }

        if (opts.on_output) opts.on_output("$ " + gen_preview.str());
        std::string gen_log;
        const int gen_rc = run_with_path(
            gen_args, src.root, path_prefix, &gen_log,
            [&](const std::string& line) {
                // Stream human CLI; JSON progress ticks already go through on_progress.
                if (opts.on_output && (line.empty() || line[0] != '{')) opts.on_output(line);
                if (line.empty() || line[0] != '{') return;
                try {
                    const json j = json::parse(line);
                    const std::string ev = j.value("event", "");
                    if (ev == "phase") {
                        float pct = -1.0f;
                        if (j.contains("pct") && j.at("pct").is_number())
                            pct = j.at("pct").get<float>();
                        else if (j.contains("pct") && j.at("pct").is_number_integer())
                            pct = static_cast<float>(j.at("pct").get<int>()) / 100.0f;
                        const std::string msg = j.value("message", j.value("phase", "generate"));
                        progress(opts.on_progress, msg, pct >= 0 ? pct * 0.5f : 0.2f);
                    } else if (ev == "error") {
                        progress(opts.on_progress, j.value("message", "generate error"), -1.0f);
                    }
                } catch (...) {
                }
            },
            gen_env);
        if (gen_rc != 0) {
            result.message = fail_with_log(
                "generate failed (exit " + std::to_string(gen_rc) + ")", gen_log, opts.on_output);
            return result;
        }
        if (!generated_ready(title, engine, src.root)) {
            result.message = "generate finished but expected sources are missing under " +
                             src.root.string();
            return result;
        }
        std::string cache_note;
        save_codegen_cache(paths, title, engine, src.root, codegen_want, &cache_note);
        if (!cache_note.empty())
            progress(opts.on_progress, cache_note, 0.48f);
    }

    const fs::path build_dir = src.root / title.build.cmake.build_dir;
    if (opts.force) {
        progress(opts.on_progress, "Cleaning previous cmake build…", 0.52f);
        fs::remove_all(build_dir, ec);
    }
    progress(opts.on_progress, "Configuring cmake…", 0.55f);
    std::string cmake_log;
    std::vector<std::string> conf = {"cmake", "-S", src.root.string(), "-B",
                                     build_dir.string()};
    // Single-config generators need CMAKE_BUILD_TYPE for Release defines
    // (e.g. PSX_GAME_VERSION from the tag, not "dev").
    if (!title.build.cmake.config.empty()) {
        conf.push_back("-DCMAKE_BUILD_TYPE=" + title.build.cmake.config);
    }
    // Catalog netplay titles: ensure recomp-net is linked even if the game
    // CMakeLists forgot to opt into PSX_NETPLAY (framework default is OFF).
    if (title.supports_netplay()) {
        conf.push_back("-DPSX_NETPLAY=ON");
    }
    // Prefer Ninja when the toolchain pack provides it. On Windows, a prior
    // failed configure often leaves CMakeCache.txt stuck on "NMake Makefiles"
    // with no compiler — wipe that and force Ninja + clang from the pack.
    {
        const fs::path cache_file = build_dir / "CMakeCache.txt";
        const bool have_ninja =
            !path_prefix.empty() &&
            (fs::exists(path_prefix / "ninja", ec) || fs::exists(path_prefix / "ninja.exe", ec));
#if defined(_WIN32)
        const fs::path clang_c =
            fs::exists(path_prefix / "clang.exe", ec) ? (path_prefix / "clang.exe")
            : fs::exists(path_prefix / "clang", ec)     ? (path_prefix / "clang")
                                                       : fs::path{};
        const fs::path clang_cxx =
            fs::exists(path_prefix / "clang++.exe", ec) ? (path_prefix / "clang++.exe")
            : fs::exists(path_prefix / "clang++", ec)     ? (path_prefix / "clang++")
                                                         : fs::path{};
        if (have_ninja) {
            const std::string cached_gen = read_cmake_cache_generator(cache_file);
            if (!cached_gen.empty() && cached_gen != "Ninja") {
                progress(opts.on_progress,
                         "Replacing cmake generator \"" + cached_gen + "\" with Ninja…", 0.54f);
                fs::remove_all(build_dir, ec);
            }
            conf.push_back("-G");
            conf.push_back("Ninja");
            const fs::path ninja_exe = fs::exists(path_prefix / "ninja.exe", ec)
                                          ? (path_prefix / "ninja.exe")
                                          : (path_prefix / "ninja");
            conf.push_back("-DCMAKE_MAKE_PROGRAM=" + path_to_utf8(ninja_exe));
        } else if (opts.on_output) {
            opts.on_output(
                "warning: ninja.exe not found under toolchain bin — cmake may pick "
                "NMake Makefiles (install/update cmake-clang-v1)");
        }
        if (!clang_c.empty())
            conf.push_back("-DCMAKE_C_COMPILER=" + path_to_utf8(clang_c));
        if (!clang_cxx.empty())
            conf.push_back("-DCMAKE_CXX_COMPILER=" + path_to_utf8(clang_cxx));
#else
        const bool fresh_build = !fs::exists(cache_file, ec);
        if (fresh_build && have_ninja) {
            conf.push_back("-G");
            conf.push_back("Ninja");
        }
#endif
    }
    const auto stream_cli = [&](const std::string& line) {
        if (opts.on_output) opts.on_output(line);
    };
    if (opts.on_output) {
        std::ostringstream cmd_preview;
        for (size_t i = 0; i < conf.size(); ++i) {
            if (i) cmd_preview << ' ';
            cmd_preview << conf[i];
        }
        opts.on_output("$ " + cmd_preview.str());
    }
    const auto tc_cmake_env = toolchain_cmake_env(path_prefix);
    int crc_rc =
        run_with_path(conf, src.root, path_prefix, &cmake_log, stream_cli, tc_cmake_env);
    if (crc_rc != 0) {
        result.message = fail_with_log(
            "cmake configure failed (exit " + std::to_string(crc_rc) + ")", cmake_log,
            opts.on_output);
        return result;
    }

    progress(opts.on_progress, "Building " + title.build.cmake.target + "…", 0.65f);
    cmake_log.clear();
    std::vector<std::string> build_args = {"cmake", "--build", build_dir.string(), "--target",
                                           title.build.cmake.target};
    if (!title.build.cmake.config.empty()) {
        build_args.push_back("--config");
        build_args.push_back(title.build.cmake.config);
    }
    build_args.push_back("-j");
    if (opts.on_output) {
        std::ostringstream cmd_preview;
        for (size_t i = 0; i < build_args.size(); ++i) {
            if (i) cmd_preview << ' ';
            cmd_preview << build_args[i];
        }
        opts.on_output("$ " + cmd_preview.str());
    }
    const int build_rc = run_with_path(build_args, src.root, path_prefix, &cmake_log,
                                       stream_cli, tc_cmake_env);
    if (build_rc != 0) {
        result.message = fail_with_log(
            "cmake build failed (exit " + std::to_string(build_rc) + ")", cmake_log,
            opts.on_output);
        return result;
    }

    progress(opts.on_progress, "Staging install…", 0.9f);
    const fs::path install_root = paths.apps_dir / title.install_dir_name;
    const fs::path release_dir = install_root / "releases" / pin_tag;
    const std::string launch_name = title.launch_binary_for_host();

    std::string stash_note;
    stash_user_state_for_update(paths, title, &stash_note);

    std::string stage_err;
    if (!stage_build_output(src.root, build_dir, launch_name, release_dir,
                            title.build.generate.config, &stage_err)) {
        result.message = stage_err;
        return result;
    }

    fs::path binary = find_named_file(release_dir, launch_name);
    if (binary.empty()) binary = release_dir / launch_name;
    make_executable(binary);
    set_current_symlink(install_root, pin_tag);

    std::string restore_note;
    restore_user_state(install_root, release_dir, &restore_note);
    std::string prune_note;
    prune_old_release_dirs(install_root, pin_tag, &prune_note);

    InstallRecord rec;
    rec.title_id = title.id;
    rec.github = title.build.source.github.empty() ? title.release.github
                                                   : title.build.source.github;
    rec.tag = pin_tag;
    rec.asset_name = "local-build";
    {
        std::error_code rel_ec;
        const fs::path rel_bin = fs::relative(binary, release_dir, rel_ec);
        rec.binary = rel_ec ? binary.filename().string() : rel_bin.generic_string();
    }
    rec.host_os = host_os_key();
    rec.target_os = host_os_key();
    rec.runtime = "native";
    rec.installed_at = iso8601_now();
    rec.release_url = "https://github.com/" + rec.github + "/releases/tag/" +
                      (src.tag.empty() ? title.build.source.ref : src.tag);
    rec.method = "build";
    // Pin to the material actually built (release tag when from game zip).
    rec.source_ref = src.tag.empty() ? title.build.source.ref : src.tag;
    rec.sdk_tag = sdk.tag;
    rec.toolchain_tag = tc.tag;
    if (!save_install_record(install_root, rec)) {
        result.message = "built but failed to write install.json";
        return result;
    }

    // Free disk: drop per-title build tree + any leftover embedded toolchain/
    // (compilers live in the shared toolchains cache after harvest).
    prune_build_tree_after_success(src.root, build_dir);

    result.plan = inspect_install(paths, title);
    result.plan.latest_tag = pin_tag;
    result.ok = true;
    result.message = "built " + title.id + " from " + rec.source_ref + "\n" +
                     "  binary: " + result.plan.binary_path.string() + "\n" +
                     "  sdk: " + sdk.tag + "  toolchain: " + tc.tag + "\n";
    if (!stash_note.empty()) result.message += "  " + stash_note;
    if (!restore_note.empty()) result.message += "  " + restore_note;
    if (!prune_note.empty()) result.message += "  " + prune_note;
    progress(opts.on_progress, "Build complete", 1.0f);
    return result;
}

InstallResult install_title_auto(const Paths& paths, const Title& title,
                                 const InstallOptions& install_opts,
                                 const BuildOptions& build_opts) {
    const bool can_zip = title.supports_prebuilt_install();
    const bool can_build = !install_opts.prefer_prebuilt && title.supports_local_build();

    auto run_build = [&]() -> InstallResult {
        BuildOptions b = build_opts;
        if (b.rom_path.empty()) {
            const auto idx = load_library_index(paths.library_index_path);
            b.rom_path = idx.preferred_rom(title.id);
        }
        b.force = install_opts.force || b.force;
        return build_title(paths, title, b);
    };

    // Setup-host / one-zip catalogs: the GitHub asset is a SOURCE pack (wizard +
    // emitters), not a finished Play binary — always generate+cmake locally.
    if (can_build) return run_build();
    return install_title(paths, title, install_opts);
}

InstallResult update_title_auto(const Paths& paths, const Title& title,
                                const InstallOptions& install_opts,
                                const BuildOptions& build_opts) {
    const auto plan = inspect_install(paths, title);
    const bool was_build = plan.record && plan.record->method == "build";
    const bool can_build =
        !install_opts.prefer_prebuilt && (was_build || title.supports_local_build());
    const bool can_zip = title.supports_prebuilt_install();

    // Setup-host titles: pull latest release zip as source and rebuild.
    // codegen-cache skips disc→C when ROM/BIOS/emitter fingerprints match, so
    // host/UI-only releases are cmake-time, not full regenerate.
    if (can_build) {
        BuildOptions b = build_opts;
        if (b.rom_path.empty()) {
            const auto idx = load_library_index(paths.library_index_path);
            b.rom_path = idx.preferred_rom(title.id);
        }
        bool need = b.force || install_opts.force || !plan.installed;
        if (!need && plan.record) {
            if (can_zip && !title.release.github.empty()) {
                GhRelease rel;
                std::string err;
                const bool allow_pre =
                    install_opts.allow_prerelease || title.release.allow_prerelease;
                if (fetch_latest_release(title.release.github, rel, &err, allow_pre) &&
                    !rel.tag.empty()) {
                    const std::string latest = sanitize_tag(rel.tag);
                    const std::string have = sanitize_tag(plan.record->source_ref);
                    need = (have != latest);
                } else {
                    need = plan.record->source_ref != title.build.source.ref;
                }
            } else {
                need = plan.record->source_ref != title.build.source.ref;
            }
        }
        if (!need) {
            InstallResult r;
            r.ok = true;
            r.skipped = true;
            r.plan = plan;
            r.message = "build install already up to date (" +
                        (plan.record ? plan.record->source_ref : title.build.source.ref) + ")\n";
            return r;
        }
        b.force = true;
        // Leave force_generate as caller set (false → reuse codegen-cache).
        return build_title(paths, title, b);
    }

    if (can_zip) return update_title(paths, title, install_opts);
    return update_title(paths, title, install_opts);
}

ToolchainUpdateInfo check_toolchain_update(const Paths& paths, const std::string& pack_id,
                                           const std::string& github) {
    ToolchainUpdateInfo info;
    info.pack_id = pack_id.empty() ? "cmake-clang-v1" : pack_id;
    const std::string repo =
        github.empty() ? "TechnicallyComputers/retcomm-toolchains" : github;

    const fs::path cached = find_cached_toolchain(paths, info.pack_id, {});
    if (!cached.empty()) {
        info.installed = true;
        info.current_version = read_toolchain_version(cached);
        if (info.current_version.empty())
            info.current_version = cached.filename().string();
    }

    GhRelease rel;
    std::string err;
    if (!fetch_latest_release(repo, rel, &err, /*allow_prerelease=*/true)) {
        info.message = "toolchain update check failed: " + err;
        return info;
    }
    info.ok = true;
    info.latest_tag = rel.tag;
    std::string latest_ver = rel.tag;
    // Prefer comparing against pack JSON semantics (strip leading v).
    while (!latest_ver.empty() && (latest_ver.front() == 'v' || latest_ver.front() == 'V'))
        latest_ver.erase(latest_ver.begin());

    if (!info.installed) {
        info.update_available = false;
        info.message = "toolchain not installed (latest " + info.latest_tag + ")";
        return info;
    }

    const std::string have = info.current_version;
    info.update_available = version_cmp(have, latest_ver) < 0;
    if (info.update_available) {
        info.message = "toolchain update available: " + have + " → " + info.latest_tag;
    } else {
        info.message = "toolchain up to date (" + have + ")";
    }
    return info;
}

PackEnsureResult update_toolchain_to_latest(const Paths& paths, BuildProgressFn on_progress,
                                            const std::string& pack_id,
                                            const std::string& github) {
    TitleBuildPack pack;
    pack.id = pack_id.empty() ? "cmake-clang-v1" : pack_id;
    pack.github = github.empty() ? "TechnicallyComputers/retcomm-toolchains" : github;
    pack.asset_glob_linux = "*cmake-clang-v1*linux*";
    pack.asset_glob_windows = "*cmake-clang-v1*windows*";
    pack.asset_glob_macos = "*cmake-clang-v1*macos*";
    pack.min_version.clear();

    // Avoid re-downloading ~700MiB / replacing ~2GiB when already on latest.
    progress(on_progress, "Checking toolchain version…");
    const auto info = check_toolchain_update(paths, pack.id, pack.github);
    if (info.ok && info.installed && !info.update_available) {
        PackEnsureResult r;
        r.ok = true;
        r.tag = info.latest_tag.empty() ? info.current_version : info.latest_tag;
        r.root = find_cached_toolchain(paths, pack.id, {});
        r.message = info.message;
        // Refresh latest→pack junction / user PATH (must stay cheap on Windows —
        // never recursively copy the toolchain into latest\).
        progress(on_progress, "Refreshing toolchain PATH…");
        maybe_publish_toolchain_path(paths, pack.id, /*toolchain=*/true, r);
        progress(on_progress, r.message);
        return r;
    }

    // force=true so a present-but-stale RETCOMM_TOOLCHAIN_DIR (e.g. latest/ →
    // 1.0.5 while GitHub is 1.0.6) cannot short-circuit the download.
    return ensure_pack(paths, pack, /*toolchain=*/true, {}, on_progress, /*force=*/true);
}

} // namespace retcomm
