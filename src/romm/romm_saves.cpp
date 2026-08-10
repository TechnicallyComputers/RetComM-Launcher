#include "retcomm/romm_saves.hpp"

#include "retcomm/app_state.hpp"
#include "retcomm/hash.hpp"
#include "retcomm/http.hpp"
#include "retcomm/install.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace retcomm {
namespace {

using nlohmann::json;

std::string lower_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string normalize_base(std::string base) {
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base;
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> auth_headers(const RommConfig& romm,
                                                              bool json_accept) {
    std::vector<std::pair<std::string, std::string>> h;
    if (!romm.api_token.empty())
        h.emplace_back("Authorization", "Bearer " + romm.api_token);
    if (json_accept) h.emplace_back("Accept", "application/json");
    return h;
}

void progress(const RommProgressFn& fn, const std::string& s) {
    if (fn) fn(s);
}

bool is_native_save_ext(const std::string& ext_lower) {
    static const std::unordered_set<std::string> k = {
        ".srm", ".sav", ".mcd", ".mcr", ".mcs", ".gme", ".eep", ".fla", ".battery", ".mem"};
    return k.count(ext_lower) > 0;
}

bool is_savestate_ext(const std::string& ext_lower) {
    if (ext_lower == ".sts" || ext_lower == ".savestate") return true;
    // .state, .state0 … .state9, .state10, etc.
    if (ext_lower.rfind(".state", 0) == 0) return true;
    return false;
}

bool looks_like_savestate_file(const fs::path& path) {
    const std::string ext = lower_copy(path.extension().string());
    if (is_savestate_ext(ext)) return true;
    // Some cores use bare numeric slots without a conventional extension.
    const std::string name = lower_copy(path.filename().string());
    if (name.find(".state") != std::string::npos) return true;
    return false;
}

std::unordered_set<std::string> native_exts_for_title(const Title& title) {
    std::unordered_set<std::string> exts;
    auto consider = [&](const std::vector<std::string>& globs) {
        for (const auto& g : globs) {
            const auto dot = g.find_last_of('.');
            if (dot == std::string::npos) continue;
            std::string e = lower_copy(g.substr(dot));
            // strip glob junk
            while (!e.empty() && (e.back() == '*' || e.back() == '?')) e.pop_back();
            if (is_native_save_ext(e)) exts.insert(e);
        }
    };
    consider(title.saves_sram_glob);
    consider(title.saves_memcard_glob);
    if (exts.empty()) {
        // Sensible defaults by platform family.
        if (title.platform == "psx" || title.platform == "ps" || title.platform == "ps1") {
            exts.insert(".mcd");
            exts.insert(".mcr");
            exts.insert(".mcs");
        } else {
            exts.insert(".srm");
            exts.insert(".sav");
        }
    }
    return exts;
}

bool match_simple_glob(const std::string& pat, const std::string& text) {
    size_t pi = 0, ti = 0, star = std::string::npos, restart = 0;
    while (ti < text.size()) {
        if (pi < pat.size() && (pat[pi] == '?' || pat[pi] == text[ti])) {
            ++pi;
            ++ti;
        } else if (pi < pat.size() && pat[pi] == '*') {
            star = pi++;
            restart = ti;
        } else if (star != std::string::npos) {
            pi = star + 1;
            ti = ++restart;
        } else {
            return false;
        }
    }
    while (pi < pat.size() && pat[pi] == '*') ++pi;
    return pi == pat.size();
}

bool path_matches_native_glob(const std::string& rel_posix, const Title& title,
                              const std::unordered_set<std::string>& exts) {
    const fs::path p(rel_posix);
    const std::string ext = lower_copy(p.extension().string());
    if (!is_native_save_ext(ext) || !exts.count(ext) || is_savestate_ext(ext)) return false;

    // Always accept under saves/ with a native extension.
    if (rel_posix.rfind("saves/", 0) == 0) return true;

    std::vector<std::string> globs = title.saves_sram_glob;
    globs.insert(globs.end(), title.saves_memcard_glob.begin(), title.saves_memcard_glob.end());
    if (globs.empty()) {
        globs = {"saves/*", "*.srm", "*.sav", "*.mcd", "*.mcr", "*.mcs"};
    }
    const auto slash = rel_posix.find_last_of('/');
    const std::string base =
        slash == std::string::npos ? rel_posix : rel_posix.substr(slash + 1);
    for (const auto& g : globs) {
        if (match_simple_glob(g, rel_posix) || match_simple_glob(g, base)) return true;
    }
    return false;
}

std::string rel_posix_under(const fs::path& root, const fs::path& file) {
    std::error_code ec;
    fs::path rel = fs::relative(file, root, ec);
    if (ec) return {};
    std::string s = rel.generic_string();
    if (s == "." || s.empty() || s.rfind("..", 0) == 0) return {};
    return s;
}

std::int64_t file_mtime_sec(const fs::path& path) {
    struct stat st {};
    if (stat(path.string().c_str(), &st) != 0) return 0;
    return static_cast<std::int64_t>(st.st_mtime);
}

// Parse RomM ISO-8601 timestamps into a comparable tick (seconds-ish).
std::int64_t parse_romm_time(const std::string& s) {
    // Expect YYYY-MM-DDTHH:MM:SS…
    if (s.size() < 19) return 0;
    std::tm tm{};
    tm.tm_year = std::atoi(s.substr(0, 4).c_str()) - 1900;
    tm.tm_mon = std::atoi(s.substr(5, 2).c_str()) - 1;
    tm.tm_mday = std::atoi(s.substr(8, 2).c_str());
    tm.tm_hour = std::atoi(s.substr(11, 2).c_str());
    tm.tm_min = std::atoi(s.substr(14, 2).c_str());
    tm.tm_sec = std::atoi(s.substr(17, 2).c_str());
#if defined(_WIN32)
    return static_cast<std::int64_t>(_mkgmtime(&tm));
#else
    return static_cast<std::int64_t>(timegm(&tm));
#endif
}

struct LocalSave {
    fs::path path;
    std::string rel; // under game root
    std::string file_name;
    std::string md5;
    std::int64_t mtime = 0;
};

struct RemoteSave {
    int id = 0;
    std::string file_name;
    std::string file_name_no_tags;
    std::string ext;
    std::string content_hash;
    std::int64_t updated = 0;
    std::uint64_t size = 0;
};

int platform_score(const Title& title, const std::string& platform_slug) {
    int score = 0;
    for (const auto& rp : title.romm_platforms) {
        if (!rp.empty() && rp == platform_slug) {
            score += 50;
            break;
        }
    }
    if (platform_slug == title.platform) score += 20;
    if (title.platform == "psx" &&
        (platform_slug == "ps" || platform_slug == "ps1" || platform_slug == "psx"))
        score += 40;
    return score;
}

std::string normalize_key(std::string s) {
    s = lower_copy(std::move(s));
    std::string out;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) out.push_back(c);
    }
    return out;
}

bool http_get_json(const std::string& url,
                   const std::vector<std::pair<std::string, std::string>>& headers, json& out,
                   std::string* error) {
    auto res = http_get(url, headers);
    if (!res.ok()) {
        if (error)
            *error = res.error.empty() ? ("HTTP " + std::to_string(res.status)) : res.error;
        return false;
    }
    try {
        out = json::parse(res.body);
    } catch (const std::exception& e) {
        if (error) *error = std::string("JSON: ") + e.what();
        return false;
    }
    return true;
}

// Locate RomM rom_id for this catalog title (identity preferred, then name+platform).
bool resolve_rom_id(const AppConfig& cfg, const Title& title, RommProgressFn on_progress,
                    int* rom_id, std::string* how, std::string* error) {
    const std::string base = normalize_base(cfg.romm.base_url);
    const auto headers = auth_headers(cfg.romm, true);

    std::string search = title.name.empty() ? title.id : title.name;
    if (!title.rom_identity.filenames.empty()) {
        const auto& fn = title.rom_identity.filenames.front();
        const auto dot = fn.find_last_of('.');
        const std::string stem = dot == std::string::npos ? fn : fn.substr(0, dot);
        if (!stem.empty()) search = stem;
    }
    progress(on_progress, "RomM saves: finding ROM \"" + search + "\"…");

    json root;
    if (!http_get_json(base + "/api/roms?limit=40&search_term=" + url_encode(search), headers,
                       root, error))
        return false;
    if (!root.contains("items") || !root.at("items").is_array() || root.at("items").empty()) {
        if (error) *error = "no RomM ROM results for \"" + search + "\"";
        return false;
    }

    int best_id = 0;
    int best_score = -1;
    std::string best_by;

    for (const auto& item : root.at("items")) {
        if (!item.is_object()) continue;
        const int id = item.value("id", 0);
        if (id <= 0) continue;
        int score = platform_score(title, item.value("platform_slug", ""));
        const std::string fs_name = item.value("fs_name", "");
        const std::string name = item.value("name", "");
        const std::string want = normalize_key(search);
        if (normalize_key(fs_name).find(want) != std::string::npos ||
            normalize_key(name).find(want) != std::string::npos)
            score += 30;

        const std::string sha1 = item.value("sha1_hash", "");
        const std::string md5 = item.value("md5_hash", "");
        const std::string crc = item.value("crc_hash", "");
        auto has = [](const std::vector<std::string>& v, const std::string& x) {
            if (x.empty()) return false;
            for (const auto& e : v)
                if (lower_copy(e) == lower_copy(x)) return true;
            return false;
        };
        std::string by = "name";
        if (has(title.rom_identity.sha1, sha1)) {
            score += 1000;
            by = "sha1";
        } else if (has(title.rom_identity.md5, md5)) {
            score += 950;
            by = "md5";
        } else if (has(title.rom_identity.crc32, crc)) {
            score += 900;
            by = "crc32";
        } else {
            const auto sz = item.value("fs_size_bytes", 0ull);
            for (auto s : title.rom_identity.sizes) {
                if (s == sz) {
                    score += 400;
                    by = "size";
                    break;
                }
            }
        }

        if (score > best_score) {
            best_score = score;
            best_id = id;
            best_by = by;
        }
    }

    if (best_id <= 0 || best_score < 40) {
        if (error) *error = "could not match a RomM ROM for save sync";
        return false;
    }
    *rom_id = best_id;
    if (how) *how = best_by;
    return true;
}

enum class SyncKind { Saves, States };

bool path_matches_state(const std::string& rel_posix) {
    if (rel_posix.empty()) return false;
    if (rel_posix.rfind("states/", 0) == 0 || rel_posix.rfind("savestates/", 0) == 0) {
        const std::string ext = lower_copy(fs::path(rel_posix).extension().string());
        // Keep screenshots / JSON out of state sync.
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".json")
            return false;
        if (is_native_save_ext(ext)) return false;
        return true;
    }
    return looks_like_savestate_file(fs::path(rel_posix));
}

std::vector<LocalSave> collect_local_assets(const fs::path& game_root, const Title& title,
                                            SyncKind kind) {
    std::vector<LocalSave> out;
    if (game_root.empty()) return out;
    const auto exts = native_exts_for_title(title);
    std::error_code ec;
    if (!fs::is_directory(game_root, ec)) return out;

    for (auto it = fs::recursive_directory_iterator(
             game_root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec)) {
            const auto name = it->path().filename().string();
            if (name == "releases" || name == ".staging" || name == ".download") {
                it.disable_recursion_pending();
                continue;
            }
        }
        if (!it->is_regular_file(ec)) continue;
        const std::string rel = rel_posix_under(game_root, it->path());
        if (rel.empty()) continue;
        if (kind == SyncKind::Saves) {
            if (!path_matches_native_glob(rel, title, exts)) continue;
        } else {
            if (!path_matches_state(rel)) continue;
        }

        LocalSave s;
        s.path = it->path();
        s.rel = rel;
        s.file_name = it->path().filename().string();
        s.mtime = file_mtime_sec(s.path);
        s.md5 = file_md5_hex(s.path);
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<RemoteSave> list_remote_assets(const AppConfig& cfg, int rom_id, SyncKind kind,
                                           std::string* error) {
    std::vector<RemoteSave> out;
    const std::string base = normalize_base(cfg.romm.base_url);
    const char* api = (kind == SyncKind::Saves) ? "/api/saves?rom_id=" : "/api/states?rom_id=";
    json root;
    if (!http_get_json(base + api + std::to_string(rom_id), auth_headers(cfg.romm, true), root,
                       error))
        return out;
    if (!root.is_array()) {
        if (error)
            *error = std::string("unexpected ") +
                     ((kind == SyncKind::Saves) ? "/api/saves" : "/api/states") + " response";
        return out;
    }
    for (const auto& item : root) {
        if (!item.is_object()) continue;
        RemoteSave s;
        s.id = item.value("id", 0);
        s.file_name = item.value("file_name", "");
        s.file_name_no_tags = item.value("file_name_no_tags", "");
        s.ext = lower_copy(item.value("file_extension", ""));
        if (!s.ext.empty() && s.ext.front() != '.') s.ext.insert(s.ext.begin(), '.');
        s.content_hash = lower_copy(item.value("content_hash", ""));
        s.updated = parse_romm_time(item.value("updated_at", ""));
        s.size = item.value("file_size_bytes", 0ull);
        if (s.id <= 0 || s.file_name.empty()) continue;
        if (kind == SyncKind::Saves) {
            if (!is_native_save_ext(s.ext) || is_savestate_ext(s.ext)) continue;
        } else {
            // Accept typical state extensions; also allow unknown if listed under states API.
            if (is_native_save_ext(s.ext) && !is_savestate_ext(s.ext)) continue;
        }
        out.push_back(std::move(s));
    }
    return out;
}

// Prefer untagged / null-slot saves when multiple share the same base name.
const RemoteSave* pick_remote_for_local(const std::vector<RemoteSave>& remotes,
                                        const LocalSave& local) {
    const std::string want = lower_copy(local.file_name);
    const std::string want_stem = lower_copy(fs::path(local.file_name).stem().string());
    const RemoteSave* exact = nullptr;
    const RemoteSave* soft = nullptr;
    for (const auto& r : remotes) {
        if (lower_copy(r.file_name) == want) {
            exact = &r;
            break;
        }
        if (lower_copy(r.file_name_no_tags) == want_stem ||
            lower_copy(fs::path(r.file_name).stem().string()) == want_stem) {
            if (!soft) soft = &r;
        }
    }
    return exact ? exact : soft;
}

bool upload_asset(const AppConfig& cfg, int rom_id, const LocalSave& local, SyncKind kind,
                  std::string* error) {
    const std::string base = normalize_base(cfg.romm.base_url);
    const std::string url =
        base + ((kind == SyncKind::Saves) ? "/api/saves?rom_id=" : "/api/states?rom_id=") +
        std::to_string(rom_id) + "&overwrite=true";
    HttpMultipartFile part;
    part.field = (kind == SyncKind::Saves) ? "saveFile" : "stateFile";
    part.path = local.path;
    part.filename = local.file_name;
    auto res = http_post_multipart(url, auth_headers(cfg.romm, false), {part}, {});
    if (!res.ok()) {
        if (error)
            *error = res.error.empty() ? ("HTTP " + std::to_string(res.status)) : res.error;
        if (!res.body.empty() && error) *error += " — " + res.body.substr(0, 200);
        return false;
    }
    return true;
}

bool download_asset(const AppConfig& cfg, const RemoteSave& remote, const fs::path& dest,
                    SyncKind kind, std::string* error) {
    const std::string base = normalize_base(cfg.romm.base_url);
    const std::string url =
        base + ((kind == SyncKind::Saves) ? "/api/saves/" : "/api/states/") +
        std::to_string(remote.id) + "/content";
    return http_download(url, dest, error, auth_headers(cfg.romm, false));
}

RommSaveSyncResult fail(const std::string& msg) {
    RommSaveSyncResult r;
    r.message = msg;
    return r;
}

bool save_name_matches_title(const std::string& file_name, const Title& title) {
    // Alphanumeric-only keys. Require a meaningful overlap so short stems like
    // "ps" / "a" / "usa" do not false-match every PSX title id.
    const std::string key = normalize_key(fs::path(file_name).stem().string());
    if (key.size() < 6) return false;
    auto overlaps = [&](const std::string& other) {
        const std::string o = normalize_key(other);
        if (o.size() < 6) return false;
        if (key == o) return true;
        // Longer side must contain the shorter; reject tiny substring hits.
        const std::string& a = key.size() >= o.size() ? key : o;
        const std::string& b = key.size() >= o.size() ? o : key;
        if (b.size() < 6) return false;
        return a.find(b) != std::string::npos;
    };
    if (!title.name.empty() && overlaps(title.name)) return true;
    if (!title.id.empty() && overlaps(title.id)) return true;
    for (const auto& fn : title.rom_identity.filenames) {
        if (overlaps(fs::path(fn).stem().string())) return true;
    }
    return false;
}

RommSaveSyncResult sync_assets_with_romm(const Paths& paths, const AppConfig& cfg,
                                         const Title& title, SyncKind kind,
                                         RommProgressFn on_progress) {
    const char* label = (kind == SyncKind::Saves) ? "saves" : "states";
    if (!cfg.romm.enabled()) return fail("RomM not configured (set base_url)");
    if (cfg.romm.api_token.empty()) return fail("RomM api_token is empty");

    const auto plan = inspect_install(paths, title);
    if (!plan.installed)
        return fail(std::string("title is not installed — install before syncing ") + label);

    std::error_code ec;
    fs::path game_root = plan.current_link;
    if (!fs::exists(game_root, ec)) game_root = plan.install_root;
    if (fs::is_symlink(game_root, ec)) {
        const auto resolved = fs::weakly_canonical(game_root, ec);
        if (!ec && !resolved.empty()) game_root = resolved;
    }

    int rom_id = 0;
    std::string how;
    std::string err;
    if (!resolve_rom_id(cfg, title, on_progress, &rom_id, &how, &err))
        return fail(std::string("RomM ") + label + ": " + err);

    // Native saves prefer the per-title library tree; savestates stay install-local.
    fs::path library_saves;
    if (kind == SyncKind::Saves && !cfg.saves_root.empty()) {
        // Install/preserved (+ legacy flat platform files) → title library first.
        const int promoted = promote_install_saves_to_library(paths, cfg, title, {});
        if (promoted > 0)
            progress(on_progress, "RomM saves: promoted " + std::to_string(promoted) +
                                      " local file(s) into save library…");
        library_saves = title_saves_dir(paths, cfg, title, true);
        if (library_saves.empty())
            return fail("cannot create saves folder under " + cfg.saves_root.string());
    }

    progress(on_progress, std::string("RomM ") + label + ": collecting local files…");
    std::vector<LocalSave> local;
    if (kind == SyncKind::Saves && !library_saves.empty()) {
        local = collect_local_assets(library_saves, title, kind);
    } else if (kind == SyncKind::Saves) {
        local = collect_local_assets(game_root / "saves", title, kind);
        const fs::path preserved = plan.install_root / "preserved";
        if (fs::is_directory(preserved, ec)) {
            auto more = collect_local_assets(preserved, title, kind);
            for (auto& s : more) {
                bool exists = false;
                for (const auto& e : local) {
                    if (lower_copy(e.file_name) == lower_copy(s.file_name)) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) local.push_back(std::move(s));
            }
        }
    } else {
        local = collect_local_assets(game_root, title, kind);
    }

    progress(on_progress, std::string("RomM ") + label + ": listing remote for rom_id=" +
                              std::to_string(rom_id) + "…");
    auto remote = list_remote_assets(cfg, rom_id, kind, &err);
    if (!err.empty() && remote.empty())
        return fail(std::string("RomM ") + label + " list failed: " + err);

    if (kind == SyncKind::Saves) {
        const auto exts = native_exts_for_title(title);
        remote.erase(std::remove_if(remote.begin(), remote.end(),
                                    [&](const RemoteSave& r) { return !exts.count(r.ext); }),
                     remote.end());
    }

    RommSaveSyncResult result;
    result.rom_id = rom_id;

    std::unordered_set<int> handled_remote;
    const fs::path dest_dir = (kind == SyncKind::Saves && !library_saves.empty())
                                  ? library_saves
                                  : (game_root / ((kind == SyncKind::Saves) ? "saves" : "states"));

    // When landing new files into the title library, adopt the first as preferred if unset.
    const bool title_library_saves = (kind == SyncKind::Saves && !library_saves.empty());
    bool wrote_pref = false;
    auto maybe_adopt_preferred = [&](const std::string& file_name) {
        if (!title_library_saves || wrote_pref || file_name.empty()) return;
        AppState st = load_app_state(paths.state_path);
        if (!preferred_save_for(st, title.id).empty()) {
            wrote_pref = true;
            return;
        }
        set_preferred_save(st, title.id, "saves/" + file_name);
        std::string serr;
        save_app_state(paths.state_path, st, &serr);
        wrote_pref = true;
        progress(on_progress, "RomM saves: set preferred → " + file_name);
    };

    for (const auto& loc : local) {
        progress(on_progress, std::string("RomM ") + label + ": " + loc.file_name + "…");
        const RemoteSave* rem = pick_remote_for_local(remote, loc);
        if (!rem) {
            if (!upload_asset(cfg, rom_id, loc, kind, &err)) {
                result.message = "upload failed for " + loc.file_name + ": " + err;
                return result;
            }
            ++result.uploaded;
            continue;
        }
        handled_remote.insert(rem->id);
        if (!loc.md5.empty() && !rem->content_hash.empty() && loc.md5 == rem->content_hash) {
            ++result.skipped;
            continue;
        }
        // When RomM omits content_hash (common for states), also skip identical sizes
        // with equal-or-newer local mtime handled below.
        if (rem->content_hash.empty() && rem->size > 0) {
            std::error_code lec;
            const auto lsz = fs::file_size(loc.path, lec);
            if (!lec && static_cast<std::uint64_t>(lsz) == rem->size &&
                loc.mtime == rem->updated) {
                ++result.skipped;
                continue;
            }
        }
        if (loc.mtime >= rem->updated) {
            if (!upload_asset(cfg, rom_id, loc, kind, &err)) {
                result.message = "upload failed for " + loc.file_name + ": " + err;
                return result;
            }
            ++result.uploaded;
            ++result.conflicts;
        } else {
            // Always land remote-wins bytes in dest_dir (library when configured).
            const fs::path dest =
                (!dest_dir.empty()) ? (dest_dir / loc.file_name) : loc.path;
            fs::create_directories(dest.parent_path(), ec);
            if (!download_asset(cfg, *rem, dest, kind, &err)) {
                result.message = "download failed for " + rem->file_name + ": " + err;
                return result;
            }
            ++result.downloaded;
            ++result.conflicts;
            maybe_adopt_preferred(loc.file_name);
        }
    }

    for (const auto& rem : remote) {
        if (handled_remote.count(rem.id)) continue;
        progress(on_progress,
                 std::string("RomM ") + label + ": downloading " + rem.file_name + "…");
        fs::create_directories(dest_dir, ec);
        const fs::path dest = dest_dir / rem.file_name;
        if (!download_asset(cfg, rem, dest, kind, &err)) {
            result.message = "download failed for " + rem.file_name + ": " + err;
            return result;
        }
        ++result.downloaded;
        maybe_adopt_preferred(rem.file_name);
    }

    result.ok = true;
    std::ostringstream oss;
    oss << "Synced " << label << " with RomM rom_id=" << rom_id << " (" << how
        << "): uploaded " << result.uploaded << ", downloaded " << result.downloaded
        << ", skipped " << result.skipped;
    result.message = oss.str();
    return result;
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

bool write_text_file(const fs::path& path, const std::string& body, std::string* error) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot write " + path.string();
        return false;
    }
    out << body;
    if (!body.empty() && body.back() != '\n') out << '\n';
    return static_cast<bool>(out);
}

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

std::string extract_toml_quoted(const std::string& body, const std::string& key) {
    // Match `key = "value"` (flexible whitespace).
    const std::string needle = key;
    std::istringstream in(body);
    std::string line;
    while (std::getline(in, line)) {
        std::string t = line;
        if (!t.empty() && t.back() == '\r') t.pop_back();
        // trim leading
        size_t i = 0;
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) ++i;
        if (t.compare(i, needle.size(), needle) != 0) continue;
        size_t j = i + needle.size();
        while (j < t.size() && (t[j] == ' ' || t[j] == '\t')) ++j;
        if (j >= t.size() || t[j] != '=') continue;
        ++j;
        while (j < t.size() && (t[j] == ' ' || t[j] == '\t')) ++j;
        if (j >= t.size() || t[j] != '"') continue;
        ++j;
        std::string val;
        while (j < t.size() && t[j] != '"') {
            if (t[j] == '\\' && j + 1 < t.size()) {
                val.push_back(t[j + 1]);
                j += 2;
            } else {
                val.push_back(t[j++]);
            }
        }
        return val;
    }
    return {};
}

bool is_memcard_ext(const std::string& ext) {
    return ext == ".mcd" || ext == ".mcr" || ext == ".mcs" || ext == ".mc";
}

bool is_battery_ext(const std::string& ext) {
    return ext == ".srm" || ext == ".sav" || ext == ".eep" || ext == ".fla";
}

bool is_disc_platform(const std::string& platform) {
    return platform == "psx" || platform == "ps2" || platform == "saturn";
}

fs::path resolve_game_root(const Paths& paths, const Title& title) {
    const auto plan = inspect_install(paths, title);
    if (!plan.installed) return {};
    std::error_code ec;
    fs::path game_root = plan.current_link;
    if (!fs::exists(game_root, ec)) game_root = plan.install_root;
    if (fs::is_symlink(game_root, ec)) {
        const auto resolved = fs::weakly_canonical(game_root, ec);
        if (!ec && !resolved.empty()) game_root = resolved;
    }
    return game_root;
}

int score_memcard_candidate(const fs::path& path, const Title& title) {
    const std::string name = lower_copy(path.filename().string());
    const std::string stem = lower_copy(path.stem().string());
    int score = 10;
    std::error_code ec;
    const auto sz = fs::file_size(path, ec);
    if (!ec && sz > 0) score += static_cast<int>(std::min<std::uint64_t>(sz / 4096, 50));
    // Prefer named dumps from RomM over blank slot defaults.
    if (stem == "card1" || stem == "card2") score -= 40;
    const std::string want = normalize_key(title.name.empty() ? title.id : title.name);
    const std::string key = normalize_key(stem);
    if (!want.empty() && key.find(want.substr(0, std::min<size_t>(want.size(), 8))) != std::string::npos)
        score += 80;
    if (name.find("usa") != std::string::npos || name.find("eur") != std::string::npos)
        score += 5;
    return score;
}

int managed_save_sort_key(const ManagedSave& s) {
    const std::string stem = lower_copy(fs::path(s.label).stem().string());
    // Prefer named RomM / imported dumps over blank default slot filenames.
    if (stem == "save" || stem == "card1") return 20;
    if (stem == "card2") return 21;
    return 0;
}

// Point slot_dir/save.<ext> at preferred so fixed-path hosts (snesrecomp) share
// the same bytes. Works across directories (library → install bridge).
bool activate_cart_default_slot(const fs::path& preferred, const fs::path& slot_dir,
                                std::string* note) {
    std::error_code ec;
    if (!fs::is_regular_file(preferred, ec) && !fs::is_symlink(preferred, ec)) return false;
    if (slot_dir.empty()) return false;
    fs::create_directories(slot_dir, ec);
    const std::string ext = preferred.extension().string();
    if (ext.empty()) return false;
    const fs::path dest = slot_dir / ("save" + ext);

    if (fs::equivalent(preferred, dest, ec)) {
        if (note) *note = dest.filename().string() + " already active";
        return true;
    }
    if (lower_copy(preferred.filename().string()) == lower_copy(dest.filename().string()) &&
        preferred.parent_path() == slot_dir) {
        if (note) *note = preferred.filename().string();
        return true;
    }

    fs::remove(dest, ec);
    ec.clear();
    if (preferred.parent_path() == slot_dir) {
        fs::create_symlink(preferred.filename(), dest, ec);
        if (!ec) {
            if (note) *note = dest.filename().string() + " → " + preferred.filename().string();
            return true;
        }
        ec.clear();
    }
    const fs::path abs = fs::weakly_canonical(preferred, ec);
    const fs::path target = (!ec && !abs.empty()) ? abs : fs::absolute(preferred);
    ec.clear();
    fs::create_symlink(target, dest, ec);
    if (!ec) {
        if (note) *note = dest.filename().string() + " → " + preferred.filename().string();
        return true;
    }
    ec.clear();
    fs::create_hard_link(preferred, dest, ec);
    if (!ec) {
        if (note) *note = dest.filename().string() + " hardlink " + preferred.filename().string();
        return true;
    }
    ec.clear();
    fs::copy_file(preferred, dest, fs::copy_options::overwrite_existing, ec);
    if (!ec) {
        if (note) *note = "copied " + preferred.filename().string() + " → " + dest.filename().string();
        return true;
    }
    return false;
}

fs::path resolve_preferred_under_saves(const fs::path& saves_dir, const fs::path& preferred_save) {
    if (preferred_save.empty() || saves_dir.empty()) return {};
    std::error_code ec;
    fs::path p = preferred_save;
    if (!p.is_absolute()) {
        p = saves_dir / preferred_save.filename();
    }
    if (fs::is_regular_file(p, ec) || fs::is_symlink(p, ec)) return p;
    return {};
}

void append_managed_from_dir(std::vector<ManagedSave>& out, const fs::path& saves_dir,
                             const Title& title,
                             const std::unordered_set<std::string>& exts) {
    std::error_code ec;
    if (!fs::is_directory(saves_dir, ec)) return;
    for (auto it = fs::directory_iterator(saves_dir, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) && !it->is_symlink(ec)) continue;
        const fs::path path = it->path();
        const std::string rel = "saves/" + path.filename().generic_string();
        if (!path_matches_native_glob(rel, title, exts) &&
            !path_matches_native_glob(path.filename().generic_string(), title, exts))
            continue;
        if (it->is_symlink(ec) && !fs::exists(path, ec)) continue;
        if (it->is_symlink(ec)) {
            const std::string stem = lower_copy(path.stem().string());
            if (stem == "save" || stem == "card1" || stem == "card2") continue;
        }
        bool dup = false;
        for (const auto& e : out) {
            if (lower_copy(e.label) == lower_copy(path.filename().string())) {
                dup = true;
                break;
            }
        }
        if (dup) continue;
        ManagedSave m;
        m.id = rel;
        m.label = path.filename().string();
        m.host_path = path;
        out.push_back(std::move(m));
    }
}

std::string sanitize_save_stem(std::string s) {
    // Drop directories / extension if a path snuck in.
    s = fs::path(s).filename().string();
    const auto dot = s.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        const std::string ext = lower_copy(s.substr(dot));
        if (is_native_save_ext(ext)) s = s.substr(0, dot);
    }
    for (char& c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|' || u < 32)
            c = '_';
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '.')) s.erase(s.begin());
    return s.empty() ? "save" : s;
}

std::string save_stem_for_title(const Title& title, const fs::path& rom_hint) {
    if (!rom_hint.empty()) return sanitize_save_stem(rom_hint.filename().string());
    if (!title.name.empty()) return sanitize_save_stem(title.name);
    return sanitize_save_stem(title.id);
}

std::string default_native_ext(const Title& title) {
    const auto exts = native_exts_for_title(title);
    if (is_disc_platform(title.platform) || !title.saves_memcard_glob.empty()) {
        if (exts.count(".mcd")) return ".mcd";
        if (exts.count(".mcr")) return ".mcr";
        if (exts.count(".mcs")) return ".mcs";
        return ".mcd";
    }
    if (exts.count(".srm")) return ".srm";
    if (exts.count(".sav")) return ".sav";
    if (!exts.empty()) return *exts.begin();
    return ".srm";
}

bool path_under_dir(const fs::path& file, const fs::path& dir) {
    std::error_code ec;
    const fs::path a = fs::weakly_canonical(file, ec);
    const fs::path b = fs::weakly_canonical(dir, ec);
    if (ec || a.empty() || b.empty()) return false;
    const std::string as = a.generic_string();
    const std::string bs = b.generic_string();
    if (as.size() < bs.size()) return false;
    if (as.compare(0, bs.size(), bs) != 0) return false;
    return as.size() == bs.size() || as[bs.size()] == '/';
}

bool is_install_bridge_symlink(const fs::path& path, const fs::path& library_dir) {
    std::error_code ec;
    if (!fs::is_symlink(path, ec)) return false;
    const std::string stem = lower_copy(path.stem().string());
    if (stem != "save" && stem != "card1" && stem != "card2") return false;
    if (library_dir.empty()) return true;
    const fs::path target = fs::weakly_canonical(path, ec);
    return !ec && !target.empty() && path_under_dir(target, library_dir);
}

fs::path unique_library_dest(const fs::path& library_dir, const std::string& stem,
                             const std::string& ext) {
    std::error_code ec;
    fs::path dest = library_dir / (stem + ext);
    if (!fs::exists(dest, ec)) return dest;
    for (int n = 2; n < 1000; ++n) {
        dest = library_dir / (stem + "-" + std::to_string(n) + ext);
        if (!fs::exists(dest, ec)) return dest;
    }
    return library_dir / (stem + "-new" + ext);
}

ManagedSave managed_from_path(const fs::path& path) {
    ManagedSave m;
    m.label = path.filename().string();
    m.id = "saves/" + m.label;
    m.host_path = path;
    return m;
}

bool mint_empty_save_file(const fs::path& dest, std::string* error) {
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    if (ec) {
        if (error) *error = "cannot create saves dir: " + ec.message();
        return false;
    }
    if (fs::exists(dest, ec)) {
        if (error) *error = "save already exists: " + dest.filename().string();
        return false;
    }
    std::ofstream out(dest, std::ios::binary);
    if (!out) {
        if (error) *error = "cannot create " + dest.string();
        return false;
    }
    return true;
}

int promote_files_into_library(const fs::path& src_dir, const fs::path& library_dir,
                               const Title& title, const std::string& stem,
                               bool remove_src_after) {
    if (src_dir.empty() || library_dir.empty() || src_dir == library_dir) return 0;
    const auto exts = native_exts_for_title(title);
    std::error_code ec;
    if (!fs::is_directory(src_dir, ec)) return 0;
    int promoted = 0;
    for (auto it = fs::directory_iterator(src_dir, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) && !it->is_symlink(ec)) continue;
        const fs::path src = it->path();
        if (is_install_bridge_symlink(src, library_dir)) continue;
        // Broken bridge links — drop them.
        if (it->is_symlink(ec) && !fs::exists(src, ec)) {
            fs::remove(src, ec);
            continue;
        }
        const std::string rel = "saves/" + src.filename().generic_string();
        if (!path_matches_native_glob(rel, title, exts) &&
            !path_matches_native_glob(src.filename().generic_string(), title, exts))
            continue;

        const std::string ext = lower_copy(src.extension().string());
        std::string dest_stem = sanitize_save_stem(src.stem().string());
        const std::string low = lower_copy(dest_stem);
        if (low == "save" || low == "card1" || low == "card2") dest_stem = stem;

        fs::path dest = library_dir / (dest_stem + ext);
        if (fs::exists(dest, ec)) {
            const std::string src_md5 = file_md5_hex(src);
            const std::string dst_md5 = file_md5_hex(dest);
            if (!src_md5.empty() && src_md5 == dst_md5) {
                if (remove_src_after && !path_under_dir(src, library_dir)) fs::remove(src, ec);
                continue;
            }
            if (file_mtime_sec(src) >= file_mtime_sec(dest)) {
                fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
                if (!ec) {
                    ++promoted;
                    if (remove_src_after && !path_under_dir(src, library_dir)) fs::remove(src, ec);
                }
                continue;
            }
            // Keep both — promote under a unique name.
            dest = unique_library_dest(library_dir, dest_stem, ext);
        }

        fs::create_directories(library_dir, ec);
        if (it->is_symlink(ec)) {
            // Materialize symlink target bytes into the library.
            fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
        } else {
            fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) continue;
        ++promoted;
        if (remove_src_after && !path_under_dir(src, library_dir)) fs::remove(src, ec);
    }
    return promoted;
}

std::string title_library_folder_name(const Title& title) {
    if (!title.id.empty()) return sanitize_save_stem(title.id);
    if (!title.name.empty()) return sanitize_save_stem(title.name);
    return "unknown";
}

// Older RetComM builds used a flat saves_root/<platform>/ pool. Quarantine this
// title's preferred / title-named files into saves_root/<platform>/<title_id>/.
// Only called from promote/ensure/sync — never from hub refresh listing.
int migrate_legacy_flat_library_saves(const Paths& paths, const AppConfig& cfg,
                                      const Title& title, const fs::path& rom_hint,
                                      const fs::path& title_dir) {
    if (cfg.saves_root.empty() || title_dir.empty()) return 0;
    const fs::path platform_dir = cfg.saves_dir_for_platform(title.platform, false);
    if (platform_dir.empty() || platform_dir == title_dir) return 0;

    std::error_code ec;
    if (!fs::is_directory(platform_dir, ec)) return 0;

    // Snapshot flat-pool files first (no rename during iteration; fast empty bail).
    std::vector<fs::path> flat_files;
    for (auto it = fs::directory_iterator(platform_dir, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec) || it->is_symlink(ec)) flat_files.push_back(it->path());
    }
    if (flat_files.empty()) return 0;

    const AppState st = load_app_state(paths.state_path);
    std::unordered_set<std::string> want; // lowercased filenames to claim
    auto add_save_id = [&](const std::string& save_id) {
        if (save_id.empty() || save_id == kBlankMemcardId) return;
        want.insert(lower_copy(fs::path(save_id).filename().string()));
    };
    add_save_id(preferred_save_for(st, title.id));
    add_save_id(preferred_save_card2_for(st, title.id));

    const std::string want_stem = lower_copy(save_stem_for_title(title, rom_hint));
    const auto exts = native_exts_for_title(title);

    auto claimed_by_other = [&](const std::string& file_name_lower) {
        auto hit = [&](const std::unordered_map<std::string, std::string>& map) {
            for (const auto& [tid, sid] : map) {
                if (tid == title.id || sid.empty() || sid == kBlankMemcardId) continue;
                if (lower_copy(fs::path(sid).filename().string()) == file_name_lower)
                    return true;
            }
            return false;
        };
        return hit(st.preferred_save) || hit(st.preferred_save_card2);
    };

    int relocated = 0;
    for (const fs::path& src : flat_files) {
        if (!fs::exists(src, ec)) continue; // already claimed by an earlier title
        const std::string name = src.filename().string();
        const std::string name_l = lower_copy(name);
        const std::string rel = "saves/" + name;
        if (!path_matches_native_glob(rel, title, exts) &&
            !path_matches_native_glob(name, title, exts))
            continue;

        const std::string stem = lower_copy(src.stem().string());
        const bool preferred = want.count(name_l) > 0;
        // Exact ROM/title stem, or a strict alphanumeric name match — never
        // short fuzzy hits that steal unrelated cards.
        const bool named =
            (!want_stem.empty() && stem == want_stem) || save_name_matches_title(name, title);
        if (!preferred && !named) continue;

        const bool shared = claimed_by_other(name_l);
        // Non-preferred name matches that other titles still point at stay put.
        if (!preferred && shared) continue;

        fs::create_directories(title_dir, ec);
        if (ec) continue;
        fs::path dest = title_dir / name;
        if (fs::exists(dest, ec)) {
            // Avoid MD5 on the refresh/promote path — size match is enough to
            // treat as already quarantined.
            const auto src_sz = fs::file_size(src, ec);
            const auto dst_sz = fs::file_size(dest, ec);
            if (!ec && src_sz == dst_sz) {
                if (!shared) fs::remove(src, ec);
                continue;
            }
            dest = unique_library_dest(title_dir, sanitize_save_stem(src.stem().string()),
                                       lower_copy(src.extension().string()));
        }

        // Shared preferred cards: copy so other titles keep the flat original.
        // Exclusive: rename (fall back to copy+remove).
        ec.clear();
        if (shared) {
            fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
            if (ec) continue;
        } else {
            fs::rename(src, dest, ec);
            if (ec) {
                ec.clear();
                fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
                if (ec) continue;
                fs::remove(src, ec);
            }
        }
        ++relocated;
    }
    return relocated;
}

} // namespace

fs::path title_saves_dir(const Paths& paths, const AppConfig& cfg, const Title& title,
                         bool create) {
    if (!cfg.saves_root.empty()) {
        // Ensure the platform folder exists when creating the title quarantine.
        const fs::path plat = cfg.saves_dir_for_platform(title.platform, create);
        if (plat.empty()) return {};
        const fs::path dir = plat / title_library_folder_name(title);
        if (create) {
            std::error_code ec;
            fs::create_directories(dir, ec);
            if (ec) return {};
        }
        return dir;
    }
    const fs::path game_root = resolve_game_root(paths, title);
    if (game_root.empty()) return {};
    const fs::path dir = game_root / "saves";
    if (create) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) return {};
    }
    return dir;
}

int promote_install_saves_to_library(const Paths& paths, const AppConfig& cfg, const Title& title,
                                     const fs::path& rom_hint) {
    if (cfg.saves_root.empty()) return 0;
    const fs::path library = title_saves_dir(paths, cfg, title, true);
    if (library.empty()) return 0;
    const std::string stem = save_stem_for_title(title, rom_hint);
    int n = 0;
    n += migrate_legacy_flat_library_saves(paths, cfg, title, rom_hint, library);
    const fs::path game_root = resolve_game_root(paths, title);
    if (!game_root.empty())
        n += promote_files_into_library(game_root / "saves", library, title, stem, true);
    const auto plan = inspect_install(paths, title);
    if (!plan.install_root.empty())
        n += promote_files_into_library(plan.install_root / "preserved", library, title, stem,
                                        false);
    return n;
}

std::vector<ManagedSave> list_managed_saves(const Paths& paths, const AppConfig& cfg,
                                            const Title& title) {
    std::vector<ManagedSave> out;
    const auto exts = native_exts_for_title(title);
    // Listing only — never migrate here (hub refresh calls this for every title).
    // Play / ensure / promote / RomM sync pull leftovers out of the flat pool.
    const fs::path primary = title_saves_dir(paths, cfg, title, false);
    append_managed_from_dir(out, primary, title, exts);
    std::sort(out.begin(), out.end(), [](const ManagedSave& a, const ManagedSave& b) {
        const int ka = managed_save_sort_key(a);
        const int kb = managed_save_sort_key(b);
        if (ka != kb) return ka < kb;
        return lower_copy(a.label) < lower_copy(b.label);
    });
    return out;
}

// Memcard titles: default slot 2 to blank when the user has never chosen one.
bool ensure_memcard2_default_blank(AppState& st, const Title& title) {
    if (!title_uses_memcards(title)) return false;
    if (!preferred_save_card2_for(st, title.id).empty()) return false;
    set_preferred_save_card2(st, title.id, kBlankMemcardId);
    return true;
}

// Prefer an existing library file named for this title / ROM; else none.
const ManagedSave* find_title_named_save(const std::vector<ManagedSave>& saves, const Title& title,
                                         const fs::path& rom_hint) {
    if (saves.empty()) return nullptr;
    const std::string want_stem = lower_copy(save_stem_for_title(title, rom_hint));
    const ManagedSave* fuzzy = nullptr;
    for (const auto& s : saves) {
        const std::string stem = lower_copy(fs::path(s.label).stem().string());
        if (!want_stem.empty() && stem == want_stem) return &s;
        if (!fuzzy && save_name_matches_title(s.label, title)) fuzzy = &s;
    }
    return fuzzy;
}

CanonicalSaveResult ensure_canonical_save(const Paths& paths, const AppConfig& cfg,
                                          const Title& title, const fs::path& rom_hint,
                                          bool mint_if_missing) {
    CanonicalSaveResult r;
    r.promoted = promote_install_saves_to_library(paths, cfg, title, rom_hint);

    const fs::path saves_dir = title_saves_dir(paths, cfg, title, true);
    if (saves_dir.empty()) {
        r.message = "cannot resolve saves directory";
        return r;
    }

    AppState st = load_app_state(paths.state_path);
    bool state_dirty = ensure_memcard2_default_blank(st, title);
    auto persist = [&] {
        if (!state_dirty) return;
        std::string err;
        save_app_state(paths.state_path, st, &err);
        state_dirty = false;
    };

    const std::string pref = preferred_save_for(st, title.id);
    if (!pref.empty()) {
        const fs::path hit = resolve_managed_save(paths, cfg, title, pref);
        if (!hit.empty()) {
            r.ok = true;
            r.save = managed_from_path(hit);
            persist();
            r.message = r.promoted > 0
                            ? ("promoted " + std::to_string(r.promoted) + " save(s); using " +
                               r.save.label)
                            : ("using " + r.save.label);
            return r;
        }
    }

    auto saves = list_managed_saves(paths, cfg, title);
    // Prefer a file named for this game; otherwise mint a title-stem file.
    if (const ManagedSave* named = find_title_named_save(saves, title, rom_hint)) {
        r.ok = true;
        r.save = *named;
        if (pref.empty()) {
            set_preferred_save(st, title.id, r.save.id);
            state_dirty = true;
        }
        persist();
        r.message = r.promoted > 0
                        ? ("promoted " + std::to_string(r.promoted) + " save(s); preferred " +
                           r.save.label)
                        : ("preferred " + r.save.label);
        return r;
    }

    if (!mint_if_missing) {
        persist();
        r.message = r.promoted > 0 ? ("promoted " + std::to_string(r.promoted) +
                                      " save(s); none available for this title")
                                   : "no managed saves yet for this title";
        return r;
    }

    const std::string stem = save_stem_for_title(title, rom_hint);
    const std::string ext = default_native_ext(title);
    const fs::path dest = unique_library_dest(saves_dir, stem, ext);
    std::string err;
    if (!mint_empty_save_file(dest, &err)) {
        persist();
        r.message = err;
        return r;
    }
    r.ok = true;
    r.created = true;
    r.save = managed_from_path(dest);
    set_preferred_save(st, title.id, r.save.id);
    state_dirty = true;
    persist();
    r.message = "created " + r.save.label +
                (cfg.saves_root.empty() ? " in install saves/" : " in save library");
    return r;
}

CanonicalSaveResult create_managed_save(const Paths& paths, const AppConfig& cfg,
                                        const Title& title, const fs::path& rom_hint) {
    CanonicalSaveResult r;
    r.promoted = promote_install_saves_to_library(paths, cfg, title, rom_hint);
    const fs::path saves_dir = title_saves_dir(paths, cfg, title, true);
    if (saves_dir.empty()) {
        r.message = "cannot resolve saves directory";
        return r;
    }
    const std::string stem = save_stem_for_title(title, rom_hint);
    const std::string ext = default_native_ext(title);
    const fs::path dest = unique_library_dest(saves_dir, stem, ext);
    std::string err;
    if (!mint_empty_save_file(dest, &err)) {
        r.message = err;
        return r;
    }
    r.ok = true;
    r.created = true;
    r.save = managed_from_path(dest);
    AppState st = load_app_state(paths.state_path);
    set_preferred_save(st, title.id, r.save.id);
    save_app_state(paths.state_path, st, &err);
    r.message = "created " + r.save.label;
    return r;
}

fs::path resolve_managed_save(const Paths& paths, const AppConfig& cfg, const Title& title,
                              const std::string& save_id) {
    if (save_id.empty()) return {};
    const fs::path primary = title_saves_dir(paths, cfg, title, false);
    fs::path hit = resolve_preferred_under_saves(primary, save_id);
    if (!hit.empty()) return hit;
    if (!cfg.saves_root.empty()) {
        // Legacy flat saves_root/<platform>/ before quarantine migrate runs.
        const fs::path legacy = cfg.saves_dir_for_platform(title.platform, false);
        hit = resolve_preferred_under_saves(legacy, save_id);
        if (!hit.empty()) return hit;
        const fs::path game_root = resolve_game_root(paths, title);
        if (!game_root.empty())
            return resolve_preferred_under_saves(game_root / "saves", save_id);
    }
    return {};
}

fs::path resolve_save_arg(const fs::path& saves_dir, const fs::path& arg) {
    if (arg.empty() || saves_dir.empty()) return {};
    std::error_code ec;
    if (arg.is_absolute()) {
        if (fs::is_regular_file(arg, ec) || fs::is_symlink(arg, ec)) return arg;
        return resolve_preferred_under_saves(saves_dir, arg);
    }
    return resolve_preferred_under_saves(saves_dir, arg);
}

RecompSaveBindResult bind_recomp_save_paths(const Paths& paths, const AppConfig& cfg,
                                            const Title& title, bool use_wine,
                                            const fs::path& preferred_save,
                                            const fs::path& preferred_save_card2,
                                            bool card2_blank) {
    RecompSaveBindResult r;
    const fs::path game_root = resolve_game_root(paths, title);
    if (game_root.empty()) {
        r.message = "save bind: title not installed";
        return r;
    }

    std::error_code ec;
    const fs::path saves_dir = title_saves_dir(paths, cfg, title, true);
    if (saves_dir.empty()) {
        r.message = "save bind: cannot create saves directory";
        return r;
    }
    r.saves_dir = saves_dir;

    // Hosts that only look under cwd/saves get a bridge into the library tree.
    const fs::path install_saves = game_root / "saves";
    fs::create_directories(install_saves, ec);

    fs::path preferred = resolve_save_arg(saves_dir, preferred_save);
    if (preferred.empty() && !preferred_save.empty())
        preferred = resolve_managed_save(paths, cfg, title, preferred_save.generic_string());
    fs::path preferred_card2 = resolve_save_arg(saves_dir, preferred_save_card2);
    if (preferred_card2.empty() && !preferred_save_card2.empty())
        preferred_card2 =
            resolve_managed_save(paths, cfg, title, preferred_save_card2.generic_string());

    std::vector<fs::path> memcards;
    std::vector<fs::path> batteries;
    for (auto it = fs::directory_iterator(saves_dir, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) && !it->is_symlink(ec)) continue;
        const std::string ext = lower_copy(it->path().extension().string());
        if (is_memcard_ext(ext)) memcards.push_back(it->path());
        else if (is_battery_ext(ext)) batteries.push_back(it->path());
    }

    std::sort(memcards.begin(), memcards.end(), [&](const fs::path& a, const fs::path& b) {
        return score_memcard_candidate(a, title) > score_memcard_candidate(b, title);
    });

    std::ostringstream notes;

    // Cart: honour preferred save (activate default slot + expose for --save-path).
    if (!is_disc_platform(title.platform)) {
        if (!preferred.empty() && is_battery_ext(lower_copy(preferred.extension().string()))) {
            r.active_save = preferred;
            std::string act;
            // Bridge into install saves/ for fixed-path hosts; also keep a
            // library-local save.* alias when preferred lives there.
            if (activate_cart_default_slot(preferred, install_saves, &act))
                notes << "active save " << preferred.filename().string()
                      << (act.empty() ? "" : (" (" + act + ")")) << "; ";
            else
                notes << "active save " << preferred.filename().string() << "; ";
            if (preferred.parent_path() == saves_dir)
                activate_cart_default_slot(preferred, saves_dir, nullptr);
        } else if (!batteries.empty()) {
            // Fallback: first battery in the managed dir (should be rare after ensure).
            r.active_save = batteries.front();
            std::string act;
            if (activate_cart_default_slot(r.active_save, install_saves, &act))
                notes << "active save " << r.active_save.filename().string()
                      << (act.empty() ? "" : (" (" + act + ")")) << "; ";
        }
    }

    // Disc / memcard titles: upsert settings.toml [memcard].
    const fs::path settings_path = game_root / "settings.toml";
    const bool want_memcard =
        is_disc_platform(title.platform) || !title.saves_memcard_glob.empty() || !memcards.empty();

    if (want_memcard) {
        std::string body;
        {
            std::ifstream in(settings_path);
            if (in) {
                std::ostringstream ss;
                ss << in.rdbuf();
                body = ss.str();
            }
        }

        const std::string prev_card1 = extract_toml_quoted(body, "card1");
        const std::string prev_card2 = extract_toml_quoted(body, "card2");

        auto still_valid = [&](const std::string& p) -> fs::path {
            if (p.empty()) return {};
            std::string host = p;
            if (host.rfind("Z:", 0) == 0 || host.rfind("z:", 0) == 0) host = host.substr(2);
            fs::path fp(host);
            if (!fs::is_regular_file(fp, ec) && !fs::is_symlink(fp, ec)) return {};
            const auto canon = fs::weakly_canonical(fp, ec);
            const auto saves_canon = fs::weakly_canonical(saves_dir, ec);
            if (!ec && !saves_canon.empty() && !canon.empty()) {
                const std::string a = canon.generic_string();
                const std::string b = saves_canon.generic_string();
                if (a.rfind(b, 0) != 0) {
                    if (!memcards.empty()) return {};
                }
            }
            return fp;
        };

        fs::path card1;
        if (!preferred.empty() && is_memcard_ext(lower_copy(preferred.extension().string())))
            card1 = preferred;
        if (card1.empty()) card1 = still_valid(prev_card1);

        fs::path card2;
        if (card2_blank) {
            card2 = saves_dir / "card2.mcd";
        } else if (!preferred_card2.empty() &&
                   is_memcard_ext(lower_copy(preferred_card2.extension().string()))) {
            card2 = preferred_card2;
        }

        if (card1.empty()) {
            // Prefer a title-scored pool file (memcards sorted by score_memcard_candidate).
            if (!memcards.empty())
                card1 = memcards.front();
            else {
                // Prefer an ES-DE-style stem name over generic card1.mcd.
                // (ensure_canonical_save normally mints this before bind.)
                const std::string stem = save_stem_for_title(title, {});
                card1 = saves_dir / (stem + ".mcd");
            }
        }
        if (card2.empty()) {
            // No explicit card2 preference — keep a prior valid card2 only if it is
            // still under the saves dir; otherwise leave slot 2 blank (card2.mcd).
            // Do not auto-assign an unrelated memcard when the title dir is empty.
            card2 = still_valid(prev_card2);
            if (card2.empty() || card2 == card1) card2 = saves_dir / "card2.mcd";
        }

        r.card1 = card1;
        r.card2 = card2;
        r.active_save = card1;

        strip_toml_table(body, "memcard");
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' ' ||
                                 body.back() == '\t'))
            body.pop_back();
        if (!body.empty()) body.push_back('\n');

        body += "\n[memcard]\n";
        body += "dir     = \"";
        body += toml_escape(path_for_guest(saves_dir, use_wine));
        body += "\"\n";
        body += "card1   = \"";
        body += toml_escape(path_for_guest(card1, use_wine));
        body += "\"\n";
        body += "card2   = \"";
        body += toml_escape(path_for_guest(card2, use_wine));
        body += "\"\n";
        body += "enable1 = true\n";
        body += "enable2 = true\n";

        std::string err;
        if (!write_text_file(settings_path, body, &err)) {
            r.message = "save bind: " + err;
            return r;
        }
        notes << "settings.toml [memcard] → " << saves_dir.string() << " (card1="
              << card1.filename().string() << ", card2=" << card2.filename().string() << ")";
    } else if (notes.str().empty()) {
        notes << "saves dir ready: " << saves_dir.string();
    }

    r.ok = true;
    r.message = notes.str();
    return r;
}

bool title_uses_memcards(const Title& title) {
    return is_disc_platform(title.platform) || !title.saves_memcard_glob.empty();
}

RommSaveSyncResult sync_saves_with_romm(const Paths& paths, const AppConfig& cfg,
                                        const Title& title, RommProgressFn on_progress) {
    auto result = sync_assets_with_romm(paths, cfg, title, SyncKind::Saves, on_progress);
    if (result.ok) {
        const auto plan = inspect_install(paths, title);
        const bool wine = plan.record && plan.record->runtime == "wine";
        fs::path preferred;
        fs::path preferred_card2;
        bool card2_blank = false;
        {
            const auto st = load_app_state(paths.state_path);
            const std::string save_id = preferred_save_for(st, title.id);
            if (!save_id.empty()) preferred = resolve_managed_save(paths, cfg, title, save_id);
            const std::string card2_id = preferred_save_card2_for(st, title.id);
            if (card2_id == kBlankMemcardId)
                card2_blank = true;
            else if (!card2_id.empty())
                preferred_card2 = resolve_managed_save(paths, cfg, title, card2_id);
        }
        auto bind = bind_recomp_save_paths(paths, cfg, title, wine, preferred, preferred_card2,
                                           card2_blank);
        if (!bind.message.empty()) {
            result.message += "\n";
            result.message += bind.message;
        }
        if (!bind.ok && result.message.find("save bind:") == std::string::npos) {
            result.message += "\nwarning: could not bind recomp save paths";
        }
    }
    return result;
}

RommSaveSyncResult sync_states_with_romm(const Paths& paths, const AppConfig& cfg,
                                         const Title& title, RommProgressFn on_progress) {
    return sync_assets_with_romm(paths, cfg, title, SyncKind::States, on_progress);
}

} // namespace retcomm
