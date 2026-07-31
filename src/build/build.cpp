#include "retcomm/build.hpp"
#include "retcomm/http.hpp"
#include "retcomm/library_index.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
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

const GhAsset* pick_asset(const GhRelease& rel, const std::string& glob) {
    if (glob.empty()) return nullptr;
    for (const auto& a : rel.assets) {
        if (match_glob(glob, a.name)) return &a;
    }
    return nullptr;
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
    for (const char* name : {"snesrecomp_cli.py", "psxrecomp_cli.py"}) {
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
    return "snesrecomp";
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

fs::path resolve_python() {
#if defined(_WIN32)
    return "python";
#else
    if (const char* p = std::getenv("RETCOMM_PYTHON")) return p;
    return "python3";
#endif
}

int run_capture_lines(const std::string& cmd, const fs::path& cwd,
                      const std::function<void(const std::string&)>& on_line,
                      std::string* combined_err) {
#if defined(_WIN32)
    std::string full = "cmd /C \"cd /D " + cwd.string() + " && " + cmd + "\"";
    FILE* pipe = _popen(full.c_str(), "r");
#else
    std::string full = "cd " + shell_quote(cwd.string()) + " && " + cmd + " 2>&1";
    FILE* pipe = popen(full.c_str(), "r");
#endif
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
#if defined(_WIN32)
    const int rc = _pclose(pipe);
#else
    const int st = pclose(pipe);
    const int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
#endif
    return rc;
}

std::string path_with_prefix(const fs::path& path_prefix) {
    const char* cur = std::getenv("PATH");
#if defined(_WIN32)
    const char sep = ';';
#else
    const char sep = ':';
#endif
    if (path_prefix.empty()) return cur ? cur : "";
    std::string out = path_prefix.string();
    if (cur && *cur) {
        out += sep;
        out += cur;
    }
    return out;
}

int run_with_path(const std::vector<std::string>& args, const fs::path& cwd,
                  const fs::path& path_prefix, std::string* err_out) {
    std::ostringstream cmd;
#if !defined(_WIN32)
    if (!path_prefix.empty()) {
        cmd << "PATH=" << shell_quote(path_with_prefix(path_prefix)) << " ";
    }
#endif
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) cmd << ' ';
        cmd << shell_quote(args[i]);
    }
#if defined(_WIN32)
    std::string env_prefix;
    if (!path_prefix.empty()) {
        env_prefix = "set \"PATH=" + path_with_prefix(path_prefix) + "\" && ";
    }
    const std::string full = env_prefix + cmd.str();
#else
    const std::string full = cmd.str();
#endif
    return run_capture_lines(full, cwd, nullptr, err_out);
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
                        std::string* error) {
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
                             const fs::path& override_dir, BuildProgressFn on_progress) {
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
    if (!ov.empty()) {
        if (!fs::is_directory(ov, ec)) {
            r.message = "override pack dir missing: " + ov.string();
            return r;
        }
        r.ok = true;
        r.root = unwrap_single_subdir(ov);
        r.tag = "override";
        r.message = "using override pack at " + r.root.string();
        return r;
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

    progress(on_progress, std::string("Fetching ") + pack.id + " pack…");
    GhRelease rel;
    std::string err;
    if (!fetch_latest_release(pack.github, rel, &err, /*allow_prerelease=*/true)) {
        r.message = "pack release: " + err +
                    " (set RETCOMM_TOOLCHAIN_DIR / RETCOMM_SDK_DIR for offline packs)";
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
    if (fs::is_regular_file(stamp, ec) && fs::is_directory(dest, ec)) {
        r.ok = true;
        r.root = unwrap_single_subdir(dest);
        r.tag = rel.tag;
        r.message = "pack cached: " + r.root.string();
        return r;
    }

    ensure_dirs(paths);
    const fs::path staging = base / pack.id / ".staging";
    const fs::path download = base / pack.id / ".download" / asset->name;
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    fs::create_directories(download.parent_path(), ec);

    progress(on_progress, "Downloading " + asset->name + "…");
    auto headers = github_http_headers();
    headers.erase(std::remove_if(headers.begin(), headers.end(),
                                 [](const auto& h) { return h.first == "Accept"; }),
                  headers.end());
    headers.emplace_back("Accept", "application/octet-stream");
    if (!http_download(asset->browser_download_url, download, &err, headers)) {
        r.message = "pack download failed: " + err;
        return r;
    }
    if (!extract_archive_to(download, staging, &err)) {
        r.message = "pack extract failed: " + err;
        return r;
    }

    fs::remove_all(dest, ec);
    fs::create_directories(dest.parent_path(), ec);
    fs::rename(staging, dest, ec);
    if (ec) {
        fs::copy(staging, dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing,
                 ec);
        fs::remove_all(staging, ec);
        if (ec) {
            r.message = "pack install failed: " + ec.message();
            return r;
        }
    }
    {
        json meta = {{"id", pack.id},
                     {"tag", rel.tag},
                     {"asset", asset->name},
                     {"github", pack.github}};
        std::ofstream out(stamp);
        out << meta.dump(2) << "\n";
    }
    fs::remove(download, ec);

    r.ok = true;
    r.root = unwrap_single_subdir(dest);
    r.tag = rel.tag;
    r.message = "installed pack " + pack.id + " " + rel.tag;
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
        r.ok = true;
        r.root = unwrap_single_subdir(ov);
        r.tag = title.build.source.ref.empty() ? "override" : title.build.source.ref;
        r.message = "using override source at " + r.root.string();
        return r;
    }

    const std::string gh = title.build.source.github.empty() ? title.release.github
                                                             : title.build.source.github;
    const std::string ref = title.build.source.ref;
    if (gh.empty() || ref.empty()) {
        r.message = "build.source.github/ref required";
        return r;
    }

    const fs::path install_root = paths.apps_dir / title.install_dir_name;
    const std::string safe_ref = sanitize_tag(ref);
    const fs::path dest = install_root / "src" / safe_ref;
    const fs::path marker = dest / ".retcomm-source.json";
    if (!force && fs::is_regular_file(marker, ec) && fs::is_directory(dest, ec)) {
        r.ok = true;
        r.root = unwrap_single_subdir(dest);
        r.tag = ref;
        r.message = "source cached: " + r.root.string();
        return r;
    }

    ensure_dirs(paths);
    progress(on_progress, "Downloading source " + gh + "@" + ref + "…");
    const fs::path staging = install_root / "src" / ".staging";
    const fs::path download = install_root / "src" / ".download" / (safe_ref + ".zip");
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    fs::create_directories(download.parent_path(), ec);

    // GitHub zipball API (works for tags, branches, commits).
    const std::string url = "https://api.github.com/repos/" + gh + "/zipball/" + ref;
    auto headers = github_http_headers();
    headers.erase(std::remove_if(headers.begin(), headers.end(),
                                 [](const auto& h) { return h.first == "Accept"; }),
                  headers.end());
    headers.emplace_back("Accept", "application/vnd.github+json");
    std::string err;
    if (!http_download(url, download, &err, headers)) {
        r.message = "source download failed: " + err;
        return r;
    }
    if (!extract_archive_to(download, staging, &err)) {
        r.message = "source extract failed: " + err;
        return r;
    }

    fs::remove_all(dest, ec);
    fs::create_directories(dest.parent_path(), ec);
    // Keep the single top-level zipball folder as dest contents.
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
    if (ec && !fs::is_directory(dest, ec)) {
        r.message = "source install failed: " + ec.message();
        return r;
    }
    {
        json meta = {{"github", gh}, {"ref", ref}};
        std::ofstream out(marker);
        out << meta.dump(2) << "\n";
    }
    fs::remove(download, ec);

    r.ok = true;
    r.root = dest;
    r.tag = ref;
    r.message = "source ready at " + dest.string();
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

    const std::string pin_tag = "build-" + sanitize_tag(title.build.source.ref);
    if (result.plan.installed && !opts.force && result.plan.record &&
        result.plan.record->method == "build" &&
        result.plan.record->source_ref == title.build.source.ref) {
        result.ok = true;
        result.skipped = true;
        result.message = "already built at source ref " + title.build.source.ref +
                         " — use --force to rebuild\n";
        return result;
    }

    auto tc = ensure_pack(paths, title.build.toolchain, true, opts.toolchain_dir, opts.on_progress);
    if (!tc.ok) {
        result.message = tc.message;
        return result;
    }
    auto sdk = ensure_pack(paths, title.build.sdk, false, opts.sdk_dir, opts.on_progress);
    if (!sdk.ok) {
        result.message = sdk.message;
        return result;
    }
    auto src = ensure_source_tree(paths, title, opts.source_dir, opts.force, opts.on_progress);
    if (!src.ok) {
        result.message = src.message;
        return result;
    }

    const fs::path cli = find_sdk_cli(sdk.root);
    if (cli.empty()) {
        result.message = "SDK CLI not found under " + sdk.root.string() +
                         " (expected snesrecomp_cli.py / psxrecomp_cli.py or retcomm-sdk.json)";
        return result;
    }

    const fs::path bin_dir = toolchain_bin_dir(tc.root);
    // Allow override packs that are just "use system tools" with empty bin/.
    const fs::path path_prefix = bin_dir;
    const std::string engine = resolve_generate_engine(title);

    progress(opts.on_progress, "Generating C sources…", 0.05f);
    std::vector<std::string> gen_args = {
        resolve_python().string(),
        cli.string(),
        "generate",
    };
    if (engine == "psxrecomp") {
        const std::string cfg = title.build.generate.config.empty() ? "game.toml"
                                                                    : title.build.generate.config;
        gen_args.push_back("--config");
        gen_args.push_back(cfg);
        gen_args.push_back("--project-root");
        gen_args.push_back(src.root.string());
        gen_args.push_back("--disc");
        gen_args.push_back(opts.rom_path.string());
        gen_args.push_back("--json-progress");
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

    fs::path psxrecomp_game;
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
    }

    std::ostringstream gen_cmd;
#if !defined(_WIN32)
    if (!path_prefix.empty())
        gen_cmd << "PATH=" << shell_quote(path_with_prefix(path_prefix)) << " ";
    if (!psxrecomp_game.empty())
        gen_cmd << "PSXRECOMP_GAME=" << shell_quote(psxrecomp_game.string()) << " ";
#endif
    for (size_t i = 0; i < gen_args.size(); ++i) {
        if (i) gen_cmd << ' ';
        gen_cmd << shell_quote(gen_args[i]);
    }
#if defined(_WIN32)
    std::string gen_full;
    if (!path_prefix.empty())
        gen_full = "set \"PATH=" + path_with_prefix(path_prefix) + "\" && ";
    if (!psxrecomp_game.empty())
        gen_full += "set \"PSXRECOMP_GAME=" + psxrecomp_game.string() + "\" && ";
    gen_full += gen_cmd.str();
#else
    const std::string gen_full = gen_cmd.str();
#endif

    std::string gen_log;
    const int gen_rc = run_capture_lines(
        gen_full, src.root,
        [&](const std::string& line) {
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
        &gen_log);
    if (gen_rc != 0) {
        result.message = "generate failed (exit " + std::to_string(gen_rc) + ")\n" + gen_log;
        return result;
    }

    const fs::path build_dir = src.root / title.build.cmake.build_dir;
    progress(opts.on_progress, "Configuring cmake…", 0.55f);
    std::string cmake_log;
    std::vector<std::string> conf = {"cmake", "-S", src.root.string(), "-B",
                                     build_dir.string()};
    // Prefer Ninja for fresh build dirs when the toolchain pack provides it.
    // Do not override an existing generator (e.g. Unix Makefiles cache).
    {
        const bool fresh_build = !fs::exists(build_dir / "CMakeCache.txt", ec);
        const bool have_ninja =
            !path_prefix.empty() &&
            (fs::exists(path_prefix / "ninja", ec) || fs::exists(path_prefix / "ninja.exe", ec));
        if (fresh_build && have_ninja) {
            conf.push_back("-G");
            conf.push_back("Ninja");
        }
    }
    int crc_rc = run_with_path(conf, src.root, path_prefix, &cmake_log);
    if (crc_rc != 0) {
        result.message = "cmake configure failed (exit " + std::to_string(crc_rc) + ")\n" +
                         cmake_log;
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
    const int build_rc = run_with_path(build_args, src.root, path_prefix, &cmake_log);
    if (build_rc != 0) {
        result.message =
            "cmake build failed (exit " + std::to_string(build_rc) + ")\n" + cmake_log;
        return result;
    }

    progress(opts.on_progress, "Staging install…", 0.9f);
    const fs::path install_root = paths.apps_dir / title.install_dir_name;
    const fs::path release_dir = install_root / "releases" / pin_tag;
    const std::string launch_name = title.launch_binary_for_host();
    std::string stage_err;
    if (!stage_build_output(src.root, build_dir, launch_name, release_dir, &stage_err)) {
        result.message = stage_err;
        return result;
    }

    fs::path binary = find_named_file(release_dir, launch_name);
    if (binary.empty()) binary = release_dir / launch_name;
    make_executable(binary);
    set_current_symlink(install_root, pin_tag);

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
    rec.release_url = "https://github.com/" + rec.github + "/tree/" + title.build.source.ref;
    rec.method = "build";
    rec.source_ref = title.build.source.ref;
    rec.sdk_tag = sdk.tag;
    rec.toolchain_tag = tc.tag;
    if (!save_install_record(install_root, rec)) {
        result.message = "built but failed to write install.json";
        return result;
    }

    result.plan = inspect_install(paths, title);
    result.plan.latest_tag = pin_tag;
    result.ok = true;
    result.message = "built " + title.id + " from " + title.build.source.ref + "\n" +
                     "  binary: " + result.plan.binary_path.string() + "\n" +
                     "  sdk: " + sdk.tag + "  toolchain: " + tc.tag + "\n";
    progress(opts.on_progress, "Build complete", 1.0f);
    return result;
}

InstallResult install_title_auto(const Paths& paths, const Title& title,
                                 const InstallOptions& install_opts,
                                 const BuildOptions& build_opts) {
    if (!install_opts.prefer_prebuilt && title.supports_local_build()) {
        BuildOptions b = build_opts;
        if (b.rom_path.empty()) {
            const auto idx = load_library_index(paths.library_index_path);
            b.rom_path = idx.preferred_rom(title.id);
        }
        b.force = install_opts.force || b.force;
        return build_title(paths, title, b);
    }
    return install_title(paths, title, install_opts);
}

InstallResult update_title_auto(const Paths& paths, const Title& title,
                                const InstallOptions& install_opts,
                                const BuildOptions& build_opts) {
    const auto plan = inspect_install(paths, title);
    const bool was_build = plan.record && plan.record->method == "build";
    if (!install_opts.prefer_prebuilt &&
        (was_build || title.supports_local_build())) {
        BuildOptions b = build_opts;
        if (b.rom_path.empty()) {
            const auto idx = load_library_index(paths.library_index_path);
            b.rom_path = idx.preferred_rom(title.id);
        }
        // Rebuild when catalog source_ref moved, or force.
        if (plan.record && plan.record->source_ref == title.build.source.ref && !b.force &&
            !install_opts.force && plan.installed) {
            InstallResult r;
            r.ok = true;
            r.skipped = true;
            r.plan = plan;
            r.message = "build install already at source ref " + title.build.source.ref + "\n";
            return r;
        }
        b.force = true;
        return build_title(paths, title, b);
    }
    return update_title(paths, title, install_opts);
}

} // namespace retcomm
