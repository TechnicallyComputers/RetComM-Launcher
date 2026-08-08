#include "retcomm/process_env.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace retcomm {

void sanitize_env_for_external_child() {
#if defined(__linux__)
    const char* appdir = std::getenv("APPDIR");
    const char* appimage = std::getenv("APPIMAGE");
    if (!appdir && !appimage) return;

    auto path_is_appimage = [&](const std::string& p) -> bool {
        if (p.empty()) return false;
        if (appdir && p.rfind(appdir, 0) == 0) return true;
        if (p.find("/tmp/.mount_") != std::string::npos) return true;
        if (p.find(".AppDir") != std::string::npos) return true;
        if (appimage) {
            const std::string ai(appimage);
            if (!ai.empty() && p.rfind(ai, 0) == 0) return true;
        }
        return false;
    };

    auto filter_path_env = [&](const char* key) {
        const char* raw = std::getenv(key);
        if (!raw || !*raw) return;
        std::string kept;
        const char* start = raw;
        while (*start) {
            const char* colon = std::strchr(start, ':');
            const std::string part =
                colon ? std::string(start, colon) : std::string(start);
            if (!part.empty() && !path_is_appimage(part)) {
                if (!kept.empty()) kept.push_back(':');
                kept += part;
            }
            if (!colon) break;
            start = colon + 1;
        }
        if (kept.empty())
            ::unsetenv(key);
        else
            ::setenv(key, kept.c_str(), 1);
    };

    filter_path_env("LD_LIBRARY_PATH");
    filter_path_env("LD_PRELOAD");
    filter_path_env("XDG_DATA_DIRS");
    filter_path_env("PATH"); // drop AppImage usr/bin so wine/system tools win

    ::unsetenv("APPDIR");
    ::unsetenv("APPIMAGE");
    ::unsetenv("ARGV0");
    ::unsetenv("OWNDIR");
#endif
}

AppImageEnvGuard::AppImageEnvGuard() {
#if defined(__linux__)
    const char* appdir = std::getenv("APPDIR");
    const char* appimage = std::getenv("APPIMAGE");
    if (!appdir && !appimage) return;

    auto save = [this](const char* key) {
        SavedEnv s;
        s.key = key;
        const char* raw = std::getenv(key);
        s.had = raw != nullptr;
        if (raw) s.value = raw;
        saved_.push_back(std::move(s));
    };
    save("LD_LIBRARY_PATH");
    save("LD_PRELOAD");
    save("XDG_DATA_DIRS");
    save("PATH");
    save("APPDIR");
    save("APPIMAGE");
    save("ARGV0");
    save("OWNDIR");
    sanitize_env_for_external_child();
    active_ = true;
#else
    (void)active_;
#endif
}

AppImageEnvGuard::~AppImageEnvGuard() {
#if defined(__linux__)
    if (!active_) return;
    for (const auto& s : saved_) {
        if (!s.key) continue;
        if (s.had)
            ::setenv(s.key, s.value.c_str(), 1);
        else
            ::unsetenv(s.key);
    }
    saved_.clear();
    active_ = false;
#endif
}

} // namespace retcomm
