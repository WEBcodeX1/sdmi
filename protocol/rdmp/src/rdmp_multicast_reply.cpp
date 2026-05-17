#include "rdmp_multicast_reply.hpp"

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
// OBJECT_PUT datagram header layout
//   4B  RDMP_MAGIC  (big-endian)
//   1B  RDMP_VERSION
//   1B  MsgType::OBJECT_PUT
//   2B  key_len  (uint16_t, big-endian)
//   NB  key
//   4B  body_len (uint32_t, big-endian)
//   MB  body
// ---------------------------------------------------------------------------

static constexpr size_t REPLY_FIXED_HEADER = 4 + 1 + 1 + 2 + 4;  // 12 bytes

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

MulticastReplyBackend::MulticastReplyBackend(const MulticastReplyConfig& cfg)
    : cfg_(cfg) {
    setupSockets();
}

MulticastReplyBackend::~MulticastReplyBackend() {
    if (recv_fd_ >= 0) { close(recv_fd_); recv_fd_ = -1; }
    if (send_fd_ >= 0) { close(send_fd_); send_fd_ = -1; }
}

// ---------------------------------------------------------------------------
// Socket setup
// ---------------------------------------------------------------------------

void MulticastReplyBackend::setupSockets() {
    // --- Send socket ---
    send_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (send_fd_ < 0)
        throw std::runtime_error(std::string("MulticastReplyBackend: send socket() failed: ")
                                 + strerror(errno));

    int ttl = cfg_.ttl;
    setsockopt(send_fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    if (!cfg_.iface.empty()) {
        struct ip_mreqn mreqn = {};
        mreqn.imr_ifindex = if_nametoindex(cfg_.iface.c_str());
        setsockopt(send_fd_, IPPROTO_IP, IP_MULTICAST_IF, &mreqn, sizeof(mreqn));
    }

    // Destination for sends
    memset(&reply_addr_, 0, sizeof(reply_addr_));
    reply_addr_.sin_family      = AF_INET;
    reply_addr_.sin_port        = htons(cfg_.port);
    reply_addr_.sin_addr.s_addr = inet_addr(cfg_.group.c_str());

    // --- Receive socket ---
    recv_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (recv_fd_ < 0)
        throw std::runtime_error(std::string("MulticastReplyBackend: recv socket() failed: ")
                                 + strerror(errno));

    int reuse = 1;
    setsockopt(recv_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(recv_fd_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

    struct sockaddr_in bind_addr = {};
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = htons(cfg_.port);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(recv_fd_, reinterpret_cast<struct sockaddr*>(&bind_addr),
             sizeof(bind_addr)) < 0) {
        throw std::runtime_error(std::string("MulticastReplyBackend: bind() failed: ")
                                 + strerror(errno));
    }

    // Join the reply multicast group
    struct ip_mreq mreq = {};
    mreq.imr_multiaddr.s_addr = inet_addr(cfg_.group.c_str());
    if (!cfg_.iface.empty()) {
        struct ifreq ifr = {};
        strncpy(ifr.ifr_name, cfg_.iface.c_str(), sizeof(ifr.ifr_name) - 1);
        if (ioctl(recv_fd_, SIOCGIFADDR, &ifr) == 0) {
            mreq.imr_interface =
                reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_addr)->sin_addr;
        }
    } else {
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    }

    if (setsockopt(recv_fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   &mreq, sizeof(mreq)) < 0) {
        throw std::runtime_error(
            std::string("MulticastReplyBackend: IP_ADD_MEMBERSHIP failed: ")
            + strerror(errno));
    }

    std::cout << "[RDMP/MulticastReply] Joined reply group "
              << cfg_.group << ":" << cfg_.port << "\n";
}

// ---------------------------------------------------------------------------
// IStorageBackend implementation
// ---------------------------------------------------------------------------

std::string MulticastReplyBackend::getObject(const std::string& key) {
    auto it = store_.find(key);
    if (it == store_.end()) return "";
    return it->second;
}

bool MulticastReplyBackend::putObject(const std::string& key,
                                      const std::string& body,
                                      const std::string& /*content_type*/) {
    store_[key] = body;   // update local map immediately
    sendPut(key, body);   // broadcast to reply multicast group
    return true;
}

std::vector<std::string> MulticastReplyBackend::listObjects(
    const std::string& prefix) {
    std::vector<std::string> result;
    result.reserve(store_.size());
    for (const auto& [k, _] : store_) {
        if (k.compare(0, prefix.size(), prefix) == 0)
            result.push_back(k);
    }
    return result;
}

void MulticastReplyBackend::sync() {
    uint8_t buf[65536];
    while (true) {
        ssize_t n = recvfrom(recv_fd_, buf, sizeof(buf), MSG_DONTWAIT,
                             nullptr, nullptr);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            std::cerr << "[RDMP/MulticastReply] recvfrom error: "
                      << strerror(errno) << "\n";
            break;
        }
        handleDatagram(buf, n);
    }
}

// ---------------------------------------------------------------------------
// Wire format helpers
// ---------------------------------------------------------------------------

void MulticastReplyBackend::sendPut(const std::string& key,
                                    const std::string& body) {
    if (key.size() > 65535) {
        std::cerr << "[RDMP/MulticastReply] key too long, skipping send\n";
        return;
    }

    const uint32_t magic    = htonl(RDMP_MAGIC);
    const uint16_t key_len  = htons(static_cast<uint16_t>(key.size()));
    const uint32_t body_len = htonl(static_cast<uint32_t>(body.size()));

    std::string buf;
    buf.reserve(REPLY_FIXED_HEADER + key.size() + body.size());
    buf.append(reinterpret_cast<const char*>(&magic),    4);
    buf.push_back(static_cast<char>(RDMP_VERSION));
    buf.push_back(static_cast<char>(MsgType::OBJECT_PUT));
    buf.append(reinterpret_cast<const char*>(&key_len),  2);
    buf.append(key);
    buf.append(reinterpret_cast<const char*>(&body_len), 4);
    buf.append(body);

    ssize_t sent = sendto(send_fd_, buf.data(), buf.size(), 0,
                          reinterpret_cast<const struct sockaddr*>(&reply_addr_),
                          sizeof(reply_addr_));
    if (sent < 0)
        std::cerr << "[RDMP/MulticastReply] sendto error: "
                  << strerror(errno) << "\n";
}

bool MulticastReplyBackend::handleDatagram(const uint8_t* buf, ssize_t n) {
    if (n < static_cast<ssize_t>(REPLY_FIXED_HEADER)) return false;

    uint32_t magic = 0;
    memcpy(&magic, buf, 4);
    if (ntohl(magic) != RDMP_MAGIC) return false;

    if (buf[4] != RDMP_VERSION) return false;
    if (static_cast<MsgType>(buf[5]) != MsgType::OBJECT_PUT) return false;

    uint16_t key_len = 0;
    memcpy(&key_len, buf + 6, 2);
    key_len = ntohs(key_len);

    const ssize_t key_start = 8;  // after fixed header up to body_len
    if (n < key_start + static_cast<ssize_t>(key_len) + 4) return false;

    const std::string key(reinterpret_cast<const char*>(buf + key_start), key_len);

    uint32_t body_len = 0;
    memcpy(&body_len, buf + key_start + key_len, 4);
    body_len = ntohl(body_len);

    const ssize_t body_start = key_start + key_len + 4;
    if (n < body_start + static_cast<ssize_t>(body_len)) return false;

    const std::string body(reinterpret_cast<const char*>(buf + body_start), body_len);

    // Merge into local store (last-writer-wins; replaces any existing value)
    store_[key] = body;
    return true;
}

} // namespace rdmp
