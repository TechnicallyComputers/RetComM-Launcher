#pragma once

#include "retcomm/paths.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace retcomm {

// How long a cached GitHub latest-tag is reused before re-fetching.
inline constexpr std::int64_t kReleaseTagCacheTtlSeconds = 4 * 60 * 60; // 4 hours

fs::path release_tags_cache_path(const Paths& paths);

// Compare release tags (optional leading 'v', dotted numerics). <0 / 0 / >0.
int release_tag_cmp(const std::string& a, const std::string& b);

// Session + disk cache for GitHub latest release tags.
// Dedupes by (slug, allow_prerelease) within one instance; persists under
// data_dir/release-tags.json so Check Updates stays fast as installs grow.
class ReleaseTagCache {
public:
    explicit ReleaseTagCache(fs::path cache_path);

    // force=true ignores TTL (still dedupes within this instance).
    std::string latest_tag(const std::string& github_slug, bool allow_prerelease, bool force,
                           std::string* error = nullptr);

    // Record a live GitHub latest (or an installed source_ref that is ahead of a
    // stale TTL entry) so Check Updates / hub rows stay consistent with Update.
    void note_latest_tag(const std::string& github_slug, bool allow_prerelease,
                         const std::string& tag);
    // Like note_latest_tag, but only replaces the cached value when `tag` is newer.
    void note_latest_tag_if_newer(const std::string& github_slug, bool allow_prerelease,
                                  const std::string& tag);

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
