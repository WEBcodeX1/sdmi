#pragma once

#include "rdmp_common.hpp"
#include "rdmp_s3.hpp"

#include <functional>
#include <string>
#include <unordered_map>

#include <netinet/in.h>

namespace rdmp {

// ---------------------------------------------------------------------------
// TaskHandler
//
// Callback registered by the application to process tasks.
// Receives the task UUID and payload; must return a result string that is
// persisted to the S3 shared transaction cache.
// ---------------------------------------------------------------------------

using TaskHandler = std::function<std::string(const std::string& uuid,
                                              const std::string& payload)>;

// ---------------------------------------------------------------------------
// Internal bookkeeping types
// ---------------------------------------------------------------------------

// Per-task receipt timestamps from individual client source IPs.
// Used by the degradation detector.
struct SourceReceipt {
    int64_t     first_receipt_ms = 0;
    // source_ip -> receipt_ms (populated as messages arrive)
    std::unordered_map<std::string, int64_t> by_source;
};

// A task that this server instance is currently executing / has claimed.
struct ExecutingTask {
    std::string uuid;
    std::string payload;
    int64_t     started_ms  = 0;
    int         retry_count = 0;
};

// ---------------------------------------------------------------------------
// RDMPServer
//
// Single-threaded RDMP server (MSG distributor server).
//
// Workflow
// --------
//  1. Joins the configured UDP multicast group.
//  2. runOnce() – called in a tight loop by the application; it:
//       a. Tries a non-blocking receive on the multicast socket.
//       b. On receiving a TASK_ANNOUNCE: checks local and S3 status; if the
//          task is unclaimed the server attempts to exclusively claim it via
//          an optimistic PUT→GET cycle, then executes it through the
//          registered TaskHandler.
//       c. Runs the watchdog (rate-limited by watchdog_interval_ms): scans
//          known tasks for stale EXECUTING entries and retries them.
//       d. Updates per-source latency records for degradation detection.
// ---------------------------------------------------------------------------

class RDMPServer {
public:
    explicit RDMPServer(const std::string& config_path);
    ~RDMPServer();

    // Register the application task handler.
    void setTaskHandler(TaskHandler handler);

    // Process one iteration of the event loop (non-blocking).
    void runOnce();

    // Blocking event loop – runs until stop() is called or SIGINT is received.
    void run();

    // Request a clean exit from run().
    void stop();

private:
    ServerConfig config_;
    S3Client     s3_;
    TaskHandler  handler_;

    // UDP receive socket (joined to the multicast group).
    int                sock_fd_    = -1;
    struct sockaddr_in bind_addr_  = {};

    // Local mirror of task statuses (reduces redundant S3 fetches).
    std::unordered_map<std::string, TaskStatusRecord> local_status_;

    // Tasks currently claimed by *this* server instance.
    std::unordered_map<std::string, ExecutingTask> executing_;

    // Per-task source-latency records for degradation detection.
    std::unordered_map<std::string, SourceReceipt> receipt_times_;

    int64_t last_watchdog_ms_ = 0;
    bool    running_          = false;

    // Socket setup (create, SO_REUSEADDR, bind, join multicast group).
    void setupSocket();

    // Non-blocking receive; returns false when no datagram was available.
    bool receiveAndProcess();

    // Handle a TASK_ANNOUNCE message.
    void handleTaskAnnounce(const std::string& uuid,
                            const std::string& payload,
                            const std::string& src_ip);

    // Attempt to exclusively claim a task via optimistic S3 PUT→GET.
    // Returns true when this server successfully becomes the executor.
    bool tryClaimTask(const std::string& uuid);

    // Execute the task and persist the result status to S3.
    void executeTask(const std::string& uuid, const std::string& payload);

    // Persist a status update for uuid to both S3 and the local cache.
    void updateTaskStatus(const std::string& uuid,
                          TaskStatus         status,
                          const std::string& server_id = "",
                          const std::string& result    = "");

    // Watchdog: detect timed-out executions and retry.
    void runWatchdog();

    // Check per-source receipt latencies and emit degradation alerts.
    void checkDegradation(const std::string& uuid);
};

} // namespace rdmp
