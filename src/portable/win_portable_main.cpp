// RetComM Windows portable stub — single .exe with appended zip payload.
//
// Trailer layout (little-endian):
//   [PE stub bytes][zip payload][uint64 payload_size][magic "RCM1"]
//
// No args  → launch retcomm-hub.exe from the extracted runtime
// cli …    → launch retcomm.exe with the remaining arguments
//
// Extract cache: %LOCALAPPDATA%\retcomm\portable\current\

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

    // Expand-Archive is available on supported Windows 10/11 images.
    const std::wstring ps = L"powershell.exe";
    std::wstring cmd = L"-NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '" +
                       zip_path.wstring() + L"' -DestinationPath '" + dest.wstring() + L"' -Force\"";
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(ps.c_str(), cmdline.data(), nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) {
        *err = L"Failed to start PowerShell to extract the portable payload.";
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    fs::remove(zip_path, ec);
    if (code != 0) {
        *err = L"Expand-Archive failed while unpacking RetComM.";
        return false;
    }
    return true;
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

void write_channel(const fs::path& dir, const fs::path& portable_exe) {
    std::ofstream out(dir / "channel.json", std::ios::trunc);
    if (!out) return;
    out << "{\n"
        << "  \"schema_version\": 1,\n"
        << "  \"channel\": \"portable\",\n"
        << "  \"portable_exe\": \"" << json_escape(narrow(portable_exe.wstring())) << "\"\n"
        << "}\n";
}

bool launch(const fs::path& binary, const std::wstring& args, const fs::path& portable_exe,
            bool attach_console, std::wstring* err) {
    std::wstring cmd = L"\"" + binary.wstring() + L"\"";
    if (!args.empty()) cmd += L" " + args;

    std::wstring channel = L"portable";
    SetEnvironmentVariableW(L"RETCOMM_INSTALL_CHANNEL", channel.c_str());
    SetEnvironmentVariableW(L"RETCOMM_PORTABLE_EXE", portable_exe.wstring().c_str());

    if (attach_console) AttachConsole(ATTACH_PARENT_PROCESS);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(L'\0');

    const BOOL ok =
        CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, 0, nullptr,
                       binary.parent_path().wstring().c_str(), &si, &pi);
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

    const fs::path base = local_app_data() / L"retcomm" / L"portable";
    const fs::path current = base / L"current";
    const fs::path version_file = base / L"version.txt";
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

    write_channel(current, self);

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
        ok = launch(bin, join_args(2, argc, argv), self, true, &err);
    } else {
        const fs::path hub = current / "retcomm-hub.exe";
        std::error_code ec;
        if (!fs::is_regular_file(hub, ec)) {
            LocalFree(argv);
            fail(L"retcomm-hub.exe missing from portable runtime.");
            return 1;
        }
        ok = launch(hub, join_args(1, argc, argv), self, false, &err);
    }

    LocalFree(argv);
    if (!ok) {
        fail(err.empty() ? L"Launch failed." : err);
        return 1;
    }
    return 0;
}
