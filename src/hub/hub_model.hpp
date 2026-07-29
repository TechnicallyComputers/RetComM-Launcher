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
    ScanRommRoms,
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
    // apps/<dir> exists but launch binary was not resolved (partial / mismatched install).
    bool install_dir_present = false;
    std::string install_issue;   // human-readable reason when !installed && dir present
    std::string expected_binary; // catalog launch name looked for
    std::string installed_tag;
    std::string latest_tag;
    bool update_available = false;
    bool has_rom = false;
    bool has_romm = false; // cached RomM identity match (may not be on disk yet)
    std::string romm_file_name; // remote dump name when has_romm
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
    std::string author_notes;  // Optional message from the recomp/decomp author
    std::string boxart_path;   // local cover image when found
    bool can_wine_install = false;
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
    AppConfig cfg;
    Catalog catalog;
    LibraryIndex library;
    BiosIndex bios;
    RommRomIndex romm_roms;
    AppState app_state;

    std::vector<TitleRow> rows;
    int selected = 0;
    LibraryNav library_nav = LibraryNav::Platforms;
    // When library_nav == Titles: empty = all platforms, else catalog platform slug.
    std::string library_platform;
    bool show_settings = false;
    bool show_romm_settings = false;
    bool show_setup = false; // first-time library/BIOS wizard
    SettingsDraft settings;
    RommSettingsDraft romm_settings;
    NetplayLobbyState netplay;

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
    // Cart: battery file. Disc: memcard 1.
    bool set_title_preferred_save(const std::string& title_id, const std::string& save_id,
                                  std::string* error = nullptr);
    // Disc memcard 2. Pass kBlankMemcardId / empty for a blank card2.mcd.
    bool set_title_preferred_save_card2(const std::string& title_id, const std::string& save_id,
                                        std::string* error = nullptr);

    // Mint a new empty managed save in the library (or install saves/), set preferred.
    bool create_title_save(const std::string& title_id, std::string* error = nullptr);

    // Apply a completed folder pick into settings drafts (call from UI thread).
    void apply_pending_folder_pick();
};

} // namespace retcomm::hub
