#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace retcomm {

namespace fs = std::filesystem;

struct RomIdentity {
    // All digest fields are optional lists — authors may publish any subset
    // (recomp gates commonly use crc32, md5, and/or sha1). Empty arrays are
    // fine and preferred in manifests so submission schemas stay uniform.
    std::vector<std::string> crc32;         // lowercase hex, no 0x
    std::vector<std::string> md5;           // 32 hex
    std::vector<std::string> sha1;          // 40 hex
    std::vector<std::string> sha256;        // 64 hex
    std::vector<std::string> disc_serials;
    // Optional byte sizes — when set, scan only hashes files matching one of
    // these lengths (keeps PSX ISO libraries from SHA-1ing hundreds of GiB).
    std::vector<std::uint64_t> sizes;
    // Suggested basenames for the hub UI when unmatched (No-Intro / Redump).
    // Display / RomM search hints — not used for hard matching.
    std::vector<std::string> filenames;
};

// Host firmware / BIOS required by some titles (e.g. psxrecomp SCPH1001).
struct BiosIdentity {
    bool required = false;
    std::vector<std::string> crc32;
    std::vector<std::string> md5;
    std::vector<std::string> sha1;
    std::vector<std::string> sha256;
    std::vector<std::uint64_t> sizes;
    std::vector<std::string> filenames; // basename hints (case-insensitive)
};

struct TitleRelease {
    std::string github; // owner/repo
    std::string asset_glob_linux;
    std::string asset_glob_windows;
    std::string asset_glob_macos;
    // When true, install/update may select GitHub pre-releases (e.g. Alpha builds
    // when no stable /releases/latest exists yet).
    bool allow_prerelease = false;
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
    std::string notes;         // Catalog/maintainer footnotes (not shown in hub)
    std::string author_notes;  // Author message to users (hub “Author's Notes”)
    RomIdentity rom_identity;
    BiosIdentity bios_identity;
    std::vector<std::string> rom_extensions;
    TitleRelease release;
    std::string install_dir_name;
    TitleLaunch launch;
    std::vector<std::string> romm_platforms;
    std::vector<std::string> saves_sram_glob;
    std::vector<std::string> saves_memcard_glob;

    bool has_rom_identity() const;
    bool has_bios_identity() const;
    bool requires_bios() const;
    const std::string& launch_binary_for_host() const;
    const std::string& asset_glob_for_host() const;
    const std::string& launch_binary_for_os(const std::string& os) const;
    const std::string& asset_glob_for_os(const std::string& os) const;
    // True when the catalog lists a Windows asset + launch binary (Wine fallback).
    bool supports_wine_install() const;
    // Owner segment of release.github (e.g. "mstan" from "mstan/FooRecomp").
    std::string github_owner() const;
    // Homepage when set, else https://github.com/<release.github>.
    std::string github_source_url() const;
};

struct Catalog {
    int schema_version = 1;
    std::string name;
    // Optional publish stamp from index.json (set by catalog release CI).
    std::string catalog_date; // YYYY-MM-DD
    std::string release_tag;  // e.g. v2026.07.29.12
    std::vector<Title> titles;
    fs::path root;

    const Title* find(const std::string& id) const;
};

// Load index.json + titles/<id>.json. Throws std::runtime_error on hard failure.
Catalog load_catalog(const fs::path& catalog_dir);

} // namespace retcomm
