#pragma once

#include "retcomm/config.hpp"
#include "retcomm/paths.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace retcomm {

namespace fs = std::filesystem;

struct CacheGcResult {
    bool ok = true;
    std::string message;
    std::vector<std::string> messages;
    std::size_t removed_toolchains = 0;
    std::size_t removed_sdks = 0;
    std::size_t removed_engines = 0;
    std::size_t removed_release_zips = 0;
    std::size_t removed_idle_builds = 0;
    std::uint64_t bytes_freed = 0;
};

// Garbage-collect shared caches + idle per-title cmake build dirs.
// Safe to call after successful builds or from Hub / `retcomm cache gc`.
// Never deletes the Play binary, generated C, or the toolchain/SDK currently
// pointed at by `latest` / in-use symlinks.
// Soft-fails on OOM/exceptions (ok=false + message); callers must not treat GC
// failure as install/build failure.
CacheGcResult run_cache_gc(const Paths& paths, const AppConfig& cfg);

// Ensure data_dir/ccache exists and return env pairs for cmake/ninja children
// (CCACHE_DIR + CCACHE_MAXSIZE). Empty when ccache_max_gb <= 0.
std::vector<std::pair<std::string, std::string>> shared_ccache_env(const Paths& paths,
                                                                   const AppConfig& cfg);

// Absolute path of the shared ccache root (data_dir/ccache).
fs::path shared_ccache_dir(const Paths& paths);

} // namespace retcomm
