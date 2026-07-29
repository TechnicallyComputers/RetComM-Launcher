#pragma once

#include "retcomm/app_state.hpp"
#include "retcomm/bios_index.hpp"
#include "retcomm/catalog.hpp"
#include "retcomm/config.hpp"
#include "retcomm/install.hpp"
#include "retcomm/launch.hpp"
#include "retcomm/library_index.hpp"
#include "retcomm/paths.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace retcomm::hub {

enum class HubJob : int {
    None = 0,
    Install,
    InstallWine,
    Update,
    Uninstall,
    UninstallPurge,
    Launch,
    ScanRoms,
    FullScanRoms,
    ScanBios,
    FullScanBios,
    CheckUpdates,
    FetchBoxart,
    FetchRommRom,
    FetchRommBios,
    SyncRommSaves,
    SyncRommStates,
    SelfUpdate,
    RefreshCatalog,
};

struct TitleRow {
    std::string id;
    std::string name;
    std::string platform;
    std::string kind;
    bool installed = false;
    std::string installed_tag;
    std::string latest_tag;
    bool update_available = false;
    bool has_rom = false;
    bool needs_bios = false;
    bool has_bios = false;
    std::string rom_path;
    std::string suggested_rom; // catalog basename hint when unmatched
    std::string bios_path;
    std::string install_root;
    std::string binary_path;
    std::string runtime; // "native" | "wine"
    std::string author;        // GitHub owner from release.github
    std::string github_url;    // source repo URL
    std::string boxart_path;   // local cover image when found
    bool can_wine_install = false;
    bool has_rom_identity = false;
    bool romm_ready = false; // base_url + api_token configured
    bool busy = false;

    // Managed native saves (install saves/) — labels for UI; ids are "saves/<file>".
    std::vector<std::string> save_ids;
    std::vector<std::string> save_labels;
    std::string preferred_save; // save id; empty = none / auto
    int preferred_save_index = -1;
};

struct PlatformFolderEdit {
    char platform[64]{};
    char folders[256]{}; // comma-separated folder names under library_root
};

struct SettingsDraft {
    char library_root[1024]{};
    char bios_root[1024]{};
    char exclude_dirs[1024]{}; // comma-separated
    bool prefer_local_boxart = false;
    std::vector<PlatformFolderEdit> platform_folders;
    bool dirty = false;
};

struct RommSettingsDraft {
    char base_url[1024]{};
    char api_token[1024]{}; // Client API token (rmm_…)
    bool sync_boxart = false; // RomM covers; off = Libretro thumbnails
    bool dirty = false;
};

// Target buffer for an in-flight SDL folder dialog (callback may be off-thread).
enum class FolderPickTarget : int {
    None = 0,
    LibraryRoot,
    BiosRoot,
};

struct HubModel {
    Paths paths;
    AppConfig cfg;
    Catalog catalog;
    LibraryIndex library;
    BiosIndex bios;
    AppState app_state;

    std::vector<TitleRow> rows;
    int selected = 0;
    bool show_settings = false;
    bool show_romm_settings = false;
    bool show_setup = false; // first-time library/BIOS wizard
    SettingsDraft settings;
    RommSettingsDraft romm_settings;

    std::mutex mu;
    std::string status;
    std::string log;
    std::atomic<bool> job_running{false};
    std::atomic<bool> request_exit{false}; // set after self-update schedules restart
    HubJob job = HubJob::None;
    std::string job_title_id;
    bool job_force_boxart = false; // FetchBoxart: re-download even when cached
    std::thread worker;
    std::string launcher_version; // display: installed/current tag

    // Native folder picker (SDL_ShowOpenFolderDialog) — apply on main thread.
    std::mutex folder_pick_mu;
    FolderPickTarget folder_pick_target = FolderPickTarget::None;
    std::string folder_pick_path;
    bool folder_pick_busy = false;

    void refresh_rows(bool check_updates);
    void append_log(const std::string& line);
    void set_status(const std::string& s);
    bool start_job(HubJob j, const std::string& title_id = {}, bool force_boxart = false);
    void join_worker();

    void open_settings();
    void open_setup();
    bool save_settings(std::string* error = nullptr);
    void add_platform_folder_row();

    void open_romm_settings();
    bool save_romm_settings(std::string* error = nullptr);

    // Persist preferred managed save for a title (empty clears).
    bool set_title_preferred_save(const std::string& title_id, const std::string& save_id,
                                  std::string* error = nullptr);

    // Apply a completed folder pick into settings drafts (call from UI thread).
    void apply_pending_folder_pick();
};

} // namespace retcomm::hub
