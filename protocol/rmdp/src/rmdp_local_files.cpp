#include "rmdp_local_files.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace rmdp {

LocalFilesBackend::LocalFilesBackend(const LocalFilesConfig& config)
    : base_path_(config.base_path) {
    // Create base directory tree if it does not exist
    fs::create_directories(base_path_);
}

// ---------------------------------------------------------------------------
// Path resolution
// ---------------------------------------------------------------------------

std::string LocalFilesBackend::resolvePath(const std::string& key) const {
    // Normalise the key: strip leading slashes to avoid escaping base_path_.
    std::string safe_key = key;
    while (!safe_key.empty() && safe_key.front() == '/') safe_key.erase(0, 1);
    return base_path_ + "/" + safe_key;
}

void LocalFilesBackend::ensureDir(const std::string& file_path) {
    fs::path p(file_path);
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path());
    }
}

// ---------------------------------------------------------------------------
// IStorageBackend interface
// ---------------------------------------------------------------------------

std::string LocalFilesBackend::getObject(const std::string& key) {
    const std::string path = resolvePath(key);
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool LocalFilesBackend::putObject(const std::string& key,
                                   const std::string& body,
                                   const std::string& /*content_type*/) {
    const std::string path = resolvePath(key);
    ensureDir(path);

    // Atomic write: write to a temp file first, then rename.
    const std::string tmp_path = path + ".tmp";
    {
        std::ofstream f(tmp_path, std::ios::binary | std::ios::trunc);
        if (!f) {
            std::cerr << "[LocalFiles] Cannot open for write: " << tmp_path << "\n";
            return false;
        }
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!f) {
            std::cerr << "[LocalFiles] Write failed: " << tmp_path << "\n";
            return false;
        }
    }

    std::error_code ec;
    fs::rename(tmp_path, path, ec);
    if (ec) {
        std::cerr << "[LocalFiles] rename failed: " << ec.message() << "\n";
        return false;
    }
    return true;
}

std::vector<std::string> LocalFilesBackend::listObjects(const std::string& prefix) {
    // S3 listObjects with a prefix returns ALL objects whose key starts with
    // the prefix, regardless of nesting depth.  Mirror this by recursively
    // walking the directory tree rooted at the prefix directory.
    const std::string dir_path = resolvePath(prefix.empty() ? "" : prefix);

    std::vector<std::string> keys;

    // Strip trailing slash from dir_path for the directory scan.
    std::string scan_dir = dir_path;
    while (!scan_dir.empty() && scan_dir.back() == '/') scan_dir.pop_back();

    std::error_code ec;
    if (!fs::exists(scan_dir, ec) || !fs::is_directory(scan_dir, ec)) return keys;

    // Recursive walk
    for (const auto& entry : fs::recursive_directory_iterator(scan_dir, ec)) {
        if (!entry.is_regular_file()) continue;
        // Skip temp files
        const std::string fname = entry.path().filename().string();
        if (fname.size() > 4 && fname.substr(fname.size() - 4) == ".tmp") continue;

        // Reconstruct the logical key relative to base_path_
        // entry.path() is an absolute path under base_path_/prefix/…
        const std::string full = entry.path().string();
        if (full.size() <= base_path_.size() + 1) continue;
        std::string rel = full.substr(base_path_.size() + 1); // strip base_path_/
        keys.push_back(rel);
    }
    return keys;
}

} // namespace rmdp
