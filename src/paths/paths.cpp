#include "retcomm/paths.hpp"

#include <cstdlib>
#include <stdexcept>

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

fs::path resolve_catalog_dir(const fs::path& exe_dir, const fs::path& override_dir) {
    if (!override_dir.empty()) return override_dir;

    if (const char* env = std::getenv("RETCOMM_CATALOG")) {
        return fs::path(env);
    }

    const fs::path beside = exe_dir / "catalog";
    if (fs::is_directory(beside)) return beside;

    const fs::path parent = exe_dir.parent_path() / "catalog";
    if (fs::is_directory(parent)) return parent;

#ifdef RETCOMM_CATALOG_DIR
    {
        const fs::path baked(RETCOMM_CATALOG_DIR);
        if (fs::is_directory(baked)) return baked;
    }
#endif

    throw std::runtime_error(
        "catalog not found; pass --catalog DIR or set RETCOMM_CATALOG");
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

} // namespace retcomm
