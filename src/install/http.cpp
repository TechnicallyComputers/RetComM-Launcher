#include "retcomm/http.hpp"

#include <curl/curl.h>

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>

namespace retcomm {
namespace {

std::mutex g_github_token_mu;
std::string g_github_token_config;

void ensure_curl_global() {
    static const bool once = [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        return true;
    }();
    (void)once;
}

size_t write_string(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t write_file(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::ofstream*>(userdata);
    out->write(ptr, static_cast<std::streamsize>(size * nmemb));
    return out->good() ? size * nmemb : 0;
}

struct ProgressBridge {
    HttpProgressFn fn;
    std::uint64_t resume_from = 0;
};

int xferinfo(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* b = static_cast<ProgressBridge*>(clientp);
    if (!b || !b->fn) return 0;
    // With CURLOPT_RESUME_FROM_LARGE, dlnow/dltotal are for the current request
    // body only — add the already-on-disk prefix for UI totals.
    const auto got = b->resume_from + static_cast<std::uint64_t>(dlnow);
    const auto total =
        dltotal > 0 ? b->resume_from + static_cast<std::uint64_t>(dltotal) : std::uint64_t{0};
    b->fn(got, total);
    return 0;
}

void apply_headers(CURL* curl, const std::vector<std::pair<std::string, std::string>>& headers,
                   curl_slist** list) {
    for (const auto& h : headers) {
        const std::string line = h.first + ": " + h.second;
        *list = curl_slist_append(*list, line.c_str());
    }
    if (*list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, *list);
}

CURL* make_easy(const std::string& url) {
    ensure_curl_global();
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "retcomm-launcher");
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0L);
    // Avoid indefinite hangs on dead peers; do not set an absolute transfer
    // timeout — large toolchain packs can take many minutes on slow links.
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L); // 1 KiB/s
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);   // abort if stalled 2 min
#if LIBCURL_VERSION_NUM >= 0x075500
    // CURLOPT_PROTOCOLS_STR since 7.85.0
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
#if defined(CURL_HTTP_VERSION_2TLS)
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
#endif
    return curl;
}

bool http_download_once(const std::string& url, const fs::path& part, curl_off_t resume_from,
                        const std::vector<std::pair<std::string, std::string>>& headers,
                        HttpProgressFn on_progress, long* status_out, std::string* error) {
    std::ofstream out;
    if (resume_from > 0)
        out.open(part, std::ios::binary | std::ios::app);
    else
        out.open(part, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot write " + part.string();
        return false;
    }

    CURL* curl = make_easy(url);
    curl_slist* hdrs = nullptr;
    apply_headers(curl, headers, &hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    if (resume_from > 0)
        curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, resume_from);

    ProgressBridge bridge{std::move(on_progress), static_cast<std::uint64_t>(resume_from)};
    if (bridge.fn) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &bridge);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }

    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    out.close();

    if (status_out) *status_out = status;

    if (code != CURLE_OK) {
        if (error) *error = curl_easy_strerror(code);
        return false;
    }
    // 206 Partial Content is success when resuming; 200 when not.
    if (status < 200 || status >= 300) {
        if (error) *error = "HTTP " + std::to_string(status);
        return false;
    }
    return true;
}

} // namespace

void set_github_token(std::string token) {
    // Trim trailing whitespace / newlines from pasted PATs.
    while (!token.empty() &&
           (token.back() == '\n' || token.back() == '\r' || token.back() == ' ' ||
            token.back() == '\t'))
        token.pop_back();
    size_t i = 0;
    while (i < token.size() &&
           (token[i] == ' ' || token[i] == '\t' || token[i] == '\n' || token[i] == '\r'))
        ++i;
    if (i > 0) token.erase(0, i);
    std::lock_guard<std::mutex> lock(g_github_token_mu);
    g_github_token_config = std::move(token);
}

std::vector<std::pair<std::string, std::string>> github_http_headers() {
    std::vector<std::pair<std::string, std::string>> h;
    h.emplace_back("Accept", "application/vnd.github+json");
    h.emplace_back("X-GitHub-Api-Version", "2022-11-28");
    const char* tok = std::getenv("GITHUB_TOKEN");
    if (!tok || !*tok) tok = std::getenv("GH_TOKEN");
    std::string bearer;
    if (tok && *tok) {
        bearer = tok;
    } else {
        std::lock_guard<std::mutex> lock(g_github_token_mu);
        bearer = g_github_token_config;
    }
    if (!bearer.empty()) h.emplace_back("Authorization", std::string("Bearer ") + bearer);
    return h;
}

HttpResponse http_get(const std::string& url,
                      const std::vector<std::pair<std::string, std::string>>& headers) {
    HttpResponse res;
    CURL* curl = make_easy(url);
    curl_slist* hdrs = nullptr;
    apply_headers(curl, headers, &hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res.body);
    const CURLcode code = curl_easy_perform(curl);
    if (code != CURLE_OK) {
        res.error = curl_easy_strerror(code);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.status);
        if (res.status < 200 || res.status >= 300) {
            res.error = "HTTP " + std::to_string(res.status);
            if ((res.status == 403 || res.status == 429) &&
                url.find("api.github.com") != std::string::npos) {
                res.error +=
                    " — GitHub API rate limit. Set a PAT in Library Settings → GitHub token "
                    "(or export GITHUB_TOKEN / GH_TOKEN), then try again.";
            }
        }
    }
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return res;
}

bool http_download(const std::string& url, const fs::path& dest, std::string* error,
                   const std::vector<std::pair<std::string, std::string>>& headers,
                   HttpProgressFn on_progress, std::uint64_t expected_size) {
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);

    if (expected_size > 0 && fs::is_regular_file(dest, ec)) {
        const auto sz = fs::file_size(dest, ec);
        if (!ec && sz == expected_size) {
            if (on_progress) on_progress(expected_size, expected_size);
            return true;
        }
        // Wrong size — treat as corrupt and replace.
        fs::remove(dest, ec);
    }

    const fs::path part = fs::path(dest.string() + ".part");
    curl_off_t resume_from = 0;
    if (fs::is_regular_file(part, ec)) {
        const auto psz = fs::file_size(part, ec);
        if (!ec && psz > 0) {
            if (expected_size > 0 && static_cast<std::uint64_t>(psz) == expected_size) {
                fs::rename(part, dest, ec);
                if (ec) {
                    if (error) *error = "rename failed: " + ec.message();
                    return false;
                }
                if (on_progress) on_progress(expected_size, expected_size);
                return true;
            }
            if (expected_size > 0 && static_cast<std::uint64_t>(psz) > expected_size) {
                fs::remove(part, ec);
            } else {
                resume_from = static_cast<curl_off_t>(psz);
            }
        }
    }

    long status = 0;
    std::string err;
    if (!http_download_once(url, part, resume_from, headers, on_progress, &status, &err)) {
        // Stale/unsupported Range — retry from scratch once.
        const bool range_issue =
            resume_from > 0 && (status == 416 || status == 400 ||
                                err.find("Range") != std::string::npos);
        if (range_issue) {
            fs::remove(part, ec);
            err.clear();
            status = 0;
            if (!http_download_once(url, part, 0, headers, std::move(on_progress), &status,
                                    &err)) {
                if (error) *error = err;
                fs::remove(part, ec);
                return false;
            }
        } else {
            if (error) *error = err;
            // Keep .part for a later resume unless the server rejected the request body.
            if (status >= 400 && status != 416) fs::remove(part, ec);
            return false;
        }
    }

    if (expected_size > 0) {
        const auto sz = fs::file_size(part, ec);
        if (ec || sz != expected_size) {
            if (error)
                *error = "download size mismatch (got " +
                         (ec ? std::string("?") : std::to_string(sz)) + ", expected " +
                         std::to_string(expected_size) + ")";
            fs::remove(part, ec);
            return false;
        }
    }

    fs::remove(dest, ec); // replace if present
    fs::rename(part, dest, ec);
    if (ec) {
        if (error) *error = "rename failed: " + ec.message();
        fs::remove(part, ec);
        return false;
    }
    return true;
}

HttpResponse http_post_multipart(
    const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
    const std::vector<HttpMultipartFile>& files,
    const std::vector<std::pair<std::string, std::string>>& fields) {
    HttpResponse res;
    CURL* curl = make_easy(url);
    curl_slist* hdrs = nullptr;
    // Do not force Content-Type — curl sets multipart boundary.
    apply_headers(curl, headers, &hdrs);

    curl_mime* mime = curl_mime_init(curl);
    if (!mime) {
        res.error = "curl_mime_init failed";
        if (hdrs) curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
        return res;
    }

    for (const auto& f : fields) {
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, f.first.c_str());
        curl_mime_data(part, f.second.c_str(), CURL_ZERO_TERMINATED);
    }
    for (const auto& f : files) {
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, f.field.c_str());
        curl_mime_filedata(part, f.path.string().c_str());
        const std::string fname =
            f.filename.empty() ? f.path.filename().string() : f.filename;
        if (!fname.empty()) curl_mime_filename(part, fname.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res.body);

    const CURLcode code = curl_easy_perform(curl);
    if (code != CURLE_OK) {
        res.error = curl_easy_strerror(code);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.status);
        if (res.status < 200 || res.status >= 300)
            res.error = "HTTP " + std::to_string(res.status);
    }

    curl_mime_free(mime);
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return res;
}

} // namespace retcomm
