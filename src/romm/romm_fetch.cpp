#include "retcomm/romm_fetch.hpp"

#include "retcomm/http.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
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

} // namespace

RommFetchResult fetch_rom_from_romm(const AppConfig& cfg, const Title& title,
                                    RommProgressFn on_progress) {
    if (!cfg.romm.enabled()) return fail("RomM not configured (set base_url)");
    if (cfg.romm.api_token.empty()) return fail("RomM api_token is empty");
    if (cfg.library_root.empty()) return fail("library_root is empty — set it in Library settings");
    if (!title.has_rom_identity()) return fail("catalog title has no rom_identity");

    const std::string base = normalize_base(cfg.romm.base_url);
    const auto headers = auth_headers(cfg.romm);

    std::string search = title.name;
    if (search.empty()) search = title.id;
    if (!title.rom_identity.filenames.empty()) {
        // Prefer filename stem for search when the display name is ambiguous.
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
    if (!http_get_json(search_url, headers, root, &err))
        return fail("RomM search failed: " + err);

    if (!root.contains("items") || !root.at("items").is_array() || root.at("items").empty())
        return fail("RomM: no ROM results for \"" + search + "\"");

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
        // Prefer list-level hash hits before fetching details.
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

        // ROM-level identity
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
                // Strong hash match: stop early.
                if (hit.matched_by == "sha1" || hit.matched_by == "md5" ||
                    hit.matched_by == "crc32")
                    break;
            }
        }

        // Per-file identity (multi-disc / cue+bin / folder dumps)
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

    // Require a real identity match — don't download on platform+name alone.
    if (best.rom_id <= 0 || best.file_name.empty() ||
        (best.matched_by != "sha1" && best.matched_by != "crc32" && best.matched_by != "size" &&
         best.matched_by != "filename")) {
        return fail("RomM: no file matching catalog rom_identity for \"" + title.name + "\"");
    }
    // Filename-only matches are soft — only accept when size also lined up earlier,
    // or when catalog has no hashes at all.
    if (best.matched_by == "filename") {
        const bool has_hashes =
            !title.rom_identity.sha1.empty() || !title.rom_identity.crc32.empty() ||
            !title.rom_identity.sha256.empty();
        if (has_hashes)
            return fail("RomM: found name match \"" + best.file_name +
                        "\" but hashes do not match catalog rom_identity");
    }

    const fs::path dest_dir =
        ensure_platform_dir(cfg.library_root, cfg.folders_for_platform(title.platform));
    if (dest_dir.empty())
        return fail("cannot create library folder under " + cfg.library_root.string());

    const fs::path dest = dest_dir / best.file_name;
    std::string url;
    if (best.file_id > 0) {
        url = base + "/api/roms/" + std::to_string(best.file_id) + "/files/content/" +
              url_encode_path_segment(best.file_name);
    } else {
        url = base + "/api/roms/" + std::to_string(best.rom_id) + "/content/" +
              url_encode_path_segment(best.file_name);
    }

    progress(on_progress, "RomM: downloading " + best.file_name + " (" + best.matched_by +
                              ")…");
    std::string dl_err;
    auto dl_headers = auth_headers(cfg.romm);
    // Prefer raw bytes over Accept: application/json for content endpoints.
    dl_headers.clear();
    if (!cfg.romm.api_token.empty())
        dl_headers.emplace_back("Authorization", "Bearer " + cfg.romm.api_token);

    int last_pct = -1;
    if (!http_download(url, dest, &dl_err, dl_headers,
                       [&](std::uint64_t got, std::uint64_t total) {
                           if (total == 0) {
                               // Coarse byte updates for unknown totals.
                               if (got == 0 || (got & ((1u << 20) - 1)) == 0) {
                                   progress(on_progress,
                                            "RomM: downloading " + best.file_name + "… " +
                                                std::to_string(got >> 20) + " MiB");
                               }
                               return;
                           }
                           const int pct = static_cast<int>((got * 100) / total);
                           if (pct == last_pct || (pct != 100 && pct % 5 != 0)) return;
                           last_pct = pct;
                           progress(on_progress, "RomM: downloading " + best.file_name + "… " +
                                                     std::to_string(pct) + "%");
                       })) {
        return fail("RomM download failed: " + dl_err);
    }

    std::error_code ec;
    const auto sz = fs::file_size(dest, ec);
    RommFetchResult r;
    r.ok = true;
    r.saved_path = dest;
    r.matched_by = best.matched_by;
    r.remote_name = best.file_name;
    r.bytes = ec ? best.size : static_cast<std::uint64_t>(sz);
    r.message = "Downloaded " + best.file_name + " via " + best.matched_by + " → " + dest.string();
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
