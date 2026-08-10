#pragma once

#include <filesystem>
#include <string>

namespace retcomm {

namespace fs = std::filesystem;

// XDG-style (and Windows-friendly) data/config roots for RetComM.
struct Paths {
    fs::path config_dir;          // ~/.config/retcomm
    fs::path data_dir;            // ~/.local/share/retcomm
    fs::path apps_dir;            // data_dir/apps
    fs::path toolchains_dir;      // data_dir/toolchains/<id>/<tag>
    fs::path sdks_dir;            // data_dir/sdks/<id>/<tag>
    fs::path state_path;          // data_dir/state.json
    fs::path config_path;         // config_dir/config.json
    fs::path library_index_path;  // data_dir/library-index.json
    fs::path bios_index_path;     // data_dir/bios-index.json
    fs::path romm_rom_index_path; // data_dir/romm-rom-index.json (RomM availability cache)
    fs::path catalog_dir;         // data_dir/catalog (remote catalog cache)
};

Paths default_paths();
void ensure_dirs(const Paths& p);
// Ensure config/data dirs; also create apps_dir when missing.
void ensure_apps_dir(const Paths& p);

// USERPROFILE (Windows) or HOME. Empty when unset.
fs::path user_home_dir();

// Resolve catalog directory: --catalog, $RETCOMM_CATALOG, then on-device cache
// under paths->catalog_dir (~/.local/share/retcomm/catalog). No bundled catalog.
fs::path resolve_catalog_dir(const fs::path& exe_dir,
                             const fs::path& override_dir = {},
                             const Paths* paths = nullptr);

std::string host_os_key(); // "linux" | "windows" | "macos"

// Open a directory (or a file's parent) in the OS file manager, detached.
// Uses explorer / open / xdg-open as appropriate. Returns false on failure.
bool open_path_in_file_manager(const fs::path& path, std::string* error = nullptr);

// Open an http(s) URL in the default browser (xdg-open / open / ShellExecute).
bool open_url_in_browser(const std::string& url, std::string* error = nullptr);

// Hub first-run wizard marker. Durable copy under data_dir (survives app
// self-update); best-effort copy next to the binary.
bool hub_setup_completed(const Paths& paths, const fs::path& exe_dir);
bool mark_hub_setup_completed(const Paths& paths, const fs::path& exe_dir,
                              std::string* error = nullptr);
// Remove data-dir + exe-dir markers so the next launch can show first-run setup.
bool clear_hub_setup_completed(const Paths& paths, const fs::path& exe_dir,
                               std::string* error = nullptr);

} // namespace retcomm
