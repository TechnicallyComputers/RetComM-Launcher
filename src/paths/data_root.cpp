#include "retcomm/data_root.hpp"

#include "retcomm/paths.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <system_error>

namespace retcomm {
namespace {

using nlohmann::json;

constexpr const char* kEnvVar = "RETCOMM_HOME";
constexpr const char* kExeMarkerName = "retcomm-root.json";
constexpr const char* kConfigPointerName = "root.json";

// Trim surrounding whitespace and quotes — env vars set by hand on Windows
// frequently keep the quotes the user typed.
std::string sanitize_root_string(std::string s) {
    const auto is_space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.pop_back();
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return s;
}

// A marker's relative root resolves against the marker's own directory so a
// portable stick works regardless of its drive letter or mount point.
fs::path resolve_against(const fs::path& base, const fs::path& p) {
    if (p.empty()) return {};
    if (p.is_absolute()) return p;
    if (base.empty()) return {};
    std::error_code ec;
    fs::path joined = fs::absolute(base / p, ec);
    return ec ? (base / p) : joined;
}

fs::path exe_marker_path(const fs::path& exe_dir) {
    if (exe_dir.empty()) return {};
    return exe_dir / kExeMarkerName;
}

fs::path config_pointer_path() {
    const fs::path cfg = default_os_config_dir();
    if (cfg.empty()) return {};
    return cfg / kConfigPointerName;
}

bool remove_if_present(const fs::path& p, std::string* error) {
    if (p.empty()) return true;
    std::error_code ec;
    if (!fs::exists(p, ec)) return true;
    ec.clear();
    fs::remove(p, ec);
    if (ec) {
        if (error) *error = "cannot remove " + p.string() + ": " + ec.message();
        return false;
    }
    return true;
}

bool write_marker_file(const fs::path& marker, const fs::path& root, std::string* error) {
    std::error_code ec;
    fs::create_directories(marker.parent_path(), ec);
    if (ec) {
        if (error)
            *error = "cannot create " + marker.parent_path().string() + ": " + ec.message();
        return false;
    }
    json j;
    j["schema_version"] = 1;
    j["root"] = root.generic_string();

    // Write via a temp file so a crash mid-write cannot leave a marker that
    // parses to garbage and strands the user's data.
    const fs::path tmp = marker.parent_path() / (marker.filename().string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "cannot write " + tmp.string();
            return false;
        }
        out << j.dump(2) << "\n";
        if (!out) {
            if (error) *error = "write failed: " + tmp.string();
            return false;
        }
    }
    ec.clear();
    fs::rename(tmp, marker, ec);
    if (ec) {
        std::error_code rm_ec;
        fs::remove(tmp, rm_ec);
        if (error) *error = "cannot replace " + marker.string() + ": " + ec.message();
        return false;
    }
    return true;
}

} // namespace

const char* data_root_source_label(DataRootSource s) {
    switch (s) {
    case DataRootSource::Env: return "RETCOMM_HOME";
    case DataRootSource::ExeMarker: return "portable marker";
    case DataRootSource::ConfigPointer: return "config pointer";
    case DataRootSource::Explicit: return "--root";
    case DataRootSource::Default: break;
    }
    return "default";
}

fs::path default_os_config_dir() {
    if (const char* p = std::getenv("XDG_CONFIG_HOME"); p && *p) return fs::path(p) / "retcomm";
#if defined(_WIN32)
    if (const char* p = std::getenv("APPDATA"); p && *p) return fs::path(p) / "retcomm";
#endif
    const fs::path home = user_home_dir();
    if (home.empty()) return {};
    return home / ".config" / "retcomm";
}

fs::path default_os_data_dir() {
    if (const char* p = std::getenv("XDG_DATA_HOME"); p && *p) return fs::path(p) / "retcomm";
#if defined(_WIN32)
    if (const char* p = std::getenv("LOCALAPPDATA"); p && *p) return fs::path(p) / "retcomm";
#endif
    const fs::path home = user_home_dir();
    if (home.empty()) return {};
    return home / ".local" / "share" / "retcomm";
}

fs::path read_data_root_marker(const fs::path& marker_file) {
    if (marker_file.empty()) return {};
    std::error_code ec;
    if (!fs::is_regular_file(marker_file, ec)) return {};
    std::ifstream in(marker_file, std::ios::binary);
    if (!in) return {};
    json j;
    try {
        in >> j;
    } catch (const std::exception&) {
        return {}; // Corrupt marker → fall through to the next source.
    }
    if (!j.is_object()) return {};
    const auto it = j.find("root");
    if (it == j.end() || !it->is_string()) return {};
    const std::string raw = sanitize_root_string(it->get<std::string>());
    if (raw.empty()) return {};
    return resolve_against(marker_file.parent_path(), fs::path(raw));
}

DataRootInfo resolve_data_root(const fs::path& exe_dir) {
    DataRootInfo info;

    if (const char* env = std::getenv(kEnvVar); env && *env) {
        const std::string raw = sanitize_root_string(env);
        if (!raw.empty()) {
            // An env var has no marker file to be relative to; resolve against cwd.
            std::error_code ec;
            fs::path p = fs::path(raw);
            if (!p.is_absolute()) {
                fs::path abs = fs::absolute(p, ec);
                if (!ec) p = abs;
            }
            info.root = p;
            info.source = DataRootSource::Env;
            return info;
        }
    }

    if (const fs::path marker = exe_marker_path(exe_dir); !marker.empty()) {
        const fs::path root = read_data_root_marker(marker);
        if (!root.empty()) {
            info.root = root;
            info.source = DataRootSource::ExeMarker;
            info.pointer_file = marker;
            return info;
        }
    }

    if (const fs::path pointer = config_pointer_path(); !pointer.empty()) {
        const fs::path root = read_data_root_marker(pointer);
        if (!root.empty()) {
            info.root = root;
            info.source = DataRootSource::ConfigPointer;
            info.pointer_file = pointer;
            return info;
        }
    }

    return info; // Default.
}

bool write_data_root_pointer(const fs::path& exe_dir, const fs::path& root,
                             bool prefer_exe_marker, std::string* error) {
    if (root.empty()) return clear_data_root_pointer(exe_dir, error);

    if (prefer_exe_marker) {
        const fs::path marker = exe_marker_path(exe_dir);
        if (marker.empty()) {
            if (error) *error = "cannot write portable marker: executable directory unknown";
            return false;
        }
        // Store a path relative to the exe when the root lives beneath it, so
        // the whole folder stays movable between machines and drive letters.
        fs::path stored = root;
        std::error_code ec;
        const fs::path rel = fs::relative(root, exe_dir, ec);
        if (!ec && !rel.empty() && rel.native().rfind(fs::path("..").native(), 0) != 0)
            stored = rel;
        if (!write_marker_file(marker, stored, error)) return false;
        // The exe marker outranks the config pointer; drop the stale one so the
        // two cannot disagree.
        remove_if_present(config_pointer_path(), nullptr);
        return true;
    }

    const fs::path pointer = config_pointer_path();
    if (pointer.empty()) {
        if (error) *error = "cannot resolve default config directory for the root pointer";
        return false;
    }
    if (!write_marker_file(pointer, root, error)) return false;
    remove_if_present(exe_marker_path(exe_dir), nullptr);
    return true;
}

bool clear_data_root_pointer(const fs::path& exe_dir, std::string* error) {
    std::string err;
    bool ok = remove_if_present(exe_marker_path(exe_dir), &err);
    std::string err2;
    if (!remove_if_present(config_pointer_path(), &err2)) {
        ok = false;
        if (!err.empty()) err += "; ";
        err += err2;
    }
    if (!ok && error) *error = err;
    return ok;
}

bool directory_is_writable(const fs::path& dir, std::string* error) {
    if (dir.empty()) {
        if (error) *error = "empty path";
        return false;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec && !fs::is_directory(dir, ec)) {
        if (error) *error = "cannot create " + dir.string() + ": " + ec.message();
        return false;
    }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path probe = dir / (".retcomm-write-test-" + std::to_string(stamp));
    {
        std::ofstream out(probe, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = dir.string() + " is not writable";
            return false;
        }
        out << "ok";
        if (!out) {
            if (error) *error = dir.string() + " is not writable";
            return false;
        }
    }
    ec.clear();
    fs::remove(probe, ec);
    return true;
}

} // namespace retcomm
