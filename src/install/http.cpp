#include "retcomm/http.hpp"

#include <curl/curl.h>

#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace retcomm {
namespace {

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
};

int xferinfo(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* b = static_cast<ProgressBridge*>(clientp);
    if (b && b->fn) b->fn(static_cast<std::uint64_t>(dlnow), static_cast<std::uint64_t>(dltotal));
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
#if LIBCURL_VERSION_NUM >= 0x074700
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
#endif
    return curl;
}

} // namespace

std::vector<std::pair<std::string, std::string>> github_http_headers() {
    std::vector<std::pair<std::string, std::string>> h;
    h.emplace_back("Accept", "application/vnd.github+json");
    h.emplace_back("X-GitHub-Api-Version", "2022-11-28");
    const char* tok = std::getenv("GITHUB_TOKEN");
    if (!tok || !*tok) tok = std::getenv("GH_TOKEN");
    if (tok && *tok) h.emplace_back("Authorization", std::string("Bearer ") + tok);
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
        if (res.status < 200 || res.status >= 300)
            res.error = "HTTP " + std::to_string(res.status);
    }
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return res;
}

bool http_download(const std::string& url, const fs::path& dest, std::string* error,
                   const std::vector<std::pair<std::string, std::string>>& headers,
                   HttpProgressFn on_progress) {
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    const fs::path part = dest.string() + ".part";
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot write " + part.string();
        return false;
    }

    CURL* curl = make_easy(url);
    curl_slist* hdrs = nullptr;
    apply_headers(curl, headers, &hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);

    ProgressBridge bridge{std::move(on_progress)};
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

    if (code != CURLE_OK) {
        if (error) *error = curl_easy_strerror(code);
        fs::remove(part, ec);
        return false;
    }
    if (status < 200 || status >= 300) {
        if (error) *error = "HTTP " + std::to_string(status);
        fs::remove(part, ec);
        return false;
    }

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
