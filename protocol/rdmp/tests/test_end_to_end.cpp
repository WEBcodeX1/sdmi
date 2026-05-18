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
//  5. Jitter / send delay: task arrives with delayed second announce
//  6. Multiple burst sends of the same task (idempotent execution)
//  7. Client node failure: task written to backend but no announce sent;
//     backend polling still discovers it

#include <boost/test/unit_test.hpp>

#include "rdmp_common.hpp"
#include "rdmp_local_files.hpp"
#include "rdmp_backend.hpp"

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
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
               std::shared_ptr<LocalFilesBackend> backend)
        : id_(id), backend_(std::move(backend)) {}

    // Simulate receiving a TASK_ANNOUNCE.
    // Returns true if this server claimed and executed the task.
    bool handleAnnounce(const std::string& uuid, const std::string& payload) {
        // Skip if already completed/failed/executing locally (per spec 2.4:
        // only proceed if status is "pending")
        auto it = local_status_.find(uuid);
        if (it != local_status_.end()) {
            if (it->second.status == TaskStatus::COMPLETED ||
                it->second.status == TaskStatus::EXECUTING ||
                it->second.status == TaskStatus::FAILED) {
                return false;
            }
        }
        if (executing_.count(uuid)) return false;
        return tryClaimAndExecute(uuid, payload);
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

private:
    struct ExecutingTask { std::string uuid; int64_t started_ms = 0; };

    std::string id_;
    std::shared_ptr<LocalFilesBackend> backend_;
    std::function<std::string(const std::string&, const std::string&)> handler_;
    std::unordered_map<std::string, TaskStatusRecord> local_status_;
    std::unordered_map<std::string, ExecutingTask> executing_;

    bool tryClaimAndExecute(const std::string& uuid, const std::string& payload) {
        // Read current status
        std::string existing = backend_->getObject("status/" + uuid);
        if (!existing.empty()) {
            TaskStatusRecord cur = parseTaskStatus(existing);
            if (cur.status == TaskStatus::EXECUTING ||
                cur.status == TaskStatus::COMPLETED ||
                cur.status == TaskStatus::FAILED) {
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
// 9. Handler throws: task marked FAILED, status persisted
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
// 10. Multiple clients relay the same task (multi-client scenario)
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

// ---------------------------------------------------------------------------
// 11. Full workflow: 2 clients + 3 servers, normal mode (spec 2.1–2.4)
//
//  - An external entity adds a task via addTask() (simulating addNewTask()).
//  - Both clients discover the pending task via backend polling.
//  - Both clients "send" the multicast (simulated as handleAnnounce calls).
//  - All 3 servers receive the announce from both clients (6 calls total).
//  - Exactly ONE server should execute the task; exactly ONE status object.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(TwoClientThreeServerWorkflowNormalMode) {
    FakeClient c1("client1", backend_);
    FakeClient c2("client2", backend_);
    FakeServer s1("server1", backend_);
    FakeServer s2("server2", backend_);
    FakeServer s3("server3", backend_);

    int exec_s1 = 0, exec_s2 = 0, exec_s3 = 0;
    s1.setHandler([&](const std::string&, const std::string&) { ++exec_s1; return "s1-ok"; });
    s2.setHandler([&](const std::string&, const std::string&) { ++exec_s2; return "s2-ok"; });
    s3.setHandler([&](const std::string&, const std::string&) { ++exec_s3; return "s3-ok"; });

    // External entity (c1) adds the task.
    const std::string uuid = c1.addTask("scale-out node-7");
    BOOST_REQUIRE(!uuid.empty());

    // c2 polls and discovers the pending task.
    auto fresh = c2.discoverNewTasks();
    BOOST_REQUIRE_EQUAL(fresh.size(), 1u);
    BOOST_CHECK_EQUAL(fresh[0], uuid);

    // Fetch the task payload (both clients have it).
    Task t = parseTask(backend_->getObject("tasks/" + uuid));
    BOOST_REQUIRE(!t.uuid.empty());

    // Simulate multicast delivery: both clients send to all 3 servers.
    // In the real system each client broadcasts UDP; here we call handleAnnounce.
    std::vector<FakeServer*> servers = {&s1, &s2, &s3};
    for (FakeServer* srv : servers)
        srv->handleAnnounce(t.uuid, t.payload); // from client1
    for (FakeServer* srv : servers)
        srv->handleAnnounce(t.uuid, t.payload); // from client2 (delayed relay)

    // Exactly one server executed the task.
    const int total_exec = exec_s1 + exec_s2 + exec_s3;
    BOOST_CHECK_EQUAL(total_exec, 1);

    // Status is COMPLETED.
    BOOST_CHECK_EQUAL(readStatus(uuid), TaskStatus::COMPLETED);

    // Exactly one status object in the bucket (normal mode: shared key).
    auto status_keys = backend_->listObjects("status/");
    BOOST_CHECK_EQUAL(status_keys.size(), 1u);
}

// ---------------------------------------------------------------------------
// 12. Full workflow: 2 clients + 3 servers, bypass mode (spec 2.5/2.6)
//
//  - bypass_pending_check = true: every server executes the task.
//  - All 3 servers write their own per-server status records.
//  - 3 status objects must exist in the bucket.
// ---------------------------------------------------------------------------

// FakeServer variant that supports bypass_pending_check.
class FakeServerBypass {
public:
    FakeServerBypass(const std::string& id,
                     std::shared_ptr<LocalFilesBackend> backend)
        : id_(id), backend_(std::move(backend)) {}

    // Simulate receiving a TASK_ANNOUNCE in bypass mode:
    // always execute regardless of backend status.
    bool handleAnnounce(const std::string& uuid, const std::string& payload) {
        if (executed_.count(uuid)) return false; // don't double-execute same node
        executed_.insert(uuid);

        // Execute task
        std::string result;
        try {
            result = handler_ ? handler_(uuid, payload) : "ok";
        } catch (...) {
            result = "error";
        }

        // Write per-server status (key: "status/<uuid>/<server_id>")
        TaskStatusRecord r;
        r.uuid       = uuid;
        r.status     = TaskStatus::COMPLETED;
        r.server_id  = id_;
        r.updated_at = currentTimestamp();
        r.result     = result;
        backend_->putObject("status/" + uuid + "/" + id_, buildStatusJson(r));
        return true;
    }

    void setHandler(std::function<std::string(const std::string&, const std::string&)> h) {
        handler_ = std::move(h);
    }

    const std::string& id() const { return id_; }

private:
    std::string id_;
    std::shared_ptr<LocalFilesBackend> backend_;
    std::function<std::string(const std::string&, const std::string&)> handler_;
    std::unordered_set<std::string> executed_;
};

BOOST_AUTO_TEST_CASE(TwoClientThreeServerWorkflowBypassMode) {
    FakeClient c1("client1", backend_);
    FakeClient c2("client2", backend_);
    FakeServerBypass s1("server1", backend_);
    FakeServerBypass s2("server2", backend_);
    FakeServerBypass s3("server3", backend_);

    int exec_s1 = 0, exec_s2 = 0, exec_s3 = 0;
    s1.setHandler([&](const std::string&, const std::string&) { ++exec_s1; return "s1-ok"; });
    s2.setHandler([&](const std::string&, const std::string&) { ++exec_s2; return "s2-ok"; });
    s3.setHandler([&](const std::string&, const std::string&) { ++exec_s3; return "s3-ok"; });

    // External entity adds the task.
    const std::string uuid = c1.addTask("scale-out node-7");
    BOOST_REQUIRE(!uuid.empty());

    // c2 polls and discovers the pending task.
    auto fresh = c2.discoverNewTasks();
    BOOST_REQUIRE_EQUAL(fresh.size(), 1u);

    Task t = parseTask(backend_->getObject("tasks/" + uuid));
    BOOST_REQUIRE(!t.uuid.empty());

    // Both clients send to all 3 servers.
    // In bypass mode each server executes once (deduplicated per node).
    std::vector<FakeServerBypass*> servers = {&s1, &s2, &s3};
    for (FakeServerBypass* srv : servers)
        srv->handleAnnounce(t.uuid, t.payload); // from client1
    for (FakeServerBypass* srv : servers)
        srv->handleAnnounce(t.uuid, t.payload); // duplicate from client2 – ignored per node

    // All 3 servers executed the task.
    BOOST_CHECK_EQUAL(exec_s1, 1);
    BOOST_CHECK_EQUAL(exec_s2, 1);
    BOOST_CHECK_EQUAL(exec_s3, 1);

    // Exactly 3 per-server status objects in the bucket.
    auto status_keys = backend_->listObjects("status/");
    BOOST_CHECK_EQUAL(status_keys.size(), 3u);

    // Verify each server's status is COMPLETED.
    for (const std::string srv_id : std::vector<std::string>{"server1", "server2", "server3"}) {
        std::string body = backend_->getObject("status/" + uuid + "/" + srv_id);
        BOOST_REQUIRE_MESSAGE(!body.empty(),
            "Missing status for server: " + srv_id);
        TaskStatusRecord rec = parseTaskStatus(body);
        BOOST_CHECK_EQUAL(rec.status, TaskStatus::COMPLETED);
        BOOST_CHECK_EQUAL(rec.server_id, srv_id);
    }
}

// ---------------------------------------------------------------------------
// 14. FAILED status prevents re-execution in normal mode (spec 2.4)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(FailedStatusPreventsReExecution) {
    FakeClient client("c1", backend_);
    FakeServer server("s1", backend_);

    int exec_count = 0;
    server.setHandler([&](const std::string&, const std::string&) -> std::string {
        ++exec_count;
        throw std::runtime_error("handler failed");
    });

    const std::string uuid = client.addTask("failing-msg");
    Task t = parseTask(backend_->getObject("tasks/" + uuid));

    // First announce: executes, fails.
    server.handleAnnounce(t.uuid, t.payload);
    BOOST_CHECK_EQUAL(exec_count, 1);
    BOOST_CHECK_EQUAL(readStatus(uuid), TaskStatus::FAILED);

    // Second announce (e.g. late packet): must NOT re-execute (FAILED ≠ PENDING).
    server.handleAnnounce(t.uuid, t.payload);
    BOOST_CHECK_EQUAL(exec_count, 1);
}

BOOST_AUTO_TEST_SUITE_END()
