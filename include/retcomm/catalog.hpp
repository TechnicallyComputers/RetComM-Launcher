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
    // Exact cue TRACK counts (e.g. MotK Redump = {17}). Empty = no TOC gate.
    // Digests prove the data track; this rejects Track-01-only mounts.
    std::vector<int> track_counts;
    // Prefer/require a .cue bind (set for multi-track titles).
    bool require_cue = false;
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

// Optional recomp-net lobby identity. Omit in JSON when unsupported.
struct TitleNetplay {
    bool supported = false;
    std::string stack;           // "recomp-net"
    std::string game_name;       // Exact WS create/join/list wire name
    std::string game_version;    // Lobby pin (empty → "dev" on the server)
    int max_slots = 2;
    std::string lobby_url;       // Optional per-title override
    std::vector<std::string> transports; // e.g. "lan", "ice"
    std::string match_caps_schema;       // e.g. "psx-v1", "snes-v1"
};

// Optional local generate + cmake recipe. When enabled, Hub Install prefers
// build_title over a prebuilt zip (third parties omit this and stay zip-only).
struct TitleBuildSource {
    std::string github; // owner/repo (defaults to release.github when empty)
    std::string ref;    // tag / branch / commit for the source zipball
};

struct TitleBuildPack {
    std::string id;
    std::string github; // owner/repo for pack releases
    std::string asset_glob_linux;
    std::string asset_glob_windows;
    std::string asset_glob_macos;
    // Optional semver floor (retcomm-toolchain.json "version" / release tag).
    std::string min_version;
    const std::string& asset_glob_for_os(const std::string& os) const;
    const std::string& asset_glob_for_host() const;
};

struct TitleBuildGenerate {
    // "snesrecomp" | "psxrecomp" | "gbarecomp" — empty ⇒ derive from title.platform
    std::string engine;
    // snesrecomp generate
    std::string cfg_dir = "recomp";
    std::string out_dir = "src/gen";
    std::string funcs_h = "recomp/funcs.h";
    bool cfg_roots = true;
    // psxrecomp / gbarecomp generate (TOML path)
    std::string config = "game.toml";
};

struct TitleBuildCmake {
    std::string build_dir = "build";
    std::string target;
    std::string config = "Release";
};

struct TitleBuild {
    bool enabled = false;
    TitleBuildSource source;
    TitleBuildPack sdk;
    TitleBuildPack toolchain;
    TitleBuildGenerate generate;
    TitleBuildCmake cmake;
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
    TitleNetplay netplay;
    TitleBuild build;

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
    // True when catalog advertises a usable recomp-net lobby identity.
    bool supports_netplay() const;
    // True when catalog declares a usable local generate+build recipe.
    bool supports_local_build() const;
    // True when a prebuilt GitHub release zip can still be installed.
    bool supports_prebuilt_install() const;
    // Matches install_title_auto: with prefer_prebuilt=false, Install runs the
    // local generate+cmake path whenever a build recipe exists — even if a
    // finished zip is also advertised. Hub ROM prompts must use this, not
    // "!supports_prebuilt_install()", or dual-mode titles skip the chooser.
    bool prefers_local_build_install(bool prefer_prebuilt = false) const;
};

struct Catalog {
    int schema_version = 1;
    std::string name;
    // Optional publish stamp from index.json (set by catalog release CI).
    std::string catalog_date; // YYYY-MM-DD or YYYY-MM-DDTHH:MM:SSZ
    std::string release_tag;  // e.g. v2026.07.29.184100.12
    std::vector<Title> titles;
    fs::path root;

    const Title* find(const std::string& id) const;
    // First title whose netplay.game_name matches (exact). Prefer installed later in hub.
    const Title* find_by_netplay_game_name(const std::string& game_name) const;
};

// Empty / whitespace → "dev". Leading 'v'/'V' stripped when the rest looks like a version.
std::string normalize_netplay_version(std::string version);
bool netplay_versions_equal(const std::string& a, const std::string& b);

// Load index.json + titles/<id>.json. Throws std::runtime_error on hard failure.
Catalog load_catalog(const fs::path& catalog_dir);

} // namespace retcomm
