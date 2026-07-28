#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace retcomm {

namespace fs = std::filesystem;

struct RomIdentity {
    std::vector<std::string> crc32;         // lowercase hex, no 0x
    std::vector<std::string> sha1;          // 40 hex
    std::vector<std::string> sha256;        // 64 hex
    std::vector<std::string> disc_serials;
};

struct TitleRelease {
    std::string github; // owner/repo
    std::string asset_glob_linux;
    std::string asset_glob_windows;
    std::string asset_glob_macos;
};

struct TitleLaunch {
    std::string linux;
    std::string windows;
    std::string macos;
};

struct Title {
    std::string id;
    std::string name;
    std::string kind;       // recomp | decomp
    std::string platform;   // snes | psx | n64 | ...
    std::string description;
    std::string homepage;
    std::string notes;
    RomIdentity rom_identity;
    std::vector<std::string> rom_extensions;
    TitleRelease release;
    std::string install_dir_name;
    TitleLaunch launch;
    std::vector<std::string> romm_platforms;
    std::vector<std::string> saves_sram_glob;
    std::vector<std::string> saves_memcard_glob;

    bool has_rom_identity() const;
    const std::string& launch_binary_for_host() const;
    const std::string& asset_glob_for_host() const;
};

struct Catalog {
    int schema_version = 1;
    std::string name;
    std::vector<Title> titles;
    fs::path root;

    const Title* find(const std::string& id) const;
};

// Load index.json + titles/<id>.json. Throws std::runtime_error on hard failure.
Catalog load_catalog(const fs::path& catalog_dir);

} // namespace retcomm
