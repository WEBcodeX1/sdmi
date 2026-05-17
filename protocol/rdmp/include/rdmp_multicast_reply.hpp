#pragma once

#include "rdmp_backend.hpp"
#include "rdmp_common.hpp"

#include <netinet/in.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace rdmp {

// ---------------------------------------------------------------------------
// MulticastReplyBackend
//
// A UDP-multicast-based implementation of IStorageBackend that eliminates the
// need for any shared external storage (S3, filesystem) by using a second
// dedicated multicast group (the "reply group") to propagate key/value
// updates to all nodes on the network.
//
// How it works
// ------------
//  * All key/value pairs are kept in an in-memory map on every node.
//  * putObject() stores the value locally AND broadcasts an OBJECT_PUT
//    datagram to the reply multicast group so that all other nodes update
//    their own in-memory maps.
//  * sync() (called from RDMPClient::runOnce and RDMPServer::runOnce) drains
//    the receive socket and merges incoming OBJECT_PUT datagrams into the
//    local map.
//  * getObject() and listObjects() read only from the in-memory map.
//
// Configuration
// -------------
//  Use synctype "multicast-reply" in global config and provide a
//  "multicast_reply" section with a different group/port from the main
//  multicast group:
//
//    "global": { "synctype": "multicast-reply" },
//    "multicast_reply": {
//        "group": "239.1.2.4",
//        "port": 5001,
//        "ttl":  32,
//        "interface": ""
//    }
//
// Wire format (OBJECT_PUT datagrams on the reply group)
// ------------------------------------------------------
//  4B  : RDMP_MAGIC  (network byte order)
//  1B  : RDMP_VERSION
//  1B  : MsgType::OBJECT_PUT (0x03)
//  2B  : key_len   (uint16_t, network byte order)
//  N B : key bytes
//  4B  : body_len  (uint32_t, network byte order)
//  M B : body bytes
// ---------------------------------------------------------------------------

class MulticastReplyBackend : public IStorageBackend {
public:
    explicit MulticastReplyBackend(const MulticastReplyConfig& cfg);
    ~MulticastReplyBackend() override;

    std::string getObject(const std::string& key) override;

    bool putObject(const std::string& key,
                   const std::string& body,
                   const std::string& content_type = "application/json") override;

    std::vector<std::string> listObjects(const std::string& prefix) override;

    // Drain incoming OBJECT_PUT datagrams from the receive socket and merge
    // them into the local in-memory map.  Non-blocking (MSG_DONTWAIT).
    void sync() override;

private:
    MulticastReplyConfig cfg_;

    // In-memory key/value store (shared state replicated via multicast).
    std::unordered_map<std::string, std::string> store_;

    // UDP send socket – used to broadcast OBJECT_PUT datagrams.
    int send_fd_  = -1;
    // UDP receive socket – joined to the reply multicast group.
    int recv_fd_  = -1;

    struct sockaddr_in reply_addr_ = {};   // destination for sends

    void setupSockets();

    // Serialise and send an OBJECT_PUT datagram for key/body.
    void sendPut(const std::string& key, const std::string& body);

    // Parse one OBJECT_PUT datagram received in buf[0..n-1] and merge into
    // store_.  Returns false if the datagram is malformed.
    bool handleDatagram(const uint8_t* buf, ssize_t n);
};

} // namespace rdmp
