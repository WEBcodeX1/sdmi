#pragma once

#include <memory>
#include <string>
#include <vector>

namespace rdmp {

// Forward declarations to avoid circular headers
struct ClientConfig;
struct ServerConfig;

// ---------------------------------------------------------------------------
// IStorageBackend
//
// Abstract interface for task/status object storage.  The S3 backend
// (S3Client) and the local-files backend (LocalFilesBackend) both implement
// this interface, allowing the client and server to switch backends via
// configuration without any other code changes.
// ---------------------------------------------------------------------------

class IStorageBackend {
public:
    virtual ~IStorageBackend() = default;

    // Retrieve the object at 'key'.  Returns the body on success, or an
    // empty string when the object does not exist / on error.
    virtual std::string getObject(const std::string& key) = 0;

    // Store 'body' at 'key'.  Returns true on success.
    virtual bool putObject(const std::string& key,
                           const std::string& body,
                           const std::string& content_type = "application/json") = 0;

    // List all keys with the given prefix.  Returns matching key strings.
    virtual std::vector<std::string> listObjects(const std::string& prefix) = 0;

    // Process any pending incoming data (e.g. drain the receive socket for
    // multicast-reply backends).  No-op for S3 and local-files backends.
    virtual void sync() {}
};

// ---------------------------------------------------------------------------
// Factory functions – create the appropriate backend from config
// ---------------------------------------------------------------------------

std::unique_ptr<IStorageBackend> makeStorageBackend(const ClientConfig& cfg);
std::unique_ptr<IStorageBackend> makeStorageBackend(const ServerConfig& cfg);

} // namespace rdmp
