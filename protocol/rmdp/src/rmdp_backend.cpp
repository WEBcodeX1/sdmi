#include "rmdp_backend.hpp"
#include "rmdp_s3.hpp"
#include "rmdp_local_files.hpp"
#include "rmdp_common.hpp"

#include <memory>
#include <stdexcept>

namespace rmdp {

// ---------------------------------------------------------------------------
// S3Backend – wraps S3Client as an IStorageBackend
// ---------------------------------------------------------------------------

class S3Backend : public IStorageBackend {
public:
    explicit S3Backend(const S3Config& cfg) : client_(cfg) {}

    std::string getObject(const std::string& key) override {
        return client_.getObject(key);
    }

    bool putObject(const std::string& key,
                   const std::string& body,
                   const std::string& content_type = "application/json") override {
        return client_.putObject(key, body, content_type);
    }

    std::vector<std::string> listObjects(const std::string& prefix) override {
        return client_.listObjects(prefix);
    }

private:
    S3Client client_;
};

// ---------------------------------------------------------------------------
// Factory function
// ---------------------------------------------------------------------------

std::unique_ptr<IStorageBackend> makeStorageBackend(const ClientConfig& cfg) {
    if (cfg.global.synctype == SyncType::LocalFiles)
        return std::make_unique<LocalFilesBackend>(cfg.local_files);
    return std::make_unique<S3Backend>(cfg.s3);
}

std::unique_ptr<IStorageBackend> makeStorageBackend(const ServerConfig& cfg) {
    if (cfg.global.synctype == SyncType::LocalFiles)
        return std::make_unique<LocalFilesBackend>(cfg.local_files);
    return std::make_unique<S3Backend>(cfg.s3);
}

} // namespace rmdp
