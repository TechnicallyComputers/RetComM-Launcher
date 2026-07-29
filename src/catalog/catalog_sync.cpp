#include "retcomm/catalog_sync.hpp"

#include "retcomm/catalog.hpp"
#include "retcomm/http.hpp"
#include "retcomm/install.hpp"

#include <nlohmann/json.hpp>

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

constexpr int kAutoUpdateIntervalHours = 24;

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

fs::path catalog_state_path(const Paths& paths) { return paths.data_dir / "catalog-state.json"; }

std::string effective_catalog_url(const AppConfig& cfg) {
    if (!cfg.catalog.url.empty()) return cfg.catalog.url;
    return default_catalog_download_url();
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

bool cache_is_stale(const Paths& paths, const std::string& url) {
    json state;
    if (!read_catalog_state(paths, &state)) return true;
    if (state.value("url", "") != url) return true;

    std::int64_t synced_epoch = 0;
    if (state.contains("synced_at_epoch") && state.at("synced_at_epoch").is_number_integer())
        synced_epoch = state.at("synced_at_epoch").get<std::int64_t>();
    if (synced_epoch <= 0) return true;

    const std::int64_t now = epoch_seconds_now();
    const double hours = static_cast<double>(now - synced_epoch) / 3600.0;
    return hours >= kAutoUpdateIntervalHours;
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

    if (!force && catalog_cache_valid(paths) && !cache_is_stale(paths, url)) {
        result.ok = true;
        result.skipped = true;
        result.message = "Catalog cache is up to date.";
        json state;
        if (read_catalog_state(paths, &state))
            result.synced_at = state.value("synced_at", "");
        return result;
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

    try {
        load_catalog(catalog_root);
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

    const std::int64_t synced_epoch = epoch_seconds_now();
    result.synced_at = iso_timestamp_from_epoch(synced_epoch);
    json state = {{"url", url},
                  {"github_repo",
                   cfg.catalog.github_repo.empty() ? default_catalog_github_repo()
                                                   : cfg.catalog.github_repo},
                  {"synced_at", result.synced_at},
                  {"synced_at_epoch", synced_epoch},
                  {"titles", load_catalog(cache).titles.size()}};
    if (!write_catalog_state(paths, state, &err)) {
        result.ok = true;
        result.message = "Catalog updated but could not write state: " + err;
        return result;
    }

    result.ok = true;
    result.message = "Catalog updated (" + std::to_string(state.value("titles", 0)) +
                     " titles) from " + url;
    return result;
}

CatalogSyncResult maybe_auto_update_catalog(const Paths& paths, const AppConfig& cfg) {
    // Always fetch when the on-device cache is missing (no bundled catalog).
    if (!catalog_cache_valid(paths)) return sync_remote_catalog(paths, cfg, true);

    if (!cfg.catalog.auto_update) {
        CatalogSyncResult result;
        result.ok = true;
        result.skipped = true;
        result.message = "Catalog auto-update disabled.";
        return result;
    }
    if (!cache_is_stale(paths, effective_catalog_url(cfg))) {
        CatalogSyncResult result;
        result.ok = true;
        result.skipped = true;
        result.message = "Catalog cache is fresh.";
        return result;
    }
    return sync_remote_catalog(paths, cfg, false);
}

} // namespace retcomm
