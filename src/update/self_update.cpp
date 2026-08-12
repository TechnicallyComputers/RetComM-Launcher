#include "retcomm/self_update.hpp"

#include "retcomm/http.hpp"
#include "retcomm/install.hpp"
#include "retcomm/paths.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
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
    DWORD cap = MAX_PATH;
    std::wstring buf(cap, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), cap);
        if (n == 0) return {};
        if (n < cap) {
            buf.resize(n);
            return fs::path(buf);
        }
        if (cap >= 32768) return {};
        cap *= 2;
        buf.assign(cap, L'\0');
    }
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
           "Launch RetComM-Launcher-linux-*.AppImage, then use Update RetComM.";
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
            /* Prefer release zip (friendly exe inside); accept legacy bare .exe. */
            if (n.find("portable") != std::string::npos && ends_with_ci(n, ".zip"))
                score += 45;
            else if (n.find("portable") != std::string::npos && ends_with_ci(n, ".exe"))
                score += 40;
            else
                score -= 100;
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
#ifndef CREATE_BREAKAWAY_FROM_JOB
#define CREATE_BREAKAWAY_FROM_JOB 0x01000000
#endif

std::wstring win_getenv_w(const wchar_t* name) {
    const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return {};
    std::wstring buf(n, L'\0');
    const DWORD got = GetEnvironmentVariableW(name, buf.data(), n);
    if (got == 0 || got >= n) return {};
    buf.resize(got);
    return buf;
}

std::wstring utf8_to_wstring(const std::string& u8) {
    if (u8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, u8.data(), static_cast<int>(u8.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.data(), static_cast<int>(u8.size()), out.data(), n);
    return out;
}

std::string wstring_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

fs::path path_from_utf8(const std::string& u8) {
    const std::wstring w = utf8_to_wstring(u8);
    return w.empty() ? fs::path(u8) : fs::path(w);
}

// PowerShell single-quoted literal (double embedded quotes).
std::wstring ps_single_quote(const fs::path& p) {
    std::wstring out = L"'";
    for (wchar_t c : p.wstring()) {
        if (c == L'\'') out += L"''";
        else out += c;
    }
    out += L"'";
    return out;
}

std::wstring find_powershell_exe() {
    wchar_t sys_dir[MAX_PATH]{};
    if (GetSystemDirectoryW(sys_dir, MAX_PATH) == 0) return L"powershell.exe";
    // System32\WindowsPowerShell\v1.0\powershell.exe
    std::wstring p = std::wstring(sys_dir) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    return L"powershell.exe";
}

// Fire-and-forget hidden powershell -File <script> <args…>.
// Paths must be passed as -Arg '…' (ps_single_quote) — never embed in .bat.
bool schedule_powershell(const fs::path& script, const std::wstring& extra_args, std::string* error) {
    const std::wstring ps = find_powershell_exe();
    std::wstring cmdline = L"\"" + ps +
                           L"\" -NoProfile -NonInteractive -WindowStyle Hidden "
                           L"-ExecutionPolicy Bypass -File \"" +
                           script.wstring() + L"\"";
    if (!extra_args.empty()) {
        cmdline += L" ";
        cmdline += extra_args;
    }
    std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    const std::wstring cwd = script.parent_path().wstring();
    auto try_spawn = [&](DWORD flags) -> bool {
        ZeroMemory(&pi, sizeof(pi));
        return CreateProcessW(ps.c_str(), mutable_cmd.data(), nullptr, nullptr, FALSE, flags,
                              nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi) != 0;
    };
    DWORD flags = CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB;
    if (!try_spawn(flags)) {
        flags &= ~static_cast<DWORD>(CREATE_BREAKAWAY_FROM_JOB);
        if (!try_spawn(flags)) {
            if (error)
                *error = "failed to launch apply script (CreateProcess " +
                         std::to_string(GetLastError()) + ")";
            return false;
        }
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool write_text_file(const fs::path& path, const std::string& utf8_body, std::string* error) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot write " + path.string();
        return false;
    }
    // UTF-8 BOM so Windows PowerShell 5.1 parses non-ASCII in -File scripts reliably.
    const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    out.write(reinterpret_cast<const char*>(bom), 3);
    out.write(utf8_body.data(), static_cast<std::streamsize>(utf8_body.size()));
    return static_cast<bool>(out);
}

/* Trailer: uint64 LE payload size + "RCM1" (see win_portable_main.cpp). */
bool looks_like_retcomm_portable_stub(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return false;
    const auto sz = fs::file_size(p, ec);
    if (ec || sz < 12) return false;
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    in.seekg(static_cast<std::streamoff>(sz - 12));
    unsigned char buf[12]{};
    in.read(reinterpret_cast<char*>(buf), 12);
    if (in.gcount() != 12) return false;
    return buf[8] == 'R' && buf[9] == 'C' && buf[10] == 'M' && buf[11] == '1';
}

bool schedule_replace_portable_and_restart(const fs::path& new_portable, const fs::path& dest_portable,
                                           std::string* error) {
    const DWORD pid = GetCurrentProcessId();
    const fs::path script = new_portable.parent_path() / "apply_portable_update.ps1";
    const fs::path log_path = new_portable.parent_path() / "apply_portable_update.log";
    // Paths are CLI args (Unicode-safe). Stub is not locked by the hub process, so a
    // wait timeout still attempts replace.
    const char* body = R"ps1(
param(
  [Parameter(Mandatory=$true)][int]$WaitPid,
  [Parameter(Mandatory=$true)][string]$New,
  [Parameter(Mandatory=$true)][string]$Dest,
  [Parameter(Mandatory=$true)][string]$Log
)
$ErrorActionPreference = 'Continue'
function Log([string]$m) { Add-Content -LiteralPath $Log -Value $m -Encoding UTF8 }
function Fail-And-Relaunch([string]$OldPath) {
  Log 'FAILED'
  if ((Test-Path -LiteralPath $OldPath) -and -not (Test-Path -LiteralPath $Dest)) {
    Move-Item -LiteralPath $OldPath -Destination $Dest -Force -ErrorAction SilentlyContinue
  }
  $msg = "RetComM portable update failed to replace the exe.`n`nMove it out of Downloads/OneDrive if needed, ensure the folder is writable, then try Update again.`n`nLog:`n" + $Log
  try {
    Add-Type -AssemblyName System.Windows.Forms
    [void][System.Windows.Forms.MessageBox]::Show($msg, 'RetComM Launcher')
  } catch {}
  if (Test-Path -LiteralPath $Dest) { Start-Process -FilePath $Dest }
  exit 1
}
Log 'RetComM portable update'
Log ("NEW=" + $New)
Log ("DEST=" + $Dest)
Log ("Waiting for PID " + $WaitPid)
$deadline = (Get-Date).AddSeconds(120)
while ((Get-Date) -lt $deadline) {
  if (-not (Get-Process -Id $WaitPid -ErrorAction SilentlyContinue)) { break }
  Start-Sleep -Milliseconds 400
}
if (Get-Process -Id $WaitPid -ErrorAction SilentlyContinue) {
  Log ("Timed out waiting for PID " + $WaitPid + " — continuing")
}
if (-not (Test-Path -LiteralPath $New)) {
  Log 'Staged portable missing'
  Fail-And-Relaunch ($Dest + '.retcomm-old')
}
$Old = $Dest + '.retcomm-old'
Remove-Item -LiteralPath $Old -Force -ErrorAction SilentlyContinue
$ok = $false
for ($tries = 1; $tries -le 40; $tries++) {
  Log ("Attempt " + $tries)
  try {
    if (Test-Path -LiteralPath $Dest) {
      Move-Item -LiteralPath $Dest -Destination $Old -Force -ErrorAction Stop
    }
    Copy-Item -LiteralPath $New -Destination $Dest -Force -ErrorAction Stop
    $ns = (Get-Item -LiteralPath $New).Length
    $ds = (Get-Item -LiteralPath $Dest).Length
    if ($ns -ne $ds) { throw "Size mismatch NEW=$ns DEST=$ds" }
    Remove-Item -LiteralPath $Old -Force -ErrorAction SilentlyContinue
    Log ("Replace OK size=" + $ds)
    $ok = $true
    break
  } catch {
    Log ("Attempt failed: " + $_)
    if ((Test-Path -LiteralPath $Old) -and -not (Test-Path -LiteralPath $Dest)) {
      Move-Item -LiteralPath $Old -Destination $Dest -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Seconds 1
  }
}
if (-not $ok) { Fail-And-Relaunch $Old }
$ver = Join-Path $env:LOCALAPPDATA 'retcomm\portable\version.txt'
Remove-Item -LiteralPath $ver -Force -ErrorAction SilentlyContinue
if (Test-Path -LiteralPath $Dest) { Start-Process -FilePath $Dest }
exit 0
)ps1";
    if (!write_text_file(script, body, error)) return false;
    const std::wstring args = L"-WaitPid " + std::to_wstring(pid) + L" -New " +
                              ps_single_quote(new_portable) + L" -Dest " +
                              ps_single_quote(dest_portable) + L" -Log " + ps_single_quote(log_path);
    return schedule_powershell(script, args, error);
}

bool schedule_run_setup_and_restart(const fs::path& setup_exe, const fs::path& install_dir,
                                    std::string* error) {
    const DWORD pid = GetCurrentProcessId();
    const fs::path script = setup_exe.parent_path() / "apply_setup_update.ps1";
    const fs::path log_path = setup_exe.parent_path() / "apply_setup_update.log";
    const fs::path hub_exe = install_dir / "retcomm-hub.exe";
    // Hub holds install-dir locks — do NOT run setup if it is still alive after timeout.
    const char* body = R"ps1(
param(
  [Parameter(Mandatory=$true)][int]$WaitPid,
  [Parameter(Mandatory=$true)][string]$Setup,
  [Parameter(Mandatory=$true)][string]$Dir,
  [Parameter(Mandatory=$true)][string]$Hub,
  [Parameter(Mandatory=$true)][string]$Log
)
$ErrorActionPreference = 'Continue'
function Log([string]$m) { Add-Content -LiteralPath $Log -Value $m -Encoding UTF8 }
function Fail-And-Relaunch {
  Log 'FAILED'
  $msg = "RetComM installer update failed.`n`nTry running the setup from the GitHub release manually.`n`nLog:`n" + $Log
  try {
    Add-Type -AssemblyName System.Windows.Forms
    [void][System.Windows.Forms.MessageBox]::Show($msg, 'RetComM Launcher')
  } catch {}
  if (Test-Path -LiteralPath $Hub) {
    Start-Process -FilePath $Hub -WorkingDirectory $Dir
  }
  exit 1
}
Log 'RetComM installer update'
Log ("SETUP=" + $Setup)
Log ("DIR=" + $Dir)
Log ("HUB=" + $Hub)
Log ("Waiting for PID " + $WaitPid)
$deadline = (Get-Date).AddSeconds(120)
while ((Get-Date) -lt $deadline) {
  if (-not (Get-Process -Id $WaitPid -ErrorAction SilentlyContinue)) { break }
  Start-Sleep -Milliseconds 400
}
if (Get-Process -Id $WaitPid -ErrorAction SilentlyContinue) {
  Log ("Timed out waiting for PID " + $WaitPid + " — aborting (hub may still lock files)")
  Fail-And-Relaunch
}
if (-not (Test-Path -LiteralPath $Setup)) {
  Log 'Staged setup missing'
  Fail-And-Relaunch
}
Log 'Running setup'
$p = Start-Process -FilePath $Setup -ArgumentList @(
  '/VERYSILENT', '/NORESTART', '/SUPPRESSMSGBOXES', '/CURRENTUSER', ("/DIR=" + $Dir)
) -Wait -PassThru
Log ("Setup exit=" + $p.ExitCode)
if ($p.ExitCode -ne 0) {
  Log 'Setup failed'
  Fail-And-Relaunch
}
if (-not (Test-Path -LiteralPath $Hub)) {
  Log 'Hub missing after setup'
  Fail-And-Relaunch
}
Log 'Setup OK — relaunching'
Start-Process -FilePath $Hub -WorkingDirectory $Dir
exit 0
)ps1";
    if (!write_text_file(script, body, error)) return false;
    const std::wstring args =
        L"-WaitPid " + std::to_wstring(pid) + L" -Setup " + ps_single_quote(setup_exe) +
        L" -Dir " + ps_single_quote(install_dir) + L" -Hub " + ps_single_quote(hub_exe) +
        L" -Log " + ps_single_quote(log_path);
    return schedule_powershell(script, args, error);
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
    wchar_t tmp[MAX_PATH]{};
    const DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n > 0 && n < MAX_PATH) script_dir = fs::path(tmp);
#else
    if (const char* t = std::getenv("TMPDIR"); t && *t) script_dir = t;
    else if (const char* t = std::getenv("XDG_RUNTIME_DIR"); t && *t) script_dir = t;
    else script_dir = "/tmp";
#endif
    if (script_dir.empty()) script_dir = launch.parent_path();

#if defined(_WIN32)
    const DWORD pid = GetCurrentProcessId();
    const fs::path script = script_dir / "retcomm_relaunch.ps1";
    const char* body = R"ps1(
param(
  [Parameter(Mandatory=$true)][int]$WaitPid,
  [Parameter(Mandatory=$true)][string]$Launch
)
$deadline = (Get-Date).AddSeconds(120)
while ((Get-Date) -lt $deadline) {
  if (-not (Get-Process -Id $WaitPid -ErrorAction SilentlyContinue)) { break }
  Start-Sleep -Milliseconds 400
}
if (Test-Path -LiteralPath $Launch) { Start-Process -FilePath $Launch }
)ps1";
    if (!write_text_file(script, body, error)) return false;
    const std::wstring args =
        L"-WaitPid " + std::to_wstring(pid) + L" -Launch " + ps_single_quote(launch);
    return schedule_powershell(script, args, error);
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
    {
        const std::wstring ch = win_getenv_w(L"RETCOMM_INSTALL_CHANNEL");
        if (!ch.empty()) channel_env = to_lower(wstring_to_utf8(ch));
    }
    {
        const std::wstring pe = win_getenv_w(L"RETCOMM_PORTABLE_EXE");
        if (!pe.empty()) portable_exe = fs::path(pe);
    }
    const fs::path exe = current_executable_path();
    std::string channel_file;
    if (!exe.empty()) {
        const json ch = read_json_file(exe.parent_path() / "channel.json");
        if (ch.is_object()) {
            channel_file = to_lower(ch.value("channel", ""));
            const std::string pe = ch.value("portable_exe", "");
            if (!pe.empty() && portable_exe.empty()) portable_exe = path_from_utf8(pe);
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
                        "Relaunch from RetComM Launcher.exe (portable zip).";
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
                                "windows-*-setup.exe / RetComM-Launcher-portable-windows.zip.");
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
                        "Relaunch from RetComM Launcher.exe (portable zip).");
        }
        if (!dir_is_writable(dest_portable.parent_path())) {
            return fail(result, "portable exe directory is not writable: " +
                                    dest_portable.parent_path().string());
        }
        const fs::path staged_portable = work / "bin" / dest_portable.filename();
        fs::create_directories(staged_portable.parent_path(), ec);

        fs::path source_exe = download;
        if (ends_with_ci(asset_lower, ".zip")) {
            const fs::path unpack = work / "portable-unpack";
            fs::create_directories(unpack, ec);
            if (!extract_archive_to(download, unpack, &err)) {
                return fail(result, "unpack portable zip failed: " + err);
            }
            source_exe.clear();
            auto try_name = [&](const char* name) -> fs::path {
                const fs::path direct = unpack / name;
                if (fs::is_regular_file(direct, ec) && looks_like_retcomm_portable_stub(direct))
                    return direct;
                for (auto it = fs::recursive_directory_iterator(
                         unpack, fs::directory_options::skip_permission_denied, ec);
                     !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                    if (!it->is_regular_file(ec)) continue;
                    if (it->path().filename() != name) continue;
                    if (looks_like_retcomm_portable_stub(it->path())) return it->path();
                }
                return {};
            };
            source_exe = try_name("RetComM Launcher.exe");
            if (source_exe.empty())
                source_exe = try_name("RetComM-Launcher-windows-portable.exe");
            if (source_exe.empty()) {
                /* Last resort: any .exe with RCM1 trailer (never pick setup/random). */
                for (auto it = fs::recursive_directory_iterator(
                         unpack, fs::directory_options::skip_permission_denied, ec);
                     !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                    if (!it->is_regular_file(ec)) continue;
                    const auto lower = to_lower(it->path().filename().string());
                    if (!ends_with_ci(lower, ".exe")) continue;
                    if (lower.find("setup") != std::string::npos) continue;
                    if (looks_like_retcomm_portable_stub(it->path())) {
                        source_exe = it->path();
                        break;
                    }
                }
            }
            if (source_exe.empty() || !fs::is_regular_file(source_exe, ec)) {
                return fail(result,
                            "portable zip has no RetComM Launcher stub (RCM1): " + asset->name);
            }
        } else if (!looks_like_retcomm_portable_stub(source_exe)) {
            return fail(result, "downloaded portable asset is not a RetComM stub (missing RCM1): " +
                                    asset->name);
        }

        fs::copy_file(source_exe, staged_portable, fs::copy_options::overwrite_existing, ec);
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
