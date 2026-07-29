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

enum class InstallChannel {
    Zip,       // linux/macos zip, or Windows folder/installer layout
    Portable,  // Windows single-file portable stub
};

struct ChannelInfo {
    InstallChannel channel = InstallChannel::Zip;
    fs::path portable_exe; // set when channel == Portable
};

json read_json_file(const fs::path& p) {
    std::ifstream in(p);
    if (!in) return {};
    try {
        json j;
        in >> j;
        return j;
    } catch (...) {
        return {};
    }
}

ChannelInfo detect_install_channel() {
    ChannelInfo info;
#if defined(_WIN32)
    if (const char* env = std::getenv("RETCOMM_INSTALL_CHANNEL")) {
        const std::string c = to_lower(env);
        if (c == "portable") info.channel = InstallChannel::Portable;
        else if (c == "installer" || c == "zip") info.channel = InstallChannel::Zip;
    }
    if (const char* pe = std::getenv("RETCOMM_PORTABLE_EXE")) {
        if (*pe) {
            info.portable_exe = fs::path(pe);
            info.channel = InstallChannel::Portable;
        }
    }
    const fs::path exe = current_executable_path();
    if (!exe.empty()) {
        const json ch = read_json_file(exe.parent_path() / "channel.json");
        if (ch.is_object()) {
            const std::string c = to_lower(ch.value("channel", ""));
            if (c == "portable") {
                info.channel = InstallChannel::Portable;
                const std::string pe = ch.value("portable_exe", "");
                if (!pe.empty() && info.portable_exe.empty()) info.portable_exe = pe;
            } else if (c == "installer" || c == "zip") {
                if (info.channel != InstallChannel::Portable) info.channel = InstallChannel::Zip;
            }
        }
    }
#else
    (void)info;
#endif
    return info;
}

const GhAsset* pick_launcher_asset(const GhRelease& rel, InstallChannel channel) {
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
        else continue;

#if defined(_WIN32)
        if (channel == InstallChannel::Portable) {
            // Prefer the single-file portable artifact.
            if (n.find("portable") != std::string::npos && ends_with_ci(n, ".exe")) score += 40;
            else if (n.find("setup") != std::string::npos) score -= 30;
            else if (ends_with_ci(n, ".zip")) score += 5; // weak fallback
            else score -= 20;
        } else {
            // Installer / folder installs update from the flat windows zip.
            if (ends_with_ci(n, ".zip") && n.find("portable") == std::string::npos) score += 40;
            if (n.find("setup") != std::string::npos) score -= 40;
            if (n.find("portable") != std::string::npos) score -= 30;
        }
#else
        (void)channel;
        // Prefer extractable archives for in-place self-update (AppImage / DMG
        // are published for users but not applied by the updater script).
        if (ends_with_ci(n, ".zip")) score += 20;
        if (ends_with_ci(n, ".tar.gz") || ends_with_ci(n, ".tgz")) score += 12;
        if (ends_with_ci(n, ".appimage")) score += 4;
        if (ends_with_ci(n, ".dmg")) score -= 40;
#endif
        if (n.find("retcomm") != std::string::npos) score += 5;
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
    if ((!best || best_score < 50) && rel.assets.size() == 1) return &rel.assets.front();
    if (best && best_score >= 50) return best;
    return best_score > 0 ? best : nullptr;
}

bool save_launcher_state(const Paths& paths, const std::string& tag, const std::string& asset,
                         const std::string& channel) {
    std::error_code ec;
    fs::create_directories(paths.data_dir, ec);
    json j = {{"schema_version", 1},
              {"github", retcomm_github_slug()},
              {"tag", tag},
              {"asset_name", asset},
              {"channel", channel},
              {"app_version", retcomm_app_version()}};
    std::ofstream out(launcher_state_path(paths));
    if (!out) return false;
    out << j.dump(2) << "\n";
    return static_cast<bool>(out);
}

bool copy_tree_files(const fs::path& src_dir, const fs::path& dest_dir, std::string* error) {
    std::error_code ec;
    fs::create_directories(dest_dir, ec);
    for (auto it = fs::directory_iterator(src_dir, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const fs::path dest = dest_dir / it->path().filename();
        fs::copy_file(it->path(), dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            if (error) *error = "copy failed: " + it->path().string() + ": " + ec.message();
            return false;
        }
        make_executable(dest);
    }
    return true;
}

#if defined(_WIN32)
bool schedule_bat(const fs::path& script, std::string* error) {
    const std::string cmd = "cmd /C start \"\" /MIN \"" + script.string() + "\"";
    if (std::system(cmd.c_str()) != 0) {
        if (error) *error = "failed to launch apply script";
        return false;
    }
    return true;
}

bool schedule_apply_dir_and_restart(const fs::path& staging_bin_dir, const fs::path& dest_dir,
                                    const std::string& hub_name, std::string* error) {
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
            << "xcopy /Y /Q \"" << staging_bin_dir.string() << "\\*\" \"" << dest_dir.string()
            << "\\\" >NUL\r\n"
            << "start \"\" \"" << (dest_dir / hub_name).string() << "\"\r\n";
    }
    return schedule_bat(script, error);
}

bool schedule_replace_portable_and_restart(const fs::path& new_portable, const fs::path& dest_portable,
                                           std::string* error) {
    const DWORD pid = GetCurrentProcessId();
    const fs::path script = new_portable.parent_path() / "apply_portable_update.bat";
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
            << "copy /Y \"" << new_portable.string() << "\" \"" << dest_portable.string()
            << "\"\r\n"
            // Invalidate extracted runtime so the new stub re-unpacks.
            << "del /Q \"%LOCALAPPDATA%\\retcomm\\portable\\version.txt\" 2>NUL\r\n"
            << "start \"\" \"" << dest_portable.string() << "\"\r\n";
    }
    return schedule_bat(script, error);
}
#else
bool schedule_apply_dir_and_restart(const fs::path& staging_bin_dir, const fs::path& dest_dir,
                                    const std::string& hub_name, std::string* error) {
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
            << "hub=\"" << hub_name << "\"\n"
            << "while kill -0 \"$pid\" 2>/dev/null; do sleep 0.2; done\n"
            << "sleep 0.3\n"
            << "mkdir -p \"$dest\"\n"
            << "cp -f \"$staging\"/* \"$dest\"/ 2>/dev/null || true\n"
            << "install -m 755 \"$staging/retcomm\" \"$dest/retcomm\" 2>/dev/null || true\n"
            << "install -m 755 \"$staging/$hub\" \"$dest/$hub\"\n"
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
    const ChannelInfo channel = detect_install_channel();
    const std::string channel_name =
        channel.channel == InstallChannel::Portable ? "portable" : "installer";

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

    const GhAsset* asset = pick_launcher_asset(rel, channel.channel);
    if (!asset) {
        return fail(result, "Release " + rel.tag + " has no downloadable asset matching this OS (" +
                                host_os_key() + ", channel=" + channel_name +
                                "). Expected a zip / windows-portable.exe containing retcomm and "
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

#if defined(_WIN32)
    const std::string cli_name = "retcomm.exe";
    const std::string hub_name = "retcomm-hub.exe";
#else
    const std::string cli_name = "retcomm";
    const std::string hub_name = "retcomm-hub";
#endif

#if defined(_WIN32)
    if (channel.channel == InstallChannel::Portable) {
        fs::path dest_portable = channel.portable_exe;
        if (dest_portable.empty() || !fs::is_regular_file(dest_portable, ec)) {
            return fail(result,
                        "portable channel detected but RETCOMM_PORTABLE_EXE / channel.json "
                        "portable_exe is missing. Relaunch from the windows-portable.exe.");
        }
        if (!dir_is_writable(dest_portable.parent_path())) {
            return fail(result, "portable exe directory is not writable: " +
                                    dest_portable.parent_path().string());
        }
        const fs::path staged_portable = work / "bin" / dest_portable.filename();
        fs::create_directories(staged_portable.parent_path(), ec);
        fs::copy_file(download, staged_portable, fs::copy_options::overwrite_existing, ec);
        if (ec) return fail(result, "staging portable exe failed: " + ec.message());

        save_launcher_state(paths, rel.tag, asset->name, channel_name);
        if (!schedule_replace_portable_and_restart(staged_portable, dest_portable, &err)) {
            return fail(result, err);
        }
        result.ok = true;
        result.restart_scheduled = true;
        result.message = "Updating RetComM Launcher " + result.current_tag + " → " + rel.tag +
                         "\n  channel: portable\n  asset: " + asset->name +
                         "\n  install: " + dest_portable.string() +
                         "\nRestarting after this window closes…";
        return result;
    }
#endif

    if (!extract_archive_to(download, staging, &err)) {
        return fail(result, "extract failed: " + err);
    }

    fs::path cli = find_named_file(staging, cli_name);
    fs::path hub = find_named_file(staging, hub_name);
    if (hub.empty()) {
        return fail(result, "release archive missing " + hub_name +
                                " — package retcomm + retcomm-hub in the GitHub Release asset.");
    }
    if (!cli.empty()) make_executable(cli);
    make_executable(hub);

    // Flatten sibling runtime files (exes + DLLs + channel.json) for the apply script.
    const fs::path bin_stage = work / "bin";
    fs::create_directories(bin_stage, ec);
    if (!copy_tree_files(hub.parent_path(), bin_stage, &err)) return fail(result, err);

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

    save_launcher_state(paths, rel.tag, asset->name, channel_name);

    if (!schedule_apply_dir_and_restart(bin_stage, dest_dir, hub_name, &err)) {
        return fail(result, err);
    }

    result.ok = true;
    result.restart_scheduled = true;
    result.message = "Updating RetComM Launcher " + result.current_tag + " → " + rel.tag +
                     "\n  channel: " + channel_name + "\n  asset: " + asset->name +
                     "\n  install: " + dest_dir.string() +
                     "\nRestarting after this window closes…";
    return result;
}

} // namespace retcomm
