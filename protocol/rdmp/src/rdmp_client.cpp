#include "rdmp_client.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <net/if.h>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace rdmp {

// ---------------------------------------------------------------------------
// Storage key prefixes
// ---------------------------------------------------------------------------

static const std::string S3_TASK_PREFIX   = "tasks/";
static const std::string S3_STATUS_PREFIX = "status/";

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

RDMPClient::RDMPClient(const std::string& config_path)
    : config_(loadClientConfig(config_path)),
      backend_(makeStorageBackend(config_)) {
    setupSocket();
    last_s3_poll_ms_ = currentTimeMs();
}

RDMPClient::~RDMPClient() {
    if (sock_fd_ >= 0) {
        close(sock_fd_);
        sock_fd_ = -1;
    }
}

// ---------------------------------------------------------------------------
// UDP multicast socket (send-only)
// ---------------------------------------------------------------------------

void RDMPClient::setupSocket() {
    sock_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd_ < 0)
        throw std::runtime_error(std::string("socket() failed: ") + strerror(errno));

    // Set multicast TTL
    int ttl = config_.multicast.ttl;
    setsockopt(sock_fd_, IPPROTO_IP, IP_MULTICAST_TTL,
               &ttl, sizeof(ttl));

    // Optionally bind to a specific outbound interface
    if (!config_.multicast.iface.empty()) {
        struct ip_mreqn mreqn = {};
        mreqn.imr_ifindex = if_nametoindex(config_.multicast.iface.c_str());
        setsockopt(sock_fd_, IPPROTO_IP, IP_MULTICAST_IF,
                   &mreqn, sizeof(mreqn));
    }

    memset(&mcast_addr_, 0, sizeof(mcast_addr_));
    mcast_addr_.sin_family      = AF_INET;
    mcast_addr_.sin_port        = htons(config_.multicast.port);
    mcast_addr_.sin_addr.s_addr = inet_addr(config_.multicast.group.c_str());
}

// ---------------------------------------------------------------------------
// Wire-format serialisation
// ---------------------------------------------------------------------------

void RDMPClient::sendMulticast(const std::string& uuid,
                                MsgType            type,
                                const std::string& payload) {
    // Build datagram:
    //   4B magic | 1B version | 1B msg_type | 36B uuid | 4B payload_len | NB payload
    const uint32_t magic   = htonl(RDMP_MAGIC);
    const uint32_t pay_len = htonl(static_cast<uint32_t>(payload.size()));

    // Pre-allocate buffer (max UDP payload is ~65507 B)
    std::string buf;
    buf.reserve(RDMP_HEADER_SIZE + payload.size());
    buf.append(reinterpret_cast<const char*>(&magic),   4);
    buf.push_back(static_cast<char>(RDMP_VERSION));
    buf.push_back(static_cast<char>(type));
    if (uuid.size() != 36)
        throw std::runtime_error("UUID must be 36 characters");
    buf.append(uuid);
    buf.append(reinterpret_cast<const char*>(&pay_len), 4);
    buf.append(payload);

    ssize_t sent = sendto(sock_fd_,
                          buf.data(), buf.size(), 0,
                          reinterpret_cast<const struct sockaddr*>(&mcast_addr_),
                          sizeof(mcast_addr_));
    if (sent < 0)
        std::cerr << "[RDMP/Client] sendto error: " << strerror(errno) << "\n";
}

// ---------------------------------------------------------------------------
// Burst queue management
// ---------------------------------------------------------------------------

void RDMPClient::enqueueBurst(const std::string& uuid,
                               const std::string& payload) {
    PendingMulticast pm;
    pm.uuid            = uuid;
    pm.payload         = payload;
    pm.sends_remaining = static_cast<int>(config_.timeouts.multicast_repeat_count);
    pm.next_send_ms    = currentTimeMs();
    pending_multicasts_.push_back(std::move(pm));
}

void RDMPClient::drainBurstQueue() {
    const int64_t now = currentTimeMs();

    for (auto it = pending_multicasts_.begin(); it != pending_multicasts_.end(); ) {
        if (it->next_send_ms <= now) {
            sendMulticast(it->uuid, MsgType::TASK_ANNOUNCE, it->payload);
            --(it->sends_remaining);

            if (it->sends_remaining <= 0) {
                it = pending_multicasts_.erase(it);
            } else {
                it->next_send_ms =
                    now + static_cast<int64_t>(
                        config_.timeouts.multicast_repeat_interval_ms);
                ++it;
            }
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------------------
// S3 persistence
// ---------------------------------------------------------------------------

bool RDMPClient::storeTask(const std::string& uuid,
                            const std::string& payload) {
    Task t;
    t.uuid       = uuid;
    t.payload    = payload;
    t.created_by = config_.node_id;
    t.created_at = currentTimestamp();

    const std::string key  = S3_TASK_PREFIX + uuid;
    const std::string body = buildTaskJson(t);
    return backend_->putObject(key, body);
}

// ---------------------------------------------------------------------------
// S3 polling – detect tasks added by other client instances
// ---------------------------------------------------------------------------

void RDMPClient::pollS3ForNewTasks() {
    const int64_t now = currentTimeMs();
    if (now - last_s3_poll_ms_ <
        static_cast<int64_t>(config_.timeouts.s3_poll_interval_ms)) {
        return;
    }
    last_s3_poll_ms_ = now;

    std::vector<std::string> keys = backend_->listObjects(S3_TASK_PREFIX);
    for (const auto& key : keys) {
        // key has the form "tasks/<uuid>"
        if (key.size() <= S3_TASK_PREFIX.size()) continue;
        const std::string uuid = key.substr(S3_TASK_PREFIX.size());

        if (known_tasks_.count(uuid)) continue;

        // New task discovered – fetch its payload and schedule a burst
        std::string body = backend_->getObject(key);
        if (body.empty()) continue;

        Task t = parseTask(body);
        if (t.uuid.empty()) continue;

        known_tasks_.insert(uuid);
        enqueueBurst(uuid, t.payload);
        std::cout << "[RDMP/Client] Relaying task from S3: " << uuid << "\n";
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::string RDMPClient::addNewTask(const std::string& payload) {
    const std::string uuid = generateUUID();

    if (!storeTask(uuid, payload)) {
        std::cerr << "[RDMP/Client] Failed to store task " << uuid
                  << " in S3\n";
        return "";
    }

    known_tasks_.insert(uuid);
    enqueueBurst(uuid, payload);
    std::cout << "[RDMP/Client] New task queued: " << uuid << "\n";
    return uuid;
}

std::string RDMPClient::addNewMessage(const std::string& payload) {
    return addNewTask(payload);
}

void RDMPClient::runOnce() {
    drainBurstQueue();
    pollS3ForNewTasks();
}

void RDMPClient::run() {
    running_ = true;
    while (running_) {
        runOnce();
        // Avoid busy-spinning; 1 ms sleep keeps latency negligible.
        usleep(1000);
    }
}

void RDMPClient::stop() {
    running_ = false;
}

} // namespace rdmp
