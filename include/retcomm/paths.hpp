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
    fs::path state_path;          // data_dir/state.json
    fs::path config_path;         // config_dir/config.json
    fs::path library_index_path;  // data_dir/library-index.json
    fs::path bios_index_path;     // data_dir/bios-index.json
};

Paths default_paths();
void ensure_dirs(const Paths& p);

// Resolve catalog directory: --catalog, $RETCOMM_CATALOG, then next to the exe
// (../catalog when running from build/), then compile-time RETCOMM_CATALOG_DIR.
fs::path resolve_catalog_dir(const fs::path& exe_dir,
                             const fs::path& override_dir = {});

std::string host_os_key(); // "linux" | "windows" | "macos"

// Open a directory (or a file's parent) in the OS file manager, detached.
// Uses explorer / open / xdg-open as appropriate. Returns false on failure.
bool open_path_in_file_manager(const fs::path& path, std::string* error = nullptr);

// Open an http(s) URL in the default browser (xdg-open / open / ShellExecute).
bool open_url_in_browser(const std::string& url, std::string* error = nullptr);

} // namespace retcomm
