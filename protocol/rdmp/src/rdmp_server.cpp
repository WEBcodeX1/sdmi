#include "rdmp_server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <net/if.h>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace rdmp {

// ---------------------------------------------------------------------------
// S3 key prefixes (must match rdmp_client.cpp)
// ---------------------------------------------------------------------------

static const std::string S3_TASK_PREFIX   = "tasks/";
static const std::string S3_STATUS_PREFIX = "status/";

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

RDMPServer::RDMPServer(const std::string& config_path)
    : config_(loadServerConfig(config_path)),
      s3_(config_.s3) {
    setupSocket();
    last_watchdog_ms_ = currentTimeMs();
}

RDMPServer::~RDMPServer() {
    if (sock_fd_ >= 0) {
        close(sock_fd_);
        sock_fd_ = -1;
    }
}

// ---------------------------------------------------------------------------
// Task handler (default: print and return "ok")
// ---------------------------------------------------------------------------

void RDMPServer::setTaskHandler(TaskHandler handler) {
    handler_ = std::move(handler);
}

// ---------------------------------------------------------------------------
// UDP multicast receive socket
// ---------------------------------------------------------------------------

void RDMPServer::setupSocket() {
    sock_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd_ < 0)
        throw std::runtime_error(std::string("socket() failed: ") + strerror(errno));

    // Allow multiple processes/sockets to share the port
    int reuse = 1;
    setsockopt(sock_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(sock_fd_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

    // Bind to INADDR_ANY on the multicast port
    memset(&bind_addr_, 0, sizeof(bind_addr_));
    bind_addr_.sin_family      = AF_INET;
    bind_addr_.sin_port        = htons(config_.multicast.port);
    bind_addr_.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock_fd_,
             reinterpret_cast<struct sockaddr*>(&bind_addr_),
             sizeof(bind_addr_)) < 0) {
        throw std::runtime_error(std::string("bind() failed: ") + strerror(errno));
    }

    // Join the multicast group
    struct ip_mreq mreq = {};
    mreq.imr_multiaddr.s_addr = inet_addr(config_.multicast.group.c_str());
    if (!config_.multicast.iface.empty()) {
        struct ifreq ifr = {};
        strncpy(ifr.ifr_name, config_.multicast.iface.c_str(),
                sizeof(ifr.ifr_name) - 1);
        if (ioctl(sock_fd_, SIOCGIFADDR, &ifr) == 0) {
            mreq.imr_interface =
                reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_addr)->sin_addr;
        }
    } else {
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    }

    if (setsockopt(sock_fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   &mreq, sizeof(mreq)) < 0) {
        throw std::runtime_error(
            std::string("IP_ADD_MEMBERSHIP failed: ") + strerror(errno));
    }

    std::cout << "[RDMP/Server] Joined multicast group "
              << config_.multicast.group << ":" << config_.multicast.port
              << "\n";
}

// ---------------------------------------------------------------------------
// Non-blocking receive
// ---------------------------------------------------------------------------

bool RDMPServer::receiveAndProcess() {
    uint8_t buf[65536];
    struct sockaddr_in src_addr = {};
    socklen_t addrlen = sizeof(src_addr);

    ssize_t n = recvfrom(sock_fd_, buf, sizeof(buf), MSG_DONTWAIT,
                         reinterpret_cast<struct sockaddr*>(&src_addr),
                         &addrlen);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
        std::cerr << "[RDMP/Server] recvfrom error: " << strerror(errno) << "\n";
        return false;
    }

    if (static_cast<size_t>(n) < RDMP_HEADER_SIZE) return false;

    // Parse header
    uint32_t magic = 0;
    memcpy(&magic, buf, 4);
    if (ntohl(magic) != RDMP_MAGIC) return false;

    const uint8_t version  = buf[4];
    if (version != RDMP_VERSION) return false;

    const auto msg_type = static_cast<MsgType>(buf[5]);

    char uuid_buf[37] = {};
    memcpy(uuid_buf, buf + 6, 36);
    const std::string uuid(uuid_buf);

    uint32_t pay_len = 0;
    memcpy(&pay_len, buf + 42, 4);
    pay_len = ntohl(pay_len);

    if (static_cast<ssize_t>(RDMP_HEADER_SIZE + pay_len) > n) return false;

    const std::string payload(reinterpret_cast<char*>(buf + RDMP_HEADER_SIZE), pay_len);
    const std::string src_ip(inet_ntoa(src_addr.sin_addr));

    if (msg_type == MsgType::TASK_ANNOUNCE) {
        handleTaskAnnounce(uuid, payload, src_ip);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Task announce handling
// ---------------------------------------------------------------------------

void RDMPServer::handleTaskAnnounce(const std::string& uuid,
                                     const std::string& payload,
                                     const std::string& src_ip) {
    // Record receipt time for degradation detection
    const int64_t now = currentTimeMs();
    auto& rec = receipt_times_[uuid];
    if (rec.first_receipt_ms == 0) rec.first_receipt_ms = now;
    rec.by_source[src_ip] = now;
    checkDegradation(uuid);

    // Check local status cache first (fast-path)
    auto it = local_status_.find(uuid);
    if (it != local_status_.end()) {
        TaskStatus s = it->second.status;
        if (s == TaskStatus::COMPLETED || s == TaskStatus::EXECUTING) return;
    }

    // Check if we're already executing this task
    if (executing_.count(uuid)) return;

    // Attempt to claim and execute
    if (tryClaimTask(uuid)) {
        executeTask(uuid, payload);
    }
}

// ---------------------------------------------------------------------------
// Optimistic S3 claim (PUT → GET verify)
// ---------------------------------------------------------------------------

bool RDMPServer::tryClaimTask(const std::string& uuid) {
    const std::string status_key = S3_STATUS_PREFIX + uuid;

    // Check current status on S3
    std::string existing = s3_.getObject(status_key);
    if (!existing.empty()) {
        TaskStatusRecord cur = parseTaskStatus(existing);
        if (cur.status == TaskStatus::EXECUTING ||
            cur.status == TaskStatus::COMPLETED) {
            local_status_[uuid] = cur;
            return false;
        }
    }

    // Attempt to claim: PUT status=executing with our server_id
    TaskStatusRecord claim;
    claim.uuid       = uuid;
    claim.status     = TaskStatus::EXECUTING;
    claim.server_id  = config_.node_id;
    claim.updated_at = currentTimestamp();
    claim.result     = "";

    if (!s3_.putObject(status_key, buildStatusJson(claim))) {
        std::cerr << "[RDMP/Server] Failed to PUT claim for " << uuid << "\n";
        return false;
    }

    // Re-fetch to verify we won the optimistic race
    std::string verify = s3_.getObject(status_key);
    if (verify.empty()) return false;

    TaskStatusRecord verified = parseTaskStatus(verify);
    if (verified.server_id != config_.node_id) {
        // Another server won the race
        local_status_[uuid] = verified;
        return false;
    }

    // We are the executor
    ExecutingTask et;
    et.uuid        = uuid;
    et.started_ms  = currentTimeMs();
    et.retry_count = 0;
    executing_[uuid] = et;
    local_status_[uuid] = claim;

    std::cout << "[RDMP/Server] Claimed task: " << uuid << "\n";
    return true;
}

// ---------------------------------------------------------------------------
// Task execution
// ---------------------------------------------------------------------------

void RDMPServer::executeTask(const std::string& uuid,
                              const std::string& payload) {
    std::string result;
    TaskStatus  outcome = TaskStatus::COMPLETED;

    try {
        if (handler_) {
            result = handler_(uuid, payload);
        } else {
            std::cout << "[RDMP/Server] Executing task " << uuid
                      << " payload=" << payload << "\n";
            result = "ok";
        }
    } catch (const std::exception& ex) {
        std::cerr << "[RDMP/Server] Task " << uuid
                  << " threw: " << ex.what() << "\n";
        result  = ex.what();
        outcome = TaskStatus::FAILED;
    } catch (...) {
        result  = "unknown exception";
        outcome = TaskStatus::FAILED;
    }

    executing_.erase(uuid);
    updateTaskStatus(uuid, outcome, config_.node_id, result);
    std::cout << "[RDMP/Server] Task " << uuid << " "
              << taskStatusToString(outcome) << "\n";
}

// ---------------------------------------------------------------------------
// Status persistence
// ---------------------------------------------------------------------------

void RDMPServer::updateTaskStatus(const std::string& uuid,
                                   TaskStatus         status,
                                   const std::string& server_id,
                                   const std::string& result) {
    TaskStatusRecord r;
    r.uuid       = uuid;
    r.status     = status;
    r.server_id  = server_id.empty() ? config_.node_id : server_id;
    r.updated_at = currentTimestamp();
    r.result     = result;

    local_status_[uuid] = r;
    s3_.putObject(S3_STATUS_PREFIX + uuid, buildStatusJson(r));
}

// ---------------------------------------------------------------------------
// Watchdog
// ---------------------------------------------------------------------------

void RDMPServer::runWatchdog() {
    const int64_t now = currentTimeMs();
    if (now - last_watchdog_ms_ <
        static_cast<int64_t>(config_.timeouts.watchdog_interval_ms)) {
        return;
    }
    last_watchdog_ms_ = now;

    // Scan all tasks we have locally recorded as EXECUTING
    for (auto& [uuid, status_rec] : local_status_) {
        if (status_rec.status != TaskStatus::EXECUTING) continue;

        // Fetch fresh S3 status
        std::string body = s3_.getObject(S3_STATUS_PREFIX + uuid);
        if (body.empty()) continue;

        TaskStatusRecord fresh = parseTaskStatus(body);
        local_status_[uuid]   = fresh;

        if (fresh.status != TaskStatus::EXECUTING) continue;

        // Check how long ago this task was claimed
        // updated_at is stored as ISO-8601; parse epoch seconds from it.
        // As a simplified approach we compare with our executing_ map start time.
        auto et_it = executing_.find(uuid);
        int64_t started_ms;
        if (et_it != executing_.end()) {
            started_ms = et_it->second.started_ms;
        } else {
            // Task is executing on another node; estimate start from now if
            // we don't have a local record.  If we have never seen it, skip.
            continue;
        }

        const int64_t elapsed = now - started_ms;
        if (elapsed < static_cast<int64_t>(config_.timeouts.task_execution_ms)) {
            continue;
        }

        // Timed out – retry if the server_id is ours (we crashed mid-task)
        // or if server_id belongs to another node (other node is down).
        std::cerr << "[RDMP/Server] Watchdog: task " << uuid
                  << " timed out (executor=" << fresh.server_id
                  << "), retrying\n";

        // Mark as failed and re-claim
        updateTaskStatus(uuid, TaskStatus::FAILED, fresh.server_id,
                         "watchdog timeout");

        executing_.erase(uuid);

        // Fetch task payload from S3 to retry
        std::string task_body = s3_.getObject(S3_TASK_PREFIX + uuid);
        if (task_body.empty()) {
            std::cerr << "[RDMP/Server] Cannot fetch task payload for retry: "
                      << uuid << "\n";
            continue;
        }
        Task t = parseTask(task_body);
        if (t.uuid.empty()) continue;

        // Update local status to PENDING so tryClaimTask can proceed
        local_status_[uuid].status = TaskStatus::PENDING;

        if (tryClaimTask(uuid)) {
            executeTask(uuid, t.payload);
        }
    }

    // Also check S3 for executing tasks this server does NOT know about,
    // which could have been left by crashed peers.
    std::vector<std::string> status_keys = s3_.listObjects(S3_STATUS_PREFIX);
    for (const auto& key : status_keys) {
        if (key.size() <= S3_STATUS_PREFIX.size()) continue;
        const std::string uuid = key.substr(S3_STATUS_PREFIX.size());
        if (local_status_.count(uuid)) continue; // already tracked

        std::string body = s3_.getObject(key);
        if (body.empty()) continue;

        TaskStatusRecord rec = parseTaskStatus(body);
        if (rec.status != TaskStatus::EXECUTING) {
            local_status_[uuid] = rec;
            continue;
        }

        // Unknown executing task – mark locally so next watchdog cycle
        // can track it.
        ExecutingTask et;
        et.uuid        = uuid;
        et.started_ms  = now; // conservative: assume it just started
        et.retry_count = 0;
        executing_[uuid]    = et;
        local_status_[uuid] = rec;
    }
}

// ---------------------------------------------------------------------------
// Degradation detection
// ---------------------------------------------------------------------------

void RDMPServer::checkDegradation(const std::string& uuid) {
    auto it = receipt_times_.find(uuid);
    if (it == receipt_times_.end()) return;

    const SourceReceipt& rec     = it->second;
    const int64_t        first   = rec.first_receipt_ms;
    const int64_t        thresh  = config_.timeouts.degradation_threshold_ms;

    for (const auto& [src, ts] : rec.by_source) {
        const int64_t delta = ts - first;
        if (delta > thresh) {
            std::cerr << "[RDMP/Server] ALERT: Controller " << src
                      << " is degraded – task " << uuid
                      << " arrived " << delta << " ms late\n";
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void RDMPServer::runOnce() {
    receiveAndProcess();
    runWatchdog();
}

void RDMPServer::run() {
    running_ = true;
    std::cout << "[RDMP/Server] " << config_.node_id << " running\n";
    while (running_) {
        runOnce();
        usleep(1000);
    }
}

void RDMPServer::stop() {
    running_ = false;
}

} // namespace rdmp
