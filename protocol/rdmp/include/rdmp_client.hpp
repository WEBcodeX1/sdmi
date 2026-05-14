#pragma once

#include "rdmp_common.hpp"
#include "rdmp_s3.hpp"

#include <list>
#include <string>
#include <unordered_set>

#include <netinet/in.h>

namespace rdmp {

// ---------------------------------------------------------------------------
// PendingMulticast
//
// Represents one task that is scheduled for repeated UDP multicast bursting.
// Each entry tracks how many transmissions remain and when the next one is due.
// ---------------------------------------------------------------------------

struct PendingMulticast {
    std::string uuid;
    std::string payload;
    int         sends_remaining;
    int64_t     next_send_ms;
};

// ---------------------------------------------------------------------------
// RDMPClient
//
// Single-threaded RDMP client (MSG distributor client).
//
// Workflow
// --------
//  1. addNewTask(msg) – writes the task to the S3 shared task queue and
//     enqueues a burst of UDP multicast announcements.
//  2. runOnce()       – called in a tight loop by the application; it:
//       a. drains the pending-multicast burst queue (sends UDP datagrams),
//       b. polls the S3 bucket for tasks added by *other* client instances
//          and enqueues multicast bursts for newly discovered UUIDs so that
//          every active client relays every task.
// ---------------------------------------------------------------------------

class RDMPClient {
public:
    explicit RDMPClient(const std::string& config_path);
    ~RDMPClient();

    // Add a new task/message.  Stores the task in S3 and schedules the UDP
    // multicast burst.  Returns the assigned UUID, or "" on failure.
    std::string addNewTask(const std::string& payload);

    // Alias matching the specification wording.
    std::string addNewMessage(const std::string& payload);

    // Process one iteration of the event loop (non-blocking).
    void runOnce();

    // Blocking event loop – runs until stop() is called or SIGINT is received.
    void run();

    // Request a clean exit from run().
    void stop();

private:
    ClientConfig config_;
    S3Client     s3_;

    // UDP socket (sender-only; no multicast group join required for senders).
    int                sock_fd_     = -1;
    struct sockaddr_in mcast_addr_  = {};

    // UUIDs already discovered (either created locally or found on S3).
    // Prevents duplicate multicast bursts for the same UUID.
    std::unordered_set<std::string> known_tasks_;

    // Queue of outgoing UDP multicast bursts.
    std::list<PendingMulticast> pending_multicasts_;

    // Time (ms) of the last S3 poll.
    int64_t last_s3_poll_ms_ = 0;

    bool running_ = false;

    // Socket setup
    void setupSocket();

    // Send a single UDP multicast datagram.
    void sendMulticast(const std::string& uuid,
                       MsgType            type,
                       const std::string& payload);

    // Enqueue a repeated multicast burst for uuid/payload.
    void enqueueBurst(const std::string& uuid, const std::string& payload);

    // Drain the pending-multicast burst queue.
    void drainBurstQueue();

    // Poll S3 for task UUIDs not yet in known_tasks_ and enqueue bursts.
    void pollS3ForNewTasks();

    // Persist a new task to S3.
    bool storeTask(const std::string& uuid, const std::string& payload);
};

} // namespace rdmp
