#include "rdmp_common.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
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
// INI-style config file parser
// ---------------------------------------------------------------------------

static std::string trimStr(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::map<std::string, std::string> parseIniFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open config file: " + path);

    std::map<std::string, std::string> kv;
    std::string section;
    std::string line;

    while (std::getline(f, line)) {
        line = trimStr(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        if (line.front() == '[') {
            size_t end = line.find(']');
            section = (end != std::string::npos) ? line.substr(1, end - 1) : line.substr(1);
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string k = trimStr(line.substr(0, eq));
        std::string v = trimStr(line.substr(eq + 1));
        kv[section + "." + k] = v;
    }
    return kv;
}

static uint32_t cfgUint(const std::map<std::string, std::string>& kv,
                         const std::string& key, uint32_t def) {
    auto it = kv.find(key);
    if (it == kv.end() || it->second.empty()) return def;
    return static_cast<uint32_t>(std::stoul(it->second));
}

static std::string cfgStr(const std::map<std::string, std::string>& kv,
                            const std::string& key, const std::string& def = "") {
    auto it = kv.find(key);
    if (it == kv.end()) return def;
    return it->second;
}

static MulticastConfig loadMulticast(const std::map<std::string, std::string>& kv) {
    MulticastConfig m;
    m.group = cfgStr(kv, "multicast.group", m.group);
    m.port  = static_cast<uint16_t>(cfgUint(kv, "multicast.port", m.port));
    m.ttl   = static_cast<uint8_t>(cfgUint(kv, "multicast.ttl",  m.ttl));
    m.iface = cfgStr(kv, "multicast.interface", m.iface);
    return m;
}

static S3Config loadS3(const std::map<std::string, std::string>& kv) {
    S3Config s;
    s.endpoint   = cfgStr(kv, "s3.endpoint",   s.endpoint);
    s.bucket     = cfgStr(kv, "s3.bucket",     s.bucket);
    s.access_key = cfgStr(kv, "s3.access_key", s.access_key);
    s.secret_key = cfgStr(kv, "s3.secret_key", s.secret_key);
    s.region     = cfgStr(kv, "s3.region",     s.region);
    return s;
}

static TimeoutConfig loadTimeouts(const std::map<std::string, std::string>& kv) {
    TimeoutConfig t;
    t.task_execution_ms          = cfgUint(kv, "timeouts.task_execution_ms",           t.task_execution_ms);
    t.s3_poll_interval_ms        = cfgUint(kv, "timeouts.s3_poll_interval_ms",         t.s3_poll_interval_ms);
    t.degradation_threshold_ms   = cfgUint(kv, "timeouts.degradation_threshold_ms",    t.degradation_threshold_ms);
    t.watchdog_interval_ms       = cfgUint(kv, "timeouts.watchdog_interval_ms",        t.watchdog_interval_ms);
    t.retry_delay_ms             = cfgUint(kv, "timeouts.retry_delay_ms",              t.retry_delay_ms);
    t.multicast_repeat_count     = cfgUint(kv, "timeouts.multicast_repeat_count",      t.multicast_repeat_count);
    t.multicast_repeat_interval_ms = cfgUint(kv, "timeouts.multicast_repeat_interval_ms",
                                              t.multicast_repeat_interval_ms);
    return t;
}

ClientConfig loadClientConfig(const std::string& path) {
    auto kv = parseIniFile(path);
    ClientConfig c;
    c.multicast = loadMulticast(kv);
    c.s3        = loadS3(kv);
    c.timeouts  = loadTimeouts(kv);
    c.node_id   = cfgStr(kv, "node.id", c.node_id);
    return c;
}

ServerConfig loadServerConfig(const std::string& path) {
    auto kv = parseIniFile(path);
    ServerConfig s;
    s.multicast = loadMulticast(kv);
    s.s3        = loadS3(kv);
    s.timeouts  = loadTimeouts(kv);
    s.node_id   = cfgStr(kv, "node.id", s.node_id);
    return s;
}

} // namespace rdmp
