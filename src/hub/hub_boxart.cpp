#include "hub/hub_boxart.hpp"

#include "retcomm/http.hpp"

#include <SDL3/SDL_opengl.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <system_error>
#include <vector>

namespace retcomm::hub {
namespace {

using json = nlohmann::json;

std::string lower_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string normalize_key(std::string s) {
    s = lower_copy(std::move(s));
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) out.push_back(c);
    }
    return out;
}

bool is_image_ext(const std::string& ext_lower) {
    return ext_lower == ".png" || ext_lower == ".jpg" || ext_lower == ".jpeg";
}

std::string stem_key_from_filename(const std::string& filename) {
    fs::path p(filename);
    return normalize_key(p.stem().string());
}

bool keys_match(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    if (a == b) return true;
    // Allow shortened covers ("Bomberman Party Edition" vs full No-Intro name).
    if (a.size() >= 8 && b.size() >= 8 && (a.find(b) != std::string::npos ||
                                           b.find(a) != std::string::npos))
        return true;
    return false;
}

bool try_sibling_images(const fs::path& base_file, fs::path* out) {
    if (base_file.empty()) return false;
    const fs::path parent = base_file.parent_path();
    const std::string stem = base_file.stem().string();
    const char* exts[] = {".png", ".PNG", ".jpg", ".JPG", ".jpeg", ".JPEG"};
    std::error_code ec;
    for (const char* ext : exts) {
        const fs::path cand = parent / (stem + ext);
        if (fs::is_regular_file(cand, ec)) {
            *out = cand;
            return true;
        }
    }
    return false;
}

bool scan_dir_for_key(const fs::path& dir, const std::string& want_key, fs::path* out,
                      bool recursive) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || want_key.empty()) return false;

    auto consider = [&](const fs::directory_entry& ent) {
        if (!ent.is_regular_file(ec)) return false;
        const auto& p = ent.path();
        const std::string ext = lower_copy(p.extension().string());
        if (!is_image_ext(ext)) return false;
        if (!keys_match(normalize_key(p.stem().string()), want_key)) return false;
        *out = p;
        return true;
    };

    if (!recursive) {
        for (fs::directory_iterator it(dir, ec); !ec && it != fs::directory_iterator();
             it.increment(ec)) {
            if (consider(*it)) return true;
        }
        return false;
    }

    auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied,
                                               ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (consider(*it)) return true;
    }
    return false;
}

fs::path find_cached_boxart(const fs::path& cache_dir, const std::string& title_id) {
    if (cache_dir.empty() || title_id.empty()) return {};
    std::error_code ec;
    const char* exts[] = {".png", ".jpg", ".jpeg", ".PNG", ".JPG", ".JPEG"};
    for (const char* ext : exts) {
        const fs::path cand = cache_dir / (title_id + ext);
        if (fs::is_regular_file(cand, ec)) return cand;
    }
    return {};
}

std::string url_encode(const std::string& s) {
    CURL* curl = curl_easy_init();
    if (!curl) return s;
    char* enc = curl_easy_escape(curl, s.c_str(), static_cast<int>(s.size()));
    std::string out = enc ? enc : s;
    if (enc) curl_free(enc);
    curl_easy_cleanup(curl);
    return out;
}

// Libretro thumbnail filename rules: replace &*/:`<>?\|" with _
std::string libretro_sanitize_name(std::string name) {
    for (char& c : name) {
        switch (c) {
        case '&':
        case '*':
        case '/':
        case ':':
        case '`':
        case '<':
        case '>':
        case '?':
        case '\\':
        case '|':
        case '"':
            c = '_';
            break;
        default:
            break;
        }
    }
    return name;
}

std::string strip_ext(std::string name) {
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0 && name.size() - dot <= 5)
        name.resize(dot);
    return name;
}

std::string short_game_name(std::string name) {
    name = strip_ext(std::move(name));
    const auto paren = name.find(" (");
    if (paren != std::string::npos) name.resize(paren);
    return name;
}

const char* libretro_system_folder(const std::string& platform) {
    if (platform == "snes") return "Nintendo - Super Nintendo Entertainment System";
    if (platform == "n64") return "Nintendo - Nintendo 64";
    if (platform == "gba") return "Nintendo - Game Boy Advance";
    if (platform == "nds") return "Nintendo - Nintendo DS";
    if (platform == "genesis" || platform == "md" || platform == "megadrive")
        return "Sega - Mega Drive - Genesis";
    if (platform == "psx" || platform == "ps1") return "Sony - PlayStation";
    return nullptr;
}

std::vector<std::string> boxart_name_candidates(const Title& title, const fs::path& rom_path,
                                                const std::string& suggested_rom) {
    std::vector<std::string> names;
    auto push = [&](std::string n) {
        n = strip_ext(std::move(n));
        if (n.empty()) return;
        for (const auto& e : names)
            if (e == n) return;
        names.push_back(n);
        const std::string sh = short_game_name(n);
        if (sh != n) {
            for (const auto& e : names)
                if (e == sh) return;
            names.push_back(sh);
        }
    };
    if (!suggested_rom.empty()) push(suggested_rom);
    for (const auto& fn : title.rom_identity.filenames) push(fn);
    if (!rom_path.empty()) push(rom_path.stem().string());
    push(title.name);
    return names;
}

bool download_image_url(const std::string& url, const fs::path& dest,
                        const std::vector<std::pair<std::string, std::string>>& headers,
                        std::string* error) {
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    const fs::path tmp = dest.string() + ".partial";
    fs::remove(tmp, ec);
    if (!http_download(url, tmp, error, headers)) {
        fs::remove(tmp, ec);
        return false;
    }
    // Reject tiny/non-image bodies (HTML error pages).
    const auto sz = fs::file_size(tmp, ec);
    if (ec || sz < 256) {
        fs::remove(tmp, ec);
        if (error) *error = "download too small / empty";
        return false;
    }
    fs::remove(dest, ec);
    fs::rename(tmp, dest, ec);
    if (ec) {
        if (error) *error = "rename failed: " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

BoxartFetchResult fetch_libretro(const Paths& paths, const Title& title, const fs::path& rom_path,
                                 const std::string& suggested_rom) {
    BoxartFetchResult r;
    r.source = "libretro";
    const char* sys = libretro_system_folder(title.platform);
    if (!sys) {
        r.message = "no Libretro system mapping for platform " + title.platform;
        return r;
    }

    AppConfig src_cfg;
    src_cfg.romm.sync_boxart = false;
    const fs::path dest = boxart_cache_dir(paths, src_cfg) / (title.id + ".png");
    const auto names = boxart_name_candidates(title, rom_path, suggested_rom);
    std::string last_err;
    for (const auto& name : names) {
        const std::string file = libretro_sanitize_name(name) + ".png";
        const std::string url = std::string("https://thumbnails.libretro.com/") +
                                url_encode(sys) + "/Named_Boxarts/" + url_encode(file);
        std::string err;
        if (download_image_url(url, dest, {}, &err)) {
            r.ok = true;
            r.path = dest;
            r.message = "Libretro: " + file;
            return r;
        }
        last_err = err.empty() ? "not found" : err;
    }
    r.message = "Libretro miss (" + std::string(sys) + "): " + last_err;
    return r;
}

// RomM path_cover_* often includes an unencoded `ts=YYYY-MM-DD HH:MM:SS` query.
// libcurl rejects raw spaces (CURLE_URL_MALFORMAT / "bad URL").
std::string sanitize_url_for_curl(std::string url) {
    std::string out;
    out.reserve(url.size() + 16);
    for (unsigned char c : url) {
        if (c == ' ') {
            out += "%20";
        } else if (c < 0x20 || c == 0x7f) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

std::string absolutize_romm_url(const std::string& base, std::string path_or_url) {
    if (path_or_url.empty()) return {};
    if (path_or_url.rfind("http://", 0) == 0 || path_or_url.rfind("https://", 0) == 0)
        return sanitize_url_for_curl(std::move(path_or_url));
    if (path_or_url.front() != '/') path_or_url.insert(path_or_url.begin(), '/');
    return sanitize_url_for_curl(base + path_or_url);
}

// Prefer RomM menu covers (path_cover_*) — these are the images shown in the
// RomM UI and include user-uploaded / themed artwork. Fall back to url_cover
// (IGDB/SGDB/etc.) only when RomM has no local cover resource.
std::string pick_romm_cover_url(const std::string& base, const json& item) {
    for (const char* key : {"path_cover_large", "path_cover_small"}) {
        std::string u = absolutize_romm_url(base, item.value(key, ""));
        if (!u.empty()) return u;
    }
    const std::string url_c = item.value("url_cover", "");
    if (url_c.rfind("http://", 0) == 0 || url_c.rfind("https://", 0) == 0)
        return sanitize_url_for_curl(url_c);
    return absolutize_romm_url(base, url_c);
}

// RomM path_cover_* often ends in .png while the body is JPEG (or vice versa).
std::string sniff_image_ext(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    unsigned char mag[12]{};
    in.read(reinterpret_cast<char*>(mag), sizeof(mag));
    const auto n = static_cast<size_t>(in.gcount());
    if (n >= 3 && mag[0] == 0xFF && mag[1] == 0xD8 && mag[2] == 0xFF) return ".jpg";
    if (n >= 8 && mag[0] == 0x89 && mag[1] == 'P' && mag[2] == 'N' && mag[3] == 'G')
        return ".png";
    if (n >= 12 && mag[0] == 'R' && mag[1] == 'I' && mag[2] == 'F' && mag[3] == 'F' &&
        mag[8] == 'W' && mag[9] == 'E' && mag[10] == 'B' && mag[11] == 'P')
        return ".webp";
    return {};
}

BoxartFetchResult fetch_romm(const Paths& paths, const AppConfig& cfg, const Title& title,
                             const fs::path& rom_path, const std::string& suggested_rom) {
    BoxartFetchResult r;
    r.source = "romm";
    if (!cfg.romm.enabled()) {
        r.message = "RomM sync boxart on, but base_url is empty";
        return r;
    }
    if (cfg.romm.api_token.empty()) {
        r.message = "RomM sync boxart on, but api_token is empty";
        return r;
    }

    auto names = boxart_name_candidates(title, rom_path, suggested_rom);
    if (names.empty()) names.push_back(title.name.empty() ? title.id : title.name);

    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("Authorization", "Bearer " + cfg.romm.api_token);
    headers.emplace_back("Accept", "application/json");

    std::string base = cfg.romm.base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();

    const json* best = nullptr;
    json best_owned; // keep a copy — search responses are ephemeral per attempt
    int best_score = -1;
    std::string used_search;

    for (const auto& search : names) {
        if (search.empty()) continue;
        const std::string url =
            base + "/api/roms?limit=25&search_term=" + url_encode(search);
        auto res = http_get(url, headers);
        if (!res.ok()) {
            r.message = "RomM search failed: " +
                        (res.error.empty() ? ("HTTP " + std::to_string(res.status)) : res.error);
            // Keep trying other name candidates.
            continue;
        }

        json root;
        try {
            root = json::parse(res.body);
        } catch (const std::exception& e) {
            r.message = std::string("RomM JSON: ") + e.what();
            continue;
        }

        if (!root.contains("items") || !root.at("items").is_array() || root.at("items").empty())
            continue;

        for (const auto& item : root.at("items")) {
            if (!item.is_object()) continue;
            int score = 0;
            const std::string plat = item.value("platform_slug", "");
            for (const auto& rp : title.romm_platforms) {
                if (!rp.empty() && rp == plat) {
                    score += 50;
                    break;
                }
            }
            if (plat == title.platform) score += 20;
            if (title.platform == "psx" &&
                (plat == "ps" || plat == "ps1" || plat == "psx"))
                score += 40;

            const std::string fs_name = item.value("fs_name", "");
            const std::string name = item.value("name", "");
            const std::string fs_key = stem_key_from_filename(fs_name);
            const std::string name_key = normalize_key(name);
            for (const auto& cand : names) {
                const std::string ck = normalize_key(cand);
                if (keys_match(ck, fs_key)) score += 40;
                if (keys_match(ck, name_key)) score += 25;
            }
            // Prefer entries that have a RomM-hosted menu cover.
            const std::string cover = item.value("path_cover_large", "");
            const std::string cover_s = item.value("path_cover_small", "");
            if (!cover.empty() || !cover_s.empty())
                score += 30;
            else if (item.value("url_cover", "").empty())
                score -= 100;

            if (score > best_score) {
                best_score = score;
                best_owned = item;
                best = &best_owned;
                used_search = search;
            }
        }
        // Strong enough match with a menu cover — stop searching.
        if (best && best_score >= 70 &&
            (!best->value("path_cover_large", "").empty() ||
             !best->value("path_cover_small", "").empty()))
            break;
    }

    if (!best || best_score < 0) {
        r.message = "RomM: no cover match for \"" + names.front() + "\"";
        return r;
    }

    const std::string cover_url = pick_romm_cover_url(base, *best);
    if (cover_url.empty()) {
        r.message = "RomM: matched rom has no cover URL";
        return r;
    }

    AppConfig src_cfg = cfg;
    src_cfg.romm.sync_boxart = true;
    // Download to a staging file, then rename using sniffed image type — RomM's
    // path_cover_*.png is frequently JPEG under the hood.
    const fs::path cache = boxart_cache_dir(paths, src_cfg);
    const fs::path staged = cache / (title.id + ".download");
    std::string err;
    if (!download_image_url(cover_url, staged, headers, &err)) {
        r.message = "RomM download failed: " + err;
        return r;
    }

    std::string ext = sniff_image_ext(staged);
    if (ext.empty()) {
        const auto q = cover_url.find('?');
        const std::string path_part =
            q == std::string::npos ? cover_url : cover_url.substr(0, q);
        const auto dot = path_part.find_last_of('.');
        if (dot != std::string::npos) {
            std::string e = lower_copy(path_part.substr(dot));
            if (e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".webp") ext = e;
        }
    }
    if (ext == ".jpeg") ext = ".jpg";
    // stb_image here only loads png/jpeg.
    if (ext == ".webp" || ext.empty()) ext = ".jpg";

    const fs::path dest = cache / (title.id + ext);
    std::error_code ec;
    // Drop stale cache files for this title (other extensions from prior fetches).
    for (const char* old_ext : {".jpg", ".jpeg", ".png", ".webp", ".download"}) {
        const fs::path stale = cache / (title.id + old_ext);
        if (stale != dest && stale != staged) fs::remove(stale, ec);
    }
    fs::remove(dest, ec);
    fs::rename(staged, dest, ec);
    if (ec) {
        r.message = "RomM: failed to finalize cover cache";
        fs::remove(staged, ec);
        return r;
    }

    r.ok = true;
    r.path = dest;
    const bool used_menu = cover_url.find("/assets/romm/") != std::string::npos ||
                           cover_url.find("/cover/") != std::string::npos;
    r.message = std::string("RomM: ") + best->value("name", used_search) +
                (used_menu ? " (menu cover)" : " (remote cover)");
    return r;
}

fs::path resolve_local_boxart(const AppConfig& cfg, const Title& title, const fs::path& rom_path,
                              const std::string& suggested_rom) {
    fs::path found;
    if (!rom_path.empty() && try_sibling_images(rom_path, &found)) return found;

    std::vector<std::string> keys;
    if (!suggested_rom.empty()) keys.push_back(stem_key_from_filename(suggested_rom));
    if (!rom_path.empty()) keys.push_back(normalize_key(rom_path.stem().string()));
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

    const auto roots = cfg.platform_roots(title.platform);
    for (const auto& key : keys) {
        if (key.empty()) continue;
        for (const auto& root : roots) {
            if (scan_dir_for_key(root, key, &found, false)) return found;
        }
        for (const auto& root : roots) {
            if (scan_dir_for_key(root, key, &found, true)) return found;
        }
    }
    return {};
}

} // namespace

const char* active_boxart_source(const AppConfig& cfg) {
    return cfg.romm.sync_boxart ? "romm" : "libretro";
}

fs::path boxart_cache_dir(const Paths& paths, const AppConfig& cfg) {
    return paths.data_dir / "boxart" / active_boxart_source(cfg);
}

fs::path resolve_boxart_path(const AppConfig& cfg, const Title& title, const fs::path& rom_path,
                             const std::string& suggested_rom, const Paths& paths) {
    // Optional: sibling / library-folder covers only when explicitly preferred.
    if (cfg.prefer_local_boxart) {
        const fs::path local = resolve_local_boxart(cfg, title, rom_path, suggested_rom);
        if (!local.empty()) return local;
    }

    // Active remote source cache (RomM vs Libretro are separate directories).
    fs::path found = find_cached_boxart(boxart_cache_dir(paths, cfg), title.id);
    if (!found.empty()) return found;

    // Legacy flat cache from earlier builds — treat as Libretro only.
    if (!cfg.romm.sync_boxart) {
        found = find_cached_boxart(paths.data_dir / "boxart", title.id);
        if (!found.empty()) return found;
    }
    return {};
}

BoxartFetchResult ensure_remote_boxart(const Paths& paths, const AppConfig& cfg, const Title& title,
                                       const fs::path& rom_path,
                                       const std::string& suggested_rom, bool force) {
    BoxartFetchResult r;

    if (cfg.prefer_local_boxart) {
        const fs::path local = resolve_local_boxart(cfg, title, rom_path, suggested_rom);
        if (!local.empty()) {
            r.ok = true;
            r.path = local;
            r.source = "local";
            r.message = "already present";
            return r;
        }
    }

    const fs::path cache_dir = boxart_cache_dir(paths, cfg);
    if (force) {
        std::error_code ec;
        for (const char* ext : {".jpg", ".jpeg", ".png", ".webp", ".download"})
            fs::remove(cache_dir / (title.id + ext), ec);
    } else {
        const fs::path cached = find_cached_boxart(cache_dir, title.id);
        if (!cached.empty()) {
            r.ok = true;
            r.path = cached;
            r.source = "cache";
            r.message = "already present";
            return r;
        }
        if (!cfg.romm.sync_boxart) {
            const fs::path legacy = find_cached_boxart(paths.data_dir / "boxart", title.id);
            if (!legacy.empty()) {
                r.ok = true;
                r.path = legacy;
                r.source = "cache";
                r.message = "already present";
                return r;
            }
        }
    }

    if (cfg.romm.sync_boxart) return fetch_romm(paths, cfg, title, rom_path, suggested_rom);
    return fetch_libretro(paths, title, rom_path, suggested_rom);
}

void clear_boxart_cache(const Paths& paths, const AppConfig& cfg) {
    const fs::path dir = boxart_cache_dir(paths, cfg);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        fs::remove(it->path(), ec);
    }
}

BoxartCache::~BoxartCache() { destroy_all(); }

void BoxartCache::destroy_all() {
    for (auto& [_, tex] : by_title_) {
        if (tex.gl_id) glDeleteTextures(1, &tex.gl_id);
        tex.gl_id = 0;
    }
    by_title_.clear();
}

const BoxartTexture* BoxartCache::get(const std::string& title_id, const fs::path& image_path) {
    if (title_id.empty() || image_path.empty()) return nullptr;

    std::error_code ec;
    const auto sz = fs::file_size(image_path, ec);
    std::int64_t mtime_sec = 0;
    if (!ec) {
        const auto mtime = fs::last_write_time(image_path, ec);
        if (!ec) mtime_sec = static_cast<std::int64_t>(mtime.time_since_epoch().count());
    }
    const std::uint64_t size = ec ? 0ull : static_cast<std::uint64_t>(sz);

    auto it = by_title_.find(title_id);
    if (it != by_title_.end()) {
        if (it->second.path == image_path.string() && it->second.gl_id != 0 &&
            it->second.size == size && it->second.mtime_sec == mtime_sec)
            return &it->second;
        if (it->second.gl_id) glDeleteTextures(1, &it->second.gl_id);
        by_title_.erase(it);
    }

    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load(image_path.string().c_str(), &w, &h, &comp, 4);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        return nullptr;
    }

    BoxartTexture tex;
    tex.path = image_path.string();
    tex.width = w;
    tex.height = h;
    tex.size = size;
    tex.mtime_sec = mtime_sec;
    glGenTextures(1, &tex.gl_id);
    glBindTexture(GL_TEXTURE_2D, tex.gl_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(pixels);

    auto [ins, _] = by_title_.emplace(title_id, std::move(tex));
    return &ins->second;
}

} // namespace retcomm::hub
