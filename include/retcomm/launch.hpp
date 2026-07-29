#pragma once

#include "retcomm/catalog.hpp"
#include "retcomm/paths.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace retcomm {

namespace fs = std::filesystem;

// How RetComM boots a title. Default opens the game's dedicated recomp-ui
// launcher. Netplay is reserved for a future lobby frontend.
enum class LaunchMode {
    Default, // dedicated launcher / normal UI (--launcher)
    Direct,  // skip launcher UI when the title supports --no-launcher
    Netplay, // not implemented yet (generic lobby later)
};

struct LaunchOptions {
    LaunchMode mode = LaunchMode::Default;
    fs::path rom_path;  // optional; library index used when empty at CLI layer
    fs::path bios_path; // optional; bios index used when empty at CLI layer
    // Optional native save (host path or release-relative "saves/Foo.sav").
    // Cart: --save-path; disc: settings.toml [memcard] card1 via bind.
    fs::path save_path;
    bool detach = false;
    bool dry_run = false;
};

struct LaunchPlan {
    const Title* title = nullptr;
    LaunchMode mode = LaunchMode::Default;
    fs::path binary;
    fs::path cwd; // usually binary parent (release dir)
    fs::path media_path; // ROM/disc actually passed / staged (host path)
    fs::path bios_path;  // BIOS passed / staged (host path)
    fs::path save_path;  // preferred native save (host path)
    fs::path staged_cfg;      // rom.cfg or disc.cfg
    fs::path staged_bios_cfg; // bios.cfg
    fs::path staged_settings; // settings.toml ([bios]/[disc] for psxrecomp)
    std::vector<std::string> argv;
    bool use_wine = false;
    bool ready = false;
    std::string message;
};

struct LaunchResult {
    bool ok = false;
    bool dry_run = false;
    int exit_code = 0; // meaningful when !detach and process exited
    LaunchPlan plan;
    std::string message;
};

LaunchMode parse_launch_mode(const std::string& s, std::string* error = nullptr);
const char* launch_mode_name(LaunchMode mode);

// Build argv/cwd for the title. Does not spawn.
LaunchPlan plan_launch(const Paths& paths, const Title& title,
                       const LaunchOptions& opts = {});

// Plan + spawn (unless dry_run). Waits for the child unless detach.
LaunchResult launch_title(const Paths& paths, const Title& title,
                          const LaunchOptions& opts = {});

} // namespace retcomm
