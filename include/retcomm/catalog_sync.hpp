#pragma once

#include "retcomm/config.hpp"
#include "retcomm/paths.hpp"

#include <filesystem>
#include <string>

namespace retcomm {

namespace fs = std::filesystem;

// Compile-time defaults (CMake RETCOMM_CATALOG_*).
std::string default_catalog_download_url();
std::string default_catalog_github_repo();

struct CatalogSyncResult {
    bool ok = false;
    bool skipped = false; // auto-update disabled or cache still fresh
    std::string message;
    std::string synced_at; // ISO-ish timestamp written to catalog-state.json
};

// Cached remote catalog root (~/.local/share/retcomm/catalog).
fs::path catalog_cache_dir(const Paths& paths);

// True when index.json exists under the cache dir.
bool catalog_cache_valid(const Paths& paths);

// Download catalog.zip from config (or default URL) and extract into the cache.
CatalogSyncResult sync_remote_catalog(const Paths& paths, const AppConfig& cfg,
                                      bool force = false);

// Always sync when the on-device cache is missing; when auto_update is enabled,
// also refresh a stale cache (best-effort).
CatalogSyncResult maybe_auto_update_catalog(const Paths& paths, const AppConfig& cfg);

} // namespace retcomm
