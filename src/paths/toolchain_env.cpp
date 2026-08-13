#include "retcomm/toolchain_env.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
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

#if defined(_WIN32)
using PathSep = char;
constexpr PathSep kPathSep = ';';
#else
using PathSep = char;
constexpr PathSep kPathSep = ':';
#endif

std::string marker_begin(const std::string& pack_id) {
    return "# >>> retcomm-toolchain (" + pack_id + ") >>>";
}
std::string marker_end(const std::string& pack_id) {
    return "# <<< retcomm-toolchain (" + pack_id + ") <<<";
}

bool path_list_contains(const std::string& path_list, const std::string& dir, PathSep sep) {
    if (dir.empty()) return false;
    std::string needle = dir;
    while (!needle.empty() && (needle.back() == '/' || needle.back() == '\\'))
        needle.pop_back();
    std::string cur;
    for (size_t i = 0; i <= path_list.size(); ++i) {
        if (i == path_list.size() || path_list[i] == sep) {
            while (!cur.empty() && (cur.back() == '/' || cur.back() == '\\'))
                cur.pop_back();
#if defined(_WIN32)
            if (_stricmp(cur.c_str(), needle.c_str()) == 0) return true;
#else
            if (cur == needle) return true;
#endif
            cur.clear();
        } else {
            cur.push_back(path_list[i]);
        }
    }
    return false;
}

#if defined(_WIN32)
bool win_is_reparse_point(const fs::path& p) {
    const DWORD attrs = GetFileAttributesW(p.wstring().c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// Unlink a junction/symlink without descending into the target. A previous bug
// fell back to a full recursive copy of the toolchain into latest\ — never
// remove_all that here (multi‑GB hang); rename it aside instead.
void win_remove_latest_entry(const fs::path& latest) {
    std::error_code ec;
    const DWORD attrs = GetFileAttributesW(latest.wstring().c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        // Broken symlink / already gone.
        fs::remove(latest, ec);
        return;
    }
    if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0)
            RemoveDirectoryW(latest.wstring().c_str());
        else
            DeleteFileW(latest.wstring().c_str());
        return;
    }
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        // Real directory (likely the old full-copy fallback). Move aside so
        // publish does not block on deleting gigabytes of LLVM.
        const fs::path aside =
            latest.parent_path() /
            ("latest.old-" + std::to_string(GetTickCount64()));
        fs::rename(latest, aside, ec);
        if (ec) {
            // Last resort: leave it; caller can publish pack_root directly.
            ec.clear();
        }
        return;
    }
    fs::remove(latest, ec);
}

#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif

bool win_create_directory_junction(const fs::path& link, const fs::path& target) {
    // mklink /J does not require admin / Developer Mode.
    std::wstring cmd = L"cmd.exe /C mklink /J \"";
    cmd += link.wstring();
    cmd += L"\" \"";
    cmd += target.wstring();
    cmd += L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0 && win_is_reparse_point(link);
}
#endif

void set_latest_pointer(const fs::path& cache_base, const fs::path& pack_root) {
    std::error_code ec;
    const fs::path latest = cache_base / "latest";
    fs::create_directories(cache_base, ec);

    // Fast path: already linked/copied to the same tree.
    if (fs::equivalent(latest, pack_root, ec)) return;
    ec.clear();

#if defined(_WIN32)
    win_remove_latest_entry(latest);

    const std::wstring target = pack_root.wstring();
    const std::wstring link = latest.wstring();
    // Junction first — no Developer Mode; same-volume toolchain cache.
    if (win_create_directory_junction(latest, pack_root)) return;
    // Dev Mode / elevated: real directory symlink (cross-volume fallback).
    if (CreateSymbolicLinkW(link.c_str(), target.c_str(),
                            SYMBOLIC_LINK_FLAG_DIRECTORY |
                                SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) ||
        CreateSymbolicLinkW(link.c_str(), target.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY)) {
        return;
    }

    // Never fs::copy the whole toolchain — that hung the Hub for minutes.
    // publish_toolchain_user_env will fall back to pack_root for PATH.
    std::ofstream pointer(cache_base / "latest.path", std::ios::binary | std::ios::trunc);
    if (pointer) pointer << pack_root.string();
#else
    if (fs::exists(latest, ec) || fs::is_symlink(latest, ec)) {
        fs::remove(latest, ec);
        if (ec) {
            ec.clear();
            fs::remove_all(latest, ec);
        }
    }
    fs::create_directory_symlink(pack_root, latest, ec);
    if (!ec) return;
    ec.clear();
    std::ofstream pointer(cache_base / "latest.path", std::ios::binary | std::ios::trunc);
    if (pointer) pointer << pack_root.string();
#endif
}

#if !defined(_WIN32)

std::string read_file(const fs::path& p) {
    std::ifstream in(p);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool write_file(const fs::path& p, const std::string& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << data;
    return static_cast<bool>(out);
}

std::string strip_marker_block(const std::string& src, const std::string& begin,
                               const std::string& end) {
    std::istringstream in(src);
    std::ostringstream out;
    std::string line;
    bool skip = false;
    while (std::getline(in, line)) {
        if (line == begin) {
            skip = true;
            continue;
        }
        if (line == end) {
            skip = false;
            continue;
        }
        if (!skip) out << line << '\n';
    }
    return out.str();
}

void upsert_profile_block(const fs::path& file, const std::string& begin,
                          const std::string& end, const std::string& block) {
    std::string body = read_file(file);
    body = strip_marker_block(body, begin, end);
    if (!body.empty() && body.back() != '\n') body.push_back('\n');
    body += block;
    if (body.back() != '\n') body.push_back('\n');
    write_file(file, body);
}

void publish_unix(const fs::path& cache_base, const std::string& pack_id,
                  const fs::path& latest, std::string* message) {
    const fs::path hook = cache_base / "path.sh";
    {
        std::ostringstream hs;
        hs << "# Auto-generated by RetComM — do not edit; re-run ensure to refresh.\n"
           << "RETCOMM_TC_LATEST=\"" << latest.string() << "\"\n"
           << "if [ -f \"${RETCOMM_TC_LATEST}/env.sh\" ]; then\n"
           << "  # shellcheck disable=SC1091\n"
           << "  . \"${RETCOMM_TC_LATEST}/env.sh\"\n"
           << "elif [ -d \"${RETCOMM_TC_LATEST}/bin\" ]; then\n"
           << "  case \":${PATH}:\" in\n"
           << "    *\":${RETCOMM_TC_LATEST}/bin:\"*) ;;\n"
           << "    *) export PATH=\"${RETCOMM_TC_LATEST}/bin${PATH:+:${PATH}}\" ;;\n"
           << "  esac\n"
           << "fi\n"
           << "export RETCOMM_TOOLCHAIN_DIR=\"${RETCOMM_TC_LATEST}\"\n";
        write_file(hook, hs.str());
    }

    const std::string begin = marker_begin(pack_id);
    const std::string end = marker_end(pack_id);
    std::ostringstream block;
    block << begin << '\n'
          << "# Managed by RetComM / retcomm-toolchains install — remove via uninstall.sh\n"
          << "if [ -f \"" << hook.string() << "\" ]; then\n"
          << "  # shellcheck disable=SC1091\n"
          << "  . \"" << hook.string() << "\"\n"
          << "fi\n"
          << end << '\n';

    const char* home = std::getenv("HOME");
    if (!home || !*home) {
        if (message) *message = "toolchain PATH: HOME unset; skipped shell profile";
        return;
    }
    const fs::path home_dir(home);
    std::vector<fs::path> profiles;
    std::error_code ec;
    for (const char* name : {".bashrc", ".zshrc", ".profile"}) {
        const fs::path p = home_dir / name;
        if (fs::exists(p, ec)) profiles.push_back(p);
    }
    if (profiles.empty()) {
#if defined(__APPLE__)
        profiles.push_back(home_dir / ".zshrc");
#else
        profiles.push_back(home_dir / ".bashrc");
#endif
    }
    for (const auto& p : profiles) {
        upsert_profile_block(p, begin, end, block.str());
    }

    // Session PATH (idempotent).
    const fs::path bin = latest / "bin";
    const char* cur = std::getenv("PATH");
    const std::string cur_s = cur ? cur : "";
    if (!path_list_contains(cur_s, bin.string(), ':')) {
        const std::string neu = bin.string() + (cur_s.empty() ? "" : ":" + cur_s);
        setenv("PATH", neu.c_str(), 1);
    }
    setenv("RETCOMM_TOOLCHAIN_DIR", latest.string().c_str(), 1);
    if (message)
        *message = "toolchain PATH: registered " + bin.string() + " (shell profile + session)";
}

#else // _WIN32

std::wstring read_user_env(const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return {};
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        size == 0) {
        RegCloseKey(key);
        return {};
    }
    std::wstring buf(size / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<LPBYTE>(buf.data()), &size) != ERROR_SUCCESS) {
        RegCloseKey(key);
        return {};
    }
    RegCloseKey(key);
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    return buf;
}

bool write_user_env(const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_SET_VALUE, &key) !=
        ERROR_SUCCESS)
        return false;
    const DWORD bytes =
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LONG rc =
        RegSetValueExW(key, name, 0, REG_EXPAND_SZ,
                       reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) return false;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 5000,
                        nullptr);
    return true;
}

std::vector<std::wstring> split_path(const std::wstring& raw) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (size_t i = 0; i <= raw.size(); ++i) {
        if (i == raw.size() || raw[i] == L';') {
            while (!cur.empty() && (cur.back() == L'/' || cur.back() == L'\\'))
                cur.pop_back();
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(raw[i]);
        }
    }
    return out;
}

std::wstring join_path(const std::vector<std::wstring>& parts) {
    std::wstring out;
    for (const auto& p : parts) {
        if (p.empty()) continue;
        if (!out.empty()) out.push_back(L';');
        out += p;
    }
    return out;
}

bool path_contains_ci(const std::vector<std::wstring>& parts, const std::wstring& dir) {
    for (const auto& p : parts) {
        if (_wcsicmp(p.c_str(), dir.c_str()) == 0) return true;
    }
    return false;
}

void publish_windows(const fs::path& latest, std::string* message) {
    const fs::path bin = latest / "bin";
    const std::wstring bin_w = bin.wstring();
    auto parts = split_path(read_user_env(L"Path"));
    bool added = false;
    if (!path_contains_ci(parts, bin_w)) {
        parts.push_back(bin_w);
        if (!write_user_env(L"Path", join_path(parts))) {
            if (message) *message = "toolchain PATH: failed to write HKCU Environment\\Path";
            return;
        }
        added = true;
    }
    write_user_env(L"RETCOMM_TOOLCHAIN_DIR", latest.wstring());

    // Session PATH
    const char* cur = std::getenv("PATH");
    const std::string cur_s = cur ? cur : "";
    if (!path_list_contains(cur_s, bin.string(), ';')) {
        const std::string neu = bin.string() + (cur_s.empty() ? "" : ";" + cur_s);
        _putenv_s("PATH", neu.c_str());
    }
    _putenv_s("RETCOMM_TOOLCHAIN_DIR", latest.string().c_str());

    if (message) {
        *message = added ? ("toolchain PATH: added " + bin.string() + " to user Path")
                         : ("toolchain PATH: already on user Path (" + bin.string() + ")");
    }
}

#endif

} // namespace

bool publish_toolchain_user_env(const Paths& paths, const std::string& pack_id,
                                const fs::path& pack_root, std::string* message) {
    if (pack_id.empty() || pack_root.empty()) {
        if (message) *message = "toolchain PATH: missing pack id/root";
        return false;
    }
    std::error_code ec;
    const fs::path cmake =
#if defined(_WIN32)
        pack_root / "bin" / "cmake.exe";
#else
        pack_root / "bin" / "cmake";
#endif
    if (!fs::is_regular_file(cmake, ec)) {
        if (message) *message = "toolchain PATH: pack unusable (no bin/cmake)";
        return false;
    }

    const fs::path cache_base = paths.toolchains_dir / pack_id;
    fs::create_directories(cache_base, ec);
    const fs::path canonical = fs::weakly_canonical(pack_root, ec);
    set_latest_pointer(cache_base, canonical);
    fs::path publish_root = cache_base / "latest";
    if (!fs::exists(publish_root, ec)) {
        // Junction/symlink unavailable — use the versioned pack dir (and any
        // latest.path note left by set_latest_pointer).
        const fs::path path_file = cache_base / "latest.path";
        if (fs::is_regular_file(path_file, ec)) {
            std::ifstream in(path_file);
            std::string line;
            if (std::getline(in, line) && !line.empty()) {
                const fs::path from_file(line);
                if (fs::is_directory(from_file, ec)) publish_root = from_file;
            }
        }
        if (!fs::exists(publish_root, ec)) publish_root = canonical;
    }
    if (!fs::is_directory(publish_root, ec)) {
        if (message) *message = "toolchain PATH: failed to resolve pack root";
        return false;
    }

#if defined(_WIN32)
    publish_windows(publish_root, message);
#else
    publish_unix(cache_base, pack_id, publish_root, message);
#endif
    return true;
}

} // namespace retcomm
