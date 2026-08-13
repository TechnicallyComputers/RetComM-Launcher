#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace retcomm {

namespace fs = std::filesystem;

struct HttpResponse {
    long status = 0;
    std::string body;
    std::string error;
    bool ok() const { return status >= 200 && status < 300 && error.empty(); }
};

using HttpProgressFn = std::function<void(std::uint64_t downloaded, std::uint64_t total)>;

// GET into memory. Optional Authorization / Accept headers via extras.
HttpResponse http_get(const std::string& url,
                      const std::vector<std::pair<std::string, std::string>>& headers = {});

// Stream download to a file path (creates parent dirs).
// When expected_size > 0 and dest already has that many bytes, skips the transfer.
// Resumes from dest.part when present (Range). Retries once without resume on
// range errors. Progress totals include resume offsets when known.
bool http_download(const std::string& url, const fs::path& dest, std::string* error,
                   const std::vector<std::pair<std::string, std::string>>& headers = {},
                   HttpProgressFn on_progress = {}, std::uint64_t expected_size = 0);

struct HttpMultipartFile {
    std::string field;    // form field name (e.g. "saveFile")
    fs::path path;        // local file to upload
    std::string filename; // remote filename (defaults to path.filename())
};

// POST multipart/form-data (libcurl mime). Optional text fields + file parts.
HttpResponse http_post_multipart(
    const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
    const std::vector<HttpMultipartFile>& files,
    const std::vector<std::pair<std::string, std::string>>& fields = {});

// Optional GitHub auth. Preference: GITHUB_TOKEN / GH_TOKEN env, else token from
// set_github_token() (hub loads AppConfig::github_token here).
void set_github_token(std::string token);
std::vector<std::pair<std::string, std::string>> github_http_headers();

} // namespace retcomm
