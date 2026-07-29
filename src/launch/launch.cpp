#include "retcomm/launch.hpp"
#include "retcomm/install.hpp"
#include "retcomm/library_index.hpp"
#include "retcomm/romm_saves.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace retcomm {
namespace {

std::string lower_ext(const fs::path& p) {
    std::string e = p.extension().string();
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

bool is_disc_platform(const std::string& platform) {
    return platform == "psx" || platform == "ps2" || platform == "saturn";
}

// Normalize to an absolute host path (native), or Wine Z:/… guest path.
// Use forward slashes for Wine: several recomp launchers interpret `\a` / `\r`
// / `\n` in paths as C escapes, which corrupts `/home/...` into garbage.
std::string path_for_guest(const fs::path& host, bool use_wine) {
    if (host.empty()) return {};
    std::error_code ec;
    fs::path abs = fs::weakly_canonical(host, ec);
    if (ec || abs.empty()) {
        ec.clear();
        abs = fs::absolute(host, ec);
        if (ec) abs = host;
    }
#if defined(_WIN32)
    (void)use_wine;
    return abs.string();
#else
    const std::string s = abs.generic_string();
    if (use_wine) {
        if (!s.empty() && s[0] == '/') return "Z:" + s;
        return host.string();
    }
    return s.empty() ? host.string() : s;
#endif
}

// Disc titles usually want a .cue; if the library preferred a raw dump, use a
// companion .cue (same stem or FILE reference) when present.
fs::path prefer_media_path(const Title& title, fs::path media) {
    if (media.empty()) return media;
    std::error_code ec;
    if (!fs::is_regular_file(media, ec)) return media;

    if (is_disc_platform(title.platform)) {
        const std::string ext = lower_ext(media);
        if (ext == ".bin" || ext == ".iso" || ext == ".img") {
            const fs::path cue = companion_cue_for_disc_dump(media);
            if (!cue.empty() && fs::is_regular_file(cue, ec)) return cue;
        }
    }
    return media;
}

bool write_text_file(const fs::path& path, const std::string& body, std::string* error) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        if (error) {
            *error = "cannot write " + path.string() +
                     " (permission denied? install dir may be owned by another user/root)";
        }
        return false;
    }
    out << body;
    if (!body.empty() && body.back() != '\n') out << '\n';
    out.flush();
    if (!out) {
        if (error) *error = "failed writing " + path.string();
        return false;
    }
    return true;
}

// Prefer release-relative saves/… for argv (works for native + Wine cwd).
std::string save_arg_for_plan(const LaunchPlan& plan) {
    if (plan.save_path.empty()) return {};
    std::error_code ec;
    if (!plan.cwd.empty()) {
        fs::path rel = fs::relative(plan.save_path, plan.cwd, ec);
        if (!ec && !rel.empty()) {
            const std::string s = rel.generic_string();
            if (s.rfind("..", 0) != 0 && s.find("/../") == std::string::npos)
                return s;
        }
    }
    return path_for_guest(plan.save_path, plan.use_wine);
}

void append_save_path_argv(LaunchPlan& plan, const Title& title) {
    if (plan.save_path.empty() || is_disc_platform(title.platform)) return;
    const std::string arg = save_arg_for_plan(plan);
    if (arg.empty()) return;
    plan.argv.emplace_back("--save-path");
    plan.argv.push_back(arg);
}

bool replace_symlink(const fs::path& link, const fs::path& target, std::string* error) {
    std::error_code ec;
    fs::path abs = fs::weakly_canonical(target, ec);
    if (ec || abs.empty()) {
        ec.clear();
        abs = fs::absolute(target, ec);
        if (ec) abs = target;
    }

    // Already correct — common when a prior launch staged the same ROM.
    if (fs::is_symlink(link, ec)) {
        const fs::path cur = fs::weakly_canonical(link, ec);
        if (!ec && !cur.empty() && cur == abs) return true;
    }

    if (fs::exists(link, ec) || fs::is_symlink(link, ec)) {
        fs::remove(link, ec);
        if (ec) {
            if (error)
                *error = "cannot replace " + link.string() + ": " + ec.message() +
                         " (chown the install release dir to your user?)";
            return false;
        }
    }
    ec.clear();
    fs::create_symlink(abs, link, ec);
    if (ec) {
        if (error) *error = "symlink " + link.string() + " → " + abs.string() + ": " + ec.message();
        return false;
    }
    return true;
}

// Point `link` at `target`, preferring a release-relative symlink when possible.
bool point_link_at(const fs::path& link, const fs::path& target, std::string* error) {
    std::error_code ec;
    if (fs::exists(link, ec) || fs::is_symlink(link, ec)) fs::remove(link, ec);
    ec.clear();

    fs::path rel = fs::relative(target, link.parent_path(), ec);
    if (!ec && !rel.empty()) {
        const std::string s = rel.generic_string();
        if (s.rfind("..", 0) != 0 && s.find("/../") == std::string::npos) {
            fs::create_symlink(rel, link, ec);
            if (!ec) return true;
            ec.clear();
        }
    }
    return replace_symlink(link, target, error);
}

// Stage install-local library.<ext> → library ROM so rom.cfg can use a path
// next to the binary (native absolute or Wine Z:/…/library.ext). GBA also
// stages library.sav for the launcher Save row (ignores --save-path).
bool stage_cart_launcher_sidecars(LaunchPlan& plan, const Title& title, std::string* note) {
    if (is_disc_platform(title.platform) || plan.media_path.empty() || plan.cwd.empty())
        return false;

    std::error_code ec;
    const std::string ext = plan.media_path.extension().string();
    if (ext.empty()) return false;

    const fs::path rom_link = plan.cwd / ("library" + ext);
    std::string err;
    if (!replace_symlink(rom_link, plan.media_path, &err)) {
        if (note) *note = err;
        return false;
    }
    plan.staged_rom_link = rom_link;

    if (title.platform == "gba" && !plan.save_path.empty() &&
        (fs::is_regular_file(plan.save_path, ec) || fs::is_symlink(plan.save_path, ec))) {
        const fs::path sav_link = plan.cwd / "library.sav";
        if (!point_link_at(sav_link, plan.save_path, &err)) {
            if (note) *note = "library" + ext + " ok; library.sav failed: " + err;
            return true; // rom link still useful
        }
        if (note)
            *note = "staged library" + ext + " + library.sav → " + plan.save_path.filename().string();
    } else if (note) {
        *note = "staged library" + ext;
    }
    return true;
}

// Rebuild argv for a cart direct boot (positional ROM, no --launcher).
void set_cart_direct_argv(LaunchPlan& plan, const Title& title) {
    plan.argv.clear();
    plan.argv.push_back(plan.binary.string());
    if (!plan.media_path.empty()) {
        plan.argv.push_back(path_for_guest(plan.media_path, plan.use_wine));
    } else {
        plan.argv.emplace_back("--no-launcher");
    }
    if (!plan.bios_path.empty()) {
        plan.argv.emplace_back("--bios");
        plan.argv.push_back(path_for_guest(plan.bios_path, plan.use_wine));
    }
    append_save_path_argv(plan, title);
}

std::string toml_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// Drop a top-level TOML table `[name]` through the next `^[` line or EOF.
void strip_toml_table(std::string& body, const std::string& name) {
    const std::string header = "[" + name + "]";
    std::istringstream in(body);
    std::ostringstream out;
    std::string line;
    bool skipping = false;
    while (std::getline(in, line)) {
        std::string trimmed = line;
        if (!trimmed.empty() && trimmed.back() == '\r') trimmed.pop_back();

        const bool is_table = !trimmed.empty() && trimmed.front() == '[';
        if (skipping) {
            if (is_table) skipping = false;
            else continue;
        }
        if (!skipping && trimmed == header) {
            skipping = true;
            continue;
        }
        out << line << '\n';
    }
    body = out.str();
}

// Upsert MotK/psxrecomp [bios]/[disc] tables next to the release binary.
// Paths should already be guest-facing (Wine Z:\… when needed).
bool upsert_psxrecomp_settings(const fs::path& settings_path, const std::string& bios,
                               const std::string& disc, std::string* error) {
    std::string body;
    {
        std::ifstream in(settings_path);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            body = ss.str();
        }
    }

    strip_toml_table(body, "bios");
    strip_toml_table(body, "disc");

    while (!body.empty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' ' ||
                             body.back() == '\t'))
        body.pop_back();
    if (!body.empty()) body.push_back('\n');

    if (!bios.empty()) {
        body += "\n[bios]\npath = \"";
        body += toml_escape(bios);
        body += "\"\n";
    }
    if (!disc.empty()) {
        body += "\n[disc]\npath = \"";
        body += toml_escape(disc);
        body += "\"\n";
    }

    return write_text_file(settings_path, body, error);
}

void append_default_argv(LaunchPlan& plan, const Title& title, bool open_launcher) {
    plan.argv.clear();
    plan.argv.push_back(plan.binary.string());

    const bool disc = is_disc_platform(title.platform);
    const bool open_ui = open_launcher && plan.mode != LaunchMode::Direct;

    if (open_ui) {
        // Dedicated launcher UI. Cart ROM is seeded via rom.cfg only — a
        // positional path after --launcher makes snesrecomp skip the GUI.
        plan.argv.emplace_back("--launcher");
    } else if (!disc && !plan.media_path.empty()) {
        // Direct cart boot: ROM must be argv[1] (first non-flag) for
        // snesrecomp's resolver. Do not prefix with --no-launcher.
        plan.argv.push_back(path_for_guest(plan.media_path, plan.use_wine));
    } else {
        plan.argv.emplace_back("--no-launcher");
    }

    if (!plan.bios_path.empty()) {
        plan.argv.emplace_back("--bios");
        plan.argv.push_back(path_for_guest(plan.bios_path, plan.use_wine));
    }

    append_save_path_argv(plan, title);

    // Disc titles: stage settings.toml + disc.cfg; never pass --disc.
    // Cart + launcher UI: no positional ROM (rom.cfg / Wine Z:\ library path).
}

std::string format_argv(const std::vector<std::string>& argv) {
    std::ostringstream oss;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) oss << ' ';
        const bool need_quote = argv[i].find(' ') != std::string::npos;
        if (need_quote) oss << '"' << argv[i] << '"';
        else oss << argv[i];
    }
    return oss.str();
}

#if defined(_WIN32)

bool spawn_process(const LaunchPlan& plan, bool detach, int* exit_code, std::string* error) {
    std::wstring cmdline;
    for (size_t i = 0; i < plan.argv.size(); ++i) {
        if (i) cmdline.push_back(L' ');
        std::wstring arg(plan.argv[i].begin(), plan.argv[i].end());
        const bool quote = arg.find(L' ') != std::wstring::npos;
        if (quote) cmdline.push_back(L'"');
        cmdline += arg;
        if (quote) cmdline.push_back(L'"');
    }
    std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back(L'\0');

    std::wstring cwd;
    if (!plan.cwd.empty()) cwd.assign(plan.cwd.wstring());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    DWORD flags = detach ? DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP : 0;
    const BOOL ok = CreateProcessW(
        nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, flags,
        nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    if (!ok) {
        if (error) *error = "CreateProcess failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    if (detach) {
        CloseHandle(pi.hProcess);
        if (exit_code) *exit_code = 0;
        return true;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (exit_code) *exit_code = static_cast<int>(code);
    return true;
}

#else

bool spawn_process(const LaunchPlan& plan, bool detach, int* exit_code, std::string* error) {
    std::vector<char*> av;
    av.reserve(plan.argv.size() + 1);
    for (const auto& s : plan.argv) av.push_back(const_cast<char*>(s.c_str()));
    av.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        if (error) *error = "fork failed";
        return false;
    }
    if (pid == 0) {
        if (!plan.cwd.empty()) {
            if (chdir(plan.cwd.c_str()) != 0) {
                // Still try exec — titles usually anchor on exe dir.
            }
        }
        // Exec argv[0], not plan.binary. For Wine launches argv is
        // [wine, game.exe, ...] — exec'ing the .exe via binfmt while leaving
        // "wine" as argv[0] shifts arguments so the .exe path is seen as a
        // positional ROM (Wrong ROM / rom.cfg rewritten to the exe).
        execv(av[0], av.data());
        _exit(127);
    }

    if (detach) {
        if (exit_code) *exit_code = 0;
        return true;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        if (error) *error = "waitpid failed";
        return false;
    }
    if (WIFEXITED(status)) {
        if (exit_code) *exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        if (exit_code) *exit_code = 128 + WTERMSIG(status);
        if (error) *error = "process killed by signal";
    } else {
        if (exit_code) *exit_code = 1;
    }
    return true;
}

#endif

} // namespace

LaunchMode parse_launch_mode(const std::string& s, std::string* error) {
    if (s.empty() || s == "default" || s == "launcher") return LaunchMode::Default;
    if (s == "direct" || s == "no-launcher") return LaunchMode::Direct;
    if (s == "netplay") return LaunchMode::Netplay;
    if (error) *error = "unknown launch mode '" + s + "' (default|direct|netplay)";
    return LaunchMode::Default;
}

const char* launch_mode_name(LaunchMode mode) {
    switch (mode) {
    case LaunchMode::Default:
        return "default";
    case LaunchMode::Direct:
        return "direct";
    case LaunchMode::Netplay:
        return "netplay";
    }
    return "default";
}

LaunchPlan plan_launch(const Paths& paths, const Title& title, const LaunchOptions& opts) {
    LaunchPlan lp;
    lp.title = &title;
    lp.mode = opts.mode;

    if (opts.mode == LaunchMode::Netplay) {
        lp.ready = false;
        lp.message =
            "launch mode 'netplay' is not implemented yet\n"
            "  tip: use --mode default for the dedicated launcher; a generic\n"
            "       netplay lobby frontend will plug in here later\n";
        return lp;
    }

    const InstallPlan inst = inspect_install(paths, title);
    lp.binary = inst.binary_path;
    if (!inst.installed || lp.binary.empty()) {
        lp.ready = false;
        std::ostringstream oss;
        oss << "cannot launch: " << inst.message << "\n"
            << "  tip: retcomm install " << title.id << "\n";
        lp.message = oss.str();
        return lp;
    }

    lp.cwd = lp.binary.parent_path();
    lp.media_path = prefer_media_path(title, opts.rom_path);
    lp.bios_path = opts.bios_path;
    if (!opts.save_path.empty()) {
        std::error_code ec;
        fs::path sp = opts.save_path;
        if (!sp.is_absolute()) {
            const std::string s = opts.save_path.generic_string();
            if (s.rfind("saves/", 0) == 0 || s.rfind("saves\\", 0) == 0)
                sp = lp.cwd / opts.save_path;
            else
                sp = lp.cwd / "saves" / opts.save_path.filename();
        }
        if (fs::exists(sp, ec)) lp.save_path = sp;
        else lp.save_path = opts.save_path; // bind may still resolve / report
    }

    lp.use_wine =
        (inst.record && inst.record->runtime == "wine") ||
        (host_os_key() != "windows" && lower_ext(lp.binary) == ".exe");
    std::string wine_bin;
    if (lp.use_wine) {
        std::string wine_err;
        wine_bin = resolve_wine_binary(&wine_err);
        if (wine_bin.empty()) {
            lp.ready = false;
            lp.message = "cannot launch via Wine: " + wine_err + "\n" +
                         "  tip: install wine/wine64 and ensure it is on PATH\n";
            return lp;
        }
    }

    const bool open_launcher = (opts.mode == LaunchMode::Default);
    append_default_argv(lp, title, open_launcher);
    if (lp.use_wine) lp.argv.insert(lp.argv.begin(), wine_bin);

    // Persist paths where recomp-ui / hosts already look.
    if (!lp.media_path.empty()) {
        const char* cfg_name = is_disc_platform(title.platform) ? "disc.cfg" : "rom.cfg";
        lp.staged_cfg = lp.cwd / cfg_name;
    }
    if (!lp.bios_path.empty()) lp.staged_bios_cfg = lp.cwd / "bios.cfg";

    // psxrecomp launcher preselect: stage settings.toml [bios]/[disc] for
    // current release builds (CLI --disc is intentionally omitted).
    if (is_disc_platform(title.platform) &&
        (!lp.bios_path.empty() || !lp.media_path.empty())) {
        lp.staged_settings = lp.cwd / "settings.toml";
    }

    lp.ready = true;
    std::ostringstream oss;
    oss << "launch plan for " << title.id << "\n"
        << "  mode:   " << launch_mode_name(lp.mode) << "\n"
        << "  binary: " << lp.binary.string() << "\n"
        << "  cwd:    " << lp.cwd.string() << "\n";
    if (lp.use_wine) oss << "  runtime: wine\n";
    if (!inst.installed_tag.empty()) oss << "  version:" << " " << inst.installed_tag << "\n";
    if (!lp.bios_path.empty())
        oss << "  bios:   " << lp.bios_path.string() << "\n"
            << "          guest: " << path_for_guest(lp.bios_path, lp.use_wine) << "\n";
    else if (title.requires_bios())
        oss << "  bios:   (missing — run retcomm bios scan or pass --bios)\n";
    if (!lp.media_path.empty())
        oss << "  media:  " << lp.media_path.string() << "\n"
            << "          guest: " << path_for_guest(lp.media_path, lp.use_wine) << "\n";
    else
        oss << "  media:  (none — game launcher can browse/prompt)\n";
    if (!lp.save_path.empty())
        oss << "  save:   " << lp.save_path.string() << "\n"
            << "          argv:  " << save_arg_for_plan(lp) << "\n";
    if (!lp.staged_bios_cfg.empty()) oss << "  stage:  " << lp.staged_bios_cfg.string() << "\n";
    if (!lp.staged_cfg.empty()) oss << "  stage:  " << lp.staged_cfg.string() << "\n";
    if (!lp.staged_settings.empty())
        oss << "  stage:  " << lp.staged_settings.string() << " ([bios]/[disc])\n";
    oss << "  argv:   " << format_argv(lp.argv) << "\n";
    lp.message = oss.str();
    return lp;
}

LaunchResult launch_title(const Paths& paths, const Title& title, const LaunchOptions& opts) {
    LaunchResult result;
    result.plan = plan_launch(paths, title, opts);
    result.dry_run = opts.dry_run;

    if (!result.plan.ready) {
        result.message = result.plan.message;
        return result;
    }

    // Point [memcard] / cart slots at the shared saves/ tree (RomM sync + picker)
    // before staging rom.cfg — GBA launcher sidecars need the resolved save path.
    {
        auto bind =
            bind_recomp_save_paths(paths, title, result.plan.use_wine, result.plan.save_path);
        if (bind.ok) {
            if (result.plan.save_path.empty() && !bind.active_save.empty())
                result.plan.save_path = bind.active_save;
            if (!is_disc_platform(title.platform) && !result.plan.save_path.empty()) {
                const bool has_flag = std::find(result.plan.argv.begin(), result.plan.argv.end(),
                                                "--save-path") != result.plan.argv.end();
                if (!has_flag) append_save_path_argv(result.plan, title);
            }
            if (!bind.message.empty())
                result.plan.message += "  saves:  " + bind.message + "\n";
        } else if (!bind.message.empty()) {
            result.plan.message += "  warning: " + bind.message + "\n";
        }
    }

    // Cart: install-local library.<ext> so rom.cfg is next to the binary
    // (native + Wine). GBA also gets library.sav for the launcher Save row.
    {
        std::string note;
        if (stage_cart_launcher_sidecars(result.plan, title, &note) && !note.empty())
            result.plan.message += "  stage:  " + note + "\n";
        else if (!note.empty())
            result.plan.message += "  warning: " + note + "\n";
    }

    if (!result.plan.staged_bios_cfg.empty() && !result.plan.bios_path.empty()) {
        std::string err;
        if (!write_text_file(result.plan.staged_bios_cfg,
                             path_for_guest(result.plan.bios_path, result.plan.use_wine),
                             &err)) {
            result.plan.message += "  warning: " + err + "\n";
        }
    }
    if (!result.plan.staged_cfg.empty() && !result.plan.media_path.empty()) {
        std::string err;
        const fs::path cfg_media =
            !result.plan.staged_rom_link.empty() ? result.plan.staged_rom_link
                                                 : result.plan.media_path;
        const std::string guest_media = path_for_guest(cfg_media, result.plan.use_wine);
        if (!write_text_file(result.plan.staged_cfg, guest_media, &err)) {
            // Cart + --launcher seeds ONLY via rom.cfg. A stale file (often the
            // .exe path left by a prior root-owned install) causes Wrong ROM.
            // Fall back to a positional direct boot so Play still works.
            const bool cart_launcher =
                !is_disc_platform(title.platform) && opts.mode == LaunchMode::Default;
            result.plan.message += "  warning: " + err + "\n";
            if (cart_launcher) {
                set_cart_direct_argv(result.plan, title);
                if (result.plan.use_wine) {
                    std::string wine_err;
                    const std::string wine_bin = resolve_wine_binary(&wine_err);
                    if (!wine_bin.empty())
                        result.plan.argv.insert(result.plan.argv.begin(), wine_bin);
                }
                result.plan.message +=
                    "  warning: could not seed rom.cfg — launching ROM directly "
                    "(skipping dedicated launcher UI)\n"
                    "  tip: chown the install dir to your user, then Play again "
                    "for the launcher UI\n"
                    "  argv:   " +
                    format_argv(result.plan.argv) + "\n";
            } else {
                result.plan.message += "  warning: continuing with existing cfg / argv\n";
            }
        }
    }
    if (!result.plan.staged_settings.empty()) {
        std::string err;
        if (!upsert_psxrecomp_settings(
                result.plan.staged_settings,
                path_for_guest(result.plan.bios_path, result.plan.use_wine),
                path_for_guest(result.plan.media_path, result.plan.use_wine), &err)) {
            result.plan.message += "  warning: " + err + "\n";
        }
    }

    if (opts.dry_run) {
        result.ok = true;
        result.message = result.plan.message + "  status: dry-run (not spawned)\n";
        return result;
    }

    std::string err;
    int code = 0;
    if (!spawn_process(result.plan, opts.detach, &code, &err)) {
        result.message = result.plan.message + "  error: " + err + "\n";
        return result;
    }

    result.ok = true;
    result.exit_code = code;
    if (opts.detach) {
        result.message = result.plan.message + "  status: launched (detached)\n";
    } else {
        std::ostringstream oss;
        oss << result.plan.message << "  status: exited with code " << code << "\n";
        result.message = oss.str();
        // Treat signal deaths / exec failure as failure; normal game quit (0) ok.
        if (code == 127) {
            result.ok = false;
            result.message += "  error: exec failed (binary missing or not executable)\n";
        }
    }
    return result;
}

} // namespace retcomm
