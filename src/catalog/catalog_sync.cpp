#include "retcomm/catalog_sync.hpp"

#include "retcomm/catalog.hpp"
#include "retcomm/http.hpp"
#include "retcomm/install.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>

namespace retcomm {
namespace {

using json = nlohmann::json;

#if !defined(RETCOMM_CATALOG_DOWNLOAD_URL)
#define RETCOMM_CATALOG_DOWNLOAD_URL \
    "https://github.com/TechnicallyComputers/retcomm-catalog/releases/latest/download/catalog.zip"
#endif
#if !defined(RETCOMM_CATALOG_GITHUB_REPO)
#define RETCOMM_CATALOG_GITHUB_REPO "TechnicallyComputers/retcomm-catalog"
#endif

struct RemoteCatalogRelease {
    std::string tag;
    std::string published_at; // GitHub ISO timestamp
    std::string catalog_date; // YYYY-MM-DD or YYYY-MM-DDTHH:MM:SSZ
};

std::int64_t epoch_seconds_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string iso_timestamp_from_epoch(std::int64_t epoch) {
    const std::time_t t = static_cast<std::time_t>(epoch);
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

bool is_digit_str(const std::string& s, size_t pos, size_t len) {
    if (pos + len > s.size()) return false;
    for (size_t i = 0; i < len; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[pos + i]))) return false;
    }
    return true;
}

// Parse catalog_date / published_at / tag-derived stamps into UTC epoch seconds.
// Accepts YYYY-MM-DD (start of day UTC) and ISO-8601 with optional fractional seconds.
std::optional<std::int64_t> parse_catalog_timestamp(const std::string& raw) {
    if (raw.empty()) return std::nullopt;
    std::string s = raw;
    // Trim
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    if (s.size() < 10 || s[4] != '-' || s[7] != '-') return std::nullopt;

    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    try {
        year = std::stoi(s.substr(0, 4));
        month = std::stoi(s.substr(5, 2));
        day = std::stoi(s.substr(8, 2));
    } catch (...) {
        return std::nullopt;
    }

    if (s.size() == 10) {
        // Date-only → start of UTC day.
    } else if (s.size() >= 19 && (s[10] == 'T' || s[10] == ' ')) {
        if (s[13] != ':' || s[16] != ':') return std::nullopt;
        try {
            hour = std::stoi(s.substr(11, 2));
            minute = std::stoi(s.substr(14, 2));
            second = std::stoi(s.substr(17, 2));
        } catch (...) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }

    if (month < 1 || month > 12 || day < 1 || day > 31) return std::nullopt;
    if (hour > 23 || minute > 59 || second > 60) return std::nullopt;

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
#if defined(_WIN32)
    const std::time_t t = _mkgmtime(&tm);
#else
    const std::time_t t = timegm(&tm);
#endif
    if (t == static_cast<std::time_t>(-1)) return std::nullopt;
    return static_cast<std::int64_t>(t);
}

// Prefer dated tags with optional time:
//   vYYYY.MM.DD           → YYYY-MM-DD
//   vYYYY.MM.DD.HHMM      → YYYY-MM-DDTHH:MM:00Z
//   vYYYY.MM.DD.HHMMSS…   → YYYY-MM-DDTHH:MM:SSZ
// Legacy vYYYY.MM.DD.<issue> (1–3 digit suffix) stays date-only.
std::string catalog_stamp_from_tag(const std::string& tag) {
    std::string t = tag;
    if (!t.empty() && (t[0] == 'v' || t[0] == 'V')) t = t.substr(1);
    // YYYY.MM.DD
    if (t.size() < 10 || !is_digit_str(t, 0, 4) || t[4] != '.' || !is_digit_str(t, 5, 2) ||
        t[7] != '.' || !is_digit_str(t, 8, 2)) {
        return {};
    }
    const std::string date =
        t.substr(0, 4) + "-" + t.substr(5, 2) + "-" + t.substr(8, 2);
    if (t.size() == 10) return date;
    if (t[10] != '.') return date;

    size_t end = 11;
    while (end < t.size() && std::isdigit(static_cast<unsigned char>(t[end]))) ++end;
    const size_t digits = end - 11;
    if (digits == 4) {
        // HHMM
        return date + "T" + t.substr(11, 2) + ":" + t.substr(13, 2) + ":00Z";
    }
    if (digits == 6) {
        // HHMMSS (.issue / .manual… may follow)
        return date + "T" + t.substr(11, 2) + ":" + t.substr(13, 2) + ":" + t.substr(15, 2) +
               "Z";
    }
    // Short numeric suffix → legacy issue number, date only.
    return date;
}

std::string catalog_date_from_tag_or_published(const std::string& tag,
                                               const std::string& published_at) {
    const std::string from_tag = catalog_stamp_from_tag(tag);
    if (!from_tag.empty()) return from_tag;
    if (parse_catalog_timestamp(published_at)) {
        // Normalize published_at to second precision when possible.
        if (published_at.size() >= 20 && published_at.back() == 'Z')
            return published_at.substr(0, 19) + "Z";
        if (published_at.size() >= 19) return published_at.substr(0, 19) + "Z";
        return published_at.substr(0, 10);
    }
    return {};
}

// Best-effort age for a release / local state (higher = newer).
std::optional<std::int64_t> release_age_epoch(const std::string& catalog_date,
                                              const std::string& published_at,
                                              const std::string& tag) {
    if (auto e = parse_catalog_timestamp(catalog_date)) return e;
    if (auto e = parse_catalog_timestamp(published_at)) return e;
    return parse_catalog_timestamp(catalog_stamp_from_tag(tag));
}

fs::path catalog_state_path(const Paths& paths) { return paths.data_dir / "catalog-state.json"; }

std::string effective_catalog_url(const AppConfig& cfg) {
    if (!cfg.catalog.url.empty()) return cfg.catalog.url;
    return default_catalog_download_url();
}

std::string effective_catalog_repo(const AppConfig& cfg) {
    if (!cfg.catalog.github_repo.empty()) return cfg.catalog.github_repo;
    return default_catalog_github_repo();
}

bool read_catalog_state(const Paths& paths, json* out) {
    if (!out) return false;
    std::ifstream in(catalog_state_path(paths));
    if (!in) return false;
    try {
        in >> *out;
        return out->is_object();
    } catch (...) {
        return false;
    }
}

bool write_catalog_state(const Paths& paths, const json& state, std::string* error) {
    const fs::path path = catalog_state_path(paths);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        if (error) *error = "cannot create data dir: " + ec.message();
        return false;
    }
    std::ofstream out(path);
    if (!out) {
        if (error) *error = "cannot write " + path.string();
        return false;
    }
    out << state.dump(2) << "\n";
    if (!out) {
        if (error) *error = "failed writing " + path.string();
        return false;
    }
    return true;
}

bool fetch_latest_catalog_release(const AppConfig& cfg, RemoteCatalogRelease* out,
                                  std::string* error) {
    if (!out) return false;
    // Prefer github.com /releases/latest redirect (no api.github.com quota).
    // Asset download still uses …/releases/latest/download/catalog.zip.
    const std::string repo = effective_catalog_repo(cfg);
    std::string err;
    out->tag = github_latest_release_tag_web(repo, &err);
    out->published_at.clear();
    if (out->tag.empty()) {
        if (error) *error = err.empty() ? ("could not resolve latest tag for " + repo) : err;
        return false;
    }
    out->catalog_date = catalog_date_from_tag_or_published(out->tag, out->published_at);
    return true;
}

// Decide whether the local cache already satisfies the remote latest release.
// - Same release_tag → up to date.
// - Remote age <= local age (by catalog_date / published_at / tag time) → keep local
//   (avoid replacing with an older stamp if /latest is surprising).
// - Otherwise → download.
enum class CatalogFreshness { UpToDate, NeedsDownload, RemoteNotNewer };

CatalogFreshness compare_local_to_remote(const Paths& paths,
                                         const RemoteCatalogRelease& remote) {
    json state;
    if (!read_catalog_state(paths, &state)) return CatalogFreshness::NeedsDownload;

    const std::string local_tag = state.value("release_tag", "");
    if (!local_tag.empty() && local_tag == remote.tag) return CatalogFreshness::UpToDate;

    const auto remote_age =
        release_age_epoch(remote.catalog_date, remote.published_at, remote.tag);
    const auto local_age =
        release_age_epoch(state.value("catalog_date", ""),
                          state.value("release_published_at", ""), local_tag);

    if (remote_age && local_age) {
        if (*remote_age < *local_age) return CatalogFreshness::RemoteNotNewer;
        if (*remote_age == *local_age && !local_tag.empty() && local_tag == remote.tag)
            return CatalogFreshness::UpToDate;
        // Same calendar instant but different tags (e.g. two publishes in one second
        // with different issue suffixes) → still fetch when tags differ.
        if (*remote_age == *local_age && !local_tag.empty() && !remote.tag.empty() &&
            local_tag != remote.tag)
            return CatalogFreshness::NeedsDownload;
        if (*remote_age == *local_age) return CatalogFreshness::UpToDate;
        return CatalogFreshness::NeedsDownload; // remote newer
    }

    // No comparable timestamps — trust tag identity only.
    if (!local_tag.empty() && local_tag == remote.tag) return CatalogFreshness::UpToDate;
    return CatalogFreshness::NeedsDownload;
}

fs::path find_catalog_root(const fs::path& staging) {
    std::error_code ec;
    const fs::path direct = staging / "index.json";
    if (fs::is_regular_file(direct, ec)) return staging;

    const fs::path nested = staging / "catalog" / "index.json";
    if (fs::is_regular_file(nested, ec)) return staging / "catalog";

    for (auto it = fs::recursive_directory_iterator(staging, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().filename() == "index.json") {
            return it->path().parent_path();
        }
    }
    return {};
}

CatalogSyncResult fail(CatalogSyncResult result, std::string message) {
    result.ok = false;
    result.message = std::move(message);
    return result;
}

CatalogSyncResult skip_ok(std::string message, const json* state = nullptr) {
    CatalogSyncResult result;
    result.ok = true;
    result.skipped = true;
    result.message = std::move(message);
    if (state) {
        result.synced_at = state->value("synced_at", "");
        result.catalog_date = state->value("catalog_date", "");
        result.release_tag = state->value("release_tag", "");
    }
    return result;
}

} // namespace

std::string default_catalog_download_url() { return RETCOMM_CATALOG_DOWNLOAD_URL; }

std::string default_catalog_github_repo() { return RETCOMM_CATALOG_GITHUB_REPO; }

fs::path catalog_cache_dir(const Paths& paths) { return paths.catalog_dir; }

bool catalog_cache_valid(const Paths& paths) {
    std::error_code ec;
    return fs::is_regular_file(paths.catalog_dir / "index.json", ec);
}

CatalogSyncResult sync_remote_catalog(const Paths& paths, const AppConfig& cfg, bool force) {
    CatalogSyncResult result;
    const std::string url = effective_catalog_url(cfg);

    RemoteCatalogRelease remote;
    std::string remote_err;
    const bool have_remote = fetch_latest_catalog_release(cfg, &remote, &remote_err);

    if (!force && catalog_cache_valid(paths)) {
        if (have_remote) {
            json state;
            read_catalog_state(paths, &state);
            switch (compare_local_to_remote(paths, remote)) {
            case CatalogFreshness::UpToDate:
                return skip_ok("Catalog up to date (" + remote.tag +
                                   (remote.catalog_date.empty()
                                        ? ""
                                        : ", " + remote.catalog_date) +
                                   ").",
                               &state);
            case CatalogFreshness::RemoteNotNewer:
                return skip_ok("Local catalog is newer than GitHub latest (" +
                                   state.value("release_tag", std::string("?")) +
                                   " vs " + remote.tag + "); keeping local cache.",
                               &state);
            case CatalogFreshness::NeedsDownload:
                break;
            }
        } else {
            // Cannot verify freshness — keep the local cache instead of re-downloading.
            json state;
            read_catalog_state(paths, &state);
            return skip_ok("Catalog update check failed (" + remote_err +
                               "); keeping local cache.",
                           &state);
        }
    }

    ensure_dirs(paths);

    const fs::path work = paths.data_dir / "catalog-sync";
    const fs::path download = work / "catalog.zip";
    const fs::path staging = work / "staging";
    std::error_code ec;
    fs::remove_all(work, ec);
    ec.clear();
    fs::create_directories(download.parent_path(), ec);
    fs::create_directories(staging, ec);

    std::string err;
    if (!http_download(url, download, &err, github_http_headers())) {
        return fail(result, "catalog download failed: " + err);
    }

    if (!extract_archive_to(download, staging, &err)) {
        return fail(result, "catalog extract failed: " + err);
    }

    const fs::path catalog_root = find_catalog_root(staging);
    if (catalog_root.empty()) {
        return fail(result, "catalog archive missing index.json");
    }

    Catalog loaded;
    try {
        loaded = load_catalog(catalog_root);
    } catch (const std::exception& e) {
        return fail(result, std::string("catalog validation failed: ") + e.what());
    }

    const fs::path cache = paths.catalog_dir;
    const fs::path backup = paths.data_dir / "catalog.old";
    fs::remove_all(backup, ec);
    ec.clear();

    if (fs::exists(cache, ec)) {
        fs::rename(cache, backup, ec);
        if (ec) {
            fs::remove_all(cache, ec);
            ec.clear();
        }
    }

    ec.clear();
    fs::create_directories(cache.parent_path(), ec);
    fs::rename(catalog_root, cache, ec);
    if (ec) {
        // rename across devices or partial failure — copy instead.
        ec.clear();
        fs::create_directories(cache, ec);
        fs::copy(catalog_root, cache,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) {
            if (fs::exists(backup, ec)) fs::rename(backup, cache, ec);
            return fail(result, "cannot install catalog cache: " + ec.message());
        }
    }

    fs::remove_all(backup, ec);
    fs::remove_all(work, ec);

    // Prefer stamps from the zip; fall back to GitHub latest release metadata.
    if (!have_remote) fetch_latest_catalog_release(cfg, &remote, &remote_err);
    std::string release_tag =
        !loaded.release_tag.empty() ? loaded.release_tag : remote.tag;
    std::string catalog_date =
        !loaded.catalog_date.empty()
            ? loaded.catalog_date
            : catalog_date_from_tag_or_published(release_tag, remote.published_at);

    const std::int64_t synced_epoch = epoch_seconds_now();
    result.synced_at = iso_timestamp_from_epoch(synced_epoch);
    result.release_tag = release_tag;
    result.catalog_date = catalog_date;

    json state = {{"url", url},
                  {"github_repo", effective_catalog_repo(cfg)},
                  {"synced_at", result.synced_at},
                  {"synced_at_epoch", synced_epoch},
                  {"release_tag", release_tag},
                  {"catalog_date", catalog_date},
                  {"release_published_at", remote.published_at},
                  {"titles", loaded.titles.size()}};
    if (!write_catalog_state(paths, state, &err)) {
        result.ok = true;
        result.message = "Catalog updated but could not write state: " + err;
        return result;
    }

    result.ok = true;
    result.message = "Catalog updated (" + std::to_string(state.value("titles", 0)) +
                     " titles" +
                     (release_tag.empty() ? "" : ", " + release_tag) +
                     (catalog_date.empty() ? "" : ", " + catalog_date) + ") from " + url;
    return result;
}

CatalogSyncResult maybe_auto_update_catalog(const Paths& paths, const AppConfig& cfg) {
    // Always fetch when the on-device cache is missing (no bundled catalog).
    if (!catalog_cache_valid(paths)) return sync_remote_catalog(paths, cfg, true);

    if (!cfg.catalog.auto_update) {
        json state;
        read_catalog_state(paths, &state);
        return skip_ok("Catalog auto-update disabled.", &state);
    }

    // Startup path: cheap GitHub latest-release check; download zip only when the
    // remote release tag/date-time is newer than catalog-state.json.
    return sync_remote_catalog(paths, cfg, false);
}

} // namespace retcomm
