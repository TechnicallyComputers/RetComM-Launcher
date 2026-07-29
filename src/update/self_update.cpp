#include "retcomm/self_update.hpp"

#include "retcomm/http.hpp"
#include "retcomm/install.hpp"
#include "retcomm/paths.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace retcomm {
namespace {

using json = nlohmann::json;

#if !defined(RETCOMM_VERSION)
#define RETCOMM_VERSION "0.0.0"
#endif
#if !defined(RETCOMM_GITHUB_SLUG)
#define RETCOMM_GITHUB_SLUG "TechnicallyComputers/RetComM-Launcher"
#endif

fs::path launcher_state_path(const Paths& paths) { return paths.data_dir / "launcher.json"; }

std::string normalize_tag(std::string tag) {
    while (!tag.empty() && (tag.front() == 'v' || tag.front() == 'V')) tag.erase(tag.begin());
    // trim
    while (!tag.empty() && (tag.back() == ' ' || tag.back() == '\t' || tag.back() == '\n'))
        tag.pop_back();
    size_t i = 0;
    while (i < tag.size() && (tag[i] == ' ' || tag[i] == '\t')) ++i;
    return tag.substr(i);
}

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ends_with_ci(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) return false;
    return to_lower(s.substr(s.size() - suffix.size())) == to_lower(suffix);
}

bool match_glob(const std::string& pattern, const std::string& name) {
    // Minimal * glob (same idea as install.cpp).
    size_t pi = 0, ni = 0, star = std::string::npos, match = 0;
    const std::string p = to_lower(pattern);
    const std::string n = to_lower(name);
    while (ni < n.size()) {
        if (pi < p.size() && (p[pi] == n[ni] || p[pi] == '?')) {
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

fs::path current_executable_path() {
#if defined(_WIN32)
    char buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return fs::path(buf);
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && !p.empty()) return p;
    return {};
#endif
}

bool dir_is_writable(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return false;
#if defined(_WIN32)
    const fs::path probe = dir / (".retcomm_write_test_" + std::to_string(GetTickCount64()));
#else
    const fs::path probe = dir / (".retcomm_write_test_" + std::to_string(::getpid()));
#endif
    {
        std::ofstream out(probe);
        if (!out) return false;
        out << "ok";
    }
    fs::remove(probe, ec);
    return true;
}

fs::path find_named_file(const fs::path& root, const std::string& filename) {
    if (filename.empty()) return {};
    std::error_code ec;
    const fs::path direct = root / filename;
    if (fs::is_regular_file(direct, ec)) return direct;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (it->path().filename() == filename) return it->path();
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
    out = {};
    out.tag = j.value("tag_name", "");
    out.html_url = j.value("html_url", "");
    if (j.contains("assets") && j.at("assets").is_array()) {
        for (const auto& a : j.at("assets")) {
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

bool fetch_latest_release(const std::string& slug, GhRelease& out, std::string* error,
                          bool allow_prerelease) {
    const auto headers = github_http_headers();
    if (!allow_prerelease) {
        auto res =
            http_get("https://api.github.com/repos/" + slug + "/releases/latest", headers);
        if (!res.ok()) {
            if (error) *error = res.error.empty() ? res.body : res.error;
            return false;
        }
        try {
            return parse_release(json::parse(res.body), out, error);
        } catch (const std::exception& e) {
            if (error) *error = e.what();
            return false;
        }
    }
    auto res =
        http_get("https://api.github.com/repos/" + slug + "/releases?per_page=15", headers);
    if (!res.ok()) {
        if (error) *error = res.error.empty() ? res.body : res.error;
        return false;
    }
    try {
        const json arr = json::parse(res.body);
        if (!arr.is_array() || arr.empty()) {
            if (error) *error = "no releases published yet";
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

const GhAsset* pick_launcher_asset(const GhRelease& rel) {
    const std::string os = host_os_key();
    std::string preferred;
    if (os == "linux") preferred = "*linux*";
    else if (os == "windows") preferred = "*windows*";
    else if (os == "macos") preferred = "*macos*";

    const GhAsset* best = nullptr;
    int best_score = -1;
    for (const auto& a : rel.assets) {
        int score = 0;
        const std::string n = to_lower(a.name);
        if (!preferred.empty() && match_glob(preferred, a.name)) score += 50;
        // Prefer extractable archives for in-place self-update (AppImage is
        // published for users but not applied by the updater script).
        if (ends_with_ci(n, ".zip")) score += 20;
        if (ends_with_ci(n, ".tar.gz") || ends_with_ci(n, ".tgz")) score += 12;
        if (ends_with_ci(n, ".appimage")) score += 4;
        if (n.find("retcomm") != std::string::npos) score += 5;
        if (n.find("hub") != std::string::npos) score += 2;
#if defined(__aarch64__) || defined(_M_ARM64)
        if (n.find("arm64") != std::string::npos || n.find("aarch64") != std::string::npos)
            score += 8;
        if (n.find("x86_64") != std::string::npos || n.find("amd64") != std::string::npos)
            score -= 4;
#else
        if (n.find("x86_64") != std::string::npos || n.find("amd64") != std::string::npos ||
            n.find("x64") != std::string::npos)
            score += 8;
        if (n.find("arm64") != std::string::npos || n.find("aarch64") != std::string::npos)
            score -= 4;
#endif
        if (score > best_score) {
            best_score = score;
            best = &a;
        }
    }
    // If OS-specific match failed but there's exactly one archive, take it.
    if ((!best || best_score < 50) && rel.assets.size() == 1) return &rel.assets.front();
    if (best && best_score >= 50) return best;
    return best_score > 0 ? best : nullptr;
}

bool save_launcher_state(const Paths& paths, const std::string& tag, const std::string& asset) {
    std::error_code ec;
    fs::create_directories(paths.data_dir, ec);
    json j = {{"schema_version", 1},
              {"github", retcomm_github_slug()},
              {"tag", tag},
              {"asset_name", asset},
              {"app_version", retcomm_app_version()}};
    std::ofstream out(launcher_state_path(paths));
    if (!out) return false;
    out << j.dump(2) << "\n";
    return static_cast<bool>(out);
}

#if defined(_WIN32)
bool schedule_apply_and_restart(const fs::path& staging_bin_dir, const fs::path& dest_dir,
                                const fs::path& hub_name, std::string* error) {
    const DWORD pid = GetCurrentProcessId();
    const fs::path script = staging_bin_dir.parent_path() / "apply_update.bat";
    {
        std::ofstream out(script);
        if (!out) {
            if (error) *error = "cannot write apply script";
            return false;
        }
        out << "@echo off\r\n"
            << "set PID=" << pid << "\r\n"
            << ":wait\r\n"
            << "tasklist /FI \"PID eq %PID%\" 2>NUL | find \"%PID%\" >NUL\r\n"
            << "if not errorlevel 1 (\r\n"
            << "  timeout /t 1 /nobreak >NUL\r\n"
            << "  goto wait\r\n"
            << ")\r\n"
            << "copy /Y \"" << (staging_bin_dir / "retcomm.exe").string() << "\" \""
            << (dest_dir / "retcomm.exe").string() << "\"\r\n"
            << "copy /Y \"" << (staging_bin_dir / hub_name.filename().string()).string() << "\" \""
            << (dest_dir / hub_name.filename().string()).string() << "\"\r\n"
            << "if exist \"" << (staging_bin_dir / "catalog").string() << "\" (\r\n"
            << "  xcopy /E /I /Y \"" << (staging_bin_dir / "catalog").string() << "\" \""
            << (dest_dir / "catalog").string() << "\\\"\r\n"
            << ")\r\n"
            << "start \"\" \"" << (dest_dir / hub_name.filename().string()).string() << "\"\r\n";
    }
    const std::string cmd = "cmd /C start \"\" /MIN \"" + script.string() + "\"";
    if (std::system(cmd.c_str()) != 0) {
        if (error) *error = "failed to launch apply script";
        return false;
    }
    return true;
}
#else
bool schedule_apply_and_restart(const fs::path& staging_bin_dir, const fs::path& dest_dir,
                                const fs::path& hub_name, std::string* error) {
    const pid_t pid = ::getpid();
    const fs::path script = staging_bin_dir.parent_path() / "apply_update.sh";
    {
        std::ofstream out(script);
        if (!out) {
            if (error) *error = "cannot write apply script";
            return false;
        }
        out << "#!/usr/bin/env bash\n"
            << "set -euo pipefail\n"
            << "pid=" << pid << "\n"
            << "staging=" << staging_bin_dir.string() << "\n"
            << "dest=" << dest_dir.string() << "\n"
            << "hub=\"" << hub_name.filename().string() << "\"\n"
            << "while kill -0 \"$pid\" 2>/dev/null; do sleep 0.2; done\n"
            << "sleep 0.3\n"
            << "mkdir -p \"$dest\"\n"
            << "install -m 755 \"$staging/retcomm\" \"$dest/retcomm\"\n"
            << "install -m 755 \"$staging/$hub\" \"$dest/$hub\"\n"
            << "if [[ -d \"$staging/catalog\" ]]; then\n"
            << "  rm -rf \"$dest/catalog\"\n"
            << "  cp -a \"$staging/catalog\" \"$dest/catalog\"\n"
            << "fi\n"
            << "exec \"$dest/$hub\"\n";
    }
    make_executable(script);
    const std::string cmd = "nohup bash " + script.string() + " >/dev/null 2>&1 &";
    if (std::system(cmd.c_str()) != 0) {
        if (error) *error = "failed to launch apply script";
        return false;
    }
    return true;
}
#endif

SelfUpdateResult fail(SelfUpdateResult r, std::string msg) {
    r.ok = false;
    r.message = std::move(msg);
    return r;
}

} // namespace

std::string retcomm_app_version() { return RETCOMM_VERSION; }

std::string retcomm_github_slug() { return RETCOMM_GITHUB_SLUG; }

std::string retcomm_installed_tag(const Paths& paths) {
    std::ifstream in(launcher_state_path(paths));
    if (in) {
        try {
            json j;
            in >> j;
            const std::string tag = j.value("tag", "");
            if (!tag.empty()) return tag;
        } catch (...) {
        }
    }
    return retcomm_app_version();
}

SelfUpdateResult self_update_retcomm(const Paths& paths, const SelfUpdateOptions& opts) {
    SelfUpdateResult result;
    result.current_tag = retcomm_installed_tag(paths);

    const std::string slug = retcomm_github_slug();
    std::string err;
    GhRelease rel;
    if (!fetch_latest_release(slug, rel, &err, opts.allow_prerelease)) {
        return fail(result, "GitHub release check failed for " + slug + ": " + err +
                                "\nPublish a Release with a host-OS zip "
                                "(retcomm + retcomm-hub) to enable Update RetComM.");
    }
    result.latest_tag = rel.tag;

    if (!opts.force &&
        normalize_tag(result.current_tag) == normalize_tag(result.latest_tag)) {
        result.ok = true;
        result.skipped = true;
        result.message = "RetComM Launcher is up to date (" + result.latest_tag + ").";
        return result;
    }

    const GhAsset* asset = pick_launcher_asset(rel);
    if (!asset) {
        return fail(result, "Release " + rel.tag + " has no downloadable asset matching this OS (" +
                                host_os_key() +
                                "). Expected a zip named like "
                                "RetComM-Launcher-*-linux-x86_64.zip containing retcomm and "
                                "retcomm-hub.");
    }
    result.asset_name = asset->name;

    ensure_dirs(paths);
    const fs::path work = paths.data_dir / "self-update";
    const fs::path download = work / "download" / asset->name;
    const fs::path staging = work / "staging";
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(download.parent_path(), ec);
    fs::create_directories(staging, ec);

    auto headers = github_http_headers();
    headers.erase(std::remove_if(headers.begin(), headers.end(),
                                 [](const auto& h) { return h.first == "Accept"; }),
                  headers.end());
    headers.emplace_back("Accept", "application/octet-stream");

    if (!http_download(asset->browser_download_url, download, &err, headers)) {
        return fail(result, "download failed: " + err);
    }
    if (!extract_archive_to(download, staging, &err)) {
        return fail(result, "extract failed: " + err);
    }

#if defined(_WIN32)
    const std::string cli_name = "retcomm.exe";
    const std::string hub_name = "retcomm-hub.exe";
#else
    const std::string cli_name = "retcomm";
    const std::string hub_name = "retcomm-hub";
#endif

    fs::path cli = find_named_file(staging, cli_name);
    fs::path hub = find_named_file(staging, hub_name);
    if (hub.empty()) {
        return fail(result, "release archive missing " + hub_name +
                                " — package retcomm + retcomm-hub in the GitHub Release asset.");
    }
    if (cli.empty()) {
        // Hub-only package is acceptable; synthesize note.
    } else {
        make_executable(cli);
    }
    make_executable(hub);

    // Flatten into staging/bin for the apply script.
    const fs::path bin_stage = work / "bin";
    fs::create_directories(bin_stage, ec);
    fs::copy_file(hub, bin_stage / hub_name, fs::copy_options::overwrite_existing, ec);
    if (!cli.empty())
        fs::copy_file(cli, bin_stage / cli_name, fs::copy_options::overwrite_existing, ec);
    const fs::path cat = find_named_file(staging, "index.json");
    if (!cat.empty() && cat.parent_path().filename() == "catalog") {
        fs::copy(cat.parent_path(), bin_stage / "catalog",
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    } else {
        const fs::path cat_dir = staging / "catalog";
        if (fs::is_directory(cat_dir, ec)) {
            fs::copy(cat_dir, bin_stage / "catalog",
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        }
    }

    fs::path dest_dir;
    const fs::path exe = current_executable_path();
    if (!exe.empty()) {
        dest_dir = exe.parent_path();
        if (!dir_is_writable(dest_dir)) dest_dir.clear();
    }
    if (dest_dir.empty()) {
        dest_dir = paths.data_dir / "bin";
        if (!dir_is_writable(dest_dir)) {
            return fail(result, "no writable install location (tried exe dir and " +
                                    dest_dir.string() + ")");
        }
    }

    if (!save_launcher_state(paths, rel.tag, asset->name)) {
        // Non-fatal — continue apply.
    }

    if (!schedule_apply_and_restart(bin_stage, dest_dir, hub_name, &err)) {
        return fail(result, err);
    }

    result.ok = true;
    result.restart_scheduled = true;
    result.message = "Updating RetComM Launcher " + result.current_tag + " → " + rel.tag +
                     "\n  asset: " + asset->name + "\n  install: " + dest_dir.string() +
                     "\nRestarting after this window closes…";
    return result;
}

} // namespace retcomm
