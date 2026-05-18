#include "rdmp_s3.hpp"

#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

namespace rdmp {
namespace {

// ---------------------------------------------------------------------------
// libcurl write-callback: accumulates response body into a std::string.
// ---------------------------------------------------------------------------

size_t curlWrite(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

// ---------------------------------------------------------------------------
// Cryptographic helpers
// ---------------------------------------------------------------------------

std::string sha256hex(const std::string& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()),
           data.size(), hash);
    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(hash[i]);
    return oss.str();
}

std::string hmacSha256raw(const std::string& key, const std::string& msg) {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int  len = 0;
    HMAC(EVP_sha256(),
         reinterpret_cast<const unsigned char*>(key.data()), key.size(),
         reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
         result, &len);
    return std::string(reinterpret_cast<char*>(result), len);
}

std::string hmacSha256hex(const std::string& key, const std::string& msg) {
    std::string raw = hmacSha256raw(key, msg);
    std::ostringstream oss;
    for (unsigned char c : raw)
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(c);
    return oss.str();
}

// ---------------------------------------------------------------------------
// URL helpers
// ---------------------------------------------------------------------------

// Extract hostname (without port) from http[s]://host[:port]/…
std::string hostFromUrl(const std::string& url) {
    size_t s = url.find("://");
    if (s == std::string::npos) return url;
    s += 3;
    size_t e = url.find_first_of(":/", s);
    return url.substr(s, (e == std::string::npos) ? std::string::npos : e - s);
}

// Extract the full path+query component starting at the first '/' after scheme.
std::string pathFromUrl(const std::string& url) {
    size_t s = url.find("://");
    if (s == std::string::npos) return "/";
    s += 3;
    size_t p = url.find('/', s);
    return (p == std::string::npos) ? "/" : url.substr(p);
}

// ---------------------------------------------------------------------------
// AWS dates
// ---------------------------------------------------------------------------

void awsDateStrings(std::string& datetime_str, std::string& date_str) {
    time_t t = time(nullptr);
    struct tm utc = {};
    gmtime_r(&t, &utc);
    char dt[17], d[9];
    strftime(dt, sizeof(dt), "%Y%m%dT%H%M%SZ", &utc);
    strftime(d,  sizeof(d),  "%Y%m%d",          &utc);
    datetime_str = dt;
    date_str     = d;
}

// ---------------------------------------------------------------------------
// AWS Signature Version 4 – build Authorization header
// ---------------------------------------------------------------------------

std::string buildAWSAuth(const S3Config&    config,
                          const std::string& http_method,
                          const std::string& url,
                          const std::string& body,
                          const std::string& datetime_str,
                          const std::string& date_str) {
    const std::string host    = hostFromUrl(config.endpoint);
    const std::string service = "s3";
    const std::string ph      = sha256hex(body);

    // Split path from query
    std::string full_path = pathFromUrl(url);
    std::string canonical_query;
    std::string canonical_uri = full_path;
    size_t qpos = full_path.find('?');
    if (qpos != std::string::npos) {
        canonical_query = full_path.substr(qpos + 1);
        canonical_uri   = full_path.substr(0, qpos);
    }

    const std::string canon_hdrs =
        "host:"                  + host         + "\n"
        "x-amz-content-sha256:" + ph           + "\n"
        "x-amz-date:"           + datetime_str + "\n";
    const std::string signed_hdrs = "host;x-amz-content-sha256;x-amz-date";

    const std::string canonical_request =
        http_method         + "\n"
        + canonical_uri     + "\n"
        + canonical_query   + "\n"
        + canon_hdrs        + "\n"
        + signed_hdrs       + "\n"
        + ph;

    const std::string cred_scope =
        date_str + "/" + config.region + "/" + service + "/aws4_request";

    const std::string string_to_sign =
        "AWS4-HMAC-SHA256\n"
        + datetime_str     + "\n"
        + cred_scope       + "\n"
        + sha256hex(canonical_request);

    const std::string k_date    = hmacSha256raw("AWS4" + config.secret_key, date_str);
    const std::string k_region  = hmacSha256raw(k_date,   config.region);
    const std::string k_service = hmacSha256raw(k_region, service);
    const std::string k_signing = hmacSha256raw(k_service, "aws4_request");
    const std::string signature = hmacSha256hex(k_signing, string_to_sign);

    return "AWS4-HMAC-SHA256 Credential=" + config.access_key + "/"
           + cred_scope + ", SignedHeaders=" + signed_hdrs
           + ", Signature=" + signature;
}

// ---------------------------------------------------------------------------
// Execute a libcurl request and return the response body.
// ---------------------------------------------------------------------------

std::string curlPerform(CURL*              curl,
                         const std::string& url,
                         struct curl_slist* headers,
                         const std::string* put_body,
                         long*              http_code_out,
                         long               timeout_ms = 10000) {
    std::string response;
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &response);
    // Use millisecond-precision timeout when available, fall back to seconds.
    const long timeout_sec = (timeout_ms + 999) / 1000;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        timeout_sec > 0 ? timeout_sec : 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    if (put_body) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    put_body->c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(put_body->size()));
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        std::cerr << "[RDMP/S3] curl error: " << curl_easy_strerror(rc) << "\n";
        if (http_code_out) *http_code_out = 0;
        return "";
    }
    if (http_code_out)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code_out);
    return response;
}

// Assemble HTTP headers (with optional AWS Sig V4) into a curl_slist.
struct curl_slist* makeHeaders(const S3Config&    config,
                                const std::string& http_method,
                                const std::string& url,
                                const std::string& body,
                                const std::string& content_type) {
    std::string datetime_str, date_str;
    awsDateStrings(datetime_str, date_str);

    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, ("Content-Type: " + content_type).c_str());
    hdrs = curl_slist_append(hdrs, ("x-amz-date: " + datetime_str).c_str());
    hdrs = curl_slist_append(hdrs,
        ("x-amz-content-sha256: " + sha256hex(body)).c_str());

    if (!config.access_key.empty()) {
        std::string auth = buildAWSAuth(config, http_method, url, body,
                                        datetime_str, date_str);
        hdrs = curl_slist_append(hdrs, ("Authorization: " + auth).c_str());
    }
    return hdrs;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// S3Client – constructor / destructor
// ---------------------------------------------------------------------------

S3Client::S3Client(const S3Config& config) : config_(config), curl_(nullptr) {
    // Build the effective endpoint list: primary endpoint first, then extras.
    endpoints_.push_back(config_.endpoint);
    for (const auto& ep : config_.endpoints)
        if (ep != config_.endpoint)
            endpoints_.push_back(ep);
    active_endpoint_idx_ = 0;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl_ = curl_easy_init();
    if (!curl_) throw std::runtime_error("Failed to initialise libcurl");
}

S3Client::~S3Client() {
    if (curl_) {
        curl_easy_cleanup(static_cast<CURL*>(curl_));
        curl_ = nullptr;
    }
    curl_global_cleanup();
}

// ---------------------------------------------------------------------------
// Active-endpoint management
// ---------------------------------------------------------------------------

const std::string& S3Client::activeEndpoint() const {
    return endpoints_[active_endpoint_idx_];
}

void S3Client::rotateEndpoint() {
    if (endpoints_.size() <= 1) return;
    active_endpoint_idx_ = (active_endpoint_idx_ + 1) % endpoints_.size();
    std::cerr << "[RDMP/S3] Switching to endpoint: "
              << activeEndpoint() << "\n";
}

// ---------------------------------------------------------------------------
// URL builders (use the currently active endpoint)
// ---------------------------------------------------------------------------

std::string S3Client::objectUrl(const std::string& key) const {
    return activeEndpoint() + "/" + config_.bucket + "/" + key;
}

std::string S3Client::listUrl(const std::string& prefix) const {
    return activeEndpoint() + "/" + config_.bucket
           + "?list-type=2&prefix=" + prefix + "&max-keys=1000";
}

// ---------------------------------------------------------------------------
// Helper: perform a request, rotating endpoints on failure
// ---------------------------------------------------------------------------

std::string S3Client::performWithFailover(const std::string& initial_url,
                                           const std::string& http_method,
                                           const std::string& body,
                                           const std::string& content_type,
                                           long*              http_code_out) {
    const long timeout_ms = static_cast<long>(config_.max_answer_timeout_ms);

    // Try every endpoint (starting from the current active one).
    for (size_t attempt = 0; attempt < endpoints_.size(); ++attempt) {
        // Build the URL for the current active endpoint.
        // initial_url already has the right path; we just swap the endpoint prefix.
        std::string url = initial_url;
        if (attempt > 0) {
            // Replace the primary endpoint prefix with the new active endpoint.
            // The path after the old endpoint is preserved.
            // We rebuild from the active endpoint + path extracted from initial_url.
            const std::string& first_ep = endpoints_[0];
            if (initial_url.compare(0, first_ep.size(), first_ep) == 0)
                url = activeEndpoint() + initial_url.substr(first_ep.size());
        }

        struct curl_slist* hdrs =
            makeHeaders(config_, http_method, url, body, content_type);

        long code = 0;
        const std::string* put_ptr =
            (http_method == "PUT") ? &body : nullptr;
        std::string response = curlPerform(static_cast<CURL*>(curl_),
                                            url, hdrs, put_ptr, &code,
                                            timeout_ms);
        curl_slist_free_all(hdrs);

        if (code != 0) {
            // Got a real HTTP response – success or server error, return it.
            if (http_code_out) *http_code_out = code;
            return response;
        }

        // Network / timeout failure – try next endpoint.
        rotateEndpoint();
    }

    if (http_code_out) *http_code_out = 0;
    return "";
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::string S3Client::getObject(const std::string& key) {
    const std::string url = objectUrl(key);
    long code = 0;
    std::string body = performWithFailover(url, "GET", "", "application/json", &code);
    return (code == 200) ? body : "";
}

bool S3Client::putObject(const std::string& key,
                          const std::string& body,
                          const std::string& content_type) {
    const std::string url = objectUrl(key);
    long code = 0;
    performWithFailover(url, "PUT", body, content_type, &code);
    return (code == 200 || code == 204);
}

std::vector<std::string> S3Client::listObjects(const std::string& prefix) {
    const std::string url = listUrl(prefix);
    long code = 0;
    std::string xml = performWithFailover(url, "GET", "", "application/xml", &code);
    return (code == 200) ? parseListXml(xml) : std::vector<std::string>{};
}

std::vector<std::string> S3Client::parseListXml(const std::string& xml) {
    std::vector<std::string> keys;
    const std::string open  = "<Key>";
    const std::string close = "</Key>";
    size_t pos = 0;
    while ((pos = xml.find(open, pos)) != std::string::npos) {
        pos += open.size();
        size_t end = xml.find(close, pos);
        if (end == std::string::npos) break;
        keys.push_back(xml.substr(pos, end - pos));
        pos = end + close.size();
    }
    return keys;
}

// Expose static helpers (required by the header declaration).
std::string S3Client::sha256hex(const std::string& d) {
    return rdmp::sha256hex(d);
}
std::string S3Client::hmacSha256raw(const std::string& k, const std::string& m) {
    return rdmp::hmacSha256raw(k, m);
}
std::string S3Client::hmacSha256hex(const std::string& k, const std::string& m) {
    return rdmp::hmacSha256hex(k, m);
}
std::string S3Client::buildAuthHeader(const std::string& method,
                                      const std::string& uri,
                                      const std::string& query,
                                      const std::string& body,
                                      const std::string& datetime_str,
                                      const std::string& date_str) const {
    const std::string url = config_.endpoint + uri
                            + (query.empty() ? "" : "?" + query);
    return buildAWSAuth(config_, method, url, body, datetime_str, date_str);
}

} // namespace rdmp
