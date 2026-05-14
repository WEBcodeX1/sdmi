#pragma once

#include "rdmp_common.hpp"

#include <string>
#include <vector>

// Forward-declare CURL to avoid pulling in curl headers in every TU.
typedef void CURL;

namespace rdmp {

// ---------------------------------------------------------------------------
// S3Client
//
// A minimal HTTP client for Ceph / AWS S3 compatible object storage.
// Supports path-style URLs: http://<endpoint>/<bucket>/<key>
//
// Authentication: AWS Signature Version 4 (when access_key is non-empty).
//                 When access_key is empty, requests are sent without auth
//                 headers (useful for anonymous / pre-authed Ceph deployments).
// ---------------------------------------------------------------------------

class S3Client {
public:
    explicit S3Client(const S3Config& config);
    ~S3Client();

    // Retrieve the object at 'key'.  Returns the object body on success, or
    // an empty string when the object does not exist / on error.
    std::string getObject(const std::string& key);

    // Store 'body' at 'key'.  Returns true on success (HTTP 200/204).
    bool putObject(const std::string& key,
                   const std::string& body,
                   const std::string& content_type = "application/json");

    // List all keys under 'prefix'.  Returns the vector of matching keys.
    std::vector<std::string> listObjects(const std::string& prefix);

private:
    S3Config    config_;
    CURL*       curl_;

    // Build the full path-style URL for a given object key.
    std::string objectUrl(const std::string& key) const;

    // Build the list URL for a given key prefix.
    std::string listUrl(const std::string& prefix) const;

    // AWS Signature V4 helpers
    static std::string sha256hex(const std::string& data);
    static std::string hmacSha256raw(const std::string& key,
                                     const std::string& msg);
    static std::string hmacSha256hex(const std::string& key,
                                     const std::string& msg);

    // Build and return the AWS Authorization header value.
    // 'signed_headers_map' must already contain host + x-amz-date (and
    //  optionally x-amz-content-sha256); the function appends all signed
    //  headers to the returned header string.
    std::string buildAuthHeader(const std::string& http_method,
                                const std::string& canonical_uri,
                                const std::string& canonical_query,
                                const std::string& body,
                                const std::string& datetime_str,
                                const std::string& date_str) const;

    // Extract all <Key>…</Key> elements from an S3 ListBucket XML response.
    static std::vector<std::string> parseListXml(const std::string& xml);
};

} // namespace rdmp
