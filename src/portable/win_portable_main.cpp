// RetComM Windows portable stub — single .exe with appended zip payload.
//
// Trailer layout (little-endian):
//   [PE stub bytes][zip payload][uint64 payload_size][magic "RCM1"]
//
// No args  → launch retcomm-hub.exe from the extracted runtime
// cli …    → launch retcomm.exe with the remaining arguments
//
// Portable layout — everything lives beside the .exe:
//   <exe_dir>\RetComM-Data\runtime\   extracted hub + CLI
//   <exe_dir>\RetComM-Data\config\    config.json
//   <exe_dir>\RetComM-Data\data\      apps, toolchains, engines, catalog, …
// RETCOMM_HOME is exported to the child so it resolves the same folder.
//
// When setup moves the RetComM folder elsewhere, the hub records the new root
// in <exe_dir>\retcomm-root.json. We then unpack into <root>\runtime\ instead
// and delete the old RetComM-Data, so the launcher ends up as a lone .exe with
// one folder holding the runtime, config and data together.
//
// When the exe folder is not writable (Downloads with MotW, a network share, a
// read-only stick) we fall back to %LOCALAPPDATA%\retcomm\portable\current\ for
// the runtime and leave RETCOMM_HOME unset, so config+data stay at the historical
// %LOCALAPPDATA%\retcomm and an existing portable user's library still resolves.
// Unpack via System32\tar.exe, then PowerShell Expand-Archive as fallback.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if !defined(RETCOMM_VERSION)
#define RETCOMM_VERSION "0.0.0"
#endif

namespace fs = std::filesystem;

namespace {

constexpr char kMagic[4] = {'R', 'C', 'M', '1'};

void fail(const std::wstring& msg) {
    MessageBoxW(nullptr, msg.c_str(), L"RetComM Launcher", MB_OK | MB_ICONERROR);
}

std::string narrow(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr,
                                      nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

fs::path exe_path() {
    // Grow past MAX_PATH so deep install directories still resolve.
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
}

// Can we write inside `dir`? Probes an EXISTING directory only — it never
// creates one. The data folder must not appear beside the .exe before the
// setup wizard has asked where the user wants it; creating it here made the
// wizard offer a folder that already existed.
bool dir_is_writable(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    const fs::path probe = dir / L".retcomm-write-test";
    {
        std::ofstream out(probe, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << "ok";
        if (!out) return false;
    }
    fs::remove(probe, ec);
    return true;
}

bool same_path(const fs::path& a, const fs::path& b) {
    if (a.empty() || b.empty()) return false;
    const std::wstring aw = a.lexically_normal().wstring();
    const std::wstring bw = b.lexically_normal().wstring();
    return _wcsicmp(aw.c_str(), bw.c_str()) == 0;
}

// Pull the "root" string out of retcomm-root.json. The stub links nothing but
// shell32 on purpose, so this reads the single field it needs rather than
// pulling in a JSON library — the mirror of read_data_root_marker() in
// data_root.cpp, including resolving a relative root against the marker's own
// directory so a moved stick still works.
fs::path read_root_marker(const fs::path& marker) {
    std::error_code ec;
    if (!fs::is_regular_file(marker, ec)) return {};
    std::ifstream in(marker, std::ios::binary);
    if (!in) return {};
    const std::string text((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());

    const std::string key = "\"root\"";
    const size_t k = text.find(key);
    if (k == std::string::npos) return {};
    size_t p = text.find(':', k + key.size());
    if (p == std::string::npos) return {};
    ++p;
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t' || text[p] == '\r' ||
                               text[p] == '\n'))
        ++p;
    if (p >= text.size() || text[p] != '"') return {};
    ++p;

    std::string raw;
    for (; p < text.size() && text[p] != '"'; ++p) {
        if (text[p] == '\\' && p + 1 < text.size()) ++p; // \\ \" \/ → literal
        raw.push_back(text[p]);
    }
    if (raw.empty()) return {};

    fs::path root(std::wstring(raw.begin(), raw.end()));
    if (root.is_relative()) {
        fs::path joined = fs::absolute(marker.parent_path() / root, ec);
        root = ec ? (marker.parent_path() / root) : joined;
    }
    return root.lexically_normal();
}

// True when `dir` holds nothing but a previously unpacked runtime. Anything
// else — config/, data/, a file the user dropped in — means it is not ours to
// delete, and we leave the whole folder alone.
bool holds_only_runtime(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::wstring name = it->path().filename().wstring();
        if (_wcsicmp(name.c_str(), L"runtime") == 0) continue;
        if (_wcsicmp(name.c_str(), L"version.txt") == 0) continue;
        return false;
    }
    return !ec;
}

fs::path local_app_data() {
    wchar_t* raw = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&raw, &len, L"LOCALAPPDATA") == 0 && raw && len > 1) {
        fs::path p(raw);
        free(raw);
        return p;
    }
    if (raw) free(raw);
    return {};
}

bool read_trailer(const fs::path& self, uint64_t* payload_size, uint64_t* payload_offset) {
    std::ifstream in(self, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const auto file_size = static_cast<uint64_t>(in.tellg());
    if (file_size < 12) return false;
    in.seekg(static_cast<std::streamoff>(file_size - 12));
    uint64_t size = 0;
    char magic[4]{};
    in.read(reinterpret_cast<char*>(&size), 8);
    in.read(magic, 4);
    if (!in || std::memcmp(magic, kMagic, 4) != 0) return false;
    if (size == 0 || size + 12 > file_size) return false;
    *payload_size = size;
    *payload_offset = file_size - 12 - size;
    return true;
}

fs::path system_directory() {
    wchar_t buf[MAX_PATH]{};
    const UINT n = GetSystemDirectoryW(buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return fs::path(buf);
}

std::wstring win_quote(const std::wstring& path) {
    // Paths under LOCALAPPDATA rarely contain quotes; strip if present.
    std::wstring cleaned;
    cleaned.reserve(path.size());
    for (wchar_t c : path) {
        if (c != L'"') cleaned.push_back(c);
    }
    return L"\"" + cleaned + L"\"";
}

std::wstring ps_single_quote(const std::wstring& path) {
    // Inside PowerShell single-quoted strings, '' is a literal apostrophe.
    std::wstring out;
    out.reserve(path.size() + 8);
    for (wchar_t c : path) {
        if (c == L'\'') out += L"''";
        else out.push_back(c);
    }
    return out;
}

// CreateProcess needs a mutable command line; include the exe as argv[0].
bool run_hidden(const fs::path& exe, const std::wstring& args, DWORD* exit_code, std::wstring* err) {
    std::error_code ec;
    if (!fs::is_regular_file(exe, ec)) {
        *err = L"Missing helper: " + exe.filename().wstring();
        return false;
    }
    std::wstring cmd = win_quote(exe.wstring());
    if (!args.empty()) {
        cmd.push_back(L' ');
        cmd += args;
    }
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.wstring().c_str(), cmdline.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        *err = L"Failed to start " + exe.filename().wstring() + L" (Win32 " +
               std::to_wstring(GetLastError()) + L").";
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (exit_code) *exit_code = code;
    return true;
}

bool extract_zip_tar(const fs::path& zip_path, const fs::path& dest, std::wstring* err) {
    // Windows 10+ ships bsdtar as System32\tar.exe; it unpacks .zip.
    const fs::path tar = system_directory() / L"tar.exe";
    const std::wstring args =
        L"-xf " + win_quote(zip_path.wstring()) + L" -C " + win_quote(dest.wstring());
    DWORD code = 1;
    if (!run_hidden(tar, args, &code, err)) return false;
    if (code != 0) {
        *err = L"tar extract failed (exit " + std::to_wstring(code) + L").";
        return false;
    }
    return true;
}

bool extract_zip_powershell(const fs::path& zip_path, const fs::path& dest, std::wstring* err) {
    const fs::path ps =
        system_directory() / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
    const std::wstring args =
        L"-NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '" +
        ps_single_quote(zip_path.wstring()) + L"' -DestinationPath '" +
        ps_single_quote(dest.wstring()) + L"' -Force\"";
    DWORD code = 1;
    if (!run_hidden(ps, args, &code, err)) return false;
    if (code != 0) {
        *err = L"Expand-Archive failed while unpacking RetComM (exit " +
               std::to_wstring(code) + L").";
        return false;
    }
    return true;
}

bool extract_payload(const fs::path& self, uint64_t offset, uint64_t size, const fs::path& dest,
                     std::wstring* err) {
    std::error_code ec;
    fs::remove_all(dest, ec);
    fs::create_directories(dest, ec);
    if (ec) {
        *err = L"Cannot create extract directory.";
        return false;
    }

    const fs::path zip_path = dest.parent_path() / "payload.zip";
    {
        std::ifstream in(self, std::ios::binary);
        if (!in) {
            *err = L"Cannot read portable executable.";
            return false;
        }
        in.seekg(static_cast<std::streamoff>(offset));
        std::ofstream out(zip_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            *err = L"Cannot write temporary zip.";
            return false;
        }
        std::vector<char> buf(1 << 20);
        uint64_t left = size;
        while (left > 0) {
            const size_t chunk = static_cast<size_t>(std::min<uint64_t>(left, buf.size()));
            in.read(buf.data(), static_cast<std::streamsize>(chunk));
            const auto got = in.gcount();
            if (got <= 0) {
                *err = L"Truncated portable payload.";
                return false;
            }
            out.write(buf.data(), got);
            left -= static_cast<uint64_t>(got);
        }
    }

    std::wstring tar_err;
    if (extract_zip_tar(zip_path, dest, &tar_err)) {
        fs::remove(zip_path, ec);
        return true;
    }

    std::wstring ps_err;
    if (extract_zip_powershell(zip_path, dest, &ps_err)) {
        fs::remove(zip_path, ec);
        return true;
    }

    fs::remove(zip_path, ec);
    *err = L"Could not unpack portable payload.\n" + tar_err + L"\n" + ps_err;
    return false;
}

std::string json_escape(std::string s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

void write_channel(const fs::path& dir, const fs::path& portable_exe,
                   const fs::path& data_root) {
    std::ofstream out(dir / "channel.json", std::ios::trunc);
    if (!out) return;
    out << "{\n"
        << "  \"schema_version\": 1,\n"
        << "  \"channel\": \"portable\",\n"
        << "  \"portable_exe\": \"" << json_escape(narrow(portable_exe.wstring())) << "\",\n"
        << "  \"data_root\": \"" << json_escape(narrow(data_root.wstring())) << "\"\n"
        << "}\n";
}

bool launch(const fs::path& binary, const std::wstring& args, const fs::path& portable_exe,
            const fs::path& data_root, bool attach_console, std::wstring* err) {
    std::wstring cmd = L"\"" + binary.wstring() + L"\"";
    if (!args.empty()) cmd += L" " + args;

    std::wstring channel = L"portable";
    SetEnvironmentVariableW(L"RETCOMM_INSTALL_CHANNEL", channel.c_str());
    SetEnvironmentVariableW(L"RETCOMM_PORTABLE_EXE", portable_exe.wstring().c_str());
    // Hub and CLI resolve config+data under this root (see data_root.cpp).
    if (!data_root.empty())
        SetEnvironmentVariableW(L"RETCOMM_HOME", data_root.wstring().c_str());

    if (attach_console) AttachConsole(ATTACH_PARENT_PROCESS);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(L'\0');

    // Hub is GUI (/SUBSYSTEM:WINDOWS); still hide any console for older builds.
    // CLI attaches to the parent console and waits.
    const DWORD flags = attach_console ? 0 : (CREATE_NO_WINDOW | DETACHED_PROCESS);
    const BOOL ok =
        CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, attach_console ? TRUE : FALSE,
                       flags, nullptr, binary.parent_path().wstring().c_str(), &si, &pi);
    if (!ok) {
        *err = L"Failed to launch " + binary.filename().wstring();
        return false;
    }
    if (attach_console) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ExitProcess(code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

std::wstring join_args(int start, int argc, wchar_t** argv) {
    std::wstring out;
    for (int i = start; i < argc; ++i) {
        if (i > start) out += L' ';
        const std::wstring a = argv[i];
        if (a.find(L' ') != std::wstring::npos) out += L'"' + a + L'"';
        else out += a;
    }
    return out;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        fail(L"Failed to parse command line.");
        return 1;
    }

    const fs::path self = exe_path();
    if (self.empty()) {
        LocalFree(argv);
        fail(L"Cannot resolve portable executable path.");
        return 1;
    }

    uint64_t payload_size = 0, payload_offset = 0;
    if (!read_trailer(self, &payload_size, &payload_offset)) {
        LocalFree(argv);
        fail(L"This file is not a valid RetComM portable package (missing payload).");
        return 1;
    }

    // Prefer a self-contained folder beside the .exe; fall back to the historical
    // LOCALAPPDATA cache when this medium is read-only.
    // Probe the exe's own directory — it exists already, so this answers "can
    // we put a data folder here" without creating one.
    const fs::path default_base = self.parent_path() / L"RetComM-Data";
    fs::path base = default_base;
    bool portable_base = dir_is_writable(self.parent_path());

    // Setup may have moved the RetComM folder elsewhere; retcomm-root.json
    // beside this .exe records where. Unpack the runtime into that folder too,
    // so the whole install stays one directory and nothing is stranded next to
    // the launcher. Missing drive or unwritable target → fall back to the
    // default beside the .exe rather than refusing to start.
    const fs::path marked = read_root_marker(self.parent_path() / L"retcomm-root.json");
    if (!marked.empty() && dir_is_writable(marked)) {
        base = marked;
        portable_base = true;
    }

    if (!portable_base) {
        const fs::path fallback = local_app_data();
        if (fallback.empty()) {
            LocalFree(argv);
            fail(L"Cannot write beside the executable, and %LOCALAPPDATA% is unset.");
            return 1;
        }
        base = fallback / L"retcomm" / L"portable";
    }
    // The runtime now lives under the chosen folder, so the one we used to
    // unpack beside the .exe is dead weight. Remove it — but only once it holds
    // nothing but that old runtime, so a folder still containing config/ or
    // data/ (an interrupted move, a user's own files) is never touched.
    if (portable_base && !same_path(base, default_base) && holds_only_runtime(default_base)) {
        std::error_code rm_ec;
        fs::remove_all(default_base, rm_ec);
    }

    const fs::path current = base / (portable_base ? L"runtime" : L"current");
    const fs::path version_file = base / L"version.txt";
    // Only the beside-the-exe layout redirects config+data. In the LOCALAPPDATA
    // fallback we leave RETCOMM_HOME unset so the hub resolves its normal
    // %LOCALAPPDATA%\retcomm default — which is where an existing portable
    // user's library already lives.
    const fs::path data_root = portable_base ? base : fs::path();
    const std::string want_ver = RETCOMM_VERSION;

    bool need_extract = true;
    {
        std::ifstream in(version_file);
        std::string have;
        if (in && std::getline(in, have) && have == want_ver) {
            std::error_code ec;
            if (fs::is_regular_file(current / "retcomm-hub.exe", ec)) need_extract = false;
        }
    }

    if (need_extract) {
        std::wstring err;
        if (!extract_payload(self, payload_offset, payload_size, current, &err)) {
            LocalFree(argv);
            fail(err.empty() ? L"Payload extract failed." : err);
            return 1;
        }
        std::error_code ec;
        fs::create_directories(base, ec);
        std::ofstream out(version_file, std::ios::trunc);
        out << want_ver << "\n";
    }

    write_channel(current, self, data_root);

    const bool cli = argc >= 2 && _wcsicmp(argv[1], L"cli") == 0;
    std::wstring err;
    bool ok = false;
    if (cli) {
        const fs::path bin = current / "retcomm.exe";
        std::error_code ec;
        if (!fs::is_regular_file(bin, ec)) {
            LocalFree(argv);
            fail(L"retcomm.exe missing from portable runtime.");
            return 1;
        }
        ok = launch(bin, join_args(2, argc, argv), self, data_root, true, &err);
    } else {
        const fs::path hub = current / "retcomm-hub.exe";
        std::error_code ec;
        if (!fs::is_regular_file(hub, ec)) {
            LocalFree(argv);
            fail(L"retcomm-hub.exe missing from portable runtime.");
            return 1;
        }
        ok = launch(hub, join_args(1, argc, argv), self, data_root, false, &err);
    }

    LocalFree(argv);
    if (!ok) {
        fail(err.empty() ? L"Launch failed." : err);
        return 1;
    }
    return 0;
}
