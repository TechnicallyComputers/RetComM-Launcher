#pragma once

#include "retcomm/catalog.hpp"
#include "retcomm/paths.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace retcomm {

namespace fs = std::filesystem;

struct InstallPlan {
    const Title* title = nullptr;
    fs::path install_root;     // apps/<install_dir_name>
    fs::path current_link;     // install_root/current
    fs::path binary_path;      // resolved launch binary under current/
    bool installed = false;
    std::string message;       // human-readable plan / status
};

InstallPlan inspect_install(const Paths& paths, const Title& title);

// MVP stub: prints/returns the plan for fetching GitHub releases. Does not download.
InstallPlan plan_install(const Paths& paths, const Title& title);

// MVP stub: does not spawn; resolves binary and reports how launch would work.
struct LaunchPlan {
    const Title* title = nullptr;
    fs::path binary;
    std::vector<std::string> argv;
    bool ready = false;
    std::string message;
};

LaunchPlan plan_launch(const Paths& paths, const Title& title,
                       const fs::path& rom_path = {});

} // namespace retcomm
