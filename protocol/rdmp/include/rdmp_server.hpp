#pragma once

#include "rdmp_backend.hpp"
#include "rdmp_common.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include <netinet/in.h>

namespace rdmp {

// ---------------------------------------------------------------------------
// TaskHandler
//
// Callback registered by the application to process tasks.
// Receives the task UUID and payload; must return a result string that is
// persisted to the shared backend status cache.
// ---------------------------------------------------------------------------

using TaskHandler = std::function<std::string(const std::string& uuid,
                                              const std::string& payload)>;

// ---------------------------------------------------------------------------
// Internal bookkeeping types
// ---------------------------------------------------------------------------

// A task that this server instance is currently executing / has claimed.
struct ExecutingTask {
    std::string uuid;
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
//       b. On receiving a TASK_ANNOUNCE: checks local and backend status; if
//          the task is unclaimed the server attempts to exclusively claim it
//          via an optimistic PUT→GET cycle, then executes it through the
//          registered TaskHandler.
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
    ServerConfig                     config_;
    std::unique_ptr<IStorageBackend> backend_;
    TaskHandler                      handler_;

    // UDP receive socket (joined to the multicast group).
    int                sock_fd_    = -1;
    struct sockaddr_in bind_addr_  = {};

    // Local mirror of task statuses (reduces redundant backend fetches).
    std::unordered_map<std::string, TaskStatusRecord> local_status_;

    // Tasks currently claimed by *this* server instance.
    std::unordered_map<std::string, ExecutingTask> executing_;

    bool    running_          = false;

    // Socket setup (create, SO_REUSEADDR, bind, join multicast group).
    void setupSocket();

    // Non-blocking receive; returns false when no datagram was available.
    bool receiveAndProcess();

    // Handle a TASK_ANNOUNCE message.
    void handleTaskAnnounce(const std::string& uuid,
                            const std::string& payload);

    // Attempt to exclusively claim a task via optimistic backend PUT→GET.
    // Returns true when this server successfully becomes the executor.
    bool tryClaimTask(const std::string& uuid);

    // Execute the task and persist the result status to the backend.
    void executeTask(const std::string& uuid, const std::string& payload);

    // Return the storage key used for a task's status record.
    // In bypass mode: per-server key ("status/<uuid>/<server_id>").
    // In normal mode: shared key ("status/<uuid>").
    std::string statusKey(const std::string& uuid) const;

    // Persist a status update for uuid to both backend and the local cache.
    void updateTaskStatus(const std::string& uuid,
                          TaskStatus         status,
                          const std::string& server_id = "",
                          const std::string& result    = "");
};

} // namespace rdmp
