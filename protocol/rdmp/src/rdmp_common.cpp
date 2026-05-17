#include "rdmp_common.hpp"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

namespace rdmp {

// ---------------------------------------------------------------------------
// Task status string conversion
// ---------------------------------------------------------------------------

std::string taskStatusToString(TaskStatus s) {
    switch (s) {
    case TaskStatus::PENDING:   return "pending";
    case TaskStatus::EXECUTING: return "executing";
    case TaskStatus::COMPLETED: return "completed";
    case TaskStatus::FAILED:    return "failed";
    default:                    return "unknown";
    }
}

TaskStatus stringToTaskStatus(const std::string& s) {
    if (s == "pending")   return TaskStatus::PENDING;
    if (s == "executing") return TaskStatus::EXECUTING;
    if (s == "completed") return TaskStatus::COMPLETED;
    if (s == "failed")    return TaskStatus::FAILED;
    return TaskStatus::UNKNOWN;
}

// ---------------------------------------------------------------------------
// Time helpers
// ---------------------------------------------------------------------------

int64_t currentTimeMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               system_clock::now().time_since_epoch()).count();
}

std::string currentTimestamp() {
    int64_t ms     = currentTimeMs();
    time_t  t      = static_cast<time_t>(ms / 1000);
    struct tm utc  = {};
    gmtime_r(&t, &utc);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// UUID v4 generation
// ---------------------------------------------------------------------------

std::string generateUUID() {
    unsigned char b[16] = {};

    // Prefer /dev/urandom for cryptographic-quality randomness.
    FILE* f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t rd = fread(b, 1, 16, f);
        fclose(f);
        if (rd != 16) {
            // /dev/urandom partial read is extremely rare; fall back to
            // std::random_device for the remaining bytes.
            std::random_device rdev;
            std::uniform_int_distribution<unsigned int> dist(0, 255);
            for (size_t i = rd; i < 16; ++i) b[i] = static_cast<unsigned char>(dist(rdev));
        }
    } else {
        // No /dev/urandom – use std::random_device (may be hardware-backed).
        std::random_device rdev;
        std::uniform_int_distribution<unsigned int> dist(0, 255);
        for (int i = 0; i < 16; ++i) b[i] = static_cast<unsigned char>(dist(rdev));
    }

    // Set version 4 and variant bits
    b[6] = (b[6] & 0x0Fu) | 0x40u;
    b[8] = (b[8] & 0x3Fu) | 0x80u;

    char out[37];
    snprintf(out, sizeof(out),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             b[0],  b[1],  b[2],  b[3],
             b[4],  b[5],  b[6],  b[7],
             b[8],  b[9],  b[10], b[11],
             b[12], b[13], b[14], b[15]);
    return std::string(out);
}

// ---------------------------------------------------------------------------
// Minimal JSON helpers
// ---------------------------------------------------------------------------

// Escape a string value for embedding in a JSON payload.
static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '"')       out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else                out += static_cast<char>(c);
    }
    return out;
}

std::string buildTaskJson(const Task& t) {
    std::ostringstream o;
    o << "{"
      << "\"uuid\":\""       << jsonEscape(t.uuid)       << "\","
      << "\"payload\":\""    << jsonEscape(t.payload)    << "\","
      << "\"created_by\":\"" << jsonEscape(t.created_by) << "\","
      << "\"created_at\":\"" << jsonEscape(t.created_at) << "\""
      << "}";
    return o.str();
}

std::string buildStatusJson(const TaskStatusRecord& r) {
    std::ostringstream o;
    o << "{"
      << "\"uuid\":\""      << jsonEscape(r.uuid)                       << "\","
      << "\"status\":\""    << jsonEscape(taskStatusToString(r.status)) << "\","
      << "\"server_id\":\"" << jsonEscape(r.server_id)                  << "\","
      << "\"updated_at\":\"" << jsonEscape(r.updated_at)                << "\","
      << "\"result\":\""    << jsonEscape(r.result)                     << "\""
      << "}";
    return o.str();
}

std::string extractJsonField(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";

    pos += needle.size();
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";

    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size()) return "";

    if (json[pos] == '"') {
        ++pos;
        std::string val;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                ++pos;
                switch (json[pos]) {
                case '"':  val += '"';  break;
                case '\\': val += '\\'; break;
                case 'n':  val += '\n'; break;
                case 'r':  val += '\r'; break;
                case 't':  val += '\t'; break;
                default:   val += json[pos]; break;
                }
            } else {
                val += json[pos];
            }
            ++pos;
        }
        return val;
    }

    // Non-string value (number, bool, null)
    size_t end = pos;
    while (end < json.size() &&
           json[end] != ',' && json[end] != '}' && json[end] != ']') {
        ++end;
    }
    std::string val = json.substr(pos, end - pos);
    // Trim trailing whitespace
    size_t last = val.find_last_not_of(" \t\r\n");
    return (last == std::string::npos) ? "" : val.substr(0, last + 1);
}

Task parseTask(const std::string& json) {
    Task t;
    t.uuid       = extractJsonField(json, "uuid");
    t.payload    = extractJsonField(json, "payload");
    t.created_by = extractJsonField(json, "created_by");
    t.created_at = extractJsonField(json, "created_at");
    return t;
}

TaskStatusRecord parseTaskStatus(const std::string& json) {
    TaskStatusRecord r;
    r.uuid       = extractJsonField(json, "uuid");
    r.status     = stringToTaskStatus(extractJsonField(json, "status"));
    r.server_id  = extractJsonField(json, "server_id");
    r.updated_at = extractJsonField(json, "updated_at");
    r.result     = extractJsonField(json, "result");
    return r;
}

// ---------------------------------------------------------------------------
// JSON config file loader (nlohmann::json)
// ---------------------------------------------------------------------------

using nlohmann::json;

static SyncType parseSyncType(const std::string& s) {
    if (s == "local-files")      return SyncType::LocalFiles;
    if (s == "multicast-reply")  return SyncType::MulticastReply;
    return SyncType::S3;
}

static GlobalConfig loadGlobal(const json& j) {
    GlobalConfig g;
    if (j.contains("global") && j["global"].is_object()) {
        const auto& gj = j["global"];
        g.synctype = parseSyncType(gj.value("synctype", "s3"));
    }
    return g;
}

static MulticastConfig loadMulticastJson(const json& j) {
    MulticastConfig m;
    if (!j.contains("multicast") || !j["multicast"].is_object()) return m;
    const auto& mj = j["multicast"];
    m.group = mj.value("group", m.group);
    m.port  = static_cast<uint16_t>(mj.value("port", static_cast<int>(m.port)));
    m.ttl   = static_cast<uint8_t>(mj.value("ttl",  static_cast<int>(m.ttl)));
    m.iface = mj.value("interface", m.iface);
    return m;
}

static S3Config loadS3Json(const json& j) {
    S3Config s;
    if (!j.contains("s3") || !j["s3"].is_object()) return s;
    const auto& sj = j["s3"];
    s.endpoint   = sj.value("endpoint",   s.endpoint);
    s.bucket     = sj.value("bucket",     s.bucket);
    s.access_key = sj.value("access_key", s.access_key);
    s.secret_key = sj.value("secret_key", s.secret_key);
    s.region     = sj.value("region",     s.region);
    return s;
}

static LocalFilesConfig loadLocalFilesJson(const json& j) {
    LocalFilesConfig lf;
    if (!j.contains("local_files") || !j["local_files"].is_object()) return lf;
    const auto& lfj = j["local_files"];
    lf.base_path = lfj.value("base_path", lf.base_path);
    return lf;
}

static MulticastReplyConfig loadMulticastReplyJson(const json& j) {
    MulticastReplyConfig mr;
    if (!j.contains("multicast_reply") || !j["multicast_reply"].is_object()) return mr;
    const auto& mrj = j["multicast_reply"];
    mr.group = mrj.value("group", mr.group);
    mr.port  = static_cast<uint16_t>(mrj.value("port", static_cast<int>(mr.port)));
    mr.ttl   = static_cast<uint8_t>(mrj.value("ttl",  static_cast<int>(mr.ttl)));
    mr.iface = mrj.value("interface", mr.iface);
    return mr;
}

static TimeoutConfig loadTimeoutsJson(const json& j) {
    TimeoutConfig t;
    if (!j.contains("timeouts") || !j["timeouts"].is_object()) return t;
    const auto& tj = j["timeouts"];
    t.task_execution_ms          = tj.value("task_execution_ms",          t.task_execution_ms);
    t.s3_poll_interval_ms        = tj.value("s3_poll_interval_ms",        t.s3_poll_interval_ms);
    t.degradation_threshold_ms   = tj.value("degradation_threshold_ms",   t.degradation_threshold_ms);
    t.watchdog_interval_ms       = tj.value("watchdog_interval_ms",        t.watchdog_interval_ms);
    t.retry_delay_ms             = tj.value("retry_delay_ms",              t.retry_delay_ms);
    t.multicast_repeat_count     = tj.value("multicast_repeat_count",      t.multicast_repeat_count);
    t.multicast_repeat_interval_ms = tj.value("multicast_repeat_interval_ms",
                                               t.multicast_repeat_interval_ms);
    return t;
}

ClientConfig loadClientConfig(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open config file: " + path);
    json j;
    try {
        f >> j;
    } catch (const json::parse_error& e) {
        throw std::runtime_error("JSON parse error in " + path + ": " + e.what());
    }

    ClientConfig c;
    c.global          = loadGlobal(j);
    c.multicast       = loadMulticastJson(j);
    c.multicast_reply = loadMulticastReplyJson(j);
    c.s3              = loadS3Json(j);
    c.local_files     = loadLocalFilesJson(j);
    c.timeouts        = loadTimeoutsJson(j);
    if (j.contains("node") && j["node"].is_object())
        c.node_id = j["node"].value("id", c.node_id);
    return c;
}

ServerConfig loadServerConfig(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open config file: " + path);
    json j;
    try {
        f >> j;
    } catch (const json::parse_error& e) {
        throw std::runtime_error("JSON parse error in " + path + ": " + e.what());
    }

    ServerConfig s;
    s.global          = loadGlobal(j);
    s.multicast       = loadMulticastJson(j);
    s.multicast_reply = loadMulticastReplyJson(j);
    s.s3              = loadS3Json(j);
    s.local_files     = loadLocalFilesJson(j);
    s.timeouts        = loadTimeoutsJson(j);
    if (j.contains("node") && j["node"].is_object())
        s.node_id = j["node"].value("id", s.node_id);
    return s;
}

} // namespace rdmp
