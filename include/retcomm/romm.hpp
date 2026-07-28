#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace retcomm {

// Thin RomM companion-client sketch. Network I/O is stubbed until wired to
// libcurl / cpp-httplib; shapes match RomM's REST surface (platforms, roms, assets).

struct RommConfig {
    std::string base_url;   // https://romm.example.com
    std::string api_token;  // Client API token or OAuth bearer
    bool enabled() const { return !base_url.empty(); }
};

struct RommPlatform {
    int id = 0;
    std::string slug;
    std::string name;
};

struct RommRom {
    int id = 0;
    int platform_id = 0;
    std::string name;
    std::string fs_name;
    std::optional<std::string> igdb_id;
};

struct RommClient {
    explicit RommClient(RommConfig cfg);

    const RommConfig& config() const { return cfg_; }

    // Stubs: return false + set last_error_ until HTTP is implemented.
    bool ping();
    bool list_platforms(std::vector<RommPlatform>& out);
    bool list_roms(int platform_id, std::vector<RommRom>& out);

    const std::string& last_error() const { return last_error_; }

private:
    RommConfig cfg_;
    std::string last_error_;
};

RommConfig load_romm_config(const std::filesystem::path& config_path);

} // namespace retcomm
