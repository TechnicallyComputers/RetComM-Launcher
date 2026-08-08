#include "retcomm/romm_fetch.hpp"

#include "retcomm/http.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <vector>

namespace retcomm {
namespace {

using nlohmann::json;

std::string lower_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
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

// Encode a single path segment (spaces as %20, keep extension dots).
std::string url_encode_path_segment(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

std::string normalize_base(std::string base) {
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base;
}

std::vector<std::pair<std::string, std::string>> auth_headers(const RommConfig& romm) {
    std::vector<std::pair<std::string, std::string>> h;
    if (!romm.api_token.empty())
        h.emplace_back("Authorization", "Bearer " + romm.api_token);
    h.emplace_back("Accept", "application/json");
    return h;
}

void progress(const RommProgressFn& fn, const std::string& s) {
    if (fn) fn(s);
}

bool contains_ci(const std::vector<std::string>& hay, const std::string& needle) {
    if (needle.empty()) return false;
    const std::string n = lower_copy(needle);
    for (const auto& h : hay) {
        if (lower_copy(h) == n) return true;
    }
    return false;
}

bool filename_hint_match(const std::vector<std::string>& hints, const std::string& remote) {
    if (hints.empty() || remote.empty()) return false;
    const std::string r = lower_copy(remote);
    for (const auto& h : hints) {
        const std::string want = lower_copy(h);
        if (want.empty()) continue;
        if (r == want) return true;
        // stem match (ignore extension)
        const auto rd = r.find_last_of('.');
        const auto wd = want.find_last_of('.');
        const std::string rs = rd == std::string::npos ? r : r.substr(0, rd);
        const std::string ws = wd == std::string::npos ? want : want.substr(0, wd);
        if (rs == ws) return true;
    }
    return false;
}

int platform_score(const Title& title, const std::string& platform_slug) {
    int score = 0;
    for (const auto& rp : title.romm_platforms) {
        if (!rp.empty() && rp == platform_slug) {
            score += 50;
            break;
        }
    }
    if (platform_slug == title.platform) score += 20;
    // Common aliases
    if (title.platform == "psx" &&
        (platform_slug == "ps" || platform_slug == "ps1" || platform_slug == "psx"))
        score += 40;
    return score;
}

struct IdentityHit {
    int score = 0;
    std::string matched_by;
};

IdentityHit score_hashes(const RomIdentity& id, const std::string& sha1, const std::string& crc,
                         std::uint64_t size, const std::string& filename,
                         const std::string& md5 = {}) {
    IdentityHit hit;
    if (!id.sha1.empty() && !sha1.empty() && contains_ci(id.sha1, sha1)) {
        hit.score = 1000;
        hit.matched_by = "sha1";
        return hit;
    }
    if (!id.md5.empty() && !md5.empty() && contains_ci(id.md5, md5)) {
        hit.score = 950;
        hit.matched_by = "md5";
        return hit;
    }
    if (!id.crc32.empty() && !crc.empty() && contains_ci(id.crc32, crc)) {
        hit.score = 900;
        hit.matched_by = "crc32";
        return hit;
    }
    if (!id.sizes.empty() && size > 0) {
        for (auto s : id.sizes) {
            if (s == size) {
                hit.score = 400;
                hit.matched_by = "size";
                break;
            }
        }
    }
    if (filename_hint_match(id.filenames, filename)) {
        if (hit.score < 200) {
            hit.score = 200;
            hit.matched_by = "filename";
        } else if (hit.matched_by == "size") {
            hit.score += 50; // size + name
        }
    }
    return hit;
}

IdentityHit score_bios(const BiosIdentity& id, const std::string& sha1, const std::string& crc,
                       std::uint64_t size, const std::string& filename,
                       const std::string& md5 = {}) {
    RomIdentity proxy;
    proxy.sha1 = id.sha1;
    proxy.md5 = id.md5;
    proxy.crc32 = id.crc32;
    proxy.sha256 = id.sha256;
    proxy.sizes = id.sizes;
    proxy.filenames = id.filenames;
    return score_hashes(proxy, sha1, crc, size, filename, md5);
}

fs::path ensure_platform_dir(const fs::path& root, const std::vector<std::string>& folders) {
    if (root.empty()) return {};
    std::error_code ec;
    // Prefer an existing folder; otherwise create the first configured name.
    for (const auto& folder : folders) {
        if (folder.empty()) continue;
        fs::path p = root / folder;
        if (fs::is_directory(p, ec)) return p;
    }
    const std::string folder = folders.empty() ? std::string{} : folders.front();
    fs::path p = folder.empty() ? root : (root / folder);
    fs::create_directories(p, ec);
    if (ec) return {};
    return p;
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

struct RomCandidate {
    int rom_id = 0;
    int file_id = 0; // when >0, download via /files/content
    std::string file_name;
    std::string matched_by;
    int score = 0;
    std::uint64_t size = 0;
};

RommFetchResult fail(const std::string& msg) {
    RommFetchResult r;
    r.message = msg;
    return r;
}

std::string iso_utc_now() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

RommRomMatch match_rom_on_romm_impl(const AppConfig& cfg, const Title& title,
                                    RommProgressFn on_progress) {
    RommRomMatch out;
    if (!cfg.romm.enabled()) {
        out.message = "RomM not configured (set base_url)";
        return out;
    }
    if (cfg.romm.api_token.empty()) {
        out.message = "RomM api_token is empty";
        return out;
    }
    if (!title.has_rom_identity()) {
        out.message = "catalog title has no rom_identity";
        return out;
    }

    const std::string base = normalize_base(cfg.romm.base_url);
    const auto headers = auth_headers(cfg.romm);

    std::string search = title.name;
    if (search.empty()) search = title.id;
    if (!title.rom_identity.filenames.empty()) {
        const auto& fn = title.rom_identity.filenames.front();
        const auto dot = fn.find_last_of('.');
        const std::string stem = dot == std::string::npos ? fn : fn.substr(0, dot);
        if (!stem.empty()) search = stem;
    }

    progress(on_progress, "RomM: searching for \"" + search + "\"…");
    const std::string search_url =
        base + "/api/roms?limit=40&search_term=" + url_encode(search);
    json root;
    std::string err;
    if (!http_get_json(search_url, headers, root, &err)) {
        out.message = "RomM search failed: " + err;
        return out;
    }

    if (!root.contains("items") || !root.at("items").is_array() || root.at("items").empty()) {
        out.message = "RomM: no ROM results for \"" + search + "\"";
        return out;
    }

    struct Ranked {
        int id = 0;
        int pre = 0;
        std::string name;
    };
    std::vector<Ranked> ranked;
    for (const auto& item : root.at("items")) {
        if (!item.is_object()) continue;
        Ranked r;
        r.id = item.value("id", 0);
        if (r.id <= 0) continue;
        r.name = item.value("fs_name", item.value("name", ""));
        r.pre = platform_score(title, item.value("platform_slug", ""));
        const auto list_hit =
            score_hashes(title.rom_identity, item.value("sha1_hash", ""),
                         item.value("crc_hash", ""), item.value("fs_size_bytes", 0ull),
                         item.value("fs_name", ""), item.value("md5_hash", ""));
        r.pre += list_hit.score;
        ranked.push_back(r);
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const Ranked& a, const Ranked& b) { return a.pre > b.pre; });

    RomCandidate best;
    const size_t detail_limit = std::min<size_t>(ranked.size(), 12);
    for (size_t i = 0; i < detail_limit; ++i) {
        const auto& r = ranked[i];
        progress(on_progress, "RomM: checking " + r.name + "…");
        json detail;
        if (!http_get_json(base + "/api/roms/" + std::to_string(r.id), headers, detail, &err))
            continue;

        const std::string plat = detail.value("platform_slug", "");
        const int plat_bonus = platform_score(title, plat);

        {
            auto hit = score_hashes(title.rom_identity, detail.value("sha1_hash", ""),
                                    detail.value("crc_hash", ""),
                                    detail.value("fs_size_bytes", 0ull),
                                    detail.value("fs_name", ""), detail.value("md5_hash", ""));
            hit.score += plat_bonus;
            if (hit.score > best.score && hit.score >= 200) {
                best.score = hit.score;
                best.matched_by = hit.matched_by;
                best.rom_id = r.id;
                best.file_id = 0;
                best.file_name = detail.value("fs_name", "");
                best.size = detail.value("fs_size_bytes", 0ull);
                if (hit.matched_by == "sha1" || hit.matched_by == "md5" ||
                    hit.matched_by == "crc32")
                    break;
            }
        }

        if (detail.contains("files") && detail.at("files").is_array()) {
            for (const auto& f : detail.at("files")) {
                if (!f.is_object()) continue;
                auto hit = score_hashes(title.rom_identity, f.value("sha1_hash", ""),
                                        f.value("crc_hash", ""),
                                        f.value("file_size_bytes", 0ull),
                                        f.value("file_name", ""), f.value("md5_hash", ""));
                hit.score += plat_bonus;
                if (hit.score > best.score && hit.score >= 200) {
                    best.score = hit.score;
                    best.matched_by = hit.matched_by;
                    best.rom_id = r.id;
                    best.file_id = f.value("id", 0);
                    best.file_name = f.value("file_name", "");
                    best.size = f.value("file_size_bytes", 0ull);
                }
            }
            if (best.matched_by == "sha1" || best.matched_by == "crc32") break;
        }
    }

    if (best.rom_id <= 0 || best.file_name.empty() ||
        (best.matched_by != "sha1" && best.matched_by != "crc32" && best.matched_by != "size" &&
         best.matched_by != "filename" && best.matched_by != "md5")) {
        out.message = "RomM: no file matching catalog rom_identity for \"" + title.name + "\"";
        return out;
    }
    if (best.matched_by == "filename") {
        const bool has_hashes =
            !title.rom_identity.sha1.empty() || !title.rom_identity.crc32.empty() ||
            !title.rom_identity.sha256.empty() || !title.rom_identity.md5.empty();
        if (has_hashes) {
            out.message = "RomM: found name match \"" + best.file_name +
                          "\" but hashes do not match catalog rom_identity";
            return out;
        }
    }

    out.available = true;
    out.rom_id = best.rom_id;
    out.file_id = best.file_id;
    out.file_name = best.file_name;
    out.matched_by = best.matched_by;
    out.message = "matched " + best.file_name + " via " + best.matched_by;
    return out;
}

} // namespace

RommRomMatch match_rom_on_romm(const AppConfig& cfg, const Title& title,
                               RommProgressFn on_progress) {
    return match_rom_on_romm_impl(cfg, title, on_progress);
}

RommRomIndex load_romm_rom_index(const fs::path& path) {
    RommRomIndex idx;
    std::ifstream in(path);
    if (!in) return idx;
    try {
        json j;
        in >> j;
        idx.schema_version = j.value("schema_version", 1);
        if (!j.contains("titles") || !j.at("titles").is_object()) return idx;
        for (auto it = j.at("titles").begin(); it != j.at("titles").end(); ++it) {
            if (!it.value().is_object()) continue;
            RommRomIndexEntry e;
            e.available = it.value().value("available", false);
            e.rom_id = it.value().value("rom_id", 0);
            e.file_id = it.value().value("file_id", 0);
            e.file_name = it.value().value("file_name", "");
            e.matched_by = it.value().value("matched_by", "");
            e.checked_at = it.value().value("checked_at", "");
            idx.by_title[it.key()] = std::move(e);
        }
    } catch (...) {
        return RommRomIndex{};
    }
    return idx;
}

bool save_romm_rom_index(const fs::path& path, const RommRomIndex& index, std::string* error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    json titles = json::object();
    for (const auto& [id, e] : index.by_title) {
        titles[id] = {{"available", e.available},
                      {"rom_id", e.rom_id},
                      {"file_id", e.file_id},
                      {"file_name", e.file_name},
                      {"matched_by", e.matched_by},
                      {"checked_at", e.checked_at}};
    }
    json j = {{"schema_version", index.schema_version}, {"titles", titles}};
    std::ofstream out(path);
    if (!out) {
        if (error) *error = "cannot write " + path.string();
        return false;
    }
    out << j.dump(2) << "\n";
    return static_cast<bool>(out);
}

RommRomScanResult scan_romm_rom_index(const Paths& paths, const AppConfig& cfg,
                                      const Catalog& catalog, RommProgressFn on_progress) {
    RommRomScanResult result;
    if (!cfg.romm.enabled()) {
        result.message = "RomM not configured (set base_url)";
        return result;
    }
    if (cfg.romm.api_token.empty()) {
        result.message = "RomM api_token is empty";
        return result;
    }

    RommRomIndex idx;
    const std::string checked = iso_utc_now();
    for (const auto& t : catalog.titles) {
        if (!t.has_rom_identity()) {
            ++result.skipped;
            continue;
        }
        progress(on_progress, "RomM scan: " + t.name + "…");
        auto match = match_rom_on_romm_impl(cfg, t, on_progress);
        RommRomIndexEntry e;
        e.checked_at = checked;
        if (match.available) {
            e.available = true;
            e.rom_id = match.rom_id;
            e.file_id = match.file_id;
            e.file_name = match.file_name;
            e.matched_by = match.matched_by;
            ++result.matched;
        } else {
            e.available = false;
            if (match.message.find("search failed") != std::string::npos ||
                match.message.find("api_token") != std::string::npos) {
                ++result.errors;
                result.message = match.message;
                // Abort on hard API failures so we don't wipe the index.
                return result;
            }
            ++result.missing;
        }
        idx.by_title[t.id] = std::move(e);
    }

    std::string err;
    if (!save_romm_rom_index(paths.romm_rom_index_path, idx, &err)) {
        result.message = err;
        return result;
    }
    result.ok = true;
    std::ostringstream oss;
    oss << "RomM library scan: " << result.matched << " available, " << result.missing
        << " missing, " << result.skipped << " skipped (no rom_identity)";
    result.message = oss.str();
    return result;
}

std::string basename_only(std::string name) {
    while (!name.empty() && (name.back() == '/' || name.back() == '\\')) name.pop_back();
    const auto slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    return name;
}

bool ends_with_ci_str(const std::string& s, const char* suf) {
    const std::string lower = lower_copy(s);
    const std::string suffix = lower_copy(suf);
    return lower.size() >= suffix.size() &&
           lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
}

struct RommRemoteFile {
    int id = 0;
    std::string name;
    std::uint64_t size = 0;
};

bool download_romm_file(const std::string& base, int rom_id, const RommRemoteFile& file,
                        const fs::path& dest,
                        const std::vector<std::pair<std::string, std::string>>& headers,
                        const RommProgressFn& on_progress, std::string* error) {
    const std::string enc = url_encode_path_segment(file.name);
    std::vector<std::string> urls;
    if (file.id > 0) {
        urls.push_back(base + "/api/roms/" + std::to_string(rom_id) + "/content/" + enc +
                       "?file_ids=" + std::to_string(file.id));
    }
    urls.push_back(base + "/api/roms/" + std::to_string(rom_id) + "/content/" + enc);
    if (file.id > 0) {
        // Legacy path that some RomM builds still expose for multi-file parts.
        urls.push_back(base + "/api/roms/" + std::to_string(file.id) + "/files/content/" + enc);
    }

    std::string last_err;
    for (const auto& url : urls) {
        progress(on_progress, "RomM: downloading " + file.name + "…");
        int last_pct = -1;
        std::string dl_err;
        if (http_download(url, dest, &dl_err, headers,
                          [&](std::uint64_t got, std::uint64_t total) {
                              if (total == 0) {
                                  if (got == 0 || (got & ((1u << 20) - 1)) == 0) {
                                      progress(on_progress,
                                               "RomM: downloading " + file.name + "… " +
                                                   std::to_string(got >> 20) + " MiB");
                                  }
                                  return;
                              }
                              const int pct = static_cast<int>((got * 100) / total);
                              if (pct == last_pct || (pct != 100 && pct % 5 != 0)) return;
                              last_pct = pct;
                              progress(on_progress, "RomM: downloading " + file.name + "… " +
                                                        std::to_string(pct) + "%");
                          })) {
            return true;
        }
        last_err = dl_err;
        std::error_code ec;
        fs::remove(dest, ec);
    }
    if (error) *error = last_err.empty() ? "download failed" : last_err;
    return false;
}

RommFetchResult fetch_rom_from_romm(const AppConfig& cfg, const Title& title,
                                    RommProgressFn on_progress) {
    if (cfg.library_root.empty()) return fail("library_root is empty — set it in Library settings");

    const RommRomMatch match = match_rom_on_romm_impl(cfg, title, on_progress);
    if (!match.available) return fail(match.message.empty() ? "RomM: no match" : match.message);

    const std::string base = normalize_base(cfg.romm.base_url);
    const fs::path plat_dir =
        ensure_platform_dir(cfg.library_root, cfg.folders_for_platform(title.platform));
    if (plat_dir.empty())
        return fail("cannot create library folder under " + cfg.library_root.string());

    std::vector<std::pair<std::string, std::string>> dl_headers;
    if (!cfg.romm.api_token.empty())
        dl_headers.emplace_back("Authorization", "Bearer " + cfg.romm.api_token);
    const auto headers = auth_headers(cfg.romm);

    // Refresh ROM detail so multi-file sets (Redump .cue + tracks) download fully.
    json detail;
    std::string err;
    progress(on_progress, "RomM: loading file list…");
    if (!http_get_json(base + "/api/roms/" + std::to_string(match.rom_id), headers, detail, &err)) {
        return fail("RomM detail failed: " + err);
    }

    std::vector<RommRemoteFile> files;
    if (detail.contains("files") && detail.at("files").is_array()) {
        for (const auto& f : detail.at("files")) {
            if (!f.is_object()) continue;
            RommRemoteFile rf;
            rf.id = f.value("id", 0);
            rf.name = basename_only(f.value("file_name", ""));
            rf.size = f.value("file_size_bytes", 0ull);
            if (!rf.name.empty()) files.push_back(std::move(rf));
        }
    }
    if (files.empty()) {
        RommRemoteFile rf;
        rf.id = match.file_id;
        rf.name = basename_only(match.file_name);
        rf.size = 0;
        if (rf.name.empty()) rf.name = basename_only(detail.value("fs_name", ""));
        if (!rf.name.empty()) files.push_back(std::move(rf));
    }
    if (files.empty()) return fail("RomM: matched ROM has no downloadable files");

    std::string set_name = basename_only(detail.value("fs_name", ""));
    if (set_name.empty()) set_name = basename_only(match.file_name);
    // Folder dumps often use the set name without an extension.
    if (ends_with_ci_str(set_name, ".cue") || ends_with_ci_str(set_name, ".bin") ||
        ends_with_ci_str(set_name, ".iso") || ends_with_ci_str(set_name, ".chd")) {
        const auto dot = set_name.find_last_of('.');
        if (dot != std::string::npos) set_name = set_name.substr(0, dot);
    }

    fs::path dest_dir = plat_dir;
    if (files.size() >= 2 && !set_name.empty()) {
        dest_dir = plat_dir / set_name;
        std::error_code ec;
        fs::create_directories(dest_dir, ec);
        if (ec) return fail("cannot create " + dest_dir.string() + ": " + ec.message());
    }

    progress(on_progress, "RomM: downloading " + std::to_string(files.size()) + " file(s) (" +
                              match.matched_by + ")…");

    std::uint64_t total_bytes = 0;
    fs::path cue_path;
    fs::path first_path;
    int saved = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        const auto& f = files[i];
        const fs::path dest = dest_dir / f.name;
        progress(on_progress, "RomM: file " + std::to_string(i + 1) + "/" +
                                  std::to_string(files.size()) + " — " + f.name);
        std::string dl_err;
        if (!download_romm_file(base, match.rom_id, f, dest, dl_headers, on_progress, &dl_err)) {
            return fail("RomM download failed (" + f.name + "): " + dl_err);
        }
        std::error_code ec;
        total_bytes += ec ? 0 : static_cast<std::uint64_t>(fs::file_size(dest, ec));
        if (first_path.empty()) first_path = dest;
        if (ends_with_ci_str(f.name, ".cue")) cue_path = dest;
        ++saved;
    }

    RommFetchResult r;
    r.ok = true;
    r.saved_path = !cue_path.empty() ? cue_path : first_path;
    r.matched_by = match.matched_by;
    r.remote_name = !set_name.empty() ? set_name : match.file_name;
    r.bytes = total_bytes;
    r.files_saved = saved;
    std::ostringstream oss;
    oss << "Downloaded " << saved << " file(s) via " << match.matched_by << " → "
        << (!cue_path.empty() ? cue_path.string() : dest_dir.string());
    if (title.rom_identity.require_cue && cue_path.empty()) {
        oss << " (warning: no .cue in RomM set — library match may fail)";
    }
    r.message = oss.str();
    return r;
}

RommFetchResult fetch_bios_from_romm(const AppConfig& cfg, const Title& title,
                                     RommProgressFn on_progress) {
    if (!cfg.romm.enabled()) return fail("RomM not configured (set base_url)");
    if (cfg.romm.api_token.empty()) return fail("RomM api_token is empty");
    if (cfg.bios_root.empty()) return fail("bios_root is empty — set it in Library settings");
    if (!title.has_bios_identity()) return fail("catalog title has no bios_identity");

    const std::string base = normalize_base(cfg.romm.base_url);
    const auto headers = auth_headers(cfg.romm);

    progress(on_progress, "RomM: listing firmware…");
    json root;
    std::string err;
    if (!http_get_json(base + "/api/firmware", headers, root, &err))
        return fail("RomM firmware list failed: " + err);

    if (!root.is_array() || root.empty()) return fail("RomM: firmware list empty");

    int best_score = 0;
    json best;
    for (const auto& item : root) {
        if (!item.is_object()) continue;
        auto hit = score_bios(title.bios_identity, item.value("sha1_hash", ""),
                              item.value("crc_hash", ""), item.value("file_size_bytes", 0ull),
                              item.value("file_name", ""), item.value("md5_hash", ""));
        if (hit.score > best_score) {
            best_score = hit.score;
            best = item;
            best["_matched_by"] = hit.matched_by;
        }
    }

    if (best_score < 200 || best.empty())
        return fail("RomM: no firmware matching bios_identity for \"" + title.name + "\"");

    const std::string matched_by = best.value("_matched_by", "");
    if (matched_by == "filename") {
        const bool has_hashes =
            !title.bios_identity.sha1.empty() || !title.bios_identity.crc32.empty();
        if (has_hashes)
            return fail("RomM: found name match \"" + best.value("file_name", "") +
                        "\" but hashes do not match catalog bios_identity");
    }

    const int id = best.value("id", 0);
    const std::string file_name = best.value("file_name", "");
    if (id <= 0 || file_name.empty()) return fail("RomM: matched firmware missing id/name");

    const fs::path dest_dir =
        ensure_platform_dir(cfg.bios_root, cfg.folders_for_platform(title.platform));
    if (dest_dir.empty())
        return fail("cannot create BIOS folder under " + cfg.bios_root.string());

    // Prefer catalog basename hint when present (e.g. SCPH1001.BIN vs SCPH-5501).
    std::string local_name = file_name;
    if (!title.bios_identity.filenames.empty()) {
        // Keep remote name if it's already one of the hints; else use first hint
        // only when extensions match.
        if (!filename_hint_match(title.bios_identity.filenames, file_name)) {
            const auto& hint = title.bios_identity.filenames.front();
            const auto re = fs::path(file_name).extension().string();
            const auto he = fs::path(hint).extension().string();
            if (lower_copy(re) == lower_copy(he) || he.empty()) local_name = hint;
        }
    }

    const fs::path dest = dest_dir / local_name;
    const std::string url = base + "/api/firmware/" + std::to_string(id) + "/content/" +
                            url_encode_path_segment(file_name);

    progress(on_progress, "RomM: downloading BIOS " + file_name + " (" + matched_by + ")…");
    std::vector<std::pair<std::string, std::string>> dl_headers;
    if (!cfg.romm.api_token.empty())
        dl_headers.emplace_back("Authorization", "Bearer " + cfg.romm.api_token);

    std::string dl_err;
    if (!http_download(url, dest, &dl_err, dl_headers))
        return fail("RomM BIOS download failed: " + dl_err);

    std::error_code ec;
    const auto sz = fs::file_size(dest, ec);
    RommFetchResult r;
    r.ok = true;
    r.saved_path = dest;
    r.matched_by = matched_by;
    r.remote_name = file_name;
    r.bytes = ec ? best.value("file_size_bytes", 0ull) : static_cast<std::uint64_t>(sz);
    r.message = "Downloaded BIOS " + local_name + " via " + matched_by + " → " + dest.string();
    return r;
}

} // namespace retcomm
