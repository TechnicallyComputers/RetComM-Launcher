#pragma once

#include <filesystem>
#include <string>

namespace retcomm {

namespace fs = std::filesystem;

// Where a custom RetComM root came from. Resolution order is the enum order
// below (Env wins, Default means "no override — use OS conventions").
enum class DataRootSource : int {
    Default = 0,   // XDG / AppData, exactly as RetComM has always behaved
    Env,           // $RETCOMM_HOME
    ExeMarker,     // <exe_dir>/retcomm-root.json — travels with a portable build
    ConfigPointer, // <os_default_config>/retcomm/root.json — written by the wizard
    Explicit,      // --root on the command line; never persisted
};

struct DataRootInfo {
    fs::path root;                                   // empty when source == Default
    DataRootSource source = DataRootSource::Default;
    fs::path pointer_file;                           // file that supplied it (diagnostics)
};

// Human-readable source name for logs / the Settings panel.
const char* data_root_source_label(DataRootSource s);

// Resolve the active root. `exe_dir` may be empty (skips the exe marker).
// A relative "root" in a marker resolves against the file's own directory, so a
// USB stick whose drive letter changes still works ({"root": "RetComM-Data"}).
DataRootInfo resolve_data_root(const fs::path& exe_dir);

// Default (no-override) config dir — where the ConfigPointer lives. Exposed so
// the UI can show what "use the default location" actually means.
fs::path default_os_config_dir();
fs::path default_os_data_dir();

// Read just one marker file, without applying precedence. Empty on any failure.
fs::path read_data_root_marker(const fs::path& marker_file);

// Persist `root` so the next launch resolves it. When `prefer_exe_marker` is set
// (portable builds) the marker is written beside the binary and the config
// pointer is removed; otherwise the config pointer is written.
// An empty `root` clears both, restoring the OS default.
bool write_data_root_pointer(const fs::path& exe_dir, const fs::path& root,
                             bool prefer_exe_marker, std::string* error = nullptr);

// Remove both markers (exe-adjacent and config pointer). Missing files are not
// an error.
bool clear_data_root_pointer(const fs::path& exe_dir, std::string* error = nullptr);

// True when `dir` could be created and written to. Creates nothing: when `dir`
// does not exist yet, the nearest existing ancestor is probed instead. Setup
// re-plans on every keystroke, so creating as-you-type would leave a folder
// behind for each prefix of a typed path.
bool directory_is_writable(const fs::path& dir, std::string* error = nullptr);

} // namespace retcomm
