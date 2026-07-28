#include "retcomm/install.hpp"
#include "retcomm/paths.hpp"

#include <sstream>

namespace retcomm {

InstallPlan inspect_install(const Paths& paths, const Title& title) {
    InstallPlan plan;
    plan.title = &title;
    plan.install_root = paths.apps_dir / title.install_dir_name;
    plan.current_link = plan.install_root / "current";

    const std::string& bin = title.launch_binary_for_host();
    if (fs::exists(plan.current_link)) {
        plan.binary_path = plan.current_link / bin;
        plan.installed = fs::exists(plan.binary_path);
        if (plan.installed) {
            plan.message = "installed: " + plan.binary_path.string();
        } else {
            plan.message =
                "current/ present but binary missing: " + plan.binary_path.string();
        }
    } else if (!bin.empty() && fs::exists(plan.install_root / bin)) {
        plan.binary_path = plan.install_root / bin;
        plan.installed = true;
        plan.message = "installed (flat): " + plan.binary_path.string();
    } else {
        plan.message = "not installed under " + plan.install_root.string();
    }
    return plan;
}

InstallPlan plan_install(const Paths& paths, const Title& title) {
    InstallPlan plan = inspect_install(paths, title);
    std::ostringstream oss;
    oss << "install plan for " << title.id << "\n"
        << "  target:  " << plan.install_root.string() << "\n"
        << "  github:  "
        << (title.release.github.empty() ? "(unset)" : title.release.github) << "\n"
        << "  asset:   " << title.asset_glob_for_host() << "\n"
        << "  binary:  " << title.launch_binary_for_host() << "\n";
    if (plan.installed) {
        oss << "  status:  already present — update check not implemented yet\n";
    } else {
        oss << "  status:  STUB — download/extract from GitHub Releases not wired yet\n";
    }
    plan.message = oss.str();
    return plan;
}

LaunchPlan plan_launch(const Paths& paths, const Title& title, const fs::path& rom_path) {
    LaunchPlan lp;
    lp.title = &title;
    const InstallPlan inst = inspect_install(paths, title);
    lp.binary = inst.binary_path;
    if (!inst.installed) {
        lp.ready = false;
        lp.message = "cannot launch: " + inst.message + "\n  tip: retcomm install " +
                     title.id + "\n";
        return lp;
    }
    lp.argv.push_back(lp.binary.string());
    if (!rom_path.empty()) lp.argv.push_back(rom_path.string());
    lp.ready = true;
    std::ostringstream oss;
    oss << "launch plan for " << title.id << "\n"
        << "  binary: " << lp.binary.string() << "\n";
    if (!rom_path.empty())
        oss << "  rom:    " << rom_path.string() << "\n";
    else
        oss << "  rom:    (none staged — game recomp-ui will prompt)\n";
    oss << "  status: STUB — process spawn not wired yet; would exec argv above\n";
    lp.message = oss.str();
    return lp;
}

} // namespace retcomm
