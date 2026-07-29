#include "retcomm/paths.hpp"

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace retcomm {
namespace {

fs::path home_dir() {
#if defined(_WIN32)
    if (const char* p = std::getenv("USERPROFILE")) return fs::path(p);
    if (const char* p = std::getenv("HOME")) return fs::path(p);
#else
    if (const char* p = std::getenv("HOME")) return fs::path(p);
#endif
    throw std::runtime_error("cannot resolve home directory");
}

fs::path xdg_config_home() {
    if (const char* p = std::getenv("XDG_CONFIG_HOME")) return fs::path(p);
#if defined(_WIN32)
    if (const char* p = std::getenv("APPDATA")) return fs::path(p);
#endif
    return home_dir() / ".config";
}

fs::path xdg_data_home() {
    if (const char* p = std::getenv("XDG_DATA_HOME")) return fs::path(p);
#if defined(_WIN32)
    if (const char* p = std::getenv("LOCALAPPDATA")) return fs::path(p);
#endif
    return home_dir() / ".local" / "share";
}

} // namespace

Paths default_paths() {
    Paths p;
    p.config_dir = xdg_config_home() / "retcomm";
    p.data_dir = xdg_data_home() / "retcomm";
    p.apps_dir = p.data_dir / "apps";
    p.state_path = p.data_dir / "state.json";
    p.config_path = p.config_dir / "config.json";
    p.library_index_path = p.data_dir / "library-index.json";
    p.bios_index_path = p.data_dir / "bios-index.json";
    p.romm_rom_index_path = p.data_dir / "romm-rom-index.json";
    p.catalog_dir = p.data_dir / "catalog";
    return p;
}

void ensure_dirs(const Paths& p) {
    std::error_code ec;
    fs::create_directories(p.config_dir, ec);
    if (ec) throw std::runtime_error("cannot create " + p.config_dir.string() + ": " +
                                     ec.message());
    ec.clear();
    fs::create_directories(p.data_dir, ec);
    if (ec) throw std::runtime_error("cannot create " + p.data_dir.string() + ": " +
                                     ec.message());
    ec.clear();
    fs::create_directories(p.apps_dir, ec);
    if (ec) throw std::runtime_error("cannot create " + p.apps_dir.string() + ": " +
                                     ec.message());
}

fs::path resolve_catalog_dir(const fs::path& /*exe_dir*/, const fs::path& override_dir,
                             const Paths* paths) {
    if (!override_dir.empty()) return override_dir;

    if (const char* env = std::getenv("RETCOMM_CATALOG")) {
        return fs::path(env);
    }

    if (paths) {
        std::error_code ec;
        if (fs::is_regular_file(paths->catalog_dir / "index.json", ec))
            return paths->catalog_dir;
        throw std::runtime_error(
            "catalog cache missing at " + paths->catalog_dir.string() +
            "; run `retcomm catalog update` (needs network) or set "
            "RETCOMM_CATALOG / --catalog");
    }

    throw std::runtime_error(
        "catalog not found; run `retcomm catalog update` or pass --catalog / "
        "RETCOMM_CATALOG");
}

std::string host_os_key() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

bool open_path_in_file_manager(const fs::path& path, std::string* error) {
    std::error_code ec;
    fs::path target = path;
    if (target.empty()) {
        if (error) *error = "empty path";
        return false;
    }
    if (fs::is_regular_file(target, ec))
        target = target.parent_path();
    else if (!fs::is_directory(target, ec)) {
        if (error) *error = "path does not exist: " + path.string();
        return false;
    }
    if (target.empty()) {
        if (error) *error = "cannot resolve folder for " + path.string();
        return false;
    }

#if defined(_WIN32)
    const std::wstring w = target.wstring();
    const HINSTANCE rc =
        ShellExecuteW(nullptr, L"explore", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    const auto code = reinterpret_cast<std::uintptr_t>(rc);
    if (code <= 32) {
        if (error) *error = "ShellExecute failed (" + std::to_string(code) + ")";
        return false;
    }
    return true;
#else
    // Double-fork so the file manager is fully detached and we don't leave zombies.
    const pid_t pid = fork();
    if (pid < 0) {
        if (error) *error = "fork failed";
        return false;
    }
    if (pid == 0) {
        if (fork() > 0) _exit(0);
        if (setsid() < 0) { /* best-effort */ }
#if defined(__APPLE__)
        const char* cmd = "open";
#else
        const char* cmd = "xdg-open";
#endif
        execlp(cmd, cmd, target.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        if (error) *error = "waitpid failed";
        return false;
    }
    return true;
#endif
}

bool open_url_in_browser(const std::string& url, std::string* error) {
    if (url.empty()) {
        if (error) *error = "empty url";
        return false;
    }
#if defined(_WIN32)
    const std::wstring w(url.begin(), url.end());
    const HINSTANCE rc =
        ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    const auto code = reinterpret_cast<std::uintptr_t>(rc);
    if (code <= 32) {
        if (error) *error = "ShellExecute failed (" + std::to_string(code) + ")";
        return false;
    }
    return true;
#else
    const pid_t pid = fork();
    if (pid < 0) {
        if (error) *error = "fork failed";
        return false;
    }
    if (pid == 0) {
        if (fork() > 0) _exit(0);
        if (setsid() < 0) { /* best-effort */ }
#if defined(__APPLE__)
        const char* cmd = "open";
#else
        const char* cmd = "xdg-open";
#endif
        execlp(cmd, cmd, url.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        if (error) *error = "waitpid failed";
        return false;
    }
    return true;
#endif
}

} // namespace retcomm
