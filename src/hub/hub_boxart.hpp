#pragma once

#include "retcomm/catalog.hpp"
#include "retcomm/config.hpp"
#include "retcomm/paths.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace retcomm::hub {

namespace fs = std::filesystem;

// Active remote source: "romm" when cfg.romm.sync_boxart, else "libretro".
const char* active_boxart_source(const AppConfig& cfg);

// Per-source cache: ~/.local/share/retcomm/boxart/<libretro|romm>/
fs::path boxart_cache_dir(const Paths& paths, const AppConfig& cfg);

// Resolve display path:
// 1) Local sibling/library art only if cfg.prefer_local_boxart
// 2) Cached art for the active remote source (RomM or Libretro)
fs::path resolve_boxart_path(const AppConfig& cfg, const Title& title,
                             const fs::path& rom_path, const std::string& suggested_rom,
                             const Paths& paths);

struct BoxartFetchResult {
    bool ok = false;
    fs::path path;
    std::string source;  // "libretro" | "romm" | "local" | "cache"
    std::string message;
};

// Ensure cached cover for the active remote source. Skips download when
// prefer_local finds filesystem art, or when that source's cache already exists.
// When force=true, re-download even if a cache file is present.
BoxartFetchResult ensure_remote_boxart(const Paths& paths, const AppConfig& cfg,
                                       const Title& title, const fs::path& rom_path,
                                       const std::string& suggested_rom, bool force = false);

struct BoxartTexture {
    unsigned int gl_id = 0;
    int width = 0;
    int height = 0;
    std::string path;
    std::uint64_t size = 0;
    std::int64_t mtime_sec = 0;
};

// OpenGL texture cache keyed by title id. Call destroy_all() on shutdown.
class BoxartCache {
public:
    ~BoxartCache();
    // Returns nullptr when no art / load failed. Reloads if path or file mtime/size changed.
    const BoxartTexture* get(const std::string& title_id, const fs::path& image_path);
    void destroy_all();

private:
    std::unordered_map<std::string, BoxartTexture> by_title_;
};

// Remove all cached cover files for the active remote source (romm or libretro).
void clear_boxart_cache(const Paths& paths, const AppConfig& cfg);

} // namespace retcomm::hub
