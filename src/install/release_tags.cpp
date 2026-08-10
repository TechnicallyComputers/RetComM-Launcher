#include "retcomm/release_tags.hpp"

#include "retcomm/install.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>

namespace retcomm {
namespace {

using nlohmann::json;

std::int64_t now_unix() {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

} // namespace

fs::path release_tags_cache_path(const Paths& paths) {
    return paths.data_dir / "release-tags.json";
}

ReleaseTagCache::ReleaseTagCache(fs::path cache_path) : path_(std::move(cache_path)) {
    load();
}

std::string ReleaseTagCache::cache_key(const std::string& github_slug, bool allow_prerelease) {
    return github_slug + (allow_prerelease ? "|pre" : "|latest");
}

void ReleaseTagCache::load() {
    entries_.clear();
    std::error_code ec;
    if (path_.empty() || !fs::is_regular_file(path_, ec)) return;
    try {
        std::ifstream in(path_);
        if (!in) return;
        json j;
        in >> j;
        if (!j.is_object()) return;
        const auto& ents = j.contains("entries") && j["entries"].is_object() ? j["entries"] : j;
        if (!ents.is_object()) return;
        for (auto it = ents.begin(); it != ents.end(); ++it) {
            if (!it.value().is_object()) continue;
            Entry e;
            e.tag = it.value().value("tag", "");
            e.checked_unix = it.value().value("checked_unix", static_cast<std::int64_t>(0));
            if (e.tag.empty()) continue;
            entries_[it.key()] = std::move(e);
        }
    } catch (...) {
        entries_.clear();
    }
}

void ReleaseTagCache::save_if_dirty() {
    if (!dirty_ || path_.empty()) return;
    try {
        std::error_code ec;
        fs::create_directories(path_.parent_path(), ec);
        json ents = json::object();
        for (const auto& [key, e] : entries_) {
            if (e.tag.empty()) continue;
            ents[key] = {{"tag", e.tag}, {"checked_unix", e.checked_unix}};
        }
        json root = {{"version", 1}, {"entries", std::move(ents)}};
        std::ofstream out(path_, std::ios::trunc);
        if (!out) return;
        out << root.dump(2) << '\n';
        dirty_ = false;
    } catch (...) {
        // Best-effort cache; ignore write failures.
    }
}

std::string ReleaseTagCache::latest_tag(const std::string& github_slug, bool allow_prerelease,
                                        bool force, std::string* error) {
    if (error) error->clear();
    if (github_slug.empty()) {
        if (error) *error = "empty github slug";
        return {};
    }
    const std::string key = cache_key(github_slug, allow_prerelease);

    if (const auto sit = session_.find(key); sit != session_.end()) {
        ++cache_hits_;
        return sit->second;
    }

    const std::int64_t now = now_unix();
    if (!force) {
        if (const auto it = entries_.find(key); it != entries_.end()) {
            const std::int64_t age = now - it->second.checked_unix;
            if (!it->second.tag.empty() && age >= 0 && age < kReleaseTagCacheTtlSeconds) {
                session_[key] = it->second.tag;
                ++cache_hits_;
                return it->second.tag;
            }
        }
    }

    std::string err;
    const std::string tag = fetch_latest_release_tag(github_slug, &err, allow_prerelease);
    ++network_fetches_;
    if (tag.empty()) {
        if (error) *error = err.empty() ? "fetch failed" : err;
        // Keep serving a stale tag if we have one (offline / rate-limit friendly).
        if (const auto it = entries_.find(key); it != entries_.end() && !it->second.tag.empty()) {
            session_[key] = it->second.tag;
            ++cache_hits_;
            return it->second.tag;
        }
        session_[key] = {};
        return {};
    }

    entries_[key] = Entry{tag, now};
    session_[key] = tag;
    dirty_ = true;
    return tag;
}

} // namespace retcomm
