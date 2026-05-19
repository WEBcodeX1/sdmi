#pragma once

#include <cstdint>
#include <map>
#include <ostream>
#include <string>
#include <vector>

namespace rdmp {

// ---------------------------------------------------------------------------
// Protocol wire-format constants
// ---------------------------------------------------------------------------

static constexpr uint32_t RDMP_MAGIC   = 0x52444D50u; // 'R','D','M','P'
static constexpr uint8_t  RDMP_VERSION = 0x01u;

// Minimum size of a valid RDMP UDP datagram:
//   4 (magic) + 1 (version) + 1 (msg_type) + 36 (uuid) + 4 (payload_len) = 46
static constexpr size_t RDMP_HEADER_SIZE = 46u;

// ---------------------------------------------------------------------------
// Message types (1 byte in wire format)
// ---------------------------------------------------------------------------

enum class MsgType : uint8_t {
    TASK_ANNOUNCE = 0x01,
};

// ---------------------------------------------------------------------------
// Task status
// ---------------------------------------------------------------------------

enum class TaskStatus {
    UNKNOWN,
    PENDING,
    EXECUTING,
    COMPLETED,
    FAILED,
};

std::string    taskStatusToString(TaskStatus s);
TaskStatus     stringToTaskStatus(const std::string& s);

// ---------------------------------------------------------------------------
// Configuration structures  (loaded from JSON config files)
// ---------------------------------------------------------------------------

// Sync backend selector: "s3" or "local-files"
enum class SyncType { S3, LocalFiles };

struct GlobalConfig {
    SyncType synctype = SyncType::S3;
};

struct MulticastConfig {
    std::string group          = "239.1.2.3";
    uint16_t    port           = 5000;
    uint8_t     ttl            = 32;
    std::string iface          = "";      // bind interface (optional)
};

struct S3Config {
    // Primary endpoint (used when 'endpoints' is empty).
    std::string              endpoint   = "http://localhost:9000";
    // Optional list of fallback endpoints tried in order on timeout.
    std::vector<std::string> endpoints;
    // Per-request timeout in milliseconds (0 = library default ~10 s).
    uint32_t                 max_answer_timeout_ms = 10000;
    std::string              bucket     = "rdmp-tasks";
    std::string              access_key = "";
    std::string              secret_key = "";
    std::string              region     = "us-east-1";
};

struct LocalFilesConfig {
    std::string base_path = "/tmp/rdmp-tasks";
};

struct TimeoutConfig {
    uint32_t task_execution_ms          = 5000;
    uint32_t s3_poll_interval_ms        = 1000;
    uint32_t retry_delay_ms             = 3000;
    uint32_t multicast_repeat_count     = 3;
    uint32_t multicast_repeat_interval_ms = 100;
};

struct ClientConfig {
    GlobalConfig         global;
    MulticastConfig      multicast;
    S3Config             s3;
    LocalFilesConfig     local_files;
    TimeoutConfig        timeouts;
    std::string          node_id = "client1";
};

struct ServerConfig {
    GlobalConfig         global;
    MulticastConfig      multicast;
    S3Config             s3;
    LocalFilesConfig     local_files;
    TimeoutConfig        timeouts;
    std::string          node_id = "server1";
    // When true: server skips the S3 pending-status check (2.4) and executes
    // every task it receives, writing its own per-server status record.
    // Intended for latency-sensitive deployments where duplicate execution is
    // acceptable and de-duplication is handled by the application layer.
    bool                 bypass_pending_check = false;
};

// ---------------------------------------------------------------------------
// Domain objects
// ---------------------------------------------------------------------------

struct Task {
    std::string uuid;
    std::string payload;
    std::string created_by;
    std::string created_at;
};

struct TaskStatusRecord {
    std::string uuid;
    TaskStatus  status      = TaskStatus::UNKNOWN;
    std::string server_id;
    std::string updated_at;
    std::string result;
};

// ---------------------------------------------------------------------------
// Utility functions
// ---------------------------------------------------------------------------

std::string generateUUID();
std::string currentTimestamp();      // ISO-8601 UTC
int64_t     currentTimeMs();         // epoch milliseconds

// Minimal JSON helpers (for the simple, well-known key/value payloads used internally)
std::string buildTaskJson(const Task& t);
std::string buildStatusJson(const TaskStatusRecord& r);
Task             parseTask(const std::string& json);
TaskStatusRecord parseTaskStatus(const std::string& json);
std::string      extractJsonField(const std::string& json, const std::string& key);

// Stream operators for enum types (required by Boost::Test BOOST_CHECK_EQUAL)
inline std::ostream& operator<<(std::ostream& os, SyncType s) {
    switch (s) {
    case SyncType::S3:         return os << "S3";
    case SyncType::LocalFiles: return os << "LocalFiles";
    default:                   return os << "Unknown";
    }
}

inline std::ostream& operator<<(std::ostream& os, TaskStatus s) {
    switch (s) {
    case TaskStatus::UNKNOWN:   return os << "UNKNOWN";
    case TaskStatus::PENDING:   return os << "PENDING";
    case TaskStatus::EXECUTING: return os << "EXECUTING";
    case TaskStatus::COMPLETED: return os << "COMPLETED";
    case TaskStatus::FAILED:    return os << "FAILED";
    default:                    return os << "?";
    }
}

inline std::ostream& operator<<(std::ostream& os, MsgType m) {
    switch (m) {
    case MsgType::TASK_ANNOUNCE: return os << "TASK_ANNOUNCE";
    default:                     return os << "Unknown";
    }
}

// ---------------------------------------------------------------------------
// Config loaders
// ---------------------------------------------------------------------------

ClientConfig loadClientConfig(const std::string& path);
ServerConfig loadServerConfig(const std::string& path);

} // namespace rdmp
