#pragma once

#include "retcomm/app_state.hpp"
#include "retcomm/bios_index.hpp"
#include "retcomm/catalog.hpp"
#include "retcomm/config.hpp"
#include "retcomm/install.hpp"
#include "retcomm/launch.hpp"
#include "retcomm/library_index.hpp"
#include "retcomm/paths.hpp"
#include "retcomm/romm_fetch.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace retcomm::hub {

// Activity log line severity (drives hub UI colors).
enum class LogLevel : int {
    Info = 0, // muted
    Accent,   // notable (catalog / toolchain notices)
    Good,     // success
    Warn,     // recoverable / missing
    Error,    // failures
};

struct LogLine {
    LogLevel level = LogLevel::Info;
    std::string text;
};

enum class HubJob : int {
    None = 0,
    Install,
    InstallPrebuilt,
    InstallWine,
    Update,
    // Force disc→C generate + cmake rebuild (ignores codegen-cache).
    GenerateRebuild,
    Uninstall,
    UninstallPurge,
    Launch,
    ScanRoms,
    FullScanRoms,
    ScanBios,
    FullScanBios,
    PurgeMissingFiles,
    CheckUpdates,
    CheckLaunchUpdate, // Play → check this title, then launch or prompt
    FetchBoxart,
    FetchRommRom,
    FetchRommBios,
    ScanRommRoms,
    SyncRommSaves,
    SyncRommStates,
    SelfUpdate,
    RefreshCatalog,
    CheckToolchainUpdate,
    UpdateToolchain,
    CleanupOrphans,
    CleanupOrphansPurge,
};

// Title install/build/update family — mutually exclusive with library scans.
inline bool hub_job_is_install(HubJob j) {
    switch (j) {
    case HubJob::Install:
    case HubJob::InstallPrebuilt:
    case HubJob::InstallWine:
    case HubJob::Update:
    case HubJob::GenerateRebuild:
    case HubJob::Uninstall:
    case HubJob::UninstallPurge:
    case HubJob::FetchRommRom:
    case HubJob::FetchRommBios:
        return true;
    default:
        return false;
    }
}

// ROM / BIOS / RomM library index scans — mutually exclusive with install jobs.
inline bool hub_job_is_scan(HubJob j) {
    switch (j) {
    case HubJob::ScanRoms:
    case HubJob::FullScanRoms:
    case HubJob::ScanBios:
    case HubJob::FullScanBios:
    case HubJob::PurgeMissingFiles:
    case HubJob::ScanRommRoms:
        return true;
    default:
        return false;
    }
}

struct TitleRow {
    std::string id;
    std::string name;
    std::string platform;
    std::string description; // catalog marketing blurb (detail panel)
    std::string kind;
    bool installed = false;
    // apps/<dir> has leftover install/build artifacts but no launch binary (partial install).
    bool install_dir_present = false;
    // apps/<dir>/preserved/ from keep-saves uninstall (not a broken install).
    bool has_preserved_state = false;
    std::string install_issue;   // human-readable reason when !installed && dir present
    std::string expected_binary; // catalog launch name looked for
    std::string installed_tag;
    // Release tag used for update compares (source_ref for build installs).
    std::string release_compare_tag;
    std::string latest_tag;
    bool update_available = false;
    bool has_rom = false;
    bool has_romm = false; // cached RomM identity match (may not be on disk yet)
    std::string romm_file_name; // remote dump name when has_romm
    bool needs_bios = false;
    bool has_bios = false;
    bool supports_openbios = false; // psxrecomp titles can generate MIT OpenBIOS
    std::string rom_path;
    std::string suggested_rom; // catalog basename hint when unmatched
    std::string bios_path;
    // Dropdown: dump paths + optional kOpenBiosChoice. Labels parallel ids.
    std::vector<std::string> bios_choice_ids;
    std::vector<std::string> bios_choice_labels;
    int preferred_bios_index = -1;
    std::string bios_choice; // selected id (path or kOpenBiosChoice)
    std::string install_root;
    std::string binary_path;
    std::string runtime; // "native" | "wine"
    std::string author;        // GitHub owner from release.github
    std::string github_url;    // source repo URL
    std::string author_notes;  // Optional message from the recomp/decomp author
    std::string boxart_path;   // local cover image when found
    bool can_wine_install = false;
    bool supports_local_build = false;
    bool can_prebuilt_install = false;
    // install.json method: "build" | "zip" | "" (unknown / not installed).
    std::string install_method;
    bool has_rom_identity = false;
    bool romm_ready = false; // base_url + api_token configured
    bool busy = false;

    // Managed native saves (library saves_root or install saves/) — ids "saves/<file>".
    std::vector<std::string> save_ids;
    std::vector<std::string> save_labels;
    bool dual_memcard = false; // disc / memcard_glob titles → two dropdowns
    std::string preferred_save; // cart battery or memcard 1 id
    int preferred_save_index = -1;
    std::string preferred_save_card2; // memcard 2 id; "__blank__" = empty card
    int preferred_save_card2_index = -1; // -1 = Blank card

    // Catalog netplay (recomp-net). Populated when Title::supports_netplay().
    bool netplay_supported = false;
    std::string netplay_game_name;
    std::string netplay_game_version;
    std::string netplay_lobby_url; // resolved: title override || config || default
    int netplay_max_slots = 2;
    bool netplay_joinable = false;     // installed + supported
    bool netplay_version_ok = false;   // installed_tag matches catalog pin (advisory)
};

enum class NetplayView : int {
    Hidden = 0,
    Browser, // cross-game room list
    Room,    // seated in a lobby
};

struct NetplayRoomRow {
    std::string lobby_id;
    std::string name;
    std::string game_name;
    std::string game_version;
    std::string catalog_id; // resolved via game_name when possible
    int players = 0;
    int max_slots = 2;
    bool has_password = false;
    bool joinable_locally = false; // matching install available
};

struct NetplaySlot {
    int slot = 0;
    std::string player_id;
    std::string display_name;
    bool ready = false;
};

struct NetplayLobbyState {
    NetplayView view = NetplayView::Hidden;
    std::string lobby_url;
    std::string display_name;
    bool connected = false;
    std::string status;
    std::string filter_catalog_id; // empty = all netplay titles

    std::vector<NetplayRoomRow> rooms;

    // Active room (after create/join)
    std::string lobby_id;
    std::string room_name;
    std::string game_name;
    std::string game_version;
    std::string catalog_id;
    bool is_host = false;
    int local_slot = -1;
    std::string session_id;
    std::string host_endpoint;
    std::string guest_endpoint;
    std::vector<NetplaySlot> slots;
    std::string match_caps_json; // opaque server echo

    // Host create draft
    std::string create_catalog_id;
    char create_room_name[128]{};
    char create_password[64]{};
    int create_udp_port = 7777;
};

// Future handoff into LaunchMode::Netplay (env + spawn).
struct NetplayLaunchRequest {
    std::string catalog_id;
    bool is_host = false;
    int local_slot = 0;
    std::string session_id;
    std::string peer_endpoint;
    std::string lobby_url;
    std::string match_caps_json;
};

struct PlatformFolderEdit {
    char platform[64]{};
    char folders[256]{}; // comma-separated folder names under library_root
};

struct SettingsDraft {
    char library_root[1024]{};
    char bios_root[1024]{};
    char saves_root[1024]{};
    char exclude_dirs[1024]{}; // comma-separated
    bool prefer_local_boxart = false;
    bool filter_unsupported_titles = false;
    bool check_updates_before_launch = true;
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
    SavesRoot,
};

// Center library panel: platforms list → titles for a platform (or all).
enum class LibraryNav : int {
    Platforms = 0,
    Titles,
};

struct HubModel {
    Paths paths;
    fs::path exe_dir; // directory containing retcomm-hub (install / portable root)
    AppConfig cfg;
    Catalog catalog;
    LibraryIndex library;
    BiosIndex bios;
    RommRomIndex romm_roms;
    AppState app_state;

    std::vector<TitleRow> rows;
    int selected = 0;
    // Set when a library boxart is clicked (even if already selected) → detail scrolls to top.
    bool detail_scroll_top = false;
    LibraryNav library_nav = LibraryNav::Platforms;
    // When library_nav == Titles: empty = all platforms, else catalog platform slug.
    std::string library_platform;
    bool show_settings = false;
    bool show_romm_settings = false;
    bool show_setup = false; // first-time library/BIOS/RomM wizard
    // Setup wizard: 0 = roots (+ optional RomM), 1 = platform folder mappings.
    int setup_step = 0;
    // After Next on step 0: confirm creating missing roms/saves/bios roots.
    bool setup_confirm_create_roots = false;
    std::vector<std::string> setup_missing_roots; // absolute paths to create
    bool setup_create_platform_folders = true;
    bool pending_open_scans = false;   // Top-bar Scans → open modal next frame
    bool pending_open_updates = false; // Top-bar Updates → open modal next frame
    bool pending_open_menu = false;    // Top-bar Menu → open modal next frame
    // After setup Finish: ask whether to scan the library now.
    bool show_setup_scan_prompt = false;
    // When set, ScanRoms/FullScanRoms quietly syncs the catalog first.
    bool job_prefetch_catalog = false;
    // Scans modal platform filter (empty = all catalog platforms).
    std::string scans_platform_filter;
    SettingsDraft settings;
    RommSettingsDraft romm_settings;
    NetplayLobbyState netplay;

    std::mutex mu;
    std::string status; // last job/status line (also mirrored into log_lines)
    std::vector<LogLine> log_lines;
    std::string log; // plain joined text for clipboard / legacy callers
    std::atomic<bool> job_running{false};
    // Launch runs on its own thread so Play stays usable during Build & Install.
    std::atomic<bool> launch_running{false};
    std::atomic<bool> request_exit{false}; // set after self-update schedules restart
    HubJob job = HubJob::None;
    std::string job_title_id;
    bool job_force_boxart = false; // FetchBoxart: re-download even when cached
    bool job_fetch_romm_first = false;
    std::thread worker;
    std::thread launch_worker;
    std::string launcher_version; // display: running binary version (RETCOMM_VERSION)

    // Shared cmake-clang-v1 toolchain update prompt (launch / Check Updates).
    std::atomic<bool> toolchain_prompt_pending{false};
    // After idle: CheckUpdates (installed games + toolchain).
    bool pending_startup_update_check = false;
    // Play preflight: update available → confirm; else launch this id on main thread.
    std::atomic<bool> launch_update_prompt_pending{false};
    std::string launch_update_prompt_id; // guarded by mu
    std::string launch_update_from;
    std::string launch_update_to;
    std::string pending_launch_title_id; // guarded by mu; main thread starts Launch
    std::string toolchain_current_version;
    std::string toolchain_latest_tag;
    std::string toolchain_status; // short UI line
    bool toolchain_update_available = false;

    // Apps/index entries left after a catalog title was removed (Refresh Catalog).
    std::atomic<bool> orphan_prompt_pending{false};
    std::vector<OrphanInstall> pending_orphans; // guarded by mu

    // Native folder picker (SDL_ShowOpenFolderDialog) — apply on main thread.
    std::mutex folder_pick_mu;
    FolderPickTarget folder_pick_target = FolderPickTarget::None;
    std::string folder_pick_path;
    bool folder_pick_busy = false;

    void refresh_rows(bool check_updates);
    void append_log(const std::string& line);
    void append_log(const std::string& line, LogLevel level);
    void set_status(const std::string& s);
    // Scan apps/ for installs not in the current catalog; store under pending_orphans.
    // Returns count found. Safe on UI or worker thread.
    size_t refresh_orphan_installs();
    // Download covers for catalog titles missing from the active cache (or all when force).
    // Safe to call from the hub worker thread.
    void fetch_boxart_for_catalog(bool force = false);
    // fetch_romm_first: Install/Build searches RomM + rescans before building when
    // the library has no verified ROM (set by the hub confirm modal).
    bool start_job(HubJob j, const std::string& title_id = {}, bool force_boxart = false,
                   bool fetch_romm_first = false);
    void join_worker();

    // Build & Install with no local ROM → quick scan / RomM download chooser.
    bool show_missing_rom_prompt = false;
    std::string missing_rom_prompt_id;
    // If the library DB still points at a missing dump, purge stale rows for the
    // title's platform and open the missing-ROM chooser. Returns true when the
    // caller must not start Install (prompt shown or still no ROM).
    bool prepare_build_rom_or_prompt(const std::string& title_id);
    // Set when Quick Scan is started from the missing-ROM prompt; after scan,
    // if still unbound, open_rom_folder_prompt_pending asks to open the folder.
    std::string pending_scan_missing_rom_id;
    std::atomic<bool> open_rom_folder_prompt_pending{false};
    std::string open_rom_folder_prompt_id;       // guarded by mu
    std::string open_rom_folder_prompt_platform; // catalog platform slug

    void open_settings();
    void open_setup();
    // Prefill empty library/bios/saves drafts from ~/Emulation/{roms,bios,saves}.
    void apply_suggested_library_roots(bool overwrite_nonempty = false);
    // Seed settings.platform_folders from catalog + ES-DE defaults.
    void seed_setup_platform_folders();
    // Collect missing non-empty root paths into setup_missing_roots.
    void collect_missing_setup_roots();
    // Create paths in setup_missing_roots. Clears the list on success.
    bool create_missing_setup_roots(std::string* error = nullptr);
    // Create per-platform folders under configured roots (first CSV name).
    bool create_setup_platform_folders(std::string* error = nullptr);
    // Continue / Skip: write install setup marker so the wizard does not reappear.
    bool complete_setup(std::string* error = nullptr);
    bool save_settings(std::string* error = nullptr);
    void add_platform_folder_row();

    void open_romm_settings();
    // When refresh_boxart is false, persist only (setup wizard does not start a job).
    bool save_romm_settings(std::string* error = nullptr, bool refresh_boxart = true);

    // Persist preferred managed save for a title (empty clears).
    // Cart: battery file. Disc: memcard 1.
    bool set_title_preferred_save(const std::string& title_id, const std::string& save_id,
                                  std::string* error = nullptr);
    // Disc memcard 2. Pass kBlankMemcardId / empty for a blank card2.mcd.
    bool set_title_preferred_save_card2(const std::string& title_id, const std::string& save_id,
                                        std::string* error = nullptr);

    // Persist BIOS choice: absolute dump path, or kOpenBiosChoice.
    bool set_title_preferred_bios(const std::string& title_id, const std::string& bios_choice,
                                  std::string* error = nullptr);

    // Mint a new empty managed save in the library (or install saves/), set preferred.
    bool create_title_save(const std::string& title_id, std::string* error = nullptr);

    // Apply a completed folder pick into settings drafts (call from UI thread).
    void apply_pending_folder_pick();
};

} // namespace retcomm::hub
