#include "retcomm/data_root_migrate.hpp"

#include "retcomm/config.hpp"

#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace retcomm {
namespace {

// True when `path` is at or below `prefix`. Both are compared lexically after
// normalization; neither needs to exist.
bool path_under(const fs::path& path, const fs::path& prefix) {
    if (path.empty() || prefix.empty()) return false;
    const fs::path a = path.lexically_normal();
    const fs::path b = prefix.lexically_normal();
    auto ai = a.begin();
    auto bi = b.begin();
    for (; bi != b.end(); ++bi, ++ai) {
        if (ai == a.end()) return false;
#if defined(_WIN32)
        // Windows paths are case-insensitive; compare folded.
        std::wstring as = ai->wstring(), bs = bi->wstring();
        if (_wcsicmp(as.c_str(), bs.c_str()) != 0) return false;
#else
        if (*ai != *bi) return false;
#endif
    }
    return true;
}

bool is_dir_link(const fs::path& p) {
    std::error_code ec;
#if defined(_WIN32)
    // Junctions are reparse points but not always reported as symlinks.
    const DWORD attrs = GetFileAttributesW(p.wstring().c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return true;
#endif
    return fs::is_symlink(fs::symlink_status(p, ec));
}

// Remove a link without descending into (or deleting) whatever it points at.
bool remove_link(const fs::path& p) {
    std::error_code ec;
#if defined(_WIN32)
    const DWORD attrs = GetFileAttributesW(p.wstring().c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0)
            return RemoveDirectoryW(p.wstring().c_str()) != 0;
        return DeleteFileW(p.wstring().c_str()) != 0;
    }
#endif
    fs::remove(p, ec);
    return !ec;
}

#if defined(_WIN32)
// mklink /J — no admin or Developer Mode needed, unlike CreateSymbolicLinkW.
// Mirrors the helpers in toolchain_env.cpp / build.cpp; kept local so migration
// does not depend on either translation unit.
bool win_make_junction(const fs::path& link, const fs::path& target) {
    auto strip = [](std::wstring p) {
        if (p.size() >= 8 && _wcsnicmp(p.c_str(), L"\\\\?\\UNC\\", 8) == 0)
            p.replace(0, 8, L"\\\\");
        else if (p.size() >= 4 && _wcsnicmp(p.c_str(), L"\\\\?\\", 4) == 0)
            p.erase(0, 4);
        return p;
    };
    wchar_t sys[MAX_PATH]{};
    if (GetSystemDirectoryW(sys, MAX_PATH) == 0) return false;
    std::wstring cmd_exe = std::wstring(sys) + L"\\cmd.exe";
    std::wstring cmd = L"\"" + cmd_exe + L"\" /C mklink /J \"" + strip(link.wstring()) +
                       L"\" \"" + strip(target.wstring()) + L"\"";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(cmd_exe.c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}
#endif

bool make_dir_link(const fs::path& link, const fs::path& target) {
    std::error_code ec;
#if defined(_WIN32)
    if (win_make_junction(link, target)) return true;
    const std::wstring lw = link.wstring(), tw = target.wstring();
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif
    if (CreateSymbolicLinkW(lw.c_str(), tw.c_str(),
                            SYMBOLIC_LINK_FLAG_DIRECTORY |
                                SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) ||
        CreateSymbolicLinkW(lw.c_str(), tw.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY))
        return true;
    return false;
#else
    fs::create_directory_symlink(target, link, ec);
    return !ec;
#endif
}

// Retarget one link if it points into old_data. Returns true when rewritten.
bool retarget_one(const fs::path& link, const fs::path& old_data, const fs::path& new_data,
                  std::vector<std::string>* notes) {
    if (!is_dir_link(link)) return false;
    std::error_code ec;
    const fs::path target = fs::read_symlink(link, ec);
    if (ec || target.empty()) return false;
    if (!path_under(target, old_data)) return false;

    const fs::path rel = target.lexically_normal().lexically_relative(old_data.lexically_normal());
    if (rel.empty() || rel.native().rfind(fs::path("..").native(), 0) == 0) return false;
    const fs::path want = new_data / rel;

    if (!remove_link(link)) {
        if (notes) notes->push_back("could not replace stale link " + link.string());
        return false;
    }
    if (!make_dir_link(link, want)) {
        // Leaving it absent is recoverable — the build path recreates engine
        // links, and a toolchain ensure republishes latest/. Say so out loud.
        if (notes)
            notes->push_back("removed stale link " + link.string() +
                             " but could not recreate it; it will be rebuilt on next use");
        return false;
    }
    return true;
}

// Move `from` to `to`, falling back to copy+remove across filesystems.
bool move_tree(const fs::path& from, const fs::path& to, std::string* error) {
    std::error_code ec;
    if (!fs::exists(from, ec)) return true; // nothing to move

    fs::create_directories(to.parent_path(), ec);
    ec.clear();
    fs::rename(from, to, ec);
    if (!ec) return true;

    // Cross-device (or a non-empty destination): copy then drop the source.
    // copy_symlinks keeps links as links; retarget_data_dir_links fixes them up.
    std::error_code copy_ec;
    fs::copy(from, to,
             fs::copy_options::recursive | fs::copy_options::copy_symlinks |
                 fs::copy_options::overwrite_existing,
             copy_ec);
    if (copy_ec) {
        if (error)
            *error = "copy " + from.string() + " → " + to.string() + " failed: " +
                     copy_ec.message() + " (rename also failed: " + ec.message() + ")";
        return false;
    }
    std::error_code rm_ec;
    fs::remove_all(from, rm_ec);
    if (rm_ec && error)
        *error = "copied to " + to.string() + " but could not remove " + from.string() + ": " +
                 rm_ec.message();
    return true;
}

} // namespace

std::uintmax_t directory_size_bytes(const fs::path& root) {
    std::error_code ec;
    if (!fs::exists(root, ec)) return 0;
    if (fs::is_regular_file(root, ec)) {
        const auto n = fs::file_size(root, ec);
        return ec ? 0 : n;
    }
    std::uintmax_t total = 0;
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code sec;
        if (it->is_symlink(sec)) {
            it.disable_recursion_pending(); // never follow — could leave the tree
            continue;
        }
        if (it->is_regular_file(sec)) {
            const auto n = it->file_size(sec);
            if (!sec) total += n;
        }
    }
    return total;
}

int retarget_data_dir_links(const fs::path& new_data, const fs::path& old_data,
                            std::vector<std::string>* notes) {
    if (new_data.empty() || old_data.empty()) return 0;
    std::error_code ec;
    int fixed = 0;

    // 1) toolchains/<id>/latest → toolchains/<id>/<tag>
    const fs::path toolchains = new_data / "toolchains";
    for (auto it = fs::directory_iterator(toolchains, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (retarget_one(it->path() / "latest", old_data, new_data, notes)) ++fixed;
    }

    // 2) apps/<title>/src/<slot>/<engine> → engines/<name>/<pin>
    // Scanning is depth-limited on purpose: a full recursive walk of apps/ would
    // mean millions of cmake build artefacts for no gain.
    ec.clear();
    const fs::path apps = new_data / "apps";
    for (auto title = fs::directory_iterator(apps, ec);
         !ec && title != fs::directory_iterator(); title.increment(ec)) {
        std::error_code tec;
        if (!title->is_directory(tec)) continue;
        const fs::path src = title->path() / "src";
        std::error_code sec;
        for (auto slot = fs::directory_iterator(src, sec);
             !sec && slot != fs::directory_iterator(); slot.increment(sec)) {
            std::error_code lec;
            if (!slot->is_directory(lec) && !is_dir_link(slot->path())) continue;
            std::error_code eec;
            for (auto entry = fs::directory_iterator(slot->path(), eec);
                 !eec && entry != fs::directory_iterator(); entry.increment(eec)) {
                if (retarget_one(entry->path(), old_data, new_data, notes)) ++fixed;
            }
        }
    }
    return fixed;
}

RootMigrationPlan plan_root_migration(const Paths& current, const fs::path& new_root) {
    RootMigrationPlan plan;
    plan.from_config = current.config_dir;
    plan.from_data = current.data_dir;
    // An empty new_root means "go back to the OS default location".
    plan.to_default = new_root.empty();
    if (plan.to_default) {
        plan.to_root.clear();
        plan.to_config = default_os_config_dir();
        plan.to_data = default_os_data_dir();
        if (plan.to_config.empty() || plan.to_data.empty()) {
            plan.blocker = "Cannot resolve the default location (no home directory).";
            return plan;
        }
    } else {
        plan.to_root = new_root.lexically_normal();
        if (!plan.to_root.is_absolute()) {
            plan.blocker = "Enter a full path, not a relative one.";
            return plan;
        }
        const Paths dest = paths_for_root(plan.to_root);
        plan.to_config = dest.config_dir;
        plan.to_data = dest.data_dir;
    }

    std::error_code ec;
    plan.from_exists = fs::exists(plan.from_data, ec) || fs::exists(plan.from_config, ec);

    plan.same_as_current = path_under(plan.to_data, plan.from_data) &&
                           path_under(plan.from_data, plan.to_data);
    if (plan.same_as_current) {
        plan.blocker = "That is already the current location.";
        return plan;
    }
    if (path_under(plan.to_data, plan.from_data) || path_under(plan.to_config, plan.from_config)) {
        plan.blocker = "That folder is inside RetComM's current data folder — pick one outside it.";
        return plan;
    }
    if (path_under(plan.from_data, plan.to_data) || path_under(plan.from_config, plan.to_config)) {
        plan.blocker = "RetComM's current data folder is inside that folder — pick a different one.";
        return plan;
    }

    std::string werr;
    const fs::path probe = plan.to_default ? plan.to_data : plan.to_root;
    plan.target_writable = directory_is_writable(probe, &werr);
    if (!plan.target_writable) {
        plan.blocker = werr.empty() ? ("Cannot write to " + probe.string()) : werr;
        return plan;
    }

    ec.clear();
    plan.target_has_data = fs::is_directory(plan.to_data, ec) && !fs::is_empty(plan.to_data, ec);
    plan.existing_bytes =
        directory_size_bytes(plan.from_data) + directory_size_bytes(plan.from_config);

    if (plan.target_has_data)
        plan.warning = plan.to_data.string() +
                       " already contains RetComM data. Moving will merge into it.";
    return plan;
}

RootMigrationResult migrate_data_root(const Paths& current, const fs::path& new_root,
                                      RootMigrationMode mode, const fs::path& exe_dir,
                                      bool prefer_exe_marker,
                                      const std::function<void(const std::string&)>& log) {
    RootMigrationResult r;
    auto note = [&](const std::string& s) {
        if (log) log(s);
    };

    const RootMigrationPlan plan = plan_root_migration(current, new_root);
    if (!plan.blocker.empty()) {
        r.message = plan.blocker;
        return r;
    }

    if (mode == RootMigrationMode::Move && plan.from_exists) {
        note("Moving " + plan.from_data.string() + " → " + plan.to_data.string());
        std::string err;
        if (!move_tree(plan.from_data, plan.to_data, &err)) {
            r.message = "Could not move data folder: " + err;
            return r;
        }
        if (!err.empty()) r.notes.push_back(err);

        note("Moving " + plan.from_config.string() + " → " + plan.to_config.string());
        err.clear();
        if (!move_tree(plan.from_config, plan.to_config, &err)) {
            // The data tree already moved; leaving config behind would silently
            // reset the user's settings, so say exactly what happened.
            r.message = "Data moved to " + plan.to_data.string() +
                        ", but the config folder could not be moved: " + err +
                        ". Copy it across by hand before restarting.";
            return r;
        }
        if (!err.empty()) r.notes.push_back(err);

        // Junctions/symlinks carry absolute targets into the old tree; a moved
        // (or cross-volume copied) tree leaves them dangling until rewritten.
        const int fixed = retarget_data_dir_links(plan.to_data, plan.from_data, &r.notes);
        if (fixed > 0) note("Repaired " + std::to_string(fixed) + " toolchain/engine link(s)");
    }

    // Config records absolute install roots; any that pointed into the old data
    // dir must follow, or Install silently offers a dangling location.
    {
        const fs::path dest_config_path = plan.to_config / "config.json";
        AppConfig cfg = load_app_config(dest_config_path);
        bool changed = false;
        auto rebase = [&](fs::path& p) {
            if (p.empty() || !path_under(p, plan.from_data)) return;
            const fs::path rel =
                p.lexically_normal().lexically_relative(plan.from_data.lexically_normal());
            if (rel.empty()) return;
            p = plan.to_data / rel;
            changed = true;
        };
        rebase(cfg.default_install_root);
        for (auto& entry : cfg.install_roots) rebase(entry.path);
        if (changed) {
            std::string err;
            if (!save_app_config(dest_config_path, cfg, &err))
                r.notes.push_back("could not rewrite install roots in config.json: " + err);
            else
                note("Rebased install roots onto the new data folder");
        }
    }

    // Pointer last: until this line, the app still resolves the old location, so
    // any failure above leaves a working install rather than a stranded one.
    std::string perr;
    if (!write_data_root_pointer(exe_dir, plan.to_root, prefer_exe_marker, &perr)) {
        r.message = "Data is in place at " + plan.to_data.string() +
                    ", but the location could not be saved: " + perr;
        return r;
    }

    r.ok = true;
    r.message = plan.to_default
                    ? "RetComM folder reset to the default (" + plan.to_data.string() + ")"
                    : "RetComM folder set to " + plan.to_root.string();
    return r;
}

} // namespace retcomm
