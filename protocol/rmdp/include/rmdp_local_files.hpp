#pragma once

#include "rmdp_backend.hpp"
#include "rmdp_common.hpp"

#include <string>
#include <vector>

namespace rmdp {

// ---------------------------------------------------------------------------
// LocalFilesBackend
//
// A filesystem-based implementation of IStorageBackend suitable for testing
// without a real S3 / Ceph cluster.
//
// Objects are stored as plain files under base_path/:
//   base_path/tasks/<uuid>   – task JSON
//   base_path/status/<uuid>  – status JSON
//
// File writes are atomic (write to a temp file then rename) to avoid
// partially-written files being visible to concurrent readers.
// ---------------------------------------------------------------------------

class LocalFilesBackend : public IStorageBackend {
public:
    explicit LocalFilesBackend(const LocalFilesConfig& config);

    std::string getObject(const std::string& key) override;

    bool putObject(const std::string& key,
                   const std::string& body,
                   const std::string& content_type = "application/json") override;

    std::vector<std::string> listObjects(const std::string& prefix) override;

private:
    std::string base_path_;

    // Resolve a logical key (e.g. "tasks/uuid") to an absolute file path.
    std::string resolvePath(const std::string& key) const;

    // Ensure the directory for 'file_path' exists.
    static void ensureDir(const std::string& file_path);
};

} // namespace rmdp
