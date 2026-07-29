#include "retcomm/catalog_sync.hpp"

#include "retcomm/catalog.hpp"
#include "retcomm/http.hpp"
#include "retcomm/install.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
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
    std::string catalog_date; // YYYY-MM-DD derived from tag or published_at
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

std::string catalog_date_from_tag_or_published(const std::string& tag,
                                               const std::string& published_at) {
    // Prefer dated tags: vYYYY.MM.DD… → YYYY-MM-DD
    if (tag.size() >= 11 && tag[0] == 'v') {
        // v2026.07.29 or v2026.07.29.12
        if (std::isdigit(static_cast<unsigned char>(tag[1])) && tag[5] == '.' &&
            tag[8] == '.') {
            return tag.substr(1, 4) + "-" + tag.substr(6, 2) + "-" + tag.substr(9, 2);
        }
    }
    // published_at: 2026-07-29T12:34:56Z
    if (published_at.size() >= 10 && published_at[4] == '-' && published_at[7] == '-')
        return published_at.substr(0, 10);
    return {};
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
    const std::string repo = effective_catalog_repo(cfg);
    const std::string url = "https://api.github.com/repos/" + repo + "/releases/latest";
    const auto res = http_get(url, github_http_headers());
    if (!res.ok()) {
        if (error)
            *error = res.error.empty()
                         ? ("HTTP " + std::to_string(res.status) + " from " + url)
                         : res.error;
        return false;
    }
    try {
        const json j = json::parse(res.body);
        out->tag = j.value("tag_name", "");
        out->published_at = j.value("published_at", "");
        if (out->tag.empty()) {
            if (error) *error = "latest release missing tag_name";
            return false;
        }
        out->catalog_date =
            catalog_date_from_tag_or_published(out->tag, out->published_at);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

// True when local state already matches the remote latest release identity.
bool local_matches_remote(const Paths& paths, const RemoteCatalogRelease& remote) {
    json state;
    if (!read_catalog_state(paths, &state)) return false;

    const std::string local_tag = state.value("release_tag", "");
    if (!local_tag.empty() && local_tag == remote.tag) return true;

    // Older state files may only have catalog_date.
    const std::string local_date = state.value("catalog_date", "");
    if (!local_date.empty() && !remote.catalog_date.empty() &&
        local_date == remote.catalog_date && local_tag.empty()) {
        // Ambiguous if multiple releases share a calendar day — prefer tag when known.
        return false;
    }
    return false;
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
        if (have_remote && local_matches_remote(paths, remote)) {
            json state;
            read_catalog_state(paths, &state);
            return skip_ok("Catalog up to date (" + remote.tag +
                               (remote.catalog_date.empty() ? ""
                                                            : ", " + remote.catalog_date) +
                               ").",
                           &state);
        }
        if (!have_remote) {
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
    // remote release tag/date differs from catalog-state.json.
    return sync_remote_catalog(paths, cfg, false);
}

} // namespace retcomm
