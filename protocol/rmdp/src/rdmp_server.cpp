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
// Storage key prefixes (must match rdmp_client.cpp)
// ---------------------------------------------------------------------------

static const std::string S3_TASK_PREFIX   = "tasks/";
static const std::string S3_STATUS_PREFIX = "status/";

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

RDMPServer::RDMPServer(const std::string& config_path)
    : config_(loadServerConfig(config_path)),
      backend_(makeStorageBackend(config_)) {
    setupSocket();
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

    if (msg_type == MsgType::TASK_ANNOUNCE) {
        handleTaskAnnounce(uuid, payload);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Status key helper
// ---------------------------------------------------------------------------

std::string RDMPServer::statusKey(const std::string& uuid) const {
    if (config_.bypass_pending_check)
        return S3_STATUS_PREFIX + uuid + "/" + config_.node_id;
    return S3_STATUS_PREFIX + uuid;
}

// ---------------------------------------------------------------------------
// Task announce handling
// ---------------------------------------------------------------------------

void RDMPServer::handleTaskAnnounce(const std::string& uuid,
                                     const std::string& payload) {
    if (config_.bypass_pending_check) {
        // Bypass mode: skip the shared S3 status check entirely.
        // Every server executes the task independently; each writes its own
        // per-server status record at "status/<uuid>/<server_id>".
        if (executing_.count(uuid)) return;  // don't double-execute on this node
        executeTask(uuid, payload);
        return;
    }

    // Normal mode (2.4): check local status cache first (fast-path).
    auto it = local_status_.find(uuid);
    if (it != local_status_.end()) {
        TaskStatus s = it->second.status;
        if (s == TaskStatus::COMPLETED || s == TaskStatus::EXECUTING ||
            s == TaskStatus::FAILED) {
            return;
        }
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
    const std::string status_key = statusKey(uuid);

    // Check current status on backend (per spec 2.4: only proceed if pending)
    std::string existing = backend_->getObject(status_key);
    if (!existing.empty()) {
        TaskStatusRecord cur = parseTaskStatus(existing);
        // Any non-pending status means another server is handling / has handled
        // this task – do not process.
        if (cur.status == TaskStatus::EXECUTING ||
            cur.status == TaskStatus::COMPLETED ||
            cur.status == TaskStatus::FAILED) {
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

    if (!backend_->putObject(status_key, buildStatusJson(claim))) {
        std::cerr << "[RDMP/Server] Failed to PUT claim for " << uuid << "\n";
        return false;
    }

    // Re-fetch to verify we won the optimistic race
    std::string verify = backend_->getObject(status_key);
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
    executing_[uuid]    = et;
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
    backend_->putObject(statusKey(uuid), buildStatusJson(r));
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void RDMPServer::runOnce() {
    backend_->sync();
    receiveAndProcess();
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
