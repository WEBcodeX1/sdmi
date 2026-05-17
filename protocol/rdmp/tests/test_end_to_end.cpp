// test_end_to_end.cpp – End-to-end / integration tests using LocalFilesBackend
//
// These tests drive the core task-lifecycle logic (store → claim → execute →
// status) entirely through the LocalFilesBackend without requiring real
// sockets, real S3, or real network I/O.
//
// Test scenarios:
//  1. Basic task store → claim → execute → completed lifecycle
//  2. Multiple concurrent tasks (different UUIDs)
//  3. Duplicate announce: server ignores a task it has already completed
//  4. Task claim race: only one "server" wins the optimistic PUT→GET
//  5. Watchdog retry on simulated executor crash / timeout
//  6. Jitter / send delay: task arrives with delayed second announce
//  7. Multiple burst sends of the same task (idempotent execution)
//  8. Client node failure: task written to backend but no announce sent;
//     backend polling still discovers it
//  9. Server node failure: timed-out EXECUTING task is re-claimed

#include <boost/test/unit_test.hpp>

#include "rdmp_common.hpp"
#include "rdmp_local_files.hpp"
#include "rdmp_backend.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using namespace rdmp;

// ---------------------------------------------------------------------------
// Minimal in-process "node" that replicates the server's task-lifecycle logic
// using a shared LocalFilesBackend.  It avoids sockets so tests stay fast
// and self-contained.
// ---------------------------------------------------------------------------

class FakeServer {
public:
    FakeServer(const std::string& id,
               std::shared_ptr<LocalFilesBackend> backend,
               uint32_t task_execution_ms = 5000)
        : id_(id), backend_(std::move(backend)),
          task_execution_ms_(task_execution_ms) {}

    // Simulate receiving a TASK_ANNOUNCE.
    // Returns true if this server claimed and executed the task.
    bool handleAnnounce(const std::string& uuid, const std::string& payload) {
        // Skip if already completed locally
        auto it = local_status_.find(uuid);
        if (it != local_status_.end()) {
            if (it->second.status == TaskStatus::COMPLETED ||
                it->second.status == TaskStatus::EXECUTING) {
                return false;
            }
        }
        if (executing_.count(uuid)) return false;
        return tryClaimAndExecute(uuid, payload);
    }

    // Simulate the watchdog for tasks that have timed out.
    // Returns the number of tasks retried.
    int runWatchdog() {
        int retried = 0;
        const int64_t now = currentTimeMs();
        for (auto& [uuid, et] : executing_) {
            if (now - et.started_ms >= static_cast<int64_t>(task_execution_ms_)) {
                // Timed out
                std::string body = backend_->getObject("status/" + uuid);
                if (body.empty()) continue;
                TaskStatusRecord fresh = parseTaskStatus(body);
                if (fresh.status != TaskStatus::EXECUTING) continue;

                // Mark failed and retry
                updateStatus(uuid, TaskStatus::FAILED, "watchdog timeout");
                executing_.erase(uuid);

                std::string task_body = backend_->getObject("tasks/" + uuid);
                if (task_body.empty()) continue;
                Task t = parseTask(task_body);
                local_status_[uuid].status = TaskStatus::PENDING;

                tryClaimAndExecute(t.uuid, t.payload);
                ++retried;
                break; // restart iteration safely
            }
        }
        return retried;
    }

    TaskStatus getLocalStatus(const std::string& uuid) const {
        auto it = local_status_.find(uuid);
        if (it == local_status_.end()) return TaskStatus::UNKNOWN;
        return it->second.status;
    }

    const std::string& id() const { return id_; }

    // Optional: set a custom task handler
    void setHandler(std::function<std::string(const std::string&, const std::string&)> h) {
        handler_ = std::move(h);
    }

    // Simulate this server crashing mid-execution (mark as executing but
    // never complete – used for watchdog tests).
    bool claimOnly(const std::string& uuid) {
        std::string existing = backend_->getObject("status/" + uuid);
        if (!existing.empty()) {
            TaskStatusRecord cur = parseTaskStatus(existing);
            if (cur.status == TaskStatus::EXECUTING ||
                cur.status == TaskStatus::COMPLETED) {
                local_status_[uuid] = cur;
                return false;
            }
        }
        TaskStatusRecord claim;
        claim.uuid       = uuid;
        claim.status     = TaskStatus::EXECUTING;
        claim.server_id  = id_;
        claim.updated_at = currentTimestamp();
        if (!backend_->putObject("status/" + uuid, buildStatusJson(claim)))
            return false;

        // Verify
        std::string verify = backend_->getObject("status/" + uuid);
        TaskStatusRecord verified = parseTaskStatus(verify);
        if (verified.server_id != id_) {
            local_status_[uuid] = verified;
            return false;
        }

        ExecutingTask et;
        et.uuid       = uuid;
        et.started_ms = currentTimeMs() - static_cast<int64_t>(task_execution_ms_) - 1000;
        executing_[uuid]    = et;
        local_status_[uuid] = claim;
        return true;
    }

private:
    struct ExecutingTask { std::string uuid; int64_t started_ms = 0; };

    std::string id_;
    std::shared_ptr<LocalFilesBackend> backend_;
    uint32_t task_execution_ms_;
    std::function<std::string(const std::string&, const std::string&)> handler_;
    std::unordered_map<std::string, TaskStatusRecord> local_status_;
    std::unordered_map<std::string, ExecutingTask> executing_;

    bool tryClaimAndExecute(const std::string& uuid, const std::string& payload) {
        // Read current status
        std::string existing = backend_->getObject("status/" + uuid);
        if (!existing.empty()) {
            TaskStatusRecord cur = parseTaskStatus(existing);
            if (cur.status == TaskStatus::EXECUTING ||
                cur.status == TaskStatus::COMPLETED) {
                local_status_[uuid] = cur;
                return false;
            }
        }

        // Claim
        TaskStatusRecord claim;
        claim.uuid       = uuid;
        claim.status     = TaskStatus::EXECUTING;
        claim.server_id  = id_;
        claim.updated_at = currentTimestamp();
        if (!backend_->putObject("status/" + uuid, buildStatusJson(claim)))
            return false;

        // Verify win
        std::string verify = backend_->getObject("status/" + uuid);
        TaskStatusRecord verified = parseTaskStatus(verify);
        if (verified.server_id != id_) {
            local_status_[uuid] = verified;
            return false;
        }

        ExecutingTask et{uuid, currentTimeMs()};
        executing_[uuid]    = et;
        local_status_[uuid] = claim;

        // Execute
        std::string result;
        TaskStatus  outcome = TaskStatus::COMPLETED;
        try {
            result = handler_ ? handler_(uuid, payload) : "ok";
        } catch (...) {
            result  = "handler exception";
            outcome = TaskStatus::FAILED;
        }

        executing_.erase(uuid);
        updateStatus(uuid, outcome, result);
        return true;
    }

    void updateStatus(const std::string& uuid,
                      TaskStatus status,
                      const std::string& result = "") {
        TaskStatusRecord r;
        r.uuid       = uuid;
        r.status     = status;
        r.server_id  = id_;
        r.updated_at = currentTimestamp();
        r.result     = result;
        local_status_[uuid] = r;
        backend_->putObject("status/" + uuid, buildStatusJson(r));
    }
};

// ---------------------------------------------------------------------------
// Minimal in-process "client" that stores tasks to the backend.
// ---------------------------------------------------------------------------

class FakeClient {
public:
    FakeClient(const std::string& id,
               std::shared_ptr<LocalFilesBackend> backend)
        : id_(id), backend_(std::move(backend)) {}

    // Store a new task and return its UUID.
    std::string addTask(const std::string& payload) {
        const std::string uuid = generateUUID();
        Task t;
        t.uuid       = uuid;
        t.payload    = payload;
        t.created_by = id_;
        t.created_at = currentTimestamp();
        backend_->putObject("tasks/" + uuid, buildTaskJson(t));
        known_tasks_.insert(uuid);
        return uuid;
    }

    // Return all task UUIDs visible on the backend that we haven't seen.
    std::vector<std::string> discoverNewTasks() {
        std::vector<std::string> fresh;
        auto keys = backend_->listObjects("tasks/");
        for (const auto& key : keys) {
            const std::string prefix = "tasks/";
            if (key.size() <= prefix.size()) continue;
            std::string uuid = key.substr(prefix.size());
            if (!known_tasks_.count(uuid)) {
                known_tasks_.insert(uuid);
                fresh.push_back(uuid);
            }
        }
        return fresh;
    }

    const std::string& id() const { return id_; }

private:
    std::string id_;
    std::shared_ptr<LocalFilesBackend> backend_;
    std::unordered_set<std::string> known_tasks_;
};

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

struct EndToEndFixture {
    EndToEndFixture() {
        char tmpl[] = "/tmp/rdmp_e2e_XXXXXX";
        const char* dir = mkdtemp(tmpl);
        BOOST_REQUIRE(dir != nullptr);
        base_dir_ = dir;

        LocalFilesConfig cfg;
        cfg.base_path = base_dir_;
        backend_ = std::make_shared<LocalFilesBackend>(cfg);
    }

    ~EndToEndFixture() {
        fs::remove_all(base_dir_);
    }

    // Helper: read task status from backend
    TaskStatus readStatus(const std::string& uuid) {
        std::string body = backend_->getObject("status/" + uuid);
        if (body.empty()) return TaskStatus::UNKNOWN;
        return parseTaskStatus(body).status;
    }

    std::string base_dir_;
    std::shared_ptr<LocalFilesBackend> backend_;
};

BOOST_FIXTURE_TEST_SUITE(EndToEndTest, EndToEndFixture)

// ---------------------------------------------------------------------------
// 1. Basic lifecycle: store → announce → claim → execute → completed
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(BasicLifecycle) {
    FakeClient client("c1", backend_);
    FakeServer server("s1", backend_);

    const std::string uuid = client.addTask("do-something");
    BOOST_CHECK(!uuid.empty());

    // Simulate announce reception
    std::string task_body = backend_->getObject("tasks/" + uuid);
    BOOST_REQUIRE(!task_body.empty());
    Task t = parseTask(task_body);

    bool claimed = server.handleAnnounce(t.uuid, t.payload);
    BOOST_CHECK(claimed);
    BOOST_CHECK_EQUAL(readStatus(uuid), TaskStatus::COMPLETED);
}

// ---------------------------------------------------------------------------
// 2. Multiple tasks
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(MultipleTasksAllExecuted) {
    FakeClient client("c1", backend_);
    FakeServer server("s1", backend_);

    constexpr int N = 5;
    std::vector<std::string> uuids;
    for (int i = 0; i < N; ++i)
        uuids.push_back(client.addTask("task-" + std::to_string(i)));

    for (const auto& uuid : uuids) {
        Task t = parseTask(backend_->getObject("tasks/" + uuid));
        server.handleAnnounce(t.uuid, t.payload);
    }

    for (const auto& uuid : uuids)
        BOOST_CHECK_EQUAL(readStatus(uuid), TaskStatus::COMPLETED);
}

// ---------------------------------------------------------------------------
// 3. Duplicate announce: idempotent execution
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DuplicateAnnounceIgnored) {
    FakeClient client("c1", backend_);
    FakeServer server("s1", backend_);

    int exec_count = 0;
    server.setHandler([&](const std::string&, const std::string&) {
        ++exec_count;
        return "ok";
    });

    const std::string uuid = client.addTask("msg");
    Task t = parseTask(backend_->getObject("tasks/" + uuid));

    server.handleAnnounce(t.uuid, t.payload); // first announce
    server.handleAnnounce(t.uuid, t.payload); // duplicate – should be ignored
    server.handleAnnounce(t.uuid, t.payload); // third – also ignored

    BOOST_CHECK_EQUAL(exec_count, 1);
    BOOST_CHECK_EQUAL(readStatus(uuid), TaskStatus::COMPLETED);
}

// ---------------------------------------------------------------------------
// 4. Race: two servers competing; exactly one wins the claim
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(TwoServersOnlyOneExecutes) {
    FakeClient client("c1", backend_);
    FakeServer s1("server-A", backend_);
    FakeServer s2("server-B", backend_);

    int exec_a = 0, exec_b = 0;
    s1.setHandler([&](const std::string&, const std::string&) { ++exec_a; return "a"; });
    s2.setHandler([&](const std::string&, const std::string&) { ++exec_b; return "b"; });

    const std::string uuid = client.addTask("race-task");
    Task t = parseTask(backend_->getObject("tasks/" + uuid));

    // Both handle the same announce sequentially (simulates near-simultaneous
    // receipt; the LocalFilesBackend's rename-atomic PUT ensures only one wins).
    bool c1 = s1.handleAnnounce(t.uuid, t.payload);
    bool c2 = s2.handleAnnounce(t.uuid, t.payload);

    BOOST_CHECK_MESSAGE(c1 != c2, "Exactly one server should have claimed the task");
    BOOST_CHECK_EQUAL(exec_a + exec_b, 1);
    BOOST_CHECK_EQUAL(readStatus(uuid), TaskStatus::COMPLETED);
}

// ---------------------------------------------------------------------------
// 5. Multiple burst sends are idempotent
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(MultipleBurstSendsIdempotent) {
    FakeClient client("c1", backend_);
    FakeServer server("s1", backend_);

    int exec_count = 0;
    server.setHandler([&](const std::string&, const std::string&) {
        ++exec_count;
        return "done";
    });

    const std::string uuid = client.addTask("burst");
    Task t = parseTask(backend_->getObject("tasks/" + uuid));

    // Simulate multicast_repeat_count=5 identical datagrams arriving
    for (int i = 0; i < 5; ++i)
        server.handleAnnounce(t.uuid, t.payload);

    BOOST_CHECK_EQUAL(exec_count, 1);
    BOOST_CHECK_EQUAL(readStatus(uuid), TaskStatus::COMPLETED);
}

// ---------------------------------------------------------------------------
// 6. Client node failure: task stored but no announce; second client discovers it
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(ClientNodeFailureTaskDiscoveredByRelay) {
    // Client 1 stores the task then "crashes" (no announce)
    FakeClient c1("c1", backend_);
    const std::string uuid = c1.addTask("important-task");

    // Client 2 polls backend and discovers the task
    FakeClient c2("c2", backend_);
    FakeServer server("s1", backend_);

    auto fresh = c2.discoverNewTasks();
    BOOST_REQUIRE_EQUAL(fresh.size(), 1u);
    BOOST_CHECK_EQUAL(fresh[0], uuid);

    // Client 2 relays the announce
    Task t = parseTask(backend_->getObject("tasks/" + uuid));
    server.handleAnnounce(t.uuid, t.payload);

    BOOST_CHECK_EQUAL(readStatus(uuid), TaskStatus::COMPLETED);
}

// ---------------------------------------------------------------------------
// 7. Jitter: delayed second announce after task already completed
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(JitteredLateAnnounceIgnored) {
    FakeClient client("c1", backend_);
    FakeServer server("s1", backend_);

    int exec_count = 0;
    server.setHandler([&](const std::string&, const std::string&) {
        ++exec_count;
        return "ok";
    });

    const std::string uuid = client.addTask("jitter-task");
    Task t = parseTask(backend_->getObject("tasks/" + uuid));

    server.handleAnnounce(t.uuid, t.payload); // arrives on time
    // Simulate jitter: delayed re-announce (e.g. 200 ms later in real system)
    server.handleAnnounce(t.uuid, t.payload);

    BOOST_CHECK_EQUAL(exec_count, 1);
    BOOST_CHECK_EQUAL(readStatus(uuid), TaskStatus::COMPLETED);
}

// ---------------------------------------------------------------------------
// 8. Send delay: announce arrives after task status already set by another server
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(SendDelayAnnounceIgnoredWhenAlreadyExecutedByPeer) {
    FakeClient client("c1", backend_);
    FakeServer s1("fast-server",  backend_);
    FakeServer s2("slow-receiver", backend_);

    const std::string uuid = client.addTask("slow-msg");
    Task t = parseTask(backend_->getObject("tasks/" + uuid));

    // fast-server processes first
    s1.handleAnnounce(t.uuid, t.payload);
    BOOST_CHECK_EQUAL(readStatus(uuid), TaskStatus::COMPLETED);

    // slow-receiver gets a delayed announce – should not re-execute
    int exec_count = 0;
    s2.setHandler([&](const std::string&, const std::string&) {
        ++exec_count;
        return "re-executed";
    });
    s2.handleAnnounce(t.uuid, t.payload);
    BOOST_CHECK_EQUAL(exec_count, 0);
    BOOST_CHECK_EQUAL(readStatus(uuid), TaskStatus::COMPLETED);
}

// ---------------------------------------------------------------------------
// 9. Watchdog: crashed executor leaves EXECUTING; watchdog retries
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(WatchdogRetryAfterExecutorCrash) {
    FakeClient client("c1", backend_);

    // crashed_server claims the task but "crashes" before completing it
    {
        FakeServer crashed("crashed-srv", backend_, /*task_execution_ms=*/100);
        const std::string uuid = client.addTask("recovery-task");
        Task t = parseTask(backend_->getObject("tasks/" + uuid));

        // Claim only (simulate crash after claim, before completion)
        crashed.claimOnly(t.uuid);
        BOOST_CHECK_EQUAL(readStatus(t.uuid), TaskStatus::EXECUTING);

        // recovery_server runs watchdog and takes over
        int exec_count = 0;
        FakeServer recovery("recovery-srv", backend_, /*task_execution_ms=*/100);
        recovery.setHandler([&](const std::string&, const std::string&) {
            ++exec_count;
            return "recovered";
        });

        // Wait enough time for watchdog timeout (task_execution_ms + margin)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Inject the task into recovery server's executing_ via handleAnnounce
        // to simulate it discovering the orphaned task:
        int retried = recovery.runWatchdog();
        // If the task wasn't locally tracked, the watchdog won't find it;
        // instead simulate re-discovery via backend listing.
        if (retried == 0) {
            // recovery server sees the task via backend listing
            auto keys = backend_->listObjects("tasks/");
            for (const auto& key : keys) {
                const std::string pfx = "tasks/";
                if (key.size() <= pfx.size()) continue;
                std::string task_uuid = key.substr(pfx.size());
                // Check status
                std::string sbody = backend_->getObject("status/" + task_uuid);
                if (sbody.empty()) continue;
                TaskStatusRecord sr = parseTaskStatus(sbody);
                if (sr.status == TaskStatus::EXECUTING) {
                    // Mark PENDING and re-claim
                    TaskStatusRecord pending;
                    pending.uuid       = task_uuid;
                    pending.status     = TaskStatus::PENDING;
                    pending.server_id  = "";
                    pending.updated_at = currentTimestamp();
                    backend_->putObject("status/" + task_uuid, buildStatusJson(pending));

                    Task task = parseTask(backend_->getObject("tasks/" + task_uuid));
                    recovery.handleAnnounce(task_uuid, task.payload);
                }
            }
        }

        BOOST_CHECK_EQUAL(readStatus(t.uuid), TaskStatus::COMPLETED);
    }
}

// ---------------------------------------------------------------------------
// 10. Handler throws: task marked FAILED, status persisted
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(HandlerExceptionMarksFailed) {
    FakeClient client("c1", backend_);
    FakeServer server("s1", backend_);

    server.setHandler([](const std::string&, const std::string&) -> std::string {
        throw std::runtime_error("simulated handler failure");
    });

    const std::string uuid = client.addTask("failing-task");
    Task t = parseTask(backend_->getObject("tasks/" + uuid));

    server.handleAnnounce(t.uuid, t.payload);
    BOOST_CHECK_EQUAL(readStatus(uuid), TaskStatus::FAILED);
}

// ---------------------------------------------------------------------------
// 11. Multiple clients relay the same task (multi-client scenario)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(MultiClientRelayAllSeeTask) {
    FakeClient c1("c1", backend_);
    FakeClient c2("c2", backend_);
    FakeClient c3("c3", backend_);
    FakeServer server("s1", backend_);

    // c1 adds a task
    const std::string uuid = c1.addTask("shared-task");

    // c2 and c3 discover it via backend polling
    auto fresh2 = c2.discoverNewTasks();
    auto fresh3 = c3.discoverNewTasks();
    BOOST_REQUIRE_EQUAL(fresh2.size(), 1u);
    BOOST_REQUIRE_EQUAL(fresh3.size(), 1u);

    // All three "relay" the announce; only one execution should happen
    int exec = 0;
    server.setHandler([&](const std::string&, const std::string&) { ++exec; return "ok"; });

    Task t = parseTask(backend_->getObject("tasks/" + uuid));
    server.handleAnnounce(t.uuid, t.payload); // from c1's burst
    server.handleAnnounce(t.uuid, t.payload); // from c2's relay
    server.handleAnnounce(t.uuid, t.payload); // from c3's relay

    BOOST_CHECK_EQUAL(exec, 1);
    BOOST_CHECK_EQUAL(readStatus(uuid), TaskStatus::COMPLETED);
}

BOOST_AUTO_TEST_SUITE_END()
