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
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
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
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return {};
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::path(buf), ec);
    return ec ? fs::path(buf) : p;
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && !p.empty()) return p;
    return {};
#endif
}

fs::path running_appimage_path() {
#if defined(__linux__)
    if (const char* env = std::getenv("APPIMAGE")) {
        if (env && *env) return fs::path(env);
    }
#endif
    const fs::path exe = current_executable_path();
    if (!exe.empty() && ends_with_ci(exe.string(), ".appimage")) return exe;
    return {};
}

fs::path macos_app_bundle_path() {
#if defined(__APPLE__)
    const fs::path exe = current_executable_path();
    // …/RetComM Launcher.app/Contents/MacOS/retcomm-hub
    if (exe.empty()) return {};
    const fs::path macos = exe.parent_path();
    const fs::path contents = macos.parent_path();
    const fs::path app = contents.parent_path();
    if (macos.filename() == "MacOS" && contents.filename() == "Contents" &&
        ends_with_ci(app.filename().string(), ".app"))
        return app;
#endif
    return {};
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

const char* channel_id_for(RetcommInstallChannel c) {
    switch (c) {
    case RetcommInstallChannel::LinuxAppImage:
        return "appimage";
    case RetcommInstallChannel::MacosApp:
        return "macos-app";
    case RetcommInstallChannel::WindowsInstaller:
        return "windows-installer";
    case RetcommInstallChannel::WindowsPortable:
        return "windows-portable";
    case RetcommInstallChannel::Unsupported:
    default:
        return "dev";
    }
}

std::string unsupported_hint() {
#if defined(_WIN32)
    return "Self-update needs the Windows installer (or portable) build. "
           "Install from the GitHub setup.exe, then use Update RetComM.";
#elif defined(__APPLE__)
    return "Self-update needs RetComM Launcher.app (from the DMG). "
           "Install to Applications, launch that app, then use Update RetComM.";
#else
    return "Self-update needs the Linux AppImage. "
           "Launch RetComM-Launcher-*-linux-*.AppImage, then use Update RetComM.";
#endif
}

const GhAsset* pick_launcher_asset(const GhRelease& rel, RetcommInstallChannel channel) {
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

        switch (channel) {
        case RetcommInstallChannel::LinuxAppImage:
            if (ends_with_ci(n, ".appimage")) score += 40;
            else score -= 100;
            break;
        case RetcommInstallChannel::MacosApp:
            if (ends_with_ci(n, ".dmg")) score += 40;
            else score -= 100;
            break;
        case RetcommInstallChannel::WindowsInstaller:
            if (n.find("setup") != std::string::npos && ends_with_ci(n, ".exe")) score += 40;
            else score -= 100;
            if (n.find("portable") != std::string::npos) score -= 50;
            break;
        case RetcommInstallChannel::WindowsPortable:
            if (n.find("portable") != std::string::npos && ends_with_ci(n, ".exe")) score += 40;
            else score -= 100;
            break;
        case RetcommInstallChannel::Unsupported:
        default:
            score -= 100;
            break;
        }

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
    // Require a real channel match (base OS score 50 + asset type 40).
    if (best && best_score >= 90) return best;
    return nullptr;
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

#if defined(_WIN32)
// Fire-and-forget a .bat with no console window. Avoid std::system / "cmd /C start"
// — those spawn visible (or idle minimized) CMD windows while the script waits on PID.
bool schedule_bat(const fs::path& script, std::string* error) {
    wchar_t sys_dir[MAX_PATH]{};
    const UINT n = GetSystemDirectoryW(sys_dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        if (error) *error = "GetSystemDirectoryW failed";
        return false;
    }
    const std::wstring cmd_exe = std::wstring(sys_dir) + L"\\cmd.exe";
    std::wstring cmdline = L"\"" + cmd_exe + L"\" /C \"" + script.wstring() + L"\"";
    std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    const DWORD flags = CREATE_NO_WINDOW | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
    if (!CreateProcessW(cmd_exe.c_str(), mutable_cmd.data(), nullptr, nullptr, FALSE, flags,
                        nullptr, script.parent_path().wstring().c_str(), &si, &pi)) {
        if (error)
            *error = "failed to launch apply script (CreateProcess " +
                     std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool schedule_replace_portable_and_restart(const fs::path& new_portable, const fs::path& dest_portable,
                                           std::string* error) {
    const DWORD pid = GetCurrentProcessId();
    const fs::path script = new_portable.parent_path() / "apply_portable_update.bat";
    const fs::path log_path = new_portable.parent_path() / "apply_portable_update.log";
    std::string log_js = log_path.generic_string(); // forward slashes for mshta/JS
    for (char& c : log_js) {
        if (c == '\'') c = ' ';
    }
    {
        std::ofstream out(script);
        if (!out) {
            if (error) *error = "cannot write apply script";
            return false;
        }
        // Harden replace: retry + rename dance; only invalidate/relaunch if the
        // new bytes land at dest. A failed copy used to delete version.txt and
        // restart the old stub (same version forever).
        out << "@echo off\r\n"
            << "setlocal EnableExtensions EnableDelayedExpansion\r\n"
            << "set PID=" << pid << "\r\n"
            << "set \"NEW=" << new_portable.string() << "\"\r\n"
            << "set \"DEST=" << dest_portable.string() << "\"\r\n"
            << "set \"OLD=!DEST!.retcomm-old\"\r\n"
            << "set \"LOG=" << log_path.string() << "\"\r\n"
            << "echo RetComM portable update > \"!LOG!\"\r\n"
            << "echo NEW=!NEW!>> \"!LOG!\"\r\n"
            << "echo DEST=!DEST!>> \"!LOG!\"\r\n"
            << "echo Waiting for PID !PID!>> \"!LOG!\"\r\n"
            << "set W=0\r\n"
            << ":wait\r\n"
            << "tasklist /FI \"PID eq !PID!\" 2>NUL | findstr /I /C:\"No tasks\" >NUL\r\n"
            << "if errorlevel 1 (\r\n"
            << "  set /a W+=1\r\n"
            << "  if !W! GTR 120 (\r\n"
            << "    echo Timed out waiting for PID !PID!>> \"!LOG!\"\r\n"
            << "    goto fail\r\n"
            << "  )\r\n"
            << "  ping -n 2 127.0.0.1 >NUL\r\n"
            << "  goto wait\r\n"
            << ")\r\n"
            << "if not exist \"!NEW!\" (\r\n"
            << "  echo Staged portable missing>> \"!LOG!\"\r\n"
            << "  goto fail\r\n"
            << ")\r\n"
            << "del /F /Q \"!OLD!\" 2>NUL\r\n"
            << "set TRIES=0\r\n"
            << ":retry\r\n"
            << "set /a TRIES+=1\r\n"
            << "if !TRIES! GTR 40 (\r\n"
            << "  echo Replace retries exhausted>> \"!LOG!\"\r\n"
            << "  goto fail\r\n"
            << ")\r\n"
            << "echo Attempt !TRIES!>> \"!LOG!\"\r\n"
            << "if exist \"!DEST!\" (\r\n"
            << "  move /Y \"!DEST!\" \"!OLD!\" >> \"!LOG!\" 2>&1\r\n"
            << "  if errorlevel 1 (\r\n"
            << "    ping -n 2 127.0.0.1 >NUL\r\n"
            << "    goto retry\r\n"
            << "  )\r\n"
            << ")\r\n"
            << "copy /Y \"!NEW!\" \"!DEST!\" >> \"!LOG!\" 2>&1\r\n"
            << "if errorlevel 1 (\r\n"
            << "  if exist \"!OLD!\" move /Y \"!OLD!\" \"!DEST!\" >> \"!LOG!\" 2>&1\r\n"
            << "  ping -n 2 127.0.0.1 >NUL\r\n"
            << "  goto retry\r\n"
            << ")\r\n"
            << "for %%A in (\"!NEW!\") do set NEWSIZE=%%~zA\r\n"
            << "for %%A in (\"!DEST!\") do set DESTSIZE=%%~zA\r\n"
            << "if not \"!NEWSIZE!\"==\"!DESTSIZE!\" (\r\n"
            << "  echo Size mismatch NEW=!NEWSIZE! DEST=!DESTSIZE!>> \"!LOG!\"\r\n"
            << "  del /F /Q \"!DEST!\" 2>NUL\r\n"
            << "  if exist \"!OLD!\" move /Y \"!OLD!\" \"!DEST!\" >> \"!LOG!\" 2>&1\r\n"
            << "  ping -n 2 127.0.0.1 >NUL\r\n"
            << "  goto retry\r\n"
            << ")\r\n"
            << "del /F /Q \"!OLD!\" 2>NUL\r\n"
            << "echo Replace OK size=!DESTSIZE!>> \"!LOG!\"\r\n"
            // Invalidate extracted runtime so the new stub re-unpacks.
            << "del /Q \"%LOCALAPPDATA%\\retcomm\\portable\\version.txt\" 2>NUL\r\n"
            << "start \"\" \"!DEST!\"\r\n"
            << "exit /b 0\r\n"
            << ":fail\r\n"
            << "echo FAILED>> \"!LOG!\"\r\n"
            << "if exist \"!OLD!\" if not exist \"!DEST!\" move /Y \"!OLD!\" \"!DEST!\" >> \"!LOG!\" 2>&1\r\n"
            << "mshta \"javascript:alert('RetComM portable update failed to replace the "
               "exe.\\n\\nMove it out of Downloads/OneDrive if needed, ensure the folder "
               "is writable, then try Update again.\\n\\nLog:\\n"
            << log_js << "');close()\"\r\n"
            << "exit /b 1\r\n";
    }
    return schedule_bat(script, error);
}

bool schedule_run_setup_and_restart(const fs::path& setup_exe, const fs::path& install_dir,
                                    std::string* error) {
    const DWORD pid = GetCurrentProcessId();
    const fs::path script = setup_exe.parent_path() / "apply_setup_update.bat";
    {
        std::ofstream out(script);
        if (!out) {
            if (error) *error = "cannot write apply script";
            return false;
        }
        // Wait for hub exit (with timeout). Do not use /CLOSEAPPLICATIONS — the hub
        // exits on its own; forcing closes mid-shutdown caused hangs/failed applies.
        out << "@echo off\r\n"
            << "setlocal EnableExtensions EnableDelayedExpansion\r\n"
            << "set PID=" << pid << "\r\n"
            << "set W=0\r\n"
            << ":wait\r\n"
            << "tasklist /FI \"PID eq !PID!\" 2>NUL | findstr /I /C:\"No tasks\" >NUL\r\n"
            << "if errorlevel 1 (\r\n"
            << "  set /a W+=1\r\n"
            << "  if !W! GTR 120 exit /b 1\r\n"
            << "  ping -n 2 127.0.0.1 >NUL\r\n"
            << "  goto wait\r\n"
            << ")\r\n"
            << "start /wait \"\" \"" << setup_exe.string()
            << "\" /VERYSILENT /NORESTART /SUPPRESSMSGBOXES /DIR=\""
            << install_dir.string() << "\"\r\n"
            << "start \"\" \"" << (install_dir / "retcomm-hub.exe").string() << "\"\r\n";
    }
    return schedule_bat(script, error);
}
#else
bool schedule_shell(const fs::path& script, std::string* error) {
    make_executable(script);
    const std::string cmd = "nohup bash " + script.string() + " >/dev/null 2>&1 &";
    if (std::system(cmd.c_str()) != 0) {
        if (error) *error = "failed to launch apply script";
        return false;
    }
    return true;
}

bool schedule_replace_appimage_and_restart(const fs::path& new_appimage, const fs::path& dest_appimage,
                                           std::string* error) {
    const pid_t pid = ::getpid();
    const fs::path script = new_appimage.parent_path() / "apply_appimage_update.sh";
    {
        std::ofstream out(script);
        if (!out) {
            if (error) *error = "cannot write apply script";
            return false;
        }
        out << "#!/usr/bin/env bash\n"
            << "set -euo pipefail\n"
            << "pid=" << pid << "\n"
            << "src=" << new_appimage.string() << "\n"
            << "dest=" << dest_appimage.string() << "\n"
            << "while kill -0 \"$pid\" 2>/dev/null; do sleep 0.2; done\n"
            << "sleep 0.3\n"
            << "chmod +x \"$src\"\n"
            << "tmp=\"${dest}.new.$$\"\n"
            << "cp -f \"$src\" \"$tmp\"\n"
            << "mv -f \"$tmp\" \"$dest\"\n"
            << "chmod +x \"$dest\"\n"
            << "exec \"$dest\"\n";
    }
    return schedule_shell(script, error);
}

bool schedule_dmg_replace_and_restart(const fs::path& dmg, const fs::path& dest_app,
                                      std::string* error) {
    const pid_t pid = ::getpid();
    const fs::path work = dmg.parent_path();
    const fs::path script = work / "apply_dmg_update.sh";
    {
        std::ofstream out(script);
        if (!out) {
            if (error) *error = "cannot write apply script";
            return false;
        }
        out << "#!/usr/bin/env bash\n"
            << "set -euo pipefail\n"
            << "pid=" << pid << "\n"
            << "dmg=" << dmg.string() << "\n"
            << "dest=" << dest_app.string() << "\n"
            << "mp=" << (work / "dmg-mount").string() << "\n"
            << "while kill -0 \"$pid\" 2>/dev/null; do sleep 0.2; done\n"
            << "sleep 0.3\n"
            << "rm -rf \"$mp\"\n"
            << "mkdir -p \"$mp\"\n"
            << "hdiutil attach -nobrowse -readonly -mountpoint \"$mp\" \"$dmg\" >/dev/null\n"
            << "app=$(find \"$mp\" -maxdepth 1 -name '*.app' -print -quit)\n"
            << "if [[ -z \"$app\" || ! -d \"$app\" ]]; then\n"
            << "  hdiutil detach \"$mp\" >/dev/null 2>&1 || true\n"
            << "  echo 'DMG missing .app' >&2\n"
            << "  exit 1\n"
            << "fi\n"
            << "mkdir -p \"$(dirname \"$dest\")\"\n"
            << "rm -rf \"$dest\"\n"
            << "ditto \"$app\" \"$dest\"\n"
            << "hdiutil detach \"$mp\" >/dev/null 2>&1 || hdiutil detach -force \"$mp\" >/dev/null 2>&1 || true\n"
            << "rm -rf \"$mp\"\n"
            << "open \"$dest\"\n";
    }
    return schedule_shell(script, error);
}
#endif

SelfUpdateResult fail(SelfUpdateResult r, std::string msg) {
    r.ok = false;
    r.message = std::move(msg);
    return r;
}

bool schedule_retcomm_relaunch_impl(std::string* error) {
    fs::path launch = running_appimage_path();
#if defined(__APPLE__)
    if (launch.empty()) {
        const fs::path app = macos_app_bundle_path();
        if (!app.empty()) launch = app / "Contents" / "MacOS" / "retcomm-hub";
    }
#endif
    if (launch.empty()) launch = current_executable_path();
    if (launch.empty()) {
        if (error) *error = "cannot resolve RetComM executable path for relaunch";
        return false;
    }

    // Prefer a writable temp dir — AppImage mount points are often read-only.
    fs::path script_dir;
#if defined(_WIN32)
    char tmp[MAX_PATH]{};
    const DWORD n = GetTempPathA(MAX_PATH, tmp);
    if (n > 0 && n < MAX_PATH) script_dir = fs::path(tmp);
#else
    if (const char* t = std::getenv("TMPDIR"); t && *t) script_dir = t;
    else if (const char* t = std::getenv("XDG_RUNTIME_DIR"); t && *t) script_dir = t;
    else script_dir = "/tmp";
#endif
    if (script_dir.empty()) script_dir = launch.parent_path();

#if defined(_WIN32)
    const DWORD pid = GetCurrentProcessId();
    const fs::path script = script_dir / "retcomm_relaunch.bat";
    {
        std::ofstream out(script);
        if (!out) {
            if (error) *error = "cannot write relaunch script";
            return false;
        }
        out << "@echo off\r\n"
            << "setlocal EnableExtensions EnableDelayedExpansion\r\n"
            << "set PID=" << pid << "\r\n"
            << "set W=0\r\n"
            << ":wait\r\n"
            << "tasklist /FI \"PID eq !PID!\" 2>NUL | findstr /I /C:\"No tasks\" >NUL\r\n"
            << "if errorlevel 1 (\r\n"
            << "  set /a W+=1\r\n"
            << "  if !W! GTR 120 exit /b 1\r\n"
            << "  ping -n 2 127.0.0.1 >NUL\r\n"
            << "  goto wait\r\n"
            << ")\r\n"
            << "start \"\" \"" << launch.string() << "\"\r\n";
    }
    return schedule_bat(script, error);
#else
    const pid_t pid = ::getpid();
    const fs::path script = script_dir / ("retcomm_relaunch_" + std::to_string(pid) + ".sh");
    {
        std::ofstream out(script);
        if (!out) {
            if (error) *error = "cannot write relaunch script";
            return false;
        }
        out << "#!/usr/bin/env bash\n"
            << "set -euo pipefail\n"
            << "pid=" << pid << "\n"
            << "dest=" << launch.string() << "\n"
            << "self=\"$0\"\n"
            << "while kill -0 \"$pid\" 2>/dev/null; do sleep 0.2; done\n"
            << "sleep 0.3\n"
            << "chmod +x \"$dest\" 2>/dev/null || true\n"
            << "rm -f \"$self\"\n"
            << "exec \"$dest\"\n";
    }
    return schedule_shell(script, error);
#endif
}

} // namespace

bool schedule_retcomm_relaunch(std::string* error) {
    return schedule_retcomm_relaunch_impl(error);
}

std::string retcomm_app_version() { return RETCOMM_VERSION; }

std::string retcomm_github_slug() { return RETCOMM_GITHUB_SLUG; }

std::string retcomm_installed_tag(const Paths& /*paths*/) {
    // Binary compile version is authoritative; launcher.json is apply metadata only.
    return retcomm_app_version();
}

RetcommInstallInfo retcomm_install_info() {
    RetcommInstallInfo info;
    info.channel = RetcommInstallChannel::Unsupported;
    info.channel_id = "dev";
    info.hint = unsupported_hint();
    info.self_update_supported = false;

#if defined(_WIN32)
    fs::path portable_exe;
    std::string channel_env;
    if (const char* env = std::getenv("RETCOMM_INSTALL_CHANNEL")) channel_env = to_lower(env);
    if (const char* pe = std::getenv("RETCOMM_PORTABLE_EXE")) {
        if (*pe) portable_exe = fs::path(pe);
    }
    const fs::path exe = current_executable_path();
    std::string channel_file;
    if (!exe.empty()) {
        const json ch = read_json_file(exe.parent_path() / "channel.json");
        if (ch.is_object()) {
            channel_file = to_lower(ch.value("channel", ""));
            const std::string pe = ch.value("portable_exe", "");
            if (!pe.empty() && portable_exe.empty()) portable_exe = pe;
        }
    }
    const bool want_portable =
        channel_env == "portable" || channel_file == "portable" || !portable_exe.empty();
    const bool want_installer =
        channel_env == "installer" || channel_file == "installer" || channel_file == "zip";

    if (want_portable) {
        std::error_code ec;
        info.channel = RetcommInstallChannel::WindowsPortable;
        info.channel_id = channel_id_for(info.channel);
        info.path = portable_exe.empty() ? exe : portable_exe;
        if (!portable_exe.empty() && fs::is_regular_file(portable_exe, ec)) {
            info.self_update_supported = true;
            info.hint.clear();
        } else {
            info.hint = "Portable channel detected but portable_exe is missing. "
                        "Relaunch from the windows-portable.exe.";
        }
        return info;
    }
    if (want_installer && !exe.empty()) {
        info.channel = RetcommInstallChannel::WindowsInstaller;
        info.channel_id = channel_id_for(info.channel);
        info.path = exe.parent_path();
        if (dir_is_writable(info.path)) {
            info.self_update_supported = true;
            info.hint.clear();
        } else {
            info.hint = "Installer directory is not writable: " + info.path.string();
        }
        return info;
    }
    return info;
#elif defined(__APPLE__)
    const fs::path app = macos_app_bundle_path();
    if (!app.empty()) {
        info.channel = RetcommInstallChannel::MacosApp;
        info.channel_id = channel_id_for(info.channel);
        info.path = app;
        if (dir_is_writable(app.parent_path())) {
            info.self_update_supported = true;
            info.hint.clear();
        } else {
            info.hint = "Cannot write app bundle parent: " + app.parent_path().string();
        }
        return info;
    }
    return info;
#else
    const fs::path appimage = running_appimage_path();
    if (!appimage.empty()) {
        info.channel = RetcommInstallChannel::LinuxAppImage;
        info.channel_id = channel_id_for(info.channel);
        info.path = appimage;
        if (dir_is_writable(appimage.parent_path())) {
            info.self_update_supported = true;
            info.hint.clear();
        } else {
            info.hint = "AppImage directory is not writable: " + appimage.parent_path().string();
        }
        return info;
    }
    return info;
#endif
}

SelfUpdateCheckInfo check_retcomm_update(const Paths& /*paths*/, const SelfUpdateOptions& opts) {
    SelfUpdateCheckInfo info;
    info.current_tag = retcomm_app_version();
    const RetcommInstallInfo install = retcomm_install_info();
    info.supported = install.self_update_supported;
    if (!install.self_update_supported) {
        info.message = install.hint.empty() ? unsupported_hint() : install.hint;
        return info;
    }
    const std::string slug = retcomm_github_slug();
    std::string err;
    GhRelease rel;
    if (!fetch_latest_release(slug, rel, &err, opts.allow_prerelease)) {
        info.message = "GitHub release check failed for " + slug + ": " + err;
        return info;
    }
    info.ok = true;
    info.latest_tag = rel.tag;
    info.update_available =
        normalize_tag(info.current_tag) != normalize_tag(info.latest_tag);
    if (info.update_available) {
        info.message = "RetComM Launcher update available: " + info.current_tag + " → " +
                       info.latest_tag;
    } else {
        info.message = "RetComM Launcher is up to date (" + info.latest_tag + ").";
    }
    return info;
}

SelfUpdateResult self_update_retcomm(const Paths& paths, const SelfUpdateOptions& opts) {
    SelfUpdateResult result;
    result.current_tag = retcomm_app_version();

    const RetcommInstallInfo install = retcomm_install_info();
    if (!install.self_update_supported) {
        return fail(result, install.hint.empty() ? unsupported_hint() : install.hint);
    }
    const std::string channel_name = install.channel_id;

    const std::string slug = retcomm_github_slug();
    std::string err;
    GhRelease rel;
    if (!fetch_latest_release(slug, rel, &err, opts.allow_prerelease)) {
        return fail(result, "GitHub release check failed for " + slug + ": " + err +
                                "\nPublish a Release with host installers "
                                "(AppImage / DMG / Windows setup.exe).");
    }
    result.latest_tag = rel.tag;

    if (!opts.force &&
        normalize_tag(result.current_tag) == normalize_tag(result.latest_tag)) {
        result.ok = true;
        result.skipped = true;
        result.message = "RetComM Launcher is up to date (" + result.latest_tag + ").";
        return result;
    }

    const GhAsset* asset = pick_launcher_asset(rel, install.channel);
    if (!asset) {
        return fail(result, "Release " + rel.tag + " has no asset for channel " + channel_name +
                                " (" + host_os_key() + "). Expected AppImage, macOS DMG, or "
                                "windows-*-setup.exe / windows-portable.exe.");
    }
    result.asset_name = asset->name;
    const std::string asset_lower = to_lower(asset->name);

    ensure_dirs(paths);
    const fs::path work = paths.data_dir / "self-update";
    const fs::path download = work / "download" / asset->name;
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(download.parent_path(), ec);

    auto headers = github_http_headers();
    headers.erase(std::remove_if(headers.begin(), headers.end(),
                                 [](const auto& h) { return h.first == "Accept"; }),
                  headers.end());
    headers.emplace_back("Accept", "application/octet-stream");

    if (!http_download(asset->browser_download_url, download, &err, headers)) {
        return fail(result, "download failed: " + err);
    }

#if defined(_WIN32)
    if (install.channel == RetcommInstallChannel::WindowsPortable) {
        fs::path dest_portable = install.path;
        if (dest_portable.empty() || !fs::is_regular_file(dest_portable, ec)) {
            return fail(result,
                        "portable channel detected but portable_exe is missing. "
                        "Relaunch from the windows-portable.exe.");
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
                         "\n  channel: " + channel_name + "\n  asset: " + asset->name +
                         "\n  install: " + dest_portable.string() +
                         "\nRestarting after this window closes…";
        return result;
    }

    if (install.channel == RetcommInstallChannel::WindowsInstaller) {
        if (asset_lower.find("setup") == std::string::npos || !ends_with_ci(asset_lower, ".exe")) {
            return fail(result, "expected a windows-*-setup.exe asset, got: " + asset->name);
        }
        fs::path dest_dir = install.path;
        if (dest_dir.empty() || !dir_is_writable(dest_dir)) {
            return fail(result, "installer update needs a writable install directory.");
        }
        const fs::path staged_setup = work / "bin" / asset->name;
        fs::create_directories(staged_setup.parent_path(), ec);
        fs::copy_file(download, staged_setup, fs::copy_options::overwrite_existing, ec);
        if (ec) return fail(result, "staging setup.exe failed: " + ec.message());

        save_launcher_state(paths, rel.tag, asset->name, channel_name);
        if (!schedule_run_setup_and_restart(staged_setup, dest_dir, &err)) {
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
#endif

#if defined(__linux__)
    if (install.channel == RetcommInstallChannel::LinuxAppImage) {
        if (!ends_with_ci(asset_lower, ".appimage")) {
            return fail(result, "expected a Linux AppImage asset, got: " + asset->name);
        }
        const fs::path dest = install.path.empty() ? running_appimage_path() : install.path;
        if (dest.empty()) {
            return fail(result, unsupported_hint());
        }
        if (!dir_is_writable(dest.parent_path())) {
            return fail(result, "AppImage directory is not writable: " + dest.parent_path().string());
        }
        make_executable(download);
        save_launcher_state(paths, rel.tag, asset->name, channel_name);
        if (!schedule_replace_appimage_and_restart(download, dest, &err)) {
            return fail(result, err);
        }
        result.ok = true;
        result.restart_scheduled = true;
        result.message = "Updating RetComM Launcher " + result.current_tag + " → " + rel.tag +
                         "\n  channel: " + channel_name + "\n  asset: " + asset->name +
                         "\n  install: " + dest.string() +
                         "\nRestarting after this window closes…";
        return result;
    }
#endif

#if defined(__APPLE__)
    if (install.channel == RetcommInstallChannel::MacosApp) {
        if (!ends_with_ci(asset_lower, ".dmg")) {
            return fail(result, "expected a macOS DMG asset, got: " + asset->name);
        }
        const fs::path dest_app = install.path.empty() ? macos_app_bundle_path() : install.path;
        if (dest_app.empty()) {
            return fail(result, unsupported_hint());
        }
        if (!dir_is_writable(dest_app.parent_path())) {
            return fail(result, "cannot write app bundle parent: " + dest_app.parent_path().string());
        }
        save_launcher_state(paths, rel.tag, asset->name, channel_name);
        if (!schedule_dmg_replace_and_restart(download, dest_app, &err)) {
            return fail(result, err);
        }
        result.ok = true;
        result.restart_scheduled = true;
        result.message = "Updating RetComM Launcher " + result.current_tag + " → " + rel.tag +
                         "\n  channel: " + channel_name + "\n  asset: " + asset->name +
                         "\n  install: " + dest_app.string() +
                         "\nRestarting after this window closes…";
        return result;
    }
#endif

    return fail(result, "Unexpected install channel for asset " + asset->name + " (" +
                            channel_name + "). " + unsupported_hint());
}

} // namespace retcomm
