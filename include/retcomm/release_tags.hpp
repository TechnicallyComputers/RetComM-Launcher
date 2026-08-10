#pragma once

#include "retcomm/paths.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace retcomm {

// How long a cached GitHub latest-tag is reused before re-fetching.
inline constexpr std::int64_t kReleaseTagCacheTtlSeconds = 4 * 60 * 60; // 4 hours

fs::path release_tags_cache_path(const Paths& paths);

// Session + disk cache for GitHub latest release tags.
// Dedupes by (slug, allow_prerelease) within one instance; persists under
// data_dir/release-tags.json so Check Updates stays fast as installs grow.
class ReleaseTagCache {
public:
    explicit ReleaseTagCache(fs::path cache_path);

    // force=true ignores TTL (still dedupes within this instance).
    std::string latest_tag(const std::string& github_slug, bool allow_prerelease, bool force,
                           std::string* error = nullptr);

    void save_if_dirty();

    int network_fetches() const { return network_fetches_; }
    int cache_hits() const { return cache_hits_; }

private:
    struct Entry {
        std::string tag;
        std::int64_t checked_unix = 0;
    };

    static std::string cache_key(const std::string& github_slug, bool allow_prerelease);
    void load();

    fs::path path_;
    std::unordered_map<std::string, Entry> entries_;
    // In-flight / completed results for this session (avoids double-fetch).
    std::unordered_map<std::string, std::string> session_;
    bool dirty_ = false;
    int network_fetches_ = 0;
    int cache_hits_ = 0;
};

} // namespace retcomm
