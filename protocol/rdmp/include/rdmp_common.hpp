#pragma once

#include <cstdint>
#include <map>
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
    HEARTBEAT     = 0x02,
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
// Configuration structures  (loaded from INI-style config files)
// ---------------------------------------------------------------------------

struct MulticastConfig {
    std::string group          = "239.1.2.3";
    uint16_t    port           = 5000;
    uint8_t     ttl            = 32;
    std::string iface          = "";      // bind interface (optional)
};

struct S3Config {
    std::string endpoint   = "http://localhost:9000";
    std::string bucket     = "rdmp-tasks";
    std::string access_key = "";
    std::string secret_key = "";
    std::string region     = "us-east-1";
};

struct TimeoutConfig {
    uint32_t task_execution_ms          = 5000;
    uint32_t s3_poll_interval_ms        = 1000;
    uint32_t degradation_threshold_ms   = 2000;
    uint32_t watchdog_interval_ms       = 2000;
    uint32_t retry_delay_ms             = 3000;
    uint32_t multicast_repeat_count     = 3;
    uint32_t multicast_repeat_interval_ms = 100;
};

struct ClientConfig {
    MulticastConfig multicast;
    S3Config        s3;
    TimeoutConfig   timeouts;
    std::string     node_id = "client1";
};

struct ServerConfig {
    MulticastConfig multicast;
    S3Config        s3;
    TimeoutConfig   timeouts;
    std::string     node_id = "server1";
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

// ---------------------------------------------------------------------------
// Config loaders
// ---------------------------------------------------------------------------

ClientConfig loadClientConfig(const std::string& path);
ServerConfig loadServerConfig(const std::string& path);

} // namespace rdmp
