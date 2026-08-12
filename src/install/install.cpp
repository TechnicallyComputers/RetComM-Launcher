#include "retcomm/install.hpp"
#include "retcomm/app_state.hpp"
#include "retcomm/bios_index.hpp"
#include "retcomm/http.hpp"
#include "retcomm/library_index.hpp"
#include "retcomm/paths.hpp"
#include "retcomm/release_tags.hpp"
#include "retcomm/romm_fetch.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <cstddef>
#include <mutex>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace retcomm {
namespace {

using nlohmann::json;

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool match_glob(const std::string& pattern, const std::string& name) {
    if (pattern.empty()) return false;
    const std::string p = to_lower(pattern);
    const std::string n = to_lower(name);
    size_t pi = 0, ni = 0, star = std::string::npos, match = 0;
    while (ni < n.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == n[ni])) {
            ++pi;
            ++ni;
        } else if (pi < p.size() && p[pi] == '*') {
            star = pi++;
            match = ni;
        } else if (star != std::string::npos) {
            pi = star + 1;
            ni = ++match;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

// Glob against a relative path (forward slashes) and/or its filename.
bool path_matches_glob(const std::string& rel_posix, const std::string& pattern) {
    if (match_glob(pattern, rel_posix)) return true;
    const auto slash = rel_posix.find_last_of('/');
    const std::string base =
        slash == std::string::npos ? rel_posix : rel_posix.substr(slash + 1);
    return match_glob(pattern, base);
}

std::vector<std::string> default_save_globs() {
    return {"saves/*",     "saves/**", "*.mcd",           "*.mcr",
            "*.srm",       "*.state",  "*.sts",           "states/*",
            "savestates/*", "savestates/**"};
}

std::vector<std::string> save_globs_for_title(const Title& title) {
    std::vector<std::string> g = title.saves_sram_glob;
    g.insert(g.end(), title.saves_memcard_glob.begin(), title.saves_memcard_glob.end());
    // Always consider common save / savestate layouts next to the binary.
    const auto defs = default_save_globs();
    g.insert(g.end(), defs.begin(), defs.end());
    return g;
}

void remove_boxart_for_title(const Paths& paths, const std::string& title_id) {
    if (title_id.empty()) return;
    const fs::path root = paths.data_dir / "boxart";
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return;
    static const char* kExts[] = {".jpg", ".jpeg", ".png", ".webp", ".download"};
    auto wipe_dir = [&](const fs::path& dir) {
        if (!fs::is_directory(dir, ec)) return;
        for (const char* ext : kExts) fs::remove(dir / (title_id + ext), ec);
    };
    wipe_dir(root);
    for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        if (it->is_directory(ec)) wipe_dir(it->path());
    }
}

int prune_stale_title_indexes(const Paths& paths, const Catalog& catalog,
                              std::vector<std::string>* messages) {
    std::unordered_set<std::string> known;
    known.reserve(catalog.titles.size() * 2);
    for (const auto& t : catalog.titles) known.insert(t.id);

    int pruned = 0;
    auto note = [&](const std::string& s) {
        if (messages) messages->push_back(s);
    };

    // Library index
    {
        LibraryIndex idx = load_library_index(paths.library_index_path);
        bool dirty = false;
        for (auto& f : idx.files) {
            if (!f.title_id.empty() && !known.count(f.title_id)) {
                f.title_id.clear();
                f.matched_by.clear();
                dirty = true;
            }
        }
        const size_t before = idx.titles.size();
        idx.titles.erase(std::remove_if(idx.titles.begin(), idx.titles.end(),
                                        [&](const LibraryTitleBind& b) {
                                            return !b.title_id.empty() && !known.count(b.title_id);
                                        }),
                         idx.titles.end());
        if (idx.titles.size() != before) dirty = true;
        if (dirty) {
            save_library_index(paths.library_index_path, idx);
            const int n = static_cast<int>(before - idx.titles.size());
            pruned += n;
            note("Pruned " + std::to_string(n) + " stale library title bind(s)");
        }
    }

    // BIOS index
    {
        BiosIndex idx = load_bios_index(paths.bios_index_path);
        bool dirty = false;
        for (auto& f : idx.files) {
            if (!f.title_id.empty() && !known.count(f.title_id)) {
                f.title_id.clear();
                f.matched_by.clear();
                dirty = true;
            }
        }
        const size_t before = idx.titles.size();
        idx.titles.erase(std::remove_if(idx.titles.begin(), idx.titles.end(),
                                        [&](const BiosTitleBind& b) {
                                            return !b.title_id.empty() && !known.count(b.title_id);
                                        }),
                         idx.titles.end());
        if (idx.titles.size() != before) dirty = true;
        if (dirty) {
            save_bios_index(paths.bios_index_path, idx);
            const int n = static_cast<int>(before - idx.titles.size());
            pruned += n;
            note("Pruned " + std::to_string(n) + " stale BIOS title bind(s)");
        }
    }

    // RomM availability cache
    {
        RommRomIndex idx = load_romm_rom_index(paths.romm_rom_index_path);
        size_t removed = 0;
        for (auto it = idx.by_title.begin(); it != idx.by_title.end();) {
            if (!known.count(it->first)) {
                it = idx.by_title.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        if (removed > 0) {
            save_romm_rom_index(paths.romm_rom_index_path, idx, nullptr);
            pruned += static_cast<int>(removed);
            note("Pruned " + std::to_string(removed) + " stale RomM index entr(y/ies)");
        }
    }

    // Hub prefs
    {
        AppState st = load_app_state(paths.state_path);
        auto erase_unknown = [&](auto& map) {
            size_t n = 0;
            for (auto it = map.begin(); it != map.end();) {
                if (!known.count(it->first)) {
                    it = map.erase(it);
                    ++n;
                } else {
                    ++it;
                }
            }
            return n;
        };
        const size_t n = erase_unknown(st.preferred_save) + erase_unknown(st.preferred_save_card2) +
                         erase_unknown(st.preferred_bios);
        if (n > 0) {
            save_app_state(paths.state_path, st, nullptr);
            pruned += static_cast<int>(n);
            note("Pruned " + std::to_string(n) + " stale app-state preference(s)");
        }
    }

    return pruned;
}

bool is_user_config_path(const std::string& rel_posix) {
    if (rel_posix.empty()) return false;
    // Basename-only or nested under the release root (never deep vendor trees).
    const auto slash = rel_posix.rfind('/');
    const std::string base =
        slash == std::string::npos ? rel_posix : rel_posix.substr(slash + 1);
    static const char* kNames[] = {"settings.toml", "keybinds.ini", "bios.cfg",
                                   "disc.cfg",      "rom.cfg",      "input.ini",
                                   "imgui.ini",     "controls.ini"};
    for (const char* n : kNames) {
        if (base == n) return true;
    }
    return false;
}

bool is_save_path(const std::string& rel_posix, const std::vector<std::string>& globs) {
    if (rel_posix.empty()) return false;
    // Whole saves/ tree (memcards + host dumps games often drop here).
    if (rel_posix == "saves" || rel_posix.rfind("saves/", 0) == 0) return true;
    if (rel_posix == "states" || rel_posix.rfind("states/", 0) == 0) return true;
    if (rel_posix == "savestates" || rel_posix.rfind("savestates/", 0) == 0) return true;
    for (const auto& g : globs) {
        if (path_matches_glob(rel_posix, g)) return true;
    }
    return false;
}

bool is_user_state_path(const std::string& rel_posix, const std::vector<std::string>& globs) {
    return is_save_path(rel_posix, globs) || is_user_config_path(rel_posix);
}

std::string rel_posix_under(const fs::path& root, const fs::path& file) {
    std::error_code ec;
    fs::path rel = fs::relative(file, root, ec);
    if (ec) return {};
    std::string s = rel.generic_string();
    if (s == "." || s.empty() || s.rfind("..", 0) == 0) return {};
    return s;
}

// Keep stash paths release-relative (saves/…), never apps-root-relative
// (releases/<tag>/saves/…) or nested preserved/ junk.
std::string normalize_preserved_rel(std::string rel) {
    if (rel.empty() || rel.rfind("preserved/", 0) == 0) return {};
    const std::string pfx = "releases/";
    if (rel.rfind(pfx, 0) == 0) {
        const auto rest = rel.substr(pfx.size());
        const auto slash = rest.find('/');
        if (slash == std::string::npos) return {};
        rel = rest.substr(slash + 1);
    }
    if (rel.empty() || rel.rfind("preserved/", 0) == 0) return {};
    return rel;
}

// Copy matching save/config files from `search_root` into preserved/<rel>.
size_t stash_user_state_from(const fs::path& search_root, const fs::path& preserved,
                             const std::vector<std::string>& globs,
                             std::vector<std::string>* out_rels, std::string* error) {
    std::error_code ec;
    if (!fs::exists(search_root, ec)) return 0;
    size_t n = 0;
    for (auto it = fs::recursive_directory_iterator(
             search_root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec) && it->path().filename() == "preserved") {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        const std::string rel =
            normalize_preserved_rel(rel_posix_under(search_root, it->path()));
        if (rel.empty() || !is_user_state_path(rel, globs)) continue;
        const fs::path dest = preserved / rel;
        fs::create_directories(dest.parent_path(), ec);
        fs::copy_file(it->path(), dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            if (error) *error = "failed to preserve " + it->path().string() + ": " + ec.message();
            return n;
        }
        if (out_rels) out_rels->push_back(rel);
        ++n;
    }
    return n;
}

// Back-compat name used by uninstall keep-saves.
size_t stash_saves_from(const fs::path& search_root, const fs::path& preserved,
                        const std::vector<std::string>& globs,
                        std::vector<std::string>* out_rels, std::string* error) {
    return stash_user_state_from(search_root, preserved, globs, out_rels, error);
}

std::string iso8601_now() {
    using clock = std::chrono::system_clock;
    const auto t = clock::to_time_t(clock::now());
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string sanitize_tag(std::string tag) {
    for (char& c : tag) {
        if (c == '/' || c == '\\' || c == ':' || c == 0) c = '_';
    }
    if (tag.empty()) tag = "unknown";
    return tag;
}

bool ends_with_ci(const std::string& s, const char* suf) {
    const std::string lower = to_lower(s);
    const std::string suffix = to_lower(suf);
    return lower.size() >= suffix.size() &&
           lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_archive_path(const fs::path& p) {
    const std::string name = p.filename().string();
    return ends_with_ci(name, ".zip") || ends_with_ci(name, ".7z") || ends_with_ci(name, ".tgz") ||
           ends_with_ci(name, ".tar") || ends_with_ci(name, ".tar.gz") ||
           ends_with_ci(name, ".tar.xz") || ends_with_ci(name, ".tar.bz2");
}

#if defined(_WIN32)

bool win_exe_on_path(const wchar_t* name) {
    wchar_t buf[MAX_PATH];
    return SearchPathW(nullptr, name, L".exe", MAX_PATH, buf, nullptr) != 0;
}

std::wstring win_quote_arg(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";
    bool need = false;
    for (wchar_t c : arg) {
        if (c == L' ' || c == L'\t' || c == L'"') {
            need = true;
            break;
        }
    }
    if (!need) return arg;
    std::wstring out = L"\"";
    for (wchar_t c : arg) {
        if (c == L'"') out += L"\\\"";
        else out += c;
    }
    out += L'"';
    return out;
}

// Spawn without a console window; returns process exit code, or -1 on spawn failure.
int win_run_hidden(const std::wstring& exe, const std::vector<std::wstring>& args,
                   std::string* err_out) {
    std::wstring cmdline = win_quote_arg(exe);
    for (const auto& a : args) {
        cmdline.push_back(L' ');
        cmdline += win_quote_arg(a);
    }
    std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok =
        CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &si, &pi);
    if (!ok) {
        if (err_out)
            *err_out = "CreateProcess failed (" + std::to_string(GetLastError()) + "): " +
                       fs::path(exe).string();
        return -1;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (code != 0 && err_out) {
        *err_out = "command failed (" + std::to_string(code) + "): " + fs::path(exe).string();
    }
    return static_cast<int>(code);
}

std::wstring win_ps_single_quote(const fs::path& p) {
    std::wstring s = p.wstring();
    std::wstring out;
    out.reserve(s.size() + 2);
    for (wchar_t c : s) {
        if (c == L'\'') out += L"''";
        else out += c;
    }
    return out;
}

bool extract_archive_windows(const fs::path& archive, const fs::path& dest, std::string* error) {
    std::string err;

    // Windows 10+ ships tar.exe (libarchive) — handles zip and common tar.* formats.
    if (win_exe_on_path(L"tar")) {
        if (win_run_hidden(L"tar.exe",
                           {L"-xf", archive.wstring(), L"-C", dest.wstring()}, &err) == 0)
            return true;
    }

    // PowerShell Expand-Archive for .zip (same approach as the portable stub).
    const std::string name = archive.filename().string();
    if (ends_with_ci(name, ".zip") && win_exe_on_path(L"powershell")) {
        const std::wstring cmd =
            L"Expand-Archive -LiteralPath '" + win_ps_single_quote(archive) +
            L"' -DestinationPath '" + win_ps_single_quote(dest) + L"' -Force";
        if (win_run_hidden(L"powershell.exe",
                           {L"-NoProfile", L"-ExecutionPolicy", L"Bypass", L"-Command", cmd},
                           &err) == 0)
            return true;
    }

    // Optional 7-Zip on PATH.
    if (win_exe_on_path(L"7z")) {
        const std::wstring out_sw = L"-o" + dest.wstring();
        if (win_run_hidden(L"7z.exe", {L"x", L"-y", out_sw, archive.wstring()}, &err) == 0)
            return true;
    }

    if (error) *error = err.empty() ? "no extractor succeeded for " + archive.string() : err;
    return false;
}

#else // !_WIN32

int run_cmd(const std::string& cmd, std::string* err_out = nullptr) {
    const int rc = std::system(cmd.c_str());
    if (rc != 0 && err_out) *err_out = "command failed (" + std::to_string(rc) + "): " + cmd;
    return rc;
}

std::string shell_quote(const fs::path& p) {
    std::string s = p.string();
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

bool tool_on_path_unix(const char* name) {
    return run_cmd(std::string("command -v ") + name + " >/dev/null 2>&1") == 0;
}

#endif

bool extract_archive(const fs::path& archive, const fs::path& dest, std::string* error) {
    std::error_code ec;
    fs::create_directories(dest, ec);

#if defined(_WIN32)
    return extract_archive_windows(archive, dest, error);
#else
    const std::string name = archive.filename().string();
    std::string err;

    // Prefer bsdtar (libarchive) — handles zip/tar/7z well on many systems.
    // --no-same-owner avoids failures when the archive records UIDs we can't set.
    if (tool_on_path_unix("bsdtar")) {
        const std::string cmd = "bsdtar --no-same-owner -xf " + shell_quote(archive) +
                                " -C " + shell_quote(dest);
        if (run_cmd(cmd, &err) == 0) return true;
    }
    // GNU/BSD tar — needed for .tar.gz when bsdtar is absent (lean AppImages).
    const bool looks_tar = ends_with_ci(name, ".tar") || ends_with_ci(name, ".tar.gz") ||
                           ends_with_ci(name, ".tgz") || ends_with_ci(name, ".tar.xz") ||
                           ends_with_ci(name, ".tar.bz2") || ends_with_ci(name, ".tar.zst");
    if (looks_tar && tool_on_path_unix("tar")) {
        const std::string cmd = "tar --no-same-owner -xf " + shell_quote(archive) + " -C " +
                                shell_quote(dest);
        if (run_cmd(cmd, &err) == 0) return true;
    }
    if (ends_with_ci(name, ".zip") && tool_on_path_unix("unzip")) {
        const std::string cmd =
            "unzip -qo " + shell_quote(archive) + " -d " + shell_quote(dest);
        if (run_cmd(cmd, &err) == 0) return true;
    }
    if (tool_on_path_unix("7z")) {
        const std::string cmd =
            "7z x -y -o" + shell_quote(dest) + " " + shell_quote(archive) + " >/dev/null";
        if (run_cmd(cmd, &err) == 0) return true;
    }
    if (error) *error = err.empty() ? "no extractor succeeded for " + archive.string() : err;
    return false;
#endif
}

std::vector<fs::path> list_archives(const fs::path& root) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::exists(root, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec) && is_archive_path(it->path())) out.push_back(it->path());
    }
    return out;
}

bool top_level_only_archives(const fs::path& root) {
    std::error_code ec;
    bool any = false;
    for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        if (it->is_directory(ec)) return false;
        if (!it->is_regular_file(ec)) continue;
        any = true;
        if (!is_archive_path(it->path())) return false;
    }
    return any;
}

bool filename_eq_ci(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

fs::path find_named_file(const fs::path& root, const std::string& filename) {
    if (filename.empty()) return {};
    std::error_code ec;
    const fs::path direct = root / filename;
    if (fs::is_regular_file(direct, ec)) return direct;
    // Case-insensitive direct hit (common when catalog launch names differ in case).
    {
        const auto parent = root;
        if (fs::is_directory(parent, ec)) {
            for (auto it = fs::directory_iterator(parent, ec);
                 !ec && it != fs::directory_iterator(); it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                if (filename_eq_ci(it->path().filename().string(), filename)) return it->path();
            }
        }
    }
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (filename_eq_ci(it->path().filename().string(), filename)) return it->path();
    }
    return {};
}

std::string to_lower_ascii_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool is_junk_launch_name(const std::string& name) {
    const std::string lower = to_lower_ascii_copy(name);
    if (lower.empty() || lower[0] == '.') return true;
    static const char* kJunk[] = {"license",     "makefile", "readme",   "version",
                                  "vagrantfile", "ctors",    "asm64",    "hello_32",
                                  "uninstall.exe"};
    for (const char* j : kJunk) {
        if (lower == j) return true;
    }
    if (lower.rfind("crash-", 0) == 0 || lower.find("crashpad") != std::string::npos)
        return true;
    return false;
}

std::string alnum_lower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c)) out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

// Score a candidate launch binary. Higher is better; <0 means reject.
int score_launch_candidate(const fs::path& root, const fs::path& file,
                           const std::string& expected_name, const std::string& target_os) {
    std::error_code ec;
    const std::string name = file.filename().string();
    if (is_junk_launch_name(name)) return -100;
    const std::string lower = to_lower_ascii_copy(name);
    const bool want_exe = target_os == "windows";
    const bool is_exe =
        lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".exe") == 0;
    const bool no_ext = name.find('.') == std::string::npos;
    if (want_exe && !is_exe) return -40;
    if (!want_exe && is_exe) return -40;
    if (!want_exe && !no_ext) return -20;

    int score = 10;
    // Prefer archive root (direct child of staging/release root).
    const fs::path parent = file.parent_path();
    if (parent == root || fs::equivalent(parent, root, ec)) score += 40;
    else score -= 15;

    if (lower.find("recompiled") != std::string::npos) score += 50;

    const auto sz = fs::file_size(file, ec);
    if (!ec) {
        if (sz >= 1024 * 1024) score += 20;      // >= 1 MiB
        else if (sz < 4096) score -= 30;         // tiny stubs / scripts
    }

    if (!expected_name.empty()) {
        if (filename_eq_ci(name, expected_name)) return 1000;
        const std::string a = alnum_lower(name);
        const std::string b = alnum_lower(expected_name);
        if (!a.empty() && !b.empty()) {
            if (a == b) score += 80;
            else if (a.find(b) != std::string::npos || b.find(a) != std::string::npos)
                score += 35;
            else {
                // TwistedMetal4Recomp ↔ TwistedMetal4_Recompiled
                std::string b2 = b;
                if (b2.size() > 6 && b2.compare(b2.size() - 6, 6, "recomp") == 0)
                    b2 = b2.substr(0, b2.size() - 6) + "recompiled";
                if (a == b2 || a.find(b2) != std::string::npos) score += 45;
            }
        }
    }
    return score;
}

struct LaunchCandidate {
    fs::path path;
    std::string name;
    int score = 0;
};

std::vector<LaunchCandidate> collect_launch_candidates(const fs::path& root,
                                                       const std::string& expected_name,
                                                       const std::string& target_os,
                                                       size_t limit = 24) {
    std::vector<LaunchCandidate> out;
    std::error_code ec;
    if (!fs::exists(root, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const int score =
            score_launch_candidate(root, it->path(), expected_name, target_os);
        if (score < 0) continue;
        LaunchCandidate c;
        c.path = it->path();
        c.name = it->path().filename().string();
        c.score = score;
        out.push_back(std::move(c));
    }
    std::sort(out.begin(), out.end(), [](const LaunchCandidate& a, const LaunchCandidate& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.name < b.name;
    });
    // Unique by filename (keep best score).
    std::vector<LaunchCandidate> uniq;
    std::unordered_set<std::string> seen;
    for (auto& c : out) {
        const std::string key = to_lower_ascii_copy(c.name);
        if (!seen.insert(key).second) continue;
        uniq.push_back(std::move(c));
        if (uniq.size() >= limit) break;
    }
    return uniq;
}

// Likely launch candidates for error messages / catalog fixes.
std::vector<std::string> list_launch_candidates(const fs::path& root, size_t limit = 12) {
    const auto scored = collect_launch_candidates(root, {}, host_os_key(), limit);
    std::vector<std::string> out;
    out.reserve(scored.size());
    for (const auto& c : scored) out.push_back(c.name);
    return out;
}

// When catalog launch.<os> misses, pick a unique high-confidence binary.
fs::path resolve_launch_binary(const fs::path& root, const std::string& expected_name,
                               const std::string& target_os, std::string* resolved_name) {
    fs::path exact = find_named_file(root, expected_name);
    if (!exact.empty()) {
        if (resolved_name) *resolved_name = exact.filename().string();
        return exact;
    }
    const auto cands = collect_launch_candidates(root, expected_name, target_os, 16);
    if (cands.empty()) return {};
    const LaunchCandidate& best = cands.front();
    // Accept: clear winner (recompiled / fuzzy / large+root) and not tied.
    const bool clear =
        best.score >= 70 && (cands.size() == 1 || best.score >= cands[1].score + 15);
    if (!clear) return {};
    if (resolved_name) *resolved_name = best.name;
    return best.path;
}

std::string unique_release_suffix() {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
#if defined(_WIN32)
    return std::to_string(GetCurrentProcessId()) + "-" + std::to_string(ticks);
#else
    return std::to_string(static_cast<unsigned>(::getpid())) + "-" + std::to_string(ticks);
#endif
}

// Forward declaration — defined below out of the anonymous namespace for build.cpp.
} // namespace

namespace {

bool basename_eq_ci_zip(std::string entry, const std::string& want) {
    while (!entry.empty() && (entry.back() == '/' || entry.back() == '\\')) entry.pop_back();
    if (entry.empty() || want.empty()) return false;
    const auto slash = entry.find_last_of("/\\");
    if (slash != std::string::npos) entry = entry.substr(slash + 1);
    if (entry.size() != want.size()) return false;
    for (size_t i = 0; i < entry.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(entry[i])) !=
            std::tolower(static_cast<unsigned char>(want[i])))
            return false;
    }
    return true;
}

bool listing_mentions_basename(const fs::path& listing_file, const std::string& filename) {
    std::ifstream in(listing_file);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        if (basename_eq_ci_zip(line, filename)) return true;
        const auto sp = line.find_last_of(" \t");
        if (sp != std::string::npos && basename_eq_ci_zip(line.substr(sp + 1), filename))
            return true;
    }
    return false;
}

} // namespace

bool archive_contains_named_file(const fs::path& archive, const std::string& filename) {
    if (filename.empty() || archive.empty()) return false;
    std::error_code ec;
    if (!fs::is_regular_file(archive, ec)) return false;

    const fs::path tmp =
        fs::temp_directory_path(ec) /
        ("retcomm-zip-list-" +
         std::to_string(
#if defined(_WIN32)
             GetCurrentProcessId()
#else
             static_cast<unsigned>(::getpid())
#endif
                 ) +
         ".txt");
    if (ec) return false;

    auto cleanup = [&]() {
        std::error_code rm;
        fs::remove(tmp, rm);
    };

#if defined(_WIN32)
    std::string err;
    bool listed = false;
    if (win_exe_on_path(L"tar") && win_exe_on_path(L"powershell")) {
        const std::wstring cmd =
            L"& tar.exe -tf '" + win_ps_single_quote(archive) +
            L"' | Out-File -Encoding utf8 '" + win_ps_single_quote(tmp) + L"'";
        listed = win_run_hidden(L"powershell.exe",
                                {L"-NoProfile", L"-ExecutionPolicy", L"Bypass", L"-Command", cmd},
                                &err) == 0;
    }
    if (!listed && ends_with_ci(archive.filename().string(), ".zip") &&
        win_exe_on_path(L"powershell")) {
        const std::wstring cmd =
            L"Add-Type -AssemblyName System.IO.Compression.FileSystem; "
            L"[IO.Compression.ZipFile]::OpenRead('" +
            win_ps_single_quote(archive) +
            L"').Entries | ForEach-Object { $_.FullName } | Out-File -Encoding utf8 '" +
            win_ps_single_quote(tmp) + L"'";
        listed = win_run_hidden(L"powershell.exe",
                                {L"-NoProfile", L"-ExecutionPolicy", L"Bypass", L"-Command", cmd},
                                &err) == 0;
    }
    if (!listed) {
        cleanup();
        return false;
    }
#else
    std::string err;
    bool listed = false;
    const std::string out_redir = " >" + shell_quote(tmp) + " 2>/dev/null";
    if (tool_on_path_unix("bsdtar")) {
        listed = run_cmd("bsdtar -tf " + shell_quote(archive) + out_redir, &err) == 0;
    }
    if (!listed && ends_with_ci(archive.filename().string(), ".zip") &&
        tool_on_path_unix("unzip")) {
        listed = run_cmd("unzip -Z1 " + shell_quote(archive) + out_redir, &err) == 0;
    }
    if (!listed && tool_on_path_unix("tar")) {
        listed = run_cmd("tar -tf " + shell_quote(archive) + out_redir, &err) == 0;
    }
    if (!listed) {
        cleanup();
        return false;
    }
#endif

    const bool hit = listing_mentions_basename(tmp, filename);
    cleanup();
    return hit;
}

bool promote_staging_to_release(const fs::path& staging, const fs::path& release_dir,
                                std::string* error, fs::path* outgoing_for_cleanup) {
    std::error_code ec;
    const fs::path parent = release_dir.parent_path();
    fs::create_directories(parent, ec);

    // Land staging at a sibling .new first so a failed promote never leaves
    // release_dir half-wiped (current/ may still point at it).
    const fs::path incoming = parent / (release_dir.filename().string() + ".new");
    fs::remove_all(incoming, ec);
    ec.clear();
    fs::rename(staging, incoming, ec);
    if (ec) {
        std::error_code copy_ec;
        fs::copy(staging, incoming,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, copy_ec);
        if (copy_ec) {
            if (error) *error = "stage to .new failed: " + copy_ec.message();
            return false;
        }
        std::error_code rm_ec;
        fs::remove_all(staging, rm_ec); // best-effort; copy succeeded
    }

    fs::path outgoing;
    bool have_outgoing = false;
    if (fs::exists(release_dir, ec) || fs::is_symlink(release_dir, ec)) {
        outgoing =
            parent / (release_dir.filename().string() + ".old-" + unique_release_suffix());
        ec.clear();
        fs::rename(release_dir, outgoing, ec);
        if (ec) {
            // Refuse to delete a live tree (game may be running / binary locked).
            std::error_code rm_ec;
            fs::remove_all(incoming, rm_ec);
            if (error)
                *error = "cannot replace live release (is the game running?): " + ec.message();
            return false;
        }
        have_outgoing = true;
    }

    auto restore_outgoing = [&]() {
        if (!have_outgoing) return;
        std::error_code rec_ec;
        if (!fs::exists(release_dir, rec_ec))
            fs::rename(outgoing, release_dir, rec_ec);
    };

    ec.clear();
    fs::rename(incoming, release_dir, ec);
    if (ec) {
        std::error_code copy_ec;
        fs::copy(incoming, release_dir,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, copy_ec);
        if (copy_ec) {
            restore_outgoing();
            std::error_code rm_ec;
            fs::remove_all(incoming, rm_ec);
            if (error) *error = "promote into release dir failed: " + copy_ec.message();
            return false;
        }
        std::error_code rm_ec;
        fs::remove_all(incoming, rm_ec);
    }

    if (have_outgoing) {
        if (outgoing_for_cleanup)
            *outgoing_for_cleanup = outgoing;
        // Else leave .old-* for prune_old_release_dirs after install.json.
    }
    return true;
}

namespace {

void make_executable(const fs::path& p) {
#if !defined(_WIN32)
    std::error_code ec;
    auto st = fs::status(p, ec);
    if (ec || !fs::is_regular_file(st)) return;
    fs::permissions(p,
                    fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
#else
    (void)p;
#endif
}

bool unwrap_nested_archives(const fs::path& dest, const std::string& launch_name,
                            std::string* error) {
    for (int pass = 0; pass < 5; ++pass) {
        if (!launch_name.empty() && !find_named_file(dest, launch_name).empty()) {
            // Binary found — still unwrap pure wrapper zips at top level.
            if (!top_level_only_archives(dest)) return true;
        }

        auto archives = list_archives(dest);
        if (archives.empty()) return true;

        const bool wrapper = top_level_only_archives(dest);
        const bool missing_bin =
            !launch_name.empty() && find_named_file(dest, launch_name).empty();
        if (!wrapper && !missing_bin) return true;

        for (const auto& arch : archives) {
            // Extract into the archive's parent so nested paths stay sensible.
            const fs::path parent = arch.parent_path();
            if (!extract_archive(arch, parent, error)) return false;
            std::error_code ec;
            fs::remove(arch, ec);
        }
    }
    return true;
}

struct GhAsset {
    std::string name;
    std::string browser_download_url;
    std::uint64_t size = 0;
};

struct GhRelease {
    std::string tag;
    std::string html_url;
    std::vector<GhAsset> assets;
};

bool parse_release(const json& j, GhRelease& out, std::string* error) {
    if (!j.is_object()) {
        if (error) *error = "invalid release JSON";
        return false;
    }
    out.tag = j.value("tag_name", "");
    out.html_url = j.value("html_url", "");
    out.assets.clear();
    if (j.contains("assets") && j.at("assets").is_array()) {
        for (const auto& a : j.at("assets")) {
            if (!a.is_object()) continue;
            GhAsset asset;
            asset.name = a.value("name", "");
            asset.browser_download_url = a.value("browser_download_url", "");
            asset.size = a.value("size", 0ull);
            if (!asset.name.empty() && !asset.browser_download_url.empty())
                out.assets.push_back(std::move(asset));
        }
    }
    if (out.tag.empty()) {
        if (error) *error = "release missing tag_name";
        return false;
    }
    return true;
}

bool fetch_latest_release(const std::string& github_slug, GhRelease& out, std::string* error,
                          bool allow_prerelease) {
    if (github_slug.empty() || github_slug.find('/') == std::string::npos) {
        if (error) *error = "invalid github slug (want owner/repo)";
        return false;
    }
    const auto headers = github_http_headers();
    if (!allow_prerelease) {
        const std::string url =
            "https://api.github.com/repos/" + github_slug + "/releases/latest";
        auto res = http_get(url, headers);
        if (!res.ok()) {
            if (error) *error = "GitHub latest: " + (res.error.empty() ? res.body : res.error);
            return false;
        }
        try {
            return parse_release(json::parse(res.body), out, error);
        } catch (const std::exception& e) {
            if (error) *error = e.what();
            return false;
        }
    }

    const std::string url = "https://api.github.com/repos/" + github_slug + "/releases?per_page=10";
    auto res = http_get(url, headers);
    if (!res.ok()) {
        if (error) *error = "GitHub releases: " + (res.error.empty() ? res.body : res.error);
        return false;
    }
    try {
        const json arr = json::parse(res.body);
        if (!arr.is_array() || arr.empty()) {
            if (error) *error = "no releases";
            return false;
        }
        for (const auto& item : arr) {
            if (item.value("draft", false)) continue;
            if (!allow_prerelease && item.value("prerelease", false)) continue;
            return parse_release(item, out, error);
        }
        if (error) *error = "no suitable release";
        return false;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

// Extra globs so catalog "*windows*" still hits win64 / win-x64 style asset names.
std::vector<std::string> asset_glob_aliases(const std::string& glob) {
    std::vector<std::string> out;
    if (glob.empty()) return out;
    out.push_back(glob);
    const std::string g = to_lower(glob);
    auto add = [&](const char* alias) {
        for (const auto& existing : out) {
            if (to_lower(existing) == to_lower(alias)) return;
        }
        out.emplace_back(alias);
    };
    const bool win_ish = g.find("windows") != std::string::npos ||
                         g.find("win64") != std::string::npos ||
                         g.find("win32") != std::string::npos ||
                         g.find("win-") != std::string::npos ||
                         g.find("win_") != std::string::npos ||
                         g.find("*win*") != std::string::npos || g == "*win*";
    if (win_ish) {
        add("*windows*");
        add("*win64*");
        add("*win-x64*");
        add("*win_x64*");
        add("*win32*");
        // Avoid bare "*win*" (matches "wine"); use hyphenated / arch forms.
        add("*-win-*");
        add("*-win.*");
        add("*_win_*");
        add("*_win.*");
    }
    const bool linux_ish = g.find("linux") != std::string::npos ||
                           g.find("appimage") != std::string::npos;
    if (linux_ish) {
        add("*linux*");
        add("*appimage*");
        add("*.AppImage");
    }
    const bool mac_ish = g.find("macos") != std::string::npos || g.find("osx") != std::string::npos ||
                         g.find("darwin") != std::string::npos || g.find("*mac*") != std::string::npos;
    if (mac_ish) {
        add("*macos*");
        add("*osx*");
        add("*darwin*");
        add("*mac*");
    }
    return out;
}

bool asset_name_matches_glob(const std::string& glob, const std::string& name) {
    for (const auto& pattern : asset_glob_aliases(glob)) {
        if (match_glob(pattern, name)) return true;
    }
    return false;
}

const GhAsset* pick_asset(const GhRelease& rel, const std::string& glob) {
    if (glob.empty()) return nullptr;
    const GhAsset* best = nullptr;
    int best_score = -1;
    // When the catalog glob itself is for a tools pack, do not penalize "tools".
    const std::string glob_l = to_lower(glob);
    const bool glob_wants_tools = glob_l.find("tools") != std::string::npos;
    for (const auto& a : rel.assets) {
        if (!asset_name_matches_glob(glob, a.name)) continue;
        int score = 0;
        const std::string n = to_lower(a.name);
        if (ends_with_ci(n, ".zip")) score += 3;
        if (n.find("x64") != std::string::npos || n.find("amd64") != std::string::npos ||
            n.find("x86_64") != std::string::npos || n.find("win64") != std::string::npos)
            score += 2;
        if (n.find("arm64") != std::string::npos || n.find("aarch64") != std::string::npos)
            score += 1; // still acceptable
        // Prefer an exact family hit on the primary glob slightly.
        if (match_glob(glob, a.name)) score += 1;
        // Game releases often ship companion *-tools-* zips on the same tag.
        // Broad globs like "*linux*" match both; prefer the non-tools asset.
        if (!glob_wants_tools && n.find("tools") != std::string::npos) score -= 10;
        if (score > best_score) {
            best_score = score;
            best = &a;
        }
    }
    return best;
}

void set_current_symlink(const fs::path& install_root, const std::string& tag) {
    const fs::path link = install_root / "current";
    const fs::path ptr_path = install_root / "current.path";
    const fs::path target = fs::path("releases") / tag;
    std::error_code ec;
    if (fs::exists(link, ec) || fs::is_symlink(link, ec)) fs::remove(link, ec);
    fs::remove(ptr_path, ec);
#if defined(_WIN32)
    // Directory junction / symlink — may require privileges; fall back to text pointer.
    fs::create_directory_symlink(install_root / target, link, ec);
#else
    fs::create_directory_symlink(target, link, ec);
#endif
    if (ec) {
        // Never leave the install without a current pointer after removing the link.
        std::ofstream ptr(ptr_path);
        if (!ptr)
            throw std::runtime_error("cannot create current symlink or current.path: " +
                                     ec.message());
        ptr << target.generic_string();
    }
}

fs::path resolve_current_dir(const fs::path& install_root) {
    const fs::path link = install_root / "current";
    std::error_code ec;
    if (fs::exists(link, ec)) return fs::weakly_canonical(link, ec);
    const fs::path ptr = install_root / "current.path";
    if (fs::is_regular_file(ptr, ec)) {
        std::ifstream in(ptr);
        std::string rel;
        std::getline(in, rel);
        if (!rel.empty()) return install_root / rel;
    }
    return {};
}

std::string launch_name_for_plan(const InstallPlan& plan) {
    if (!plan.title) return {};
    if (plan.record) {
        if (plan.record->runtime == "wine" || plan.record->target_os == "windows")
            return plan.title->launch.windows;
        if (!plan.record->target_os.empty())
            return plan.title->launch_binary_for_os(plan.record->target_os);
    }
    return plan.title->launch_binary_for_host();
}

// Keep-saves uninstall leaves only apps/<title>/preserved/. That is not a
// broken install — treat as not installed with optional restore-on-reinstall.
bool classify_install_root_leftovers(const fs::path& install_root, bool* has_preserved,
                                     bool* other_leftovers) {
    if (has_preserved) *has_preserved = false;
    if (other_leftovers) *other_leftovers = false;
    std::error_code ec;
    if (!fs::exists(install_root, ec)) return false;

    bool saw_preserved = false;
    bool saw_other = false;
    for (auto it = fs::directory_iterator(install_root, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name == "preserved") {
            saw_preserved = true;
            // Non-empty preserved/ counts; empty dir is leftover noise.
            if (has_preserved && fs::is_directory(it->path(), ec) &&
                !fs::is_empty(it->path(), ec)) {
                *has_preserved = true;
            }
            continue;
        }
        saw_other = true;
    }
    if (other_leftovers) *other_leftovers = saw_other;
    (void)saw_preserved;
    return true;
}

// Playable release under apps/<title>/releases/<tag>/ (never src/).
struct ReleaseCandidate {
    fs::path release_dir;
    fs::path binary;
    std::string tag; // releases/ folder name
    std::string resolved_name;
    int score = 0;
    std::filesystem::file_time_type mtime{};
};

bool tag_looks_like_local_build(const std::string& tag) {
    const std::string t = to_lower(tag);
    return t.rfind("build-", 0) == 0 || t.rfind("build_", 0) == 0;
}

int score_release_candidate(const ReleaseCandidate& c, const InstallRecord* rec,
                            bool any_build_tag) {
    int score = 0;
    const bool build_tag = tag_looks_like_local_build(c.tag);
    if (build_tag) score += 1000;
    if (rec && !rec->tag.empty() && sanitize_tag(rec->tag) == c.tag) {
        score += 200;
        if (rec->method == "build") score += 400;
        if (rec->method == "zip" && any_build_tag && !build_tag)
            score -= 300; // prefer a sibling build-* over the recorded zip pin
    }
    if (rec && rec->method == "build" && build_tag) score += 100;
    // Setup-host GitHub zips share the launch name; demote non-build tags when a
    // local build release exists so Play does not open the wizard by accident.
    if (any_build_tag && !build_tag) score -= 500;
    return score;
}

bool release_candidate_better(const ReleaseCandidate& a, const ReleaseCandidate& b) {
    if (a.score != b.score) return a.score > b.score;
    if (a.mtime != b.mtime) return a.mtime > b.mtime;
    return a.tag > b.tag; // stable lexicographic tie-break
}

std::vector<ReleaseCandidate> collect_release_candidates(const fs::path& install_root,
                                                         const std::string& bin,
                                                         const std::string& target_os,
                                                         const InstallRecord* rec) {
    std::vector<ReleaseCandidate> out;
    std::error_code ec;
    const fs::path releases = install_root / "releases";
    if (!fs::is_directory(releases, ec) || bin.empty()) return out;

    bool any_build_tag = false;
    for (auto it = fs::directory_iterator(releases, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const std::string tag = it->path().filename().string();
        if (tag.empty() || tag[0] == '.') continue;
        if (tag_looks_like_local_build(tag)) any_build_tag = true;
    }

    for (auto it = fs::directory_iterator(releases, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const fs::path root = it->path();
        const std::string tag = root.filename().string();
        if (tag.empty() || tag[0] == '.') continue;
        std::string resolved;
        fs::path hit = resolve_launch_binary(root, bin, target_os, &resolved);
        if (hit.empty() && rec && !rec->binary.empty()) {
            const fs::path from_rec = root / rec->binary;
            if (fs::is_regular_file(from_rec, ec)) {
                hit = from_rec;
                resolved = fs::path(rec->binary).filename().string();
            }
        }
        if (hit.empty()) continue;
        ReleaseCandidate c;
        c.release_dir = root;
        c.binary = hit;
        c.tag = tag;
        c.resolved_name = resolved.empty() ? hit.filename().string() : resolved;
        c.mtime = fs::last_write_time(hit, ec);
        c.score = score_release_candidate(c, rec, any_build_tag);
        out.push_back(std::move(c));
    }
    std::sort(out.begin(), out.end(), [](const ReleaseCandidate& a, const ReleaseCandidate& b) {
        return release_candidate_better(a, b);
    });
    return out;
}

void apply_release_candidate(InstallPlan& plan, const ReleaseCandidate& best, bool heal) {
    std::error_code ec;
    plan.binary_path = best.binary;
    plan.installed = true;
    plan.install_dir_present = true;
    plan.installed_tag = best.tag;
    if (heal) {
        plan.message = "installed (healed current → " + best.tag + "): " + best.binary.string();
    } else {
        plan.message = "installed: " + best.binary.string() + " [" + best.tag + "]";
    }
    if (plan.record && plan.record->runtime == "wine") plan.message += " [wine]";

    InstallRecord rec = plan.record ? *plan.record : InstallRecord{};
    if (rec.title_id.empty() && plan.title) rec.title_id = plan.title->id;
    const fs::path rel_bin = fs::relative(best.binary, best.release_dir, ec);
    const std::string rel = ec ? best.resolved_name : rel_bin.generic_string();
    const bool record_stale =
        rec.tag != best.tag || rec.binary != rel ||
        (tag_looks_like_local_build(best.tag) && rec.method != "build");
    if (heal || record_stale || !plan.record) {
        rec.tag = best.tag;
        rec.binary = rel;
        if (tag_looks_like_local_build(best.tag)) rec.method = "build";
        if (rec.host_os.empty()) rec.host_os = host_os_key();
        if (rec.target_os.empty()) rec.target_os = host_os_key();
        if (rec.runtime.empty()) rec.runtime = "native";
        save_install_record(plan.install_root, rec);
    }
    plan.record = rec;
    if (heal) {
        try {
            set_current_symlink(plan.install_root, best.tag);
        } catch (...) {
            // Launch still works via binary_path; current heal is best-effort.
        }
    }
}

void fill_from_disk(InstallPlan& plan) {
    plan.record.reset();
    plan.installed = false;
    plan.installed_tag.clear();
    plan.binary_path.clear();
    plan.expected_binary.clear();
    plan.install_dir_present = false;
    plan.has_preserved_state = false;
    std::error_code ec;

    bool has_preserved = false;
    bool other_leftovers = false;
    const bool root_exists =
        classify_install_root_leftovers(plan.install_root, &has_preserved, &other_leftovers);
    plan.has_preserved_state = has_preserved;

    InstallRecord rec = load_install_record(plan.install_root);
    if (!rec.title_id.empty()) {
        plan.record = rec;
        plan.installed_tag = rec.tag;
    }

    const std::string bin = launch_name_for_plan(plan);
    plan.expected_binary = bin;
    const std::string target_os =
        (plan.record && !plan.record->target_os.empty()) ? plan.record->target_os
                                                        : host_os_key();

    // Accept only a finished install layout — never the setup host vendored
    // inside src/<tag>/ from a release/source zip (that falsely enables Play).
    auto candidates =
        collect_release_candidates(plan.install_root, bin, target_os,
                                   plan.record ? &*plan.record : nullptr);

    const fs::path current = resolve_current_dir(plan.install_root);
    const ReleaseCandidate* current_hit = nullptr;
    if (!current.empty() && !candidates.empty()) {
        const fs::path cur_canon = fs::weakly_canonical(current, ec);
        for (const auto& c : candidates) {
            const fs::path rel_canon = fs::weakly_canonical(c.release_dir, ec);
            if (!cur_canon.empty() && !rel_canon.empty() && cur_canon == rel_canon) {
                current_hit = &c;
                break;
            }
        }
    }

    if (!candidates.empty()) {
        const ReleaseCandidate& best = candidates.front();
        const bool current_is_best =
            current_hit && current_hit->tag == best.tag && current_hit->score == best.score;
        const bool need_heal = !current_is_best;
        apply_release_candidate(plan, best, need_heal);
        return;
    }

    // current/ present but no scored release candidate (broken link / empty releases).
    if (!current.empty()) {
        fs::path candidate;
        if (plan.record && !plan.record->binary.empty())
            candidate = current / plan.record->binary;
        if (candidate.empty() || !fs::exists(candidate, ec))
            candidate = find_named_file(current, bin);
        if (candidate.empty() && !bin.empty()) candidate = current / bin;
        if (!candidate.empty() && fs::exists(candidate, ec)) {
            plan.binary_path = candidate;
            plan.installed = true;
            plan.install_dir_present = true;
            plan.message = "installed: " + candidate.string();
            if (!plan.installed_tag.empty())
                plan.message += " [" + plan.installed_tag + "]";
            if (plan.record && plan.record->runtime == "wine")
                plan.message += " [wine]";
            return;
        }
        plan.install_dir_present = true;
        // Fall through — may still find a flat binary.
    }

    if (!bin.empty()) {
        const fs::path top = plan.install_root / bin;
        if (fs::is_regular_file(top, ec)) {
            plan.binary_path = top;
            plan.installed = true;
            plan.install_dir_present = true;
            plan.message = "installed (flat): " + top.string();
            return;
        }
    }

    // Partial/broken install artifacts (src/, releases/, install.json, …).
    // preserved/-only after keep-saves uninstall is not NEEDS SETUP.
    if (root_exists && other_leftovers) {
        plan.install_dir_present = true;
        plan.message = "install folder present but binary missing";
        if (!bin.empty()) plan.message += ": " + bin;
        return;
    }
    if (plan.has_preserved_state) {
        plan.message = "not installed (preserved saves/config under " +
                       (plan.install_root / "preserved").string() + ")";
        return;
    }
    plan.message = "not installed under " + plan.install_root.string();
}

} // namespace

fs::path resolve_current_release_dir(const fs::path& install_root) {
    const fs::path link = install_root / "current";
    std::error_code ec;
    if (fs::exists(link, ec)) return fs::weakly_canonical(link, ec);
    const fs::path ptr = install_root / "current.path";
    if (fs::is_regular_file(ptr, ec)) {
        std::ifstream in(ptr);
        std::string rel;
        std::getline(in, rel);
        if (!rel.empty()) return install_root / rel;
    }
    return {};
}

size_t prune_old_release_dirs(const fs::path& install_root, const std::string& keep_tag,
                              std::string* note) {
    std::string keep = keep_tag;
    for (char& c : keep) {
        if (c == '/' || c == '\\' || c == ':' || c == 0) c = '_';
    }
    if (keep.empty()) keep = "unknown";
    const fs::path releases = install_root / "releases";
    std::error_code ec;
    if (!fs::is_directory(releases, ec)) return 0;
    size_t removed = 0;
    for (auto it = fs::directory_iterator(releases, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        if (it->path().filename().string() == keep) continue;
        std::error_code rm_ec;
        fs::remove_all(it->path(), rm_ec);
        if (!rm_ec) ++removed;
    }
    if (note && removed > 0)
        *note = "removed " + std::to_string(removed) + " old release folder(s)\n";
    return removed;
}

OldReleaseCleanupResult cleanup_old_release_dirs(const Paths& paths, const Catalog& catalog) {
    OldReleaseCleanupResult result;
    std::error_code ec;
    const AppConfig cfg = load_app_config(paths.config_path);
    for (const auto& title : catalog.titles) {
        if (title.install_dir_name.empty()) continue;
        InstallPlan plan = inspect_install_any(paths, cfg, title);
        const fs::path install_root = plan.install_root;
        const fs::path releases = install_root / "releases";
        if (!fs::is_directory(releases, ec)) continue;

        size_t release_count = 0;
        for (auto it = fs::directory_iterator(releases, ec); !ec && it != fs::directory_iterator();
             it.increment(ec)) {
            if (it->is_directory(ec)) ++release_count;
        }
        if (release_count <= 1) continue;

        ++result.titles_scanned;
        std::string keep_tag = plan.installed_tag;
        if (plan.record && !plan.record->tag.empty()) keep_tag = plan.record->tag;
        if (keep_tag.empty()) {
            const fs::path cur = resolve_current_release_dir(install_root);
            if (!cur.empty()) keep_tag = cur.filename().string();
        }
        if (keep_tag.empty()) {
            result.messages.push_back(title.id + ": skipped — cannot determine current release");
            continue;
        }

        std::string stash_note;
        if (!stash_user_state_for_update(paths, title, &stash_note)) {
            result.messages.push_back(title.id + ": stash failed — skipped prune (" + stash_note +
                                      ")");
            continue;
        }
        fs::path current = resolve_current_release_dir(install_root);
        if (current.empty()) {
            // Fall back to releases/<sanitized keep_tag>.
            std::string keep = keep_tag;
            for (char& c : keep) {
                if (c == '/' || c == '\\' || c == ':' || c == 0) c = '_';
            }
            current = releases / keep;
        }
        if (fs::is_directory(current, ec)) {
            std::string restore_note;
            restore_user_state(install_root, current, &restore_note);
            if (!restore_note.empty()) result.messages.push_back(title.id + ": " + restore_note);
        }
        if (!stash_note.empty()) result.messages.push_back(title.id + ": " + stash_note);

        std::string prune_note;
        const size_t n = prune_old_release_dirs(install_root, keep_tag, &prune_note);
        if (n > 0) {
            ++result.titles_cleaned;
            result.dirs_removed += static_cast<int>(n);
            result.messages.push_back(title.id + ": removed " + std::to_string(n) +
                                      " old release folder(s) (kept " + keep_tag + ")");
        }
    }

    std::ostringstream oss;
    if (result.dirs_removed == 0)
        oss << "No old update folders to remove";
    else
        oss << "Cleaned " << result.dirs_removed << " old release folder(s) across "
            << result.titles_cleaned << " title(s)";
    result.message = oss.str();
    return result;
}

void restore_user_state(const fs::path& install_root, const fs::path& release_dir,
                        std::string* note) {
    const fs::path preserved = install_root / "preserved";
    std::error_code ec;
    if (!fs::is_directory(preserved, ec)) return;
    size_t n = 0;
    for (auto it = fs::recursive_directory_iterator(
             preserved, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string rel =
            normalize_preserved_rel(rel_posix_under(preserved, it->path()));
        if (rel.empty() || !is_user_state_path(rel, {})) continue;
        const fs::path dest = release_dir / rel;
        fs::create_directories(dest.parent_path(), ec);
        // Preserved user state wins over any defaults shipped in the package.
        fs::copy_file(it->path(), dest, fs::copy_options::overwrite_existing, ec);
        if (!ec) ++n;
    }
    if (note && n > 0)
        *note = "restored " + std::to_string(n) + " preserved save/config file(s) into release\n";
}

bool stash_user_state_for_update(const Paths& paths, const Title& title, std::string* note) {
    const AppConfig cfg = load_app_config(paths.config_path);
    const InstallPlan plan = inspect_install_any(paths, cfg, title);
    const fs::path install_root = plan.install_root.empty()
                                      ? (paths.apps_dir / title.install_dir_name)
                                      : plan.install_root;
    const fs::path preserved = install_root / "preserved";
    const auto globs = save_globs_for_title(title);
    std::error_code ec;
    std::vector<fs::path> roots;
    std::unordered_set<std::string> seen_roots;
    auto add_root = [&](const fs::path& p) {
        std::error_code lec;
        if (!fs::is_directory(p, lec)) return;
        const fs::path canon = fs::weakly_canonical(p, lec);
        const fs::path use = lec ? p : canon;
        if (!seen_roots.insert(use.string()).second) return;
        roots.push_back(use);
    };
    const fs::path releases_dir = install_root / "releases";
    if (fs::is_directory(releases_dir, ec)) {
        for (auto it = fs::directory_iterator(releases_dir, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (it->is_directory(ec)) add_root(it->path());
        }
    }
    if (!plan.binary_path.empty()) add_root(plan.binary_path.parent_path());

    size_t n = 0;
    std::string err;
    for (const auto& root : roots) {
        n += stash_user_state_from(root, preserved, globs, nullptr, &err);
        if (!err.empty()) break;
    }
    if (note) {
        if (!err.empty())
            *note = err;
        else if (n > 0)
            *note = "preserved " + std::to_string(n) + " save/config file(s)\n";
    }
    return err.empty();
}

bool extract_archive_to(const fs::path& archive, const fs::path& dest, std::string* error) {
    return extract_archive(archive, dest, error);
}

InstallRecord load_install_record(const fs::path& install_root) {
    InstallRecord rec;
    const fs::path path = install_root / "install.json";
    std::ifstream in(path);
    if (!in) return rec;
    try {
        json j;
        in >> j;
        rec.schema_version = j.value("schema_version", 1);
        rec.title_id = j.value("title_id", "");
        rec.github = j.value("github", "");
        rec.tag = j.value("tag", "");
        rec.asset_name = j.value("asset_name", "");
        rec.binary = j.value("binary", "");
        rec.host_os = j.value("host_os", "");
        rec.target_os = j.value("target_os", "");
        rec.runtime = j.value("runtime", "");
        rec.installed_at = j.value("installed_at", "");
        rec.release_url = j.value("release_url", "");
        rec.method = j.value("method", "");
        if (rec.method.empty()) rec.method = "zip";
        rec.source_ref = j.value("source_ref", "");
        rec.sdk_tag = j.value("sdk_tag", "");
        rec.toolchain_tag = j.value("toolchain_tag", "");
        rec.bios_source = j.value("bios_source", "");
        // Legacy installs: infer Wine from a Windows .exe on a non-Windows host.
        if (rec.runtime.empty()) {
            const std::string& b = rec.binary;
            const bool looks_exe =
                b.size() >= 4 &&
                (b.compare(b.size() - 4, 4, ".exe") == 0 || b.compare(b.size() - 4, 4, ".EXE") == 0);
            if (looks_exe && host_os_key() != "windows") {
                rec.runtime = "wine";
                if (rec.target_os.empty()) rec.target_os = "windows";
            } else {
                rec.runtime = "native";
                if (rec.target_os.empty())
                    rec.target_os = rec.host_os.empty() ? host_os_key() : rec.host_os;
            }
        }
    } catch (...) {
        return InstallRecord{};
    }
    return rec;
}

bool save_install_record(const fs::path& install_root, const InstallRecord& rec) {
    std::error_code ec;
    fs::create_directories(install_root, ec);
    const fs::path path = install_root / "install.json";
    json j = {{"schema_version", rec.schema_version},
              {"title_id", rec.title_id},
              {"github", rec.github},
              {"tag", rec.tag},
              {"asset_name", rec.asset_name},
              {"binary", rec.binary},
              {"host_os", rec.host_os},
              {"target_os", rec.target_os},
              {"runtime", rec.runtime},
              {"installed_at", rec.installed_at},
              {"release_url", rec.release_url},
              {"method", rec.method.empty() ? "zip" : rec.method}};
    if (!rec.source_ref.empty()) j["source_ref"] = rec.source_ref;
    if (!rec.sdk_tag.empty()) j["sdk_tag"] = rec.sdk_tag;
    if (!rec.toolchain_tag.empty()) j["toolchain_tag"] = rec.toolchain_tag;
    if (!rec.bios_source.empty()) j["bios_source"] = rec.bios_source;
    std::ofstream out(path);
    if (!out) return false;
    out << j.dump(2) << "\n";
    return static_cast<bool>(out);
}

void write_partial_install_record(const fs::path& install_root, const Title& title,
                                  const std::string& release_tag, const std::string& asset_name,
                                  const std::string& release_url, const std::string& target_os,
                                  bool use_wine, const std::string& tag_dir) {
    InstallRecord rec;
    rec.title_id = title.id;
    rec.github = title.release.github;
    rec.tag = release_tag;
    rec.asset_name = asset_name;
    rec.binary = ""; // unresolved — hub shows NEEDS SETUP
    rec.host_os = host_os_key();
    rec.target_os = target_os;
    rec.runtime = use_wine ? "wine" : "native";
    rec.installed_at = iso8601_now();
    rec.release_url = release_url;
    save_install_record(install_root, rec);
    set_current_symlink(install_root, tag_dir);
}

std::string resolve_wine_binary(std::string* error) {
#if defined(_WIN32)
    if (error) *error = "Wine is not used on native Windows";
    return {};
#else
    const char* path_env = std::getenv("PATH");
    if (!path_env || !*path_env) {
        if (error) *error = "PATH is empty; cannot find wine";
        return {};
    }
    const char* names[] = {"wine64", "wine"};
    std::istringstream iss(path_env);
    std::string dir;
    while (std::getline(iss, dir, ':')) {
        if (dir.empty()) continue;
        for (const char* name : names) {
            const fs::path candidate = fs::path(dir) / name;
            std::error_code ec;
            if (!fs::exists(candidate, ec)) continue;
            if (::access(candidate.c_str(), X_OK) == 0) return candidate.string();
        }
    }
    if (error) *error = "wine/wine64 not found on PATH";
    return {};
#endif
}

bool host_supports_wine() {
#if defined(_WIN32)
    return false;
#else
    return !resolve_wine_binary(nullptr).empty();
#endif
}

std::string fetch_latest_release_tag(const std::string& github_slug, std::string* error,
                                     bool allow_prerelease) {
    GhRelease rel;
    if (!fetch_latest_release(github_slug, rel, error, allow_prerelease)) return {};
    return rel.tag;
}

bool install_built_with_openbios(const InstallPlan& plan) {
    if (plan.record && !plan.record->bios_source.empty())
        return plan.record->bios_source == "openbios";
    if (!plan.record || plan.record->method != "build") return false;
    if (plan.install_root.empty()) return false;

    std::error_code ec;
    const fs::path src_base = plan.install_root / "src";
    std::vector<fs::path> roots;
    const fs::path current = src_base / "current";
    if (fs::is_directory(current, ec)) roots.push_back(current);
    if (fs::is_directory(src_base, ec)) {
        for (auto it = fs::directory_iterator(src_base, ec); !ec && it != fs::directory_iterator();
             it.increment(ec)) {
            if (!it->is_directory(ec)) continue;
            if (it->path().filename() == "current") continue;
            roots.push_back(it->path());
        }
    }

    for (const auto& root : roots) {
        const fs::path stamp = root / ".retcomm-codegen.json";
        if (fs::is_regular_file(stamp, ec)) {
            try {
                std::ifstream in(stamp);
                const json meta = json::parse(in);
                const std::string bios = meta.value("bios", "");
                if (bios == "openbios") return true;
                if (!bios.empty() && bios != "openbios" && bios != "bios-missing") return false;
            } catch (...) {
            }
        }
        const fs::path bios_gen = root / "psxrecomp" / "generated";
        const bool has_open = fs::is_regular_file(bios_gen / "OpenBIOS_dispatch.c", ec);
        const bool has_scph = fs::is_regular_file(bios_gen / "SCPH1001_dispatch.c", ec);
        if (has_open && !has_scph) return true;
        if (has_scph) return false;
    }
    return false;
}

std::string install_release_compare_tag(const InstallRecord& rec) {
    if (rec.method == "build") {
        if (!rec.source_ref.empty()) return rec.source_ref;
        if (rec.tag.rfind("build-", 0) == 0) return rec.tag.substr(6);
    }
    return rec.tag;
}

std::string install_release_compare_tag(const InstallPlan& plan) {
    if (plan.record) return install_release_compare_tag(*plan.record);
    if (plan.installed_tag.rfind("build-", 0) == 0) return plan.installed_tag.substr(6);
    return plan.installed_tag;
}

InstallPlan inspect_install(const Paths& paths, const Title& title) {
    return inspect_install(paths, title, paths.apps_dir);
}

InstallPlan inspect_install(const Paths& paths, const Title& title, const fs::path& apps_dir) {
    InstallPlan plan;
    plan.title = &title;
    const fs::path apps = apps_dir.empty() ? paths.apps_dir : apps_dir;
    plan.install_root = apps / title.install_dir_name;
    plan.current_link = plan.install_root / "current";
    fill_from_disk(plan);
    return plan;
}

InstallPlan inspect_install_any(const Paths& paths, const AppConfig& cfg, const Title& title) {
    InstallPlan best_installed;
    bool have_installed = false;
    InstallPlan best_partial;
    bool have_partial = false;
    const fs::path preferred_root = resolve_default_install_root(cfg, paths);

    auto prefer_installed = [&](const InstallPlan& a, const InstallPlan& b) -> bool {
        const bool a_build = a.record && a.record->method == "build";
        const bool b_build = b.record && b.record->method == "build";
        if (a_build != b_build) return a_build;
        const bool a_build_tag = tag_looks_like_local_build(a.installed_tag);
        const bool b_build_tag = tag_looks_like_local_build(b.installed_tag);
        if (a_build_tag != b_build_tag) return a_build_tag;
        if (a.installed_tag != b.installed_tag) return a.installed_tag > b.installed_tag;
        if (a.install_root == preferred_root && b.install_root != preferred_root) return true;
        if (b.install_root == preferred_root && a.install_root != preferred_root) return false;
        return false;
    };

    for (const auto& root : scan_install_roots(cfg, paths)) {
        auto plan = inspect_install(paths, title, root.path);
        if (plan.installed) {
            if (!have_installed || prefer_installed(plan, best_installed)) {
                best_installed = std::move(plan);
                have_installed = true;
            }
            continue;
        }
        if (!have_partial && (plan.install_dir_present || plan.has_preserved_state)) {
            best_partial = std::move(plan);
            have_partial = true;
        }
    }
    if (have_installed) return best_installed;
    if (have_partial) return best_partial;
    return inspect_install(paths, title, preferred_root);
}

InstallPlan plan_install(const Paths& paths, const Title& title, const InstallOptions& opts) {
    Paths job_paths = with_apps_dir(paths, opts.apps_dir);
    InstallPlan plan = inspect_install(job_paths, title);
    const bool use_wine = opts.use_wine;
    const std::string target_os = use_wine ? "windows" : host_os_key();
    std::ostringstream oss;
    oss << "install plan for " << title.id << "\n"
        << "  target:  " << plan.install_root.string() << "\n"
        << "  github:  "
        << (title.release.github.empty() ? "(unset)" : title.release.github) << "\n"
        << "  asset:   " << title.asset_glob_for_os(target_os) << "\n"
        << "  binary:  " << title.launch_binary_for_os(target_os) << "\n"
        << "  runtime: " << (use_wine ? "wine" : "native") << "\n";

    if (opts.check_latest && !title.release.github.empty()) {
        std::string err;
        const bool allow_pre = opts.allow_prerelease || title.release.allow_prerelease;
        // Prefer hub/CLI hint, then the same ReleaseTagCache as Check Updates (keeps
        // serving a recent tag when GitHub returns 403).
        if (!opts.hint_latest_tag.empty()) {
            plan.latest_tag = opts.hint_latest_tag;
        } else {
            ReleaseTagCache tag_cache(release_tags_cache_path(paths));
            plan.latest_tag =
                tag_cache.latest_tag(title.release.github, allow_pre, /*force=*/false, &err);
            tag_cache.save_if_dirty();
        }
        if (!plan.latest_tag.empty()) {
            const std::string have = install_release_compare_tag(plan);
            if (plan.installed && !have.empty() && release_tag_cmp(have, plan.latest_tag) > 0)
                plan.latest_tag = have; // installed ahead of stale cache
            oss << "  latest:  " << plan.latest_tag << "\n";
            if (plan.installed && !have.empty() && release_tag_cmp(have, plan.latest_tag) < 0) {
                plan.update_available = true;
                oss << "  update:  available (" << have << " → " << plan.latest_tag << ")\n";
            } else if (plan.installed && !have.empty()) {
                oss << "  update:  up to date\n";
            }
        } else if (!err.empty()) {
            oss << "  latest:  (unavailable: " << err << ")\n";
        }
    }

    if (plan.installed) {
        oss << "  status:  " << plan.message << "\n";
    } else {
        oss << "  status:  not installed\n";
    }
    plan.message = oss.str();
    return plan;
}

fs::path release_download_cache_path(const Paths& paths, const std::string& github_slug,
                                    const std::string& tag, const std::string& asset_name) {
    auto sanitize = [](std::string s) {
        for (char& c : s) {
            if (c == '/' || c == '\\' || c == ':' || c == 0) c = '_';
        }
        if (s.empty()) s = "unknown";
        return s;
    };
    const std::string file = fs::path(asset_name).filename().string();
    return paths.data_dir / "cache" / "releases" / sanitize(github_slug) / sanitize_tag(tag) /
           (file.empty() ? sanitize(asset_name) : file);
}

namespace {

struct EnsuredReleaseAsset {
    bool ok = false;
    bool from_cache = false;
    bool skipped_uptodate = false;
    fs::path download;
    GhRelease rel;
    std::string asset_name;
    std::uint64_t asset_size = 0;
    std::string browser_download_url;
    std::string target_os;
    bool use_wine = false;
    std::string message;
};

EnsuredReleaseAsset ensure_release_asset_cached(const Paths& paths, const Title& title,
                                                const InstallOptions& opts,
                                                bool allow_skip_uptodate,
                                                HttpProgressFn on_progress) {
    EnsuredReleaseAsset out;
    if (title.release.github.empty()) {
        out.message = "no release.github in catalog for " + title.id;
        return out;
    }

    out.use_wine = opts.use_wine;
    if (out.use_wine) {
        if (host_os_key() == "windows") {
            out.message = "Wine install is only for Linux/macOS hosts";
            return out;
        }
        if (!title.supports_wine_install()) {
            out.message = "catalog has no Windows asset_glob/launch binary for " + title.id;
            return out;
        }
        std::string wine_err;
        if (resolve_wine_binary(&wine_err).empty()) {
            out.message = "Wine install requested but " + wine_err;
            return out;
        }
    }

    out.target_os = out.use_wine ? "windows" : host_os_key();
    const std::string glob = title.asset_glob_for_os(out.target_os);
    if (glob.empty()) {
        out.message = "no asset_glob for target OS (" + out.target_os + ")";
        return out;
    }

    std::string err;
    const bool allow_pre = opts.allow_prerelease || title.release.allow_prerelease;
    const bool fetched = fetch_latest_release(title.release.github, out.rel, &err, allow_pre);
    if (!fetched) {
        // Live API failed (often HTTP 403). Fall back to a known tag + on-disk cache
        // so Update can still apply a zip that Check Updates / prefetch already got.
        std::string tag = opts.hint_latest_tag;
        if (tag.empty()) {
            ReleaseTagCache tag_cache(release_tags_cache_path(paths));
            tag = tag_cache.latest_tag(title.release.github, allow_pre, /*force=*/false, nullptr);
            tag_cache.save_if_dirty();
        }
        if (tag.empty()) {
            out.message = "failed to fetch release: " + err;
            return out;
        }
        const fs::path cache_dir =
            release_download_cache_path(paths, title.release.github, tag, "_").parent_path();
        std::error_code ec;
        std::string best_name;
        fs::path best_path;
        std::uint64_t best_size = 0;
        int best_score = -1;
        if (fs::is_directory(cache_dir, ec)) {
            for (auto it = fs::directory_iterator(cache_dir, ec);
                 !ec && it != fs::directory_iterator(); it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                const std::string name = it->path().filename().string();
                if (name.size() > 5 && name.compare(name.size() - 5, 5, ".part") == 0) continue;
                if (!asset_name_matches_glob(glob, name)) continue;
                int score = ends_with_ci(name, ".zip") ? 3 : 0;
                if (score < best_score) continue;
                best_score = score;
                best_name = name;
                best_path = it->path();
                best_size = fs::file_size(best_path, ec);
            }
        }
        if (best_name.empty()) {
            out.message = "failed to fetch release: " + err +
                          " (no cached asset for " + tag + ")";
            return out;
        }
        out.rel.tag = tag;
        out.rel.html_url =
            "https://github.com/" + title.release.github + "/releases/tag/" + tag;
        out.asset_name = best_name;
        out.asset_size = best_size;
        out.browser_download_url.clear();
        out.download = best_path;
        out.ok = true;
        out.from_cache = true;
        out.message = "cached " + best_name + " (" + tag + ") — offline/API fallback";
        if (allow_skip_uptodate) {
            InstallPlan plan = inspect_install(paths, title);
            const std::string have = install_release_compare_tag(plan);
            if (plan.installed && !opts.force && !have.empty() && have == tag) {
                const bool same_runtime =
                    !plan.record ||
                    (out.use_wine ? plan.record->runtime == "wine"
                                  : plan.record->runtime != "wine");
                if (same_runtime) {
                    out.skipped_uptodate = true;
                    out.message = "already installed at " + tag;
                }
            }
        }
        return out;
    }

    if (allow_skip_uptodate) {
        InstallPlan plan = inspect_install(paths, title);
        // Compare release tag to source_ref / zip tag — not raw "build-<ref>".
        const std::string have = install_release_compare_tag(plan);
        if (plan.installed && !opts.force && !have.empty() && have == out.rel.tag) {
            const bool same_runtime =
                !plan.record ||
                (out.use_wine ? plan.record->runtime == "wine"
                              : plan.record->runtime != "wine");
            if (same_runtime) {
                out.ok = true;
                out.skipped_uptodate = true;
                out.message = "already installed at " + out.rel.tag;
                return out;
            }
        }
    }

    const GhAsset* asset = pick_asset(out.rel, glob);
    if (!asset) {
        out.message = "no release asset matching '" + glob + "' on " + out.rel.tag +
                      " (target " + out.target_os + ")";
        return out;
    }
    out.asset_name = asset->name;
    out.asset_size = asset->size;
    out.browser_download_url = asset->browser_download_url;
    out.download =
        release_download_cache_path(paths, title.release.github, out.rel.tag, asset->name);

    // Serialize cache fills so hub prefetch and Update share one .part safely.
    static std::mutex release_cache_mu;
    std::lock_guard<std::mutex> cache_lock(release_cache_mu);

    // Migrate a leftover per-title .download if it already matches.
    {
        std::error_code ec;
        const fs::path legacy =
            paths.apps_dir / title.install_dir_name / ".download" / asset->name;
        if (!fs::exists(out.download, ec) && fs::is_regular_file(legacy, ec) &&
            asset->size > 0 && fs::file_size(legacy, ec) == asset->size) {
            fs::create_directories(out.download.parent_path(), ec);
            fs::copy_file(legacy, out.download, fs::copy_options::overwrite_existing, ec);
        }
    }

    std::error_code ec;
    if (asset->size > 0 && fs::is_regular_file(out.download, ec) &&
        fs::file_size(out.download, ec) == asset->size) {
        out.ok = true;
        out.from_cache = true;
        out.message = "cached " + asset->name + " (" + out.rel.tag + ")";
        return out;
    }

    ensure_dirs(paths);
    fs::create_directories(out.download.parent_path(), ec);

    auto headers = github_http_headers();
    // browser_download_url wants a normal Accept for the binary body.
    headers.erase(std::remove_if(headers.begin(), headers.end(),
                                 [](const auto& h) { return h.first == "Accept"; }),
                  headers.end());
    headers.emplace_back("Accept", "application/octet-stream");

    if (!http_download(asset->browser_download_url, out.download, &err, headers,
                       std::move(on_progress), asset->size)) {
        out.message = "download failed: " + err;
        return out;
    }
    out.ok = true;
    out.message = "downloaded " + asset->name + " (" + out.rel.tag + ")";
    return out;
}

} // namespace

ResolvedReleaseZip resolve_title_release_zip(const Paths& paths, const Title& title,
                                             const InstallOptions& opts,
                                             HttpProgressFn on_progress) {
    auto ensured =
        ensure_release_asset_cached(paths, title, opts, /*allow_skip_uptodate=*/false,
                                    std::move(on_progress));
    ResolvedReleaseZip out;
    out.ok = ensured.ok;
    out.from_cache = ensured.from_cache;
    out.zip_path = ensured.download;
    out.tag = ensured.rel.tag;
    out.asset_name = ensured.asset_name;
    out.message = ensured.message;
    return out;
}

PrefetchResult prefetch_title_release(const Paths& paths, const Title& title,
                                      const InstallOptions& opts) {
    PrefetchResult result;
    InstallOptions o = opts;
    if (!o.use_wine) {
        InstallPlan existing = inspect_install(paths, title);
        if (existing.record && existing.record->runtime == "wine") o.use_wine = true;
    }
    auto ensured = ensure_release_asset_cached(paths, title, o, /*allow_skip_uptodate=*/true,
                                               {});
    result.ok = ensured.ok;
    result.skipped = ensured.skipped_uptodate || ensured.from_cache;
    result.cached_path = ensured.download;
    if (ensured.skipped_uptodate)
        result.message = title.id + ": " + ensured.message + " — prefetch skipped\n";
    else if (ensured.from_cache)
        result.message = title.id + ": " + ensured.message + " — already prefetched\n";
    else if (ensured.ok)
        result.message = title.id + ": " + ensured.message + " — ready for update\n";
    else
        result.message = title.id + ": prefetch failed — " + ensured.message + "\n";
    return result;
}

InstallResult install_title(const Paths& paths_in, const Title& title, const InstallOptions& opts) {
    Paths paths = with_apps_dir(paths_in, opts.apps_dir);
    ensure_apps_dir(paths);
    InstallResult result;
    result.plan = inspect_install(paths, title);

    if (title.release.github.empty()) {
        result.message = "no release.github in catalog for " + title.id;
        return result;
    }

#if !defined(_WIN32)
    // Installing as root into a normal user's XDG data dir leaves rom.cfg /
    // settings.toml unwritable, so Play cannot seed the Wine library path.
    if (geteuid() == 0) {
        std::cerr << "warning: installing as root — install files will be owned by "
                     "root; run the hub/CLI as your normal user instead\n";
    }
#endif

    auto ensured = ensure_release_asset_cached(
        paths, title, opts, /*allow_skip_uptodate=*/true,
        [](std::uint64_t got, std::uint64_t total) {
            if (total == 0) {
                std::cerr << "\r  " << got << " bytes" << std::flush;
            } else {
                const int pct = static_cast<int>((got * 100) / total);
                std::cerr << "\r  " << pct << "%  (" << got << "/" << total << ")" << std::flush;
            }
        });
    if (ensured.skipped_uptodate) {
        result.ok = true;
        result.skipped = true;
        result.plan.latest_tag = ensured.rel.tag;
        result.plan.update_available = false;
        result.message = "already installed at " + ensured.rel.tag + " — use --force to reinstall\n";
        return result;
    }
    if (!ensured.ok) {
        result.message = ensured.message;
        return result;
    }

    const bool use_wine = ensured.use_wine;
    const std::string& target_os = ensured.target_os;
    const GhRelease& rel = ensured.rel;
    result.plan.latest_tag = rel.tag;
    const fs::path download = ensured.download;

    if (ensured.from_cache)
        std::cerr << "Using cached " << ensured.asset_name << " (" << rel.tag << ")…\n";
    else
        std::cerr << "\n";
    std::cerr << "Extracting…\n";

    ensure_dirs(paths);
    const std::string tag = sanitize_tag(rel.tag);
    const fs::path install_root = paths.apps_dir / title.install_dir_name;
    const fs::path release_dir = install_root / "releases" / tag;
    const fs::path staging = install_root / ".staging";

    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);

    std::string err;
    if (!extract_archive(download, staging, &err)) {
        result.message = "extract failed: " + err;
        return result;
    }
    const std::string launch_name = title.launch_binary_for_os(target_os);
    if (!unwrap_nested_archives(staging, launch_name, &err)) {
        result.message = "nested extract failed: " + err;
        return result;
    }

    std::string resolved_launch = launch_name;
    fs::path binary = resolve_launch_binary(staging, launch_name, target_os, &resolved_launch);
    if (binary.empty()) {
        const auto candidates = list_launch_candidates(staging);
        std::string place_err;
        if (!promote_staging_to_release(staging, release_dir, &place_err)) {
            result.message = "launch binary not found after extract: " + launch_name +
                             "\n  also failed to keep extract: " + place_err +
                             "\n  (look under " + staging.string() + " if it still exists)";
            return result;
        }
        write_partial_install_record(install_root, title, rel.tag, ensured.asset_name,
                                     rel.html_url, target_os, use_wine, tag);
        result.plan = inspect_install(paths, title);
        result.plan.latest_tag = rel.tag;
        result.message =
            "launch binary not found after extract: " + launch_name + "\n" +
            "  kept extract at: " + release_dir.string() + "\n" +
            "  Open Folder in the hub to run first-time setup or rename the exe to match.\n" +
            "  Or fix catalog launch." + target_os +
            " and use Retry Install / Clean install folder.\n";
        if (!candidates.empty()) {
            result.message += "  candidates in archive:";
            for (const auto& c : candidates) result.message += " " + c;
            result.message += "\n";
        }
        return result;
    }
    const bool launch_name_guessed = !filename_eq_ci(resolved_launch, launch_name);
    make_executable(binary);

    // Keep saves + user config across same-tag force reinstall / overwrite.
    // Abort before promote so a stash I/O failure cannot lose the only copy.
    std::string stash_note;
    if (!stash_user_state_for_update(paths, title, &stash_note)) {
        result.message = "failed to preserve saves/config before update: " + stash_note;
        return result;
    }

    if (!promote_staging_to_release(staging, release_dir, &err)) {
        result.message = "failed to place release dir: " + err;
        return result;
    }

    // Re-resolve binary under release_dir (prefer the name we actually found).
    binary = resolve_launch_binary(release_dir, resolved_launch, target_os, &resolved_launch);
    if (binary.empty())
        binary = resolve_launch_binary(release_dir, launch_name, target_os, &resolved_launch);
    if (binary.empty()) {
        write_partial_install_record(install_root, title, rel.tag, ensured.asset_name,
                                     rel.html_url, target_os, use_wine, tag);
        result.plan = inspect_install(paths, title);
        result.plan.latest_tag = rel.tag;
        result.message = "binary missing after move (expected " + launch_name + ")\n" +
                         "  kept extract at: " + release_dir.string() + "\n";
        return result;
    }
    make_executable(binary);

    const fs::path rel_bin = fs::relative(binary, release_dir, ec);
    set_current_symlink(install_root, tag);

    std::string restore_note;
    restore_user_state(install_root, release_dir, &restore_note);

    InstallRecord rec;
    rec.title_id = title.id;
    rec.github = title.release.github;
    rec.tag = rel.tag;
    rec.asset_name = ensured.asset_name;
    rec.binary = ec ? binary.filename().string() : rel_bin.generic_string();
    rec.host_os = host_os_key();
    rec.target_os = target_os;
    rec.runtime = use_wine ? "wine" : "native";
    rec.installed_at = iso8601_now();
    rec.release_url = rel.html_url;
    rec.method = "zip";
    if (!save_install_record(install_root, rec)) {
        // current/ already flipped — keep .old-* siblings for manual recovery.
        result.message = "installed files but failed to write install.json "
                         "(old release folders kept for recovery)";
        return result;
    }

    // Keep download cache for resume / faster reinstalls and hub prefetch.
    // Prune only after install.json is durable.
    std::string prune_note;
    prune_old_release_dirs(install_root, tag, &prune_note);

    result.plan = inspect_install(paths, title);
    result.plan.latest_tag = rel.tag;
    result.plan.update_available = false;
    result.ok = true;
    result.message = "installed " + title.id + " " + rel.tag +
                     (use_wine ? " (wine)\n" : "\n") + "  asset:  " + ensured.asset_name +
                     "\n  binary: " + result.plan.binary_path.string() + "\n";
    if (ensured.from_cache) result.message += "  note: used cached download\n";
    if (launch_name_guessed) {
        result.message += "  note: catalog launch." + target_os + " was '" + launch_name +
                          "'; used '" + resolved_launch + "' from the archive\n";
    }
    if (!stash_note.empty()) result.message += "  " + stash_note;
    if (!restore_note.empty()) result.message += "  " + restore_note;
    if (!prune_note.empty()) result.message += "  " + prune_note;
    return result;
}

InstallResult update_title(const Paths& paths_in, const Title& title, const InstallOptions& opts) {
    Paths paths = with_apps_dir(paths_in, opts.apps_dir);
    ensure_apps_dir(paths);
    InstallOptions o = opts;
    o.apps_dir = paths.apps_dir;
    o.check_latest = true;
    // Preserve Wine runtime across updates unless the caller forced native.
    if (!o.use_wine) {
        InstallPlan existing = inspect_install(paths, title);
        if (existing.record && existing.record->runtime == "wine") o.use_wine = true;
    }
    InstallPlan plan = plan_install(paths, title, o);
    if (plan.installed && !plan.update_available && !o.force) {
        // Only treat as up to date when we successfully resolved a latest tag.
        // A failed/empty check used to report "up to date" while the hub still
        // showed an update from ReleaseTagCache — leave that path and try install.
        if (!plan.latest_tag.empty()) {
            InstallResult r;
            r.ok = true;
            r.skipped = true;
            r.plan = std::move(plan);
            r.message = title.id + " is up to date (" + r.plan.installed_tag + ")\n";
            return r;
        }
        // latest unknown — continue; install_title may use cache / live fetch.
        if (!o.hint_latest_tag.empty()) o.force = true;
    }
    if (!plan.latest_tag.empty() && o.hint_latest_tag.empty())
        o.hint_latest_tag = plan.latest_tag;
    o.force = o.force || plan.update_available;
    return install_title(paths, title, o);
}

UninstallResult uninstall_install_root(const Paths& /*paths*/, const fs::path& install_root,
                                       const UninstallOptions& opts,
                                       const std::string& display_id,
                                       const std::vector<std::string>& save_globs) {
    UninstallResult result;
    result.plan.install_root = install_root;
    result.plan.current_link = install_root / "current";
    const std::string id =
        !display_id.empty() ? display_id : install_root.filename().string();

    std::error_code ec;
    if (!fs::exists(install_root, ec)) {
        result.skipped = true;
        result.ok = true;
        result.message = id + " is not installed (" + install_root.string() + ")\n";
        return result;
    }

    const fs::path preserved = install_root / "preserved";
    const std::vector<std::string> globs =
        save_globs.empty() ? default_save_globs() : save_globs;

    // Only scan install_root/releases/<tag>/ trees (canonicalized + deduped).
    std::vector<fs::path> roots;
    std::unordered_set<std::string> seen_roots;
    auto add_root = [&](const fs::path& p) {
        std::error_code lec;
        if (!fs::is_directory(p, lec)) return;
        const fs::path canon = fs::weakly_canonical(p, lec);
        const fs::path use = lec ? p : canon;
        if (!seen_roots.insert(use.string()).second) return;
        roots.push_back(use);
    };
    {
        const fs::path releases_dir = install_root / "releases";
        if (fs::is_directory(releases_dir, ec)) {
            for (auto it = fs::directory_iterator(releases_dir, ec);
                 !ec && it != fs::directory_iterator(); it.increment(ec)) {
                if (it->is_directory(ec)) add_root(it->path());
            }
        }
    }
    {
        const fs::path current = resolve_current_dir(install_root);
        if (!current.empty()) add_root(current);
    }

    if (opts.keep_saves) {
        std::unordered_set<std::string> seen_rel;
        if (opts.dry_run) {
            for (const auto& root : roots) {
                for (auto it = fs::recursive_directory_iterator(
                         root, fs::directory_options::skip_permission_denied, ec);
                     !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                    if (it->is_directory(ec) && it->path().filename() == "preserved") {
                        it.disable_recursion_pending();
                        continue;
                    }
                    if (!it->is_regular_file(ec)) continue;
                    const std::string rel =
                        normalize_preserved_rel(rel_posix_under(root, it->path()));
                    if (rel.empty() || !is_user_state_path(rel, globs)) continue;
                    if (seen_rel.insert(rel).second) result.preserved_paths.push_back(rel);
                }
            }
        } else {
            std::string err;
            // Fresh stash each uninstall keep-saves pass.
            fs::remove_all(preserved, ec);
            for (const auto& root : roots) {
                std::vector<std::string> batch;
                stash_saves_from(root, preserved, globs, &batch, &err);
                if (!err.empty()) {
                    result.message = err + "\n";
                    return result;
                }
                for (auto& r : batch) {
                    const std::string norm = normalize_preserved_rel(r);
                    if (norm.empty()) continue;
                    if (seen_rel.insert(norm).second) result.preserved_paths.push_back(norm);
                }
            }
        }
        std::sort(result.preserved_paths.begin(), result.preserved_paths.end());
    }

    std::ostringstream oss;
    oss << "uninstall " << id << "\n"
        << "  target: " << install_root.string() << "\n";
    if (opts.keep_saves) {
        oss << "  saves:  preserve (" << result.preserved_paths.size() << " file(s)";
        if (!result.preserved_paths.empty())
            oss << " → " << preserved.string();
        oss << ")\n";
        for (size_t i = 0; i < result.preserved_paths.size() && i < 12; ++i)
            oss << "    - " << result.preserved_paths[i] << "\n";
        if (result.preserved_paths.size() > 12)
            oss << "    … +" << (result.preserved_paths.size() - 12) << " more\n";
    } else {
        oss << "  saves:  delete (including any preserved/ stash)\n";
    }

    if (opts.dry_run) {
        oss << "  status: dry-run (not removed)\n";
        result.ok = true;
        result.message = oss.str();
        return result;
    }

    // Remove build artifacts; optionally keep preserved/.
    const fs::path current = install_root / "current";
    const fs::path current_path = install_root / "current.path";
    fs::remove(current, ec);
    fs::remove(current_path, ec);
    fs::remove_all(install_root / "releases", ec);
    fs::remove(install_root / "install.json", ec);
    fs::remove_all(install_root / ".download", ec);
    fs::remove_all(install_root / ".staging", ec);

    for (auto it = fs::directory_iterator(install_root, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (opts.keep_saves && name == "preserved") continue;
        fs::remove_all(it->path(), ec);
    }

    if (!opts.keep_saves) {
        fs::remove_all(preserved, ec);
        fs::remove_all(install_root, ec);
    } else if (!fs::exists(preserved, ec) || fs::is_empty(preserved, ec)) {
        fs::remove_all(preserved, ec);
        fs::remove(install_root, ec);
    }

    oss << "  status: removed\n";
    result.ok = true;
    result.plan.install_root = install_root;
    result.plan.installed = false;
    bool has_preserved = false, other = false;
    result.plan.install_dir_present =
        classify_install_root_leftovers(install_root, &has_preserved, &other);
    result.plan.has_preserved_state = has_preserved;
    result.message = oss.str();
    return result;
}

UninstallResult uninstall_title(const Paths& paths_in, const Title& title,
                                const UninstallOptions& opts) {
    // Prefer the root that actually holds this title when callers pass default paths.
    const AppConfig cfg = load_app_config(paths_in.config_path);
    const InstallPlan plan = inspect_install_any(paths_in, cfg, title);
    const fs::path apps =
        plan.install_root.empty() ? paths_in.apps_dir : plan.install_root.parent_path();
    Paths paths = with_apps_dir(paths_in, apps);
    return uninstall_install_root(paths, paths.apps_dir / title.install_dir_name, opts, title.id,
                                  save_globs_for_title(title));
}

static bool same_path_relaxed(const fs::path& a, const fs::path& b) {
    if (a.empty() || b.empty()) return false;
    std::error_code ec;
    if (fs::equivalent(a, b, ec)) return true;
    ec.clear();
    const fs::path ca = fs::weakly_canonical(a, ec);
    const fs::path cb = fs::weakly_canonical(b, ec);
    if (!ec && !ca.empty() && !cb.empty()) return ca == cb;
    return a == b;
}

MoveInstallResult move_title_install(const Paths& paths, const AppConfig& cfg, const Title& title,
                                     const fs::path& dest_apps_dir) {
    MoveInstallResult result;
    if (title.install_dir_name.empty()) {
        result.message = "move install: title has no install_dir_name";
        return result;
    }
    if (dest_apps_dir.empty()) {
        result.message = "move install: destination apps root is empty";
        return result;
    }

    const InstallPlan src = inspect_install_any(paths, cfg, title);
    if (!src.installed && !src.install_dir_present && !src.has_preserved_state) {
        result.message = "move install: nothing to move for " + title.id;
        return result;
    }
    result.from_root = src.install_root;
    result.to_root = dest_apps_dir / title.install_dir_name;

    if (same_path_relaxed(result.from_root, result.to_root) ||
        same_path_relaxed(result.from_root.parent_path(), dest_apps_dir)) {
        result.ok = true;
        result.skipped = true;
        result.to_root = result.from_root;
        result.message = "Already at " + result.from_root.string();
        return result;
    }

    std::error_code ec;
    if (!fs::exists(result.from_root, ec)) {
        result.message = "move install: source missing: " + result.from_root.string();
        return result;
    }
    if (fs::exists(result.to_root, ec)) {
        const bool empty = fs::is_directory(result.to_root, ec) && fs::is_empty(result.to_root, ec);
        if (!empty) {
            result.message = "move install: destination already exists: " + result.to_root.string();
            return result;
        }
        fs::remove_all(result.to_root, ec);
    }

    fs::create_directories(dest_apps_dir, ec);
    if (ec) {
        result.message = "move install: could not create " + dest_apps_dir.string() + ": " +
                         ec.message();
        return result;
    }

    ec.clear();
    fs::rename(result.from_root, result.to_root, ec);
    if (ec) {
        // Cross-device (or other rename failure): copy then remove source.
        std::error_code copy_ec;
        fs::copy(result.from_root, result.to_root,
                 fs::copy_options::recursive | fs::copy_options::copy_symlinks, copy_ec);
        if (copy_ec) {
            std::error_code rm_ec;
            fs::remove_all(result.to_root, rm_ec);
            result.message = "move install: copy failed (" + copy_ec.message() +
                             "); rename also failed (" + ec.message() + ")";
            return result;
        }
        std::error_code rm_ec;
        fs::remove_all(result.from_root, rm_ec);
        if (rm_ec) {
            result.ok = true;
            result.message = "Moved " + title.id + " → " + result.to_root.string() +
                             " (source cleanup warning: " + rm_ec.message() + ")";
            return result;
        }
    }

    result.ok = true;
    result.message = "Moved " + title.id + "\n  from: " + result.from_root.string() +
                     "\n  to:   " + result.to_root.string();
    return result;
}

std::vector<OrphanInstall> list_orphan_installs(const Paths& paths, const Catalog& catalog) {
    // Apps live under install_dir_name only (defaults to title id in the catalog).
    std::unordered_set<std::string> claimed;
    claimed.reserve(catalog.titles.size());
    for (const auto& t : catalog.titles) {
        if (!t.install_dir_name.empty()) claimed.insert(t.install_dir_name);
    }

    std::vector<OrphanInstall> out;
    std::error_code ec;
    const AppConfig cfg = load_app_config(paths.config_path);
    for (const auto& apps_root : scan_install_roots(cfg, paths)) {
        if (!fs::is_directory(apps_root.path, ec)) continue;
        for (auto it = fs::directory_iterator(apps_root.path, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (!it->is_directory(ec)) continue;
            const fs::path root = it->path();
            const std::string dir_name = root.filename().string();
            if (dir_name.empty() || dir_name[0] == '.') continue;
            if (claimed.count(dir_name)) continue;

            OrphanInstall o;
            o.install_root = root;
            o.dir_name = dir_name;
            const InstallRecord rec = load_install_record(root);
            o.has_install_record =
                !rec.title_id.empty() || !rec.tag.empty() || !rec.binary.empty();
            o.title_id = !rec.title_id.empty() ? rec.title_id : dir_name;
            o.tag = rec.tag;
            bool has_preserved = false, other = false;
            classify_install_root_leftovers(root, &has_preserved, &other);
            o.has_preserved_only = has_preserved && !other && !o.has_install_record;
            out.push_back(std::move(o));
        }
    }

    std::sort(out.begin(), out.end(), [](const OrphanInstall& a, const OrphanInstall& b) {
        if (a.dir_name != b.dir_name) return a.dir_name < b.dir_name;
        return a.install_root.string() < b.install_root.string();
    });
    return out;
}

OrphanCleanupResult cleanup_removed_catalog_titles(const Paths& paths, const Catalog& catalog,
                                                   const OrphanCleanupOptions& opts) {
    OrphanCleanupResult result;
    const auto orphans = list_orphan_installs(paths, catalog);
    UninstallOptions uopts;
    uopts.keep_saves = opts.keep_saves;
    uopts.dry_run = opts.dry_run;

    for (const auto& o : orphans) {
        auto ur = uninstall_install_root(paths, o.install_root, uopts, o.title_id, {});
        result.messages.push_back(ur.message);
        if (ur.ok) {
            ++result.removed;
            if (!opts.dry_run) remove_boxart_for_title(paths, o.title_id);
            if (!opts.dry_run && o.title_id != o.dir_name) remove_boxart_for_title(paths, o.dir_name);
        } else {
            ++result.failed;
        }
    }

    if (opts.prune_indexes && !opts.dry_run) {
        result.pruned_ids = prune_stale_title_indexes(paths, catalog, &result.messages);
    } else if (opts.prune_indexes && opts.dry_run) {
        result.messages.push_back("dry-run: would prune stale library/BIOS/RomM/app-state entries\n");
    }

    std::ostringstream oss;
    oss << "Orphan cleanup: " << result.removed << " install(s)"
        << (opts.dry_run ? " (dry-run)" : " removed");
    if (result.failed) oss << ", " << result.failed << " failed";
    if (result.pruned_ids) oss << ", pruned " << result.pruned_ids << " index/state entr(y/ies)";
    oss << "\n";
    result.message = oss.str();
    result.ok = result.failed == 0;
    return result;
}

} // namespace retcomm
