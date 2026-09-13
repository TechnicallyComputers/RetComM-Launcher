// In-process zip reading (miniz): no temp file, no helper process.
//
// Header-only on purpose. The Windows portable stub links none of
// retcomm_core -- it has to run before the payload's DLLs exist on disk -- yet
// needs the same reader and, more to the point, the same path-sanitising
// rules. One copy of that logic, two binaries.
//
// This replaces shelling out to tar.exe and to
//   powershell -NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive ..."
// Writing an archive to disk and driving a system interpreter to unpack it is
// a shape Defender's heuristics score as a dropper, and it was getting
// releases quarantined. See packaging/README.md, "Antivirus false positives".
//
// Zip only. tar.gz / 7z still go through the external tools in install.cpp.
#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <istream>
#include <string>
#include <vector>

#include "miniz.h"

namespace retcomm {
namespace zip {

namespace fs = std::filesystem;

namespace detail {

// Reading through a stream lets the portable stub point miniz at its own .exe
// starting at the payload offset, with nothing copied anywhere first.
struct StreamSource {
    std::istream* in = nullptr;
    uint64_t base = 0;  // offset of the archive's first byte within the stream
};

inline size_t stream_read(void* opaque, mz_uint64 file_ofs, void* buf, size_t n) {
    auto* src = static_cast<StreamSource*>(opaque);
    if (!src || !src->in) return 0;
    src->in->clear();
    src->in->seekg(static_cast<std::streamoff>(src->base + file_ofs));
    if (!*src->in) return 0;
    src->in->read(static_cast<char*>(buf), static_cast<std::streamsize>(n));
    const auto got = src->in->gcount();
    return got > 0 ? static_cast<size_t>(got) : 0;
}

struct WriteSink {
    std::ofstream out;
    bool failed = false;
};

// miniz aborts the entry when the callback returns short of n.
inline size_t sink_write(void* opaque, mz_uint64, const void* buf, size_t n) {
    auto* sink = static_cast<WriteSink*>(opaque);
    if (!sink || sink->failed) return 0;
    sink->out.write(static_cast<const char*>(buf), static_cast<std::streamsize>(n));
    if (!sink->out) {
        sink->failed = true;
        return 0;
    }
    return n;
}

// Map a zip entry name onto a relative path under the extract directory, or
// return empty when it could escape. Entry names are attacker-controlled for
// anything downloaded, and tar.exe / Expand-Archive used to do this check for
// us, so it has to happen here now.
inline fs::path safe_entry_path(const char* name) {
    if (!name || !*name) return {};
    std::string s(name);
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    if (s.front() == '/') return {};              // rooted
    if (s.size() >= 2 && s[1] == ':') return {};  // drive-qualified
    fs::path rel;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t slash = s.find('/', start);
        const std::string seg =
            s.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (seg == "..") return {};
        if (!seg.empty() && seg != ".") {
            // u8path keeps this portable: on Windows it decodes the entry's
            // UTF-8 to UTF-16 rather than mangling it through the ANSI codepage.
            const fs::path piece = fs::u8path(seg);
            if (piece.empty() || piece.has_root_name() || piece.has_root_directory()) return {};
            rel /= piece;
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return rel;
}

// Zip has no permission model of its own. Archivers on Unix stash st_mode in
// the high 16 bits of the external attributes and mark the host in
// version_made_by; readers that ignore it hand back every file as 0644. That
// is how a release zip's psxrecomp-game / psxrecomp-bios emitters, tools/*.sh
// and the game binary itself came out non-executable, and the build died with
// "Permission denied" the first time psxrecomp_cli.py exec'd one. Only the
// permission bits are applied — type bits (symlink, device) are not honoured;
// a symlink entry is still written as a regular file holding its target.
// Owner-write is always kept so a later overwrite_existing copy cannot fail on
// a 0444 entry.
#if !defined(_WIN32)
inline void apply_unix_mode(const fs::path& target, const mz_zip_archive_file_stat& st) {
    const unsigned host = (st.m_version_made_by >> 8) & 0xFFu;
    if (host != 3u /* Unix */ && host != 19u /* OS X (Darwin) */) return;
    const unsigned mode = (st.m_external_attr >> 16) & 0xFFFFu;
    const unsigned perm_bits = mode & 0777u;
    if (perm_bits == 0) return;  // nothing recorded; keep the umask default
    std::error_code ec;
    fs::permissions(target, static_cast<fs::perms>(perm_bits | 0200u),
                    fs::perm_options::replace, ec);
}
#endif

inline bool extract_open_archive(mz_zip_archive* za, const fs::path& dest, std::string* err) {
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (ec) {
        if (err) *err = "cannot create extract directory " + dest.string();
        return false;
    }

    const mz_uint count = mz_zip_reader_get_num_files(za);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(za, i, &st)) {
            if (err) *err = "corrupt entry in archive";
            return false;
        }
        const bool is_dir = mz_zip_reader_is_file_a_directory(za, i) != 0;
        const fs::path rel = safe_entry_path(st.m_filename);
        if (rel.empty()) {
            // A directory entry that normalises away ("./") has nothing to
            // create; anything else empty means the name tried to leave dest.
            if (is_dir) continue;
            if (err) *err = std::string("refusing unsafe path in archive: ") + st.m_filename;
            return false;
        }

        const fs::path target = dest / rel;
        if (is_dir) {
            fs::create_directories(target, ec);
            continue;
        }
        fs::create_directories(target.parent_path(), ec);

        WriteSink sink;
        sink.out.open(target, std::ios::binary | std::ios::trunc);
        if (!sink.out) {
            if (err) *err = "cannot write " + target.string();
            return false;
        }
        if (!mz_zip_reader_extract_to_callback(za, i, sink_write, &sink, 0)) {
            if (err) *err = "failed to unpack " + rel.string();
            return false;
        }
        sink.out.close();
        if (sink.failed || !sink.out) {
            if (err) *err = "failed to write " + target.string() + " (disk full?)";
            return false;
        }
#if !defined(_WIN32)
        apply_unix_mode(target, st);
#endif
    }
    return true;
}

}  // namespace detail

// Extract `size` bytes of zip beginning at `base` within `in`.
inline bool extract_stream(std::istream& in, uint64_t base, uint64_t size, const fs::path& dest,
                           std::string* err) {
    if (size == 0) {
        if (err) *err = "empty archive";
        return false;
    }
    detail::StreamSource src;
    src.in = &in;
    src.base = base;

    mz_zip_archive za;
    memset(&za, 0, sizeof(za));
    za.m_pRead = detail::stream_read;
    za.m_pIO_opaque = &src;
    if (!mz_zip_reader_init(&za, size, 0)) {
        if (err) *err = "not a readable zip archive";
        return false;
    }
    const bool ok = detail::extract_open_archive(&za, dest, err);
    mz_zip_reader_end(&za);
    return ok;
}

inline bool extract_file(const fs::path& archive, const fs::path& dest, std::string* err) {
    std::error_code ec;
    const auto size = fs::file_size(archive, ec);
    if (ec) {
        if (err) *err = "cannot stat " + archive.string();
        return false;
    }
    std::ifstream in(archive, std::ios::binary);
    if (!in) {
        if (err) *err = "cannot open " + archive.string();
        return false;
    }
    return extract_stream(in, 0, static_cast<uint64_t>(size), dest, err);
}

// Entry names as stored, without extracting anything.
inline bool list_file(const fs::path& archive, std::vector<std::string>* out, std::string* err) {
    if (!out) return false;
    out->clear();
    std::error_code ec;
    const auto size = fs::file_size(archive, ec);
    if (ec) {
        if (err) *err = "cannot stat " + archive.string();
        return false;
    }
    std::ifstream in(archive, std::ios::binary);
    if (!in) {
        if (err) *err = "cannot open " + archive.string();
        return false;
    }
    detail::StreamSource src;
    src.in = &in;
    src.base = 0;

    mz_zip_archive za;
    memset(&za, 0, sizeof(za));
    za.m_pRead = detail::stream_read;
    za.m_pIO_opaque = &src;
    if (!mz_zip_reader_init(&za, static_cast<mz_uint64>(size), 0)) {
        if (err) *err = "not a readable zip archive";
        return false;
    }
    const mz_uint count = mz_zip_reader_get_num_files(&za);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&za, i, &st)) {
            mz_zip_reader_end(&za);
            if (err) *err = "corrupt entry in archive";
            return false;
        }
        out->emplace_back(st.m_filename);
    }
    mz_zip_reader_end(&za);
    return true;
}

}  // namespace zip
}  // namespace retcomm
