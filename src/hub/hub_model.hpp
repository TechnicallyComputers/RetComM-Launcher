#pragma once

#include "retcomm/app_state.hpp"
#include "retcomm/bios_index.hpp"
#include "retcomm/catalog.hpp"
#include "retcomm/config.hpp"
#include "retcomm/data_root_migrate.hpp"
#include "retcomm/install.hpp"
#include "retcomm/launch.hpp"
#include "retcomm/library_index.hpp"
#include "retcomm/paths.hpp"
#include "retcomm/psx_platform_settings.hpp"
#include "retcomm/romm_fetch.hpp"
#include "retcomm/texture_packs.hpp"

#include <atomic>
#include <cstddef>
#include <deque>
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
    // Drop per-title cmake build/ intermediates (keeps Play binary + saves).
    DeleteBuildData,
    // Move apps/<title> tree between configured install roots.
    MoveInstall,
    Launch,
    ScanRoms,
    FullScanRoms,
    ScanBios,
    FullScanBios,
    PurgeMissingFiles,
    CheckUpdates,
    CheckLaunchUpdate, // Play preflight on launch_worker (overlaps Install)
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
    CleanupOldReleases,
    // Wipe apps/*/src/*/build/ cmake trees for every install (keeps Play + saves).
    CleanupCmakeBuildDirs,
    // Prune old toolchains / sdks / unreferenced engines / release zips / idle builds.
    CleanupSharedCaches,
    // Purge every install root + managed saves_root + platform/game config prefs.
    DeleteAllAppsAndSaves,
    // Move config+data to a user-chosen root, then relaunch (portable / 2nd drive).
    MigrateDataRoot,
    // Wipe library/RomM config + indexes, clear setup marker, relaunch into wizard.
    HardResetLibrarySettings,
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
    case HubJob::DeleteBuildData:
    case HubJob::MoveInstall:
    case HubJob::DeleteAllAppsAndSaves:
    case HubJob::HardResetLibrarySettings:
    case HubJob::FetchRommRom:
    case HubJob::FetchRommBios:
        return true;
    default:
        return false;
    }
}

// Jobs that may wait behind the current main-worker job (Install/Update/scan queue).
inline bool hub_job_is_queueable(HubJob j) {
    switch (j) {
    case HubJob::Install:
    case HubJob::InstallPrebuilt:
    case HubJob::InstallWine:
    case HubJob::Update:
    case HubJob::GenerateRebuild:
    case HubJob::MoveInstall:
    case HubJob::ScanRoms:
    case HubJob::FullScanRoms:
    case HubJob::ScanBios:
    case HubJob::FullScanBios:
    case HubJob::PurgeMissingFiles:
        return true;
    default:
        return false;
    }
}

struct QueuedHubJob {
    HubJob job = HubJob::None;
    std::string title_id;
    bool force_boxart = false;
    bool fetch_romm_first = false;
    // Install family: apps root chosen in the location modal (empty → default).
    fs::path apps_dir;
    // Scan/Purge: platform scope at enqueue time (empty = all platforms).
    std::string platform_filter;
    bool prefetch_catalog = false;
};

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
    // Local build used OpenBIOS only (no SCPH1001 backend). Selecting a retail
    // dump flips Play → "Reinstall w BIOS".
    bool built_with_openbios = false;
    std::string rom_path;
    std::string suggested_rom; // catalog basename hint when unmatched
    std::string bios_path;
    // Dropdown: dump paths + optional kOpenBiosChoice. Labels parallel ids.
    std::vector<std::string> bios_choice_ids;
    std::vector<std::string> bios_choice_labels;
    int preferred_bios_index = -1;
    std::string bios_choice; // selected id (path or kOpenBiosChoice)
    // Active HD texture pack id, or "" for native textures. Just the selection —
    // the pack list itself is scanned on demand (HubModel::texpack_*) because
    // counting a few hundred PNGs per pack has no business running on every
    // library refresh.
    std::string active_texture_pack;
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
    // True when apps/<install>/src/*/build/ exists (cmake intermediates).
    bool has_cmake_build_data = false;
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
    int preferred_save_card2_index = -1; // -1 = Empty Card Slot

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

struct InstallRootEdit {
    char label[128]{};
    char path[1024]{};
};

struct SettingsDraft {
    char library_root[1024]{};
    char bios_root[1024]{};
    char saves_root[1024]{};
    char exclude_dirs[1024]{}; // comma-separated
    bool prefer_local_boxart = false;
    bool filter_unsupported_titles = false;
    bool check_updates_on_startup = true;
    bool check_updates_before_launch = true;
    char github_token[512]{}; // PAT for api.github.com (optional; env still wins)
    bool auto_clean_build_dirs = false;
    bool auto_gc_caches = true;
    int keep_toolchain_versions = 2;
    int keep_sdk_versions = 3;
    int keep_orphan_engine_pins = 1;
    int keep_release_zips_per_repo = 1;
    int idle_build_keep_days = 14;
    int ccache_max_gb = 5;
    std::vector<PlatformFolderEdit> platform_folders;
    std::vector<InstallRootEdit> install_roots;
    int default_install_root_index = 0;
    bool dirty = false;
};

struct RommSettingsDraft {
    char base_url[1024]{};
    char api_token[1024]{}; // Client API token (rmm_…)
    bool sync_boxart = false; // RomM covers; off = Libretro thumbnails
    bool dirty = false;
};

struct PsxSettingsDraft {
    PsxPlatformSettings settings;
    bool dirty = false;
    // Hotkey rebind: index into PsxPlatformSettings::hotkeys, or -1.
    int capturing_hotkey = -1;
    // Controller-hotkey rebind: 0 = rewind, 1 = save-state menu, -1 = idle.
    // A chord is committed on RELEASE, so holding select+r3 records both
    // buttons rather than the first one that happened to land.
    int capturing_pad_hotkey = -1;
    unsigned pad_hotkey_mask = 0;   // buttons seen held during this capture
    // false = Display/Audio/Input system page; true = Gamepads slots page.
    bool gamepads_tab = false;
    // Configure modal: 0..kMaxPlayers-1, or -1 when closed.
    int configuring_player = -1;
    // Bind capture inside Configure (Esc cancels; Map All walks all 24).
    int capturing_bind = -1; // button index, or -1
    bool capture_is_pad = false;
    bool map_all_active = false;
    bool map_all_wait_release = false;
    int map_all_step = 0;
    bool rename_open = false;
    char rename_buf[64]{};
};

// Target buffer for an in-flight SDL folder dialog (callback may be off-thread).
enum class FolderPickTarget : int {
    None = 0,
    LibraryRoot,
    BiosRoot,
    SavesRoot,
    EmulationRoot, // Easy setup: parent of roms/bios/saves
    InstallRoot,   // Library Settings → add/edit game install location
    DataRoot,      // Advanced setup / Settings: RetComM's own config+data folder
};

// First-run wizard path after the Easy / Advanced chooser.
enum class SetupPath : int {
    Chooser = 0,
    Easy = 1,
    Advanced = 2,
};

// Native file dialog for Library → Import / activity export (callback may be off-thread).
enum class FilePickKind : int {
    None = 0,
    ImportRom,
    ImportSave,
    ImportBios,
    ImportTexturePack,
    ExportActivityLog,
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
    bool show_psx_settings = false; // global PlayStation Configure page
    bool show_setup = false; // first-time library/BIOS/RomM wizard
    SetupPath setup_path = SetupPath::Chooser;
    // Advanced wizard: 0 = RetComM data folder, 1 = library roots (+ optional
    // RomM), 2 = platform folder mappings.
    int setup_step = 0;
    // Step 0: where RetComM keeps config + data. Empty buffer = OS default.
    bool setup_use_custom_data_root = false;
    char setup_data_root[1024]{};
    // Easy setup: Emulation parent folder (…/roms, …/bios, …/saves derived from this).
    char setup_emulation_root[1024]{};
    // After Next on step 0: confirm creating missing roms/saves/bios roots.
    bool setup_confirm_create_roots = false;
    std::vector<std::string> setup_missing_roots; // absolute paths to create
    bool setup_create_platform_folders = true;
    bool pending_open_library = false; // Top-bar Library → open modal next frame
    bool pending_open_menu = false;    // Top-bar Menu → open modal next frame
    // After setup Finish: ask whether to scan the library now.
    bool show_setup_scan_prompt = false;
    // Activity log collapsed by default; expand from the bottom bar.
    bool log_expanded = false;
    // When set, ScanRoms/FullScanRoms quietly syncs the catalog first.
    bool job_prefetch_catalog = false;
    // Library modal Advanced scope (empty = all catalog platforms).
    std::string scans_platform_filter;
    SettingsDraft settings;
    RommSettingsDraft romm_settings;
    PsxSettingsDraft psx_settings;
    NetplayLobbyState netplay;

    mutable std::mutex mu;
    std::string status; // last job/status line (also mirrored into log_lines)
    std::vector<LogLine> log_lines;
    std::string log; // plain joined text for clipboard / legacy callers
    std::atomic<bool> job_running{false};
    // Launch / CheckLaunchUpdate share a thread so Play works during Install.
    std::atomic<bool> launch_running{false};
    std::atomic<bool> request_exit{false}; // set after self-update schedules restart
    HubJob job = HubJob::None;
    std::string job_title_id;
    // Active scan/purge platform scope (mirrors QueuedHubJob::platform_filter).
    std::string job_platform_filter;
    bool job_force_boxart = false; // FetchBoxart: re-download even when cached
    bool job_fetch_romm_first = false;
    std::thread worker;
    std::thread launch_worker;
    // Install/Update/scan backlog while the main worker is busy (guarded by mu).
    std::deque<QueuedHubJob> job_queue;
    std::string launcher_version; // display: running binary version (RETCOMM_VERSION)

    // Shared cmake-clang-v1 toolchain update prompt (launch / Check Updates).
    std::atomic<bool> toolchain_prompt_pending{false};
    // RetComM launcher self-update prompt (shown before toolchain on startup).
    std::atomic<bool> launcher_update_prompt_pending{false};
    std::string launcher_current_version;
    std::string launcher_latest_tag;
    // After launcher prompt: summary when CheckUpdates finds outdated titles.
    std::atomic<bool> game_updates_prompt_pending{false};
    int game_updates_prompt_count = 0; // guarded by mu
    // Held while the launcher self-update modal is up — released on Later, dropped on Update.
    bool deferred_followup_updates = false; // guarded by mu
    int deferred_game_updates_count = 0;    // guarded by mu
    bool deferred_toolchain_prompt = false; // guarded by mu
    // Background release-zip prefetch (separate from job_running so Update still works).
    std::atomic<bool> prefetch_running{false};
    std::atomic<bool> prefetch_cancel{false}; // SelfUpdate / exit: stop between titles
    std::thread prefetch_worker;
    // Set by Install/Update/GenerateRebuild when skip_cache_gc; drained when the
    // main worker is idle and the queue is empty (out-of-process prune preferred).
    std::atomic<bool> pending_deferred_cache_gc{false};
    // After idle: CheckUpdates (catalog → launcher → toolchain → games).
    bool pending_startup_update_check = false;
    // Brief top-of-window notice (import / scan results). Main thread times display.
    std::string toast_message; // guarded by mu
    std::atomic<bool> toast_pending{false};
    // After Import ROM/BIOS: toast when the follow-up scan finishes.
    std::string pending_import_toast_name;
    std::string pending_import_toast_platform;
    std::string pending_import_toast_kind; // "rom" | "bios"
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
    int folder_pick_install_index = -1; // InstallRoot row being browsed

    // ---- RetComM data folder (portable / second-drive root) ----------------
    // Change requested from Settings or wizard Finish; applied by
    // HubJob::MigrateDataRoot, which relaunches on success.
    bool show_data_root_dialog = false;
    char data_root_input[1024]{};
    RootMigrationMode data_root_mode = RootMigrationMode::Move;
    RootMigrationPlan data_root_plan;      // recomputed when the input changes
    bool data_root_plan_dirty = true;
    std::string data_root_plan_path;       // input the plan was computed for
    // Set when the wizard's Finish should migrate before completing setup.
    // pending_data_root empty + pending_data_root_change true = back to default.
    fs::path pending_data_root;
    bool pending_data_root_change = false;

    // ---- per-title HD texture packs (Texture Packs modal) ------------------
    // Scanned on demand rather than in refresh_rows: the scan walks every pack
    // directory, and the list is only ever visible while the modal is open.
    std::string texpack_title_id;                     // which title the list is for
    std::vector<retcomm::TexturePack> texpack_list;
    std::string texpack_status;                       // last import/remove result
    void refresh_texture_packs(const std::string& title_id);
    // Select a pack (empty = native textures) and persist it to state.json.
    void set_texture_pack(const std::string& title_id, const std::string& pack_id);
    void remove_texture_pack_now(const std::string& title_id, const std::string& pack_id);

    // Library modal → Import platform (empty = none selected; no "All").
    std::string library_import_platform;
    std::mutex file_pick_mu;
    FilePickKind file_pick_kind = FilePickKind::None;
    std::string file_pick_platform;
    std::string file_pick_title_id; // optional: ImportSave binds preferred for this title
    std::vector<std::string> file_pick_paths; // set by dialog callback
    bool file_pick_busy = false;
    // Filter / default path strings must outlive SDL file dialogs until the callback.
    std::string file_pick_filter_name;
    std::string file_pick_filter_pattern;
    std::string file_pick_default_location;

    // check_updates: query GitHub latest tags for installed titles.
    // force_github_tags: ignore the release-tag TTL (manual Check for Updates).
    void refresh_rows(bool check_updates, bool force_github_tags = false);
    void append_log(const std::string& line);
    void append_log(const std::string& line, LogLevel level);
    void set_status(const std::string& s);
    // Queue a short-lived UI toast (shown on the main thread).
    void show_toast(const std::string& message);
    // Scan apps/ for installs not in the current catalog; store under pending_orphans.
    // Returns count found. Safe on UI or worker thread.
    size_t refresh_orphan_installs();
    // Download covers for catalog titles missing from the active cache (or all when force).
    // Safe to call from the hub worker thread.
    void fetch_boxart_for_catalog(bool force = false);
    // fetch_romm_first: Install/Build searches RomM + rescans before building when
    // the library has no verified ROM (set by the hub confirm modal).
    // Queueable jobs (Install/Update/library scan/…) enqueue when the main worker is busy.
    bool start_job(HubJob j, const std::string& title_id = {}, bool force_boxart = false,
                   bool fetch_romm_first = false);
    // Waiting queueable jobs (not including the one currently running).
    std::size_t queued_job_count() const;
    // title_id for install family; platform_filter for scan/purge (empty = all).
    bool is_job_queued(HubJob j, const std::string& title_id,
                       const std::string& platform_filter = {}) const;
    // True when any library scan/purge is already waiting in the backlog.
    bool has_queued_scan() const;
    // True if any queueable job for this title is waiting in the backlog.
    bool is_title_queued(const std::string& title_id) const;
    // Enqueue Update for every installed title with update_available; starts the
    // first when the worker is idle.
    int queue_all_updates();
    // Pop the next queued job and start it (no-op if busy or empty).
    bool start_next_queued_job();
    // When the worker is idle and the queue is empty, run deferred post-build
    // cache GC out-of-process (no-op if nothing pending / auto_gc off).
    void maybe_run_deferred_cache_gc();
    void join_worker();
    // Download pending game update zips into the release cache (non-blocking job slot).
    // empty ids → all rows with update_available. Safe to call from worker or UI thread.
    void start_prefetch_updates(const std::vector<std::string>& title_ids = {});
    // Ask the prefetch thread to stop between titles (does not join).
    void cancel_prefetch_updates();
    // After dismissing RetComM update with Later: show deferred game/toolchain prompts.
    void release_deferred_followup_updates();
    // Accepting RetComM self-update / restart: drop follow-up prompts so nothing else starts.
    void discard_followup_update_prompts();

    // Build & Install with no local ROM → quick scan / RomM download chooser.
    bool show_missing_rom_prompt = false;
    std::string missing_rom_prompt_id;
    // Multi-root Install / Move: choose apps/ location before ROM prompts / job.
    bool show_install_root_prompt = false;
    std::string install_root_prompt_id;
    int install_root_prompt_index = 0;
    // When true, confirm starts MoveInstall instead of Install.
    bool install_root_prompt_move = false;
    // Apps root where the title currently lives (empty if none); used to preselect
    // and label "(current)" in the location chooser.
    fs::path install_root_prompt_from_apps;
    // Apps root for the next Install/Update/Move job (empty → default / existing).
    fs::path job_apps_dir;
    // Start Install: may open install-root chooser. Returns true if job started
    // (or ROM prompt shown); false if waiting on install-root modal.
    bool begin_install(const std::string& title_id);
    // Open install-root chooser to relocate an existing install / preserved tree.
    bool begin_move_install(const std::string& title_id);
    void confirm_install_root_and_continue();
    // If Install will local-build and no verified ROM is bound, purge stale
    // index rows and open the missing-ROM chooser (rescan / RomM). Returns true
    // when the caller must not start Install (prompt shown or hard failure).
    bool prepare_build_rom_or_prompt(const std::string& title_id);
    void add_install_root_row();
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
    // Prefill Easy setup emulation parent (default ~/Emulation).
    void apply_suggested_emulation_root(bool overwrite_nonempty = false);
    // Derive library/bios/saves drafts from setup_emulation_root (+ /roms,/bios,/saves).
    void apply_roots_from_emulation_parent();
    // Seed settings.platform_folders from catalog + ES-DE defaults.
    void seed_setup_platform_folders();
    // Collect missing non-empty root paths into setup_missing_roots.
    void collect_missing_setup_roots();
    // Create paths in setup_missing_roots. Clears the list on success.
    bool create_missing_setup_roots(std::string* error = nullptr);
    // Create per-platform folders under configured roots (first CSV name).
    bool create_setup_platform_folders(std::string* error = nullptr);
    // Easy path: apply parent → create missing roots + platform folders → save → complete.
    bool finish_easy_setup(std::string* error = nullptr);
    // Continue / Skip: write install setup marker so the wizard does not reappear.
    bool complete_setup(std::string* error = nullptr);
    // Recompute data_root_plan for data_root_input when it changed. Cheap except
    // for the directory-size walk, so it only runs when the text actually moves.
    void refresh_data_root_plan();
    // Seed data_root_input / setup_use_custom_data_root from the live paths.
    void seed_data_root_input();
    // True when this install should persist its root beside the binary.
    bool prefers_portable_root_marker() const;
    bool save_settings(std::string* error = nullptr);
    void add_platform_folder_row();

    void open_romm_settings();
    // When refresh_boxart is false, persist only (setup wizard does not start a job).
    bool save_romm_settings(std::string* error = nullptr, bool refresh_boxart = true);

    void open_psx_settings();
    bool save_psx_settings(std::string* error = nullptr);
    // Manage Game Data: blacklist title from global platform Configure merge.
    bool set_title_exclude_platform_config(const std::string& title_id, bool exclude,
                                           std::string* error = nullptr);

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
    // for_card2: assign to memcard 2 without changing card 1 preference.
    bool create_title_save(const std::string& title_id, std::string* error = nullptr,
                           bool for_card2 = false);
    // Delete a managed save file. Clears preferences that pointed at it.
    bool delete_title_save(const std::string& title_id, const std::string& save_id,
                           std::string* error = nullptr);
    // Rename a managed save file (new_label = filename, extension optional).
    bool rename_title_save(const std::string& title_id, const std::string& save_id,
                           const std::string& new_label, std::string* error = nullptr);

    // Apply a completed folder pick into settings drafts (call from UI thread).
    void apply_pending_folder_pick();
    // Copy Library → Import picks into library/bios/saves trees; may start a scan.
    void apply_pending_file_pick();
};

} // namespace retcomm::hub
