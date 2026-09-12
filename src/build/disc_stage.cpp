#include "retcomm/disc_stage.hpp"

#include "../platform/ini_text.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif
#endif

namespace retcomm {
namespace {

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string lower_ext_of(const fs::path& p) { return lower_ascii(p.extension().string()); }

// game.toml values arrive as TOML scalars: strip one layer of quoting.
std::string unquote(std::string v) {
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front())
        return v.substr(1, v.size() - 2);
    return v;
}

// The work tree holds exactly one thing worth keeping: the boot EXE prepare_disc
// carved out of the image. Everything with a dump extension is a duplicate of a
// library file.
bool is_disc_image_ext(const std::string& ext) {
    static const char* kExts[] = {".bin", ".img", ".iso", ".chd", ".cso", ".pbp"};
    for (const char* e : kExts) {
        if (ext == e) return true;
    }
    return false;
}

// Work trees psxrecomp writes under a project root. "disc" is what the shipped
// per-game configs use; "prepared_disc" is the CLI default when a game.toml
// omits prepare_disc.out_dir.
std::vector<fs::path> work_tree_candidates(const fs::path& src_root,
                                           const DiscWorkTreeSpec& spec) {
    std::vector<fs::path> out;
    auto add = [&](const fs::path& p) {
        if (p.empty()) return;
        for (const fs::path& seen : out) {
            if (seen == p) return;
        }
        out.push_back(p);
    };
    add(spec.dir);
    add(src_root / "disc");
    add(src_root / "prepared_disc");
    return out;
}

// std::filesystem::create_symlink on Windows does not always ask for the
// unprivileged link that Developer Mode grants, and without it every PSX build
// on a non-admin account falls back to a 700 MB copy. Ask directly first.
bool make_symlink(const fs::path& target, const fs::path& dest) {
#if defined(_WIN32)
    if (CreateSymbolicLinkW(dest.c_str(), target.c_str(),
                            SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
        return true;
#endif
    std::error_code ec;
    fs::create_symlink(target, dest, ec);
    return !ec;
}

// True when this file's bytes live only here — a symlink borrows them, and a
// second hard link means the library already holds the same inode.
bool file_owns_its_bytes(const fs::path& p) {
    std::error_code ec;
    if (fs::is_symlink(p, ec)) return false;
    if (!fs::is_regular_file(p, ec)) return false;
    const auto links = fs::hard_link_count(p, ec);
    if (ec) return true; // cannot prove sharing — treat as an owned copy
    return links <= 1;
}

} // namespace

bool is_rom_or_disc_media_ext(const std::string& lower_ext) {
    if (lower_ext.empty()) return false;
    if (is_disc_image_ext(lower_ext)) return true;
    static const char* kExts[] = {".cue", ".m3u", ".car", ".sfc", ".smc", ".fig", ".swc",
                                  ".gba", ".gb",  ".gbc", ".nes", ".fds", ".n64", ".z64",
                                  ".v64", ".md",  ".gen", ".smd", ".sms", ".gg",  ".nds",
                                  ".vb",  ".32x", ".col", ".pce"};
    for (const char* e : kExts) {
        if (lower_ext == e) return true;
    }
    return false;
}

bool read_disc_work_tree_spec(const fs::path& src_root, const std::string& config_rel,
                              DiscWorkTreeSpec* out) {
    if (!out) return false;
    *out = DiscWorkTreeSpec{};
    if (src_root.empty()) return false;

    const fs::path cfg = src_root / (config_rel.empty() ? std::string("game.toml") : config_rel);
    std::error_code ec;
    if (!fs::is_regular_file(cfg, ec)) return false;

    const std::string body = ini_text::read_text_file(cfg);
    if (body.empty()) return false;

    bool saw_section = false;
    std::string out_dir;
    ini_text::for_each_ini_pair(
        body, [&](const std::string& section, const std::string& key, const std::string& value) {
            if (!ini_text::ieq(section, "prepare_disc")) return;
            saw_section = true;
            const std::string v = unquote(value);
            if (ini_text::ieq(key, "out_dir"))
                out_dir = v;
            else if (ini_text::ieq(key, "cue_name"))
                out->cue_name = v;
            else if (ini_text::ieq(key, "bin_name"))
                out->bin_name = v;
            else if (ini_text::ieq(key, "boot_exe"))
                out->boot_exe = v;
        });

    if (!saw_section) return false;
    // psxrecomp_cli: out_dir defaults to "prepared_disc" when the key is absent.
    out->dir = src_root / (out_dir.empty() ? std::string("prepared_disc") : out_dir);
    return true;
}

std::vector<fs::path> cue_track_files(const fs::path& cue_path) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (cue_path.empty() || !fs::is_regular_file(cue_path, ec)) return out;
    if (lower_ext_of(cue_path) != ".cue") return out;

    std::ifstream in(cue_path);
    if (!in) return out;

    const fs::path dir = cue_path.parent_path();
    std::set<std::string> seen;
    std::string line;
    while (std::getline(in, line)) {
        size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (line.size() - i < 4 || line.compare(i, 4, "FILE") != 0) continue;
        i += 4;
        if (i < line.size() && std::isalnum(static_cast<unsigned char>(line[i]))) continue;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size()) continue;

        std::string ref;
        if (line[i] == '"' || line[i] == '\'') {
            const char q = line[i++];
            const size_t start = i;
            while (i < line.size() && line[i] != q) ++i;
            ref = line.substr(start, i - start);
        } else {
            const size_t start = i;
            while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
            ref = line.substr(start, i - start);
        }
        if (ref.empty()) continue;

        // Sheets reference tracks by name beside the cue; ignore any path part so
        // a hand-edited absolute reference cannot escape the library folder.
        const fs::path track = dir / fs::path(ref).filename();
        if (!fs::is_regular_file(track, ec)) continue;
        if (!seen.insert(lower_ascii(track.filename().string())).second) continue;
        out.push_back(track);
    }
    return out;
}

DiscStageResult stage_disc_tracks_from_library(const fs::path& src_root,
                                               const std::string& config_rel,
                                               const fs::path& library_cue) {
    DiscStageResult r;
    DiscWorkTreeSpec spec;
    if (!read_disc_work_tree_spec(src_root, config_rel, &spec) || spec.dir.empty()) {
        r.message = "no [prepare_disc] work tree for this title";
        return r;
    }

    const std::vector<fs::path> tracks = cue_track_files(library_cue);
    if (tracks.empty()) {
        r.message = "no FILE tracks resolved from " + library_cue.string();
        return r;
    }

    std::error_code ec;
    fs::create_directories(spec.dir, ec);
    if (ec) {
        r.message = "cannot create " + spec.dir.string() + ": " + ec.message();
        return r;
    }

    // Probe first, and never delete an existing track until a link is proven
    // possible: on a host that refuses symlinks the build must still be able to
    // fall back to prepare_disc's copy.
    const fs::path probe = spec.dir / ".retcomm-link-probe";
    fs::remove(probe, ec);
    const bool can_link = make_symlink(tracks.front(), probe);
    fs::remove(probe, ec);
    if (!can_link) {
        r.message = "this host will not create symlinks in " + spec.dir.string() +
                    " — the recompiler will copy the disc to read it, and the copy is "
                    "removed again after the build (on Windows, enabling Developer Mode "
                    "skips the copy entirely)";
        return r;
    }

    // Hard links are deliberately not used here: prepare_disc copies onto its
    // destination, and copying onto a hard link of the source truncates the
    // library's own file before reading it.
    auto link_one = [&](const fs::path& dest, const fs::path& target) -> bool {
        std::error_code lec;
        // Same file already (library_root configured inside the install tree, or
        // a hard link): there is nothing to stage, and removing dest here would
        // delete the library's own copy.
        if (fs::exists(dest, lec) && fs::equivalent(dest, target, lec) && !lec) return true;
        lec.clear();
        if (fs::is_symlink(dest, lec)) {
            std::error_code aec, bec;
            const fs::path cur = fs::weakly_canonical(dest, aec);
            const fs::path want = fs::weakly_canonical(target, bec);
            if (!aec && !bec && cur == want) return true;
        }
        lec.clear();
        if (fs::exists(dest, lec) || fs::is_symlink(dest, lec)) {
            fs::remove(dest, lec);
            if (lec) {
                r.message = "cannot replace " + dest.string() + ": " + lec.message();
                return false;
            }
        }
        if (!make_symlink(target, dest)) {
            r.message = "cannot link " + dest.string() + " → " + target.string();
            return false;
        }
        return true;
    };

    // prepare_disc writes the files it carves out of the image with write_bytes,
    // which follows a symlink. A link whose name collides with one of those
    // would be written straight through into the library, so stage nothing.
    if (!spec.boot_exe.empty()) {
        const std::string boot = lower_ascii(spec.boot_exe);
        for (const fs::path& track : tracks) {
            if (lower_ascii(track.filename().string()) != boot) continue;
            r.message = "track name collides with prepare_disc boot_exe (" + spec.boot_exe +
                        ") — not staging links";
            return r;
        }
    }

    std::set<std::string> placed;
    for (const fs::path& track : tracks) {
        if (!link_one(spec.dir / track.filename(), track)) return r;
        placed.insert(lower_ascii(track.filename().string()));
        ++r.linked;
    }
    // Single-track sets whose game.toml names the data track differently.
    if (!spec.bin_name.empty() && !placed.count(lower_ascii(spec.bin_name))) {
        if (!link_one(spec.dir / spec.bin_name, tracks.front())) return r;
        ++r.linked;
    }

    r.ok = true;
    r.message = "linked " + std::to_string(r.linked) + " library track(s) into " +
                spec.dir.filename().string() + "/ — no disc copy will be made";
    return r;
}

DiscPruneResult prune_copied_disc_media(const fs::path& src_root, const std::string& config_rel) {
    DiscPruneResult r;
    if (src_root.empty()) return r;

    DiscWorkTreeSpec spec;
    read_disc_work_tree_spec(src_root, config_rel, &spec);

    std::error_code ec;
    for (const fs::path& dir : work_tree_candidates(src_root, spec)) {
        if (!fs::is_directory(dir, ec)) continue;

        std::vector<fs::path> owned_images;
        for (auto it = fs::recursive_directory_iterator(
                 dir, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            const fs::path p = it->path();
            if (!is_disc_image_ext(lower_ext_of(p))) continue;
            if (!file_owns_its_bytes(p)) continue;
            owned_images.push_back(p);
        }
        ec.clear();
        if (owned_images.empty()) continue;

        std::size_t removed_here = 0;
        for (const fs::path& p : owned_images) {
            std::error_code fec;
            const auto size = fs::file_size(p, fec);
            const std::uint64_t bytes = fec ? 0 : static_cast<std::uint64_t>(size);
            fec.clear();
            if (!fs::remove(p, fec) || fec) {
                r.messages.push_back("could not remove duplicated disc image " + p.string() +
                                     (fec ? ": " + fec.message() : std::string{}));
                continue;
            }
            ++r.removed;
            ++removed_here;
            r.bytes_freed += bytes;
        }

        // The working cue and its receipt describe tracks that are gone; leaving
        // them would hand the next run a sheet pointing at nothing.
        if (removed_here == 0) continue;
        for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator();
             it.increment(ec)) {
            const fs::path p = it->path();
            if (!fs::is_regular_file(p, ec)) continue;
            const std::string name = lower_ascii(p.filename().string());
            static const std::string kReceipt = ".disc-receipt.json";
            const bool receipt = name.size() > kReceipt.size() &&
                                 name.compare(name.size() - kReceipt.size(), kReceipt.size(),
                                              kReceipt) == 0;
            if (lower_ext_of(p) != ".cue" && !receipt) continue;
            std::error_code fec;
            fs::remove(p, fec);
        }
        ec.clear();
    }

    return r;
}

} // namespace retcomm
