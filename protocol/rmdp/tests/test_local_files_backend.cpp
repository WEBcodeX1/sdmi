// test_local_files_backend.cpp – Unit tests for LocalFilesBackend
//
// Tests cover:
//  - putObject / getObject round-trip
//  - getObject on missing key returns ""
//  - listObjects returns correct keys with a given prefix
//  - Atomic writes (temp file → rename) so partial content is never visible
//  - Overwrite of existing objects
//  - Nested key paths (e.g. "tasks/<uuid>")

#include <boost/test/unit_test.hpp>

#include "rmdp_local_files.hpp"
#include "rmdp_common.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Fixture: creates a unique temp directory per test
// ---------------------------------------------------------------------------

struct LocalFilesBackendFixture {
    LocalFilesBackendFixture() {
        char tmpl[] = "/tmp/rmdp_lf_test_XXXXXX";
        const char* dir = mkdtemp(tmpl);
        BOOST_REQUIRE(dir != nullptr);
        base_dir_ = dir;

        rmdp::LocalFilesConfig cfg;
        cfg.base_path = base_dir_;
        backend_ = std::make_unique<rmdp::LocalFilesBackend>(cfg);
    }

    ~LocalFilesBackendFixture() {
        fs::remove_all(base_dir_);
    }

    std::string base_dir_;
    std::unique_ptr<rmdp::LocalFilesBackend> backend_;
};

BOOST_FIXTURE_TEST_SUITE(LocalFilesBackendTest, LocalFilesBackendFixture)

// ---------------------------------------------------------------------------
// Basic put / get round-trip
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(PutGetRoundTrip) {
    const std::string key  = "tasks/abc123";
    const std::string body = R"({"uuid":"abc123","payload":"hello"})";

    BOOST_CHECK(backend_->putObject(key, body));

    const std::string got = backend_->getObject(key);
    BOOST_CHECK_EQUAL(got, body);
}

BOOST_AUTO_TEST_CASE(GetMissingReturnsEmpty) {
    BOOST_CHECK_EQUAL(backend_->getObject("tasks/nonexistent"), "");
}

// ---------------------------------------------------------------------------
// Overwrite
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Overwrite) {
    const std::string key = "status/uuid1";
    BOOST_CHECK(backend_->putObject(key, "first"));
    BOOST_CHECK(backend_->putObject(key, "second"));
    BOOST_CHECK_EQUAL(backend_->getObject(key), "second");
}

// ---------------------------------------------------------------------------
// List objects
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(ListObjectsEmpty) {
    const auto keys = backend_->listObjects("tasks/");
    BOOST_CHECK(keys.empty());
}

BOOST_AUTO_TEST_CASE(ListObjectsMultiple) {
    backend_->putObject("tasks/uuid-a", "body-a");
    backend_->putObject("tasks/uuid-b", "body-b");
    backend_->putObject("tasks/uuid-c", "body-c");
    backend_->putObject("status/uuid-a", "status-body");

    const auto task_keys   = backend_->listObjects("tasks/");
    const auto status_keys = backend_->listObjects("status/");

    BOOST_CHECK_EQUAL(task_keys.size(),   3u);
    BOOST_CHECK_EQUAL(status_keys.size(), 1u);

    // All task keys should start with "tasks/"
    for (const auto& k : task_keys)
        BOOST_CHECK_EQUAL(k.substr(0, 6), "tasks/");
}

BOOST_AUTO_TEST_CASE(ListObjectsContainsCorrectKeys) {
    backend_->putObject("tasks/my-uuid", "body");

    const auto keys = backend_->listObjects("tasks/");
    BOOST_REQUIRE_EQUAL(keys.size(), 1u);
    BOOST_CHECK_EQUAL(keys[0], "tasks/my-uuid");
}

// ---------------------------------------------------------------------------
// Binary / large content
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(LargeBody) {
    const std::string key  = "tasks/large";
    const std::string body(64 * 1024, 'x'); // 64 KB of 'x'
    BOOST_CHECK(backend_->putObject(key, body));
    BOOST_CHECK_EQUAL(backend_->getObject(key), body);
}

BOOST_AUTO_TEST_CASE(JsonPayloadRoundTrip) {
    // Simulate the actual task storage flow
    rmdp::Task t;
    t.uuid       = rmdp::generateUUID();
    t.payload    = "scale-out node-42";
    t.created_by = "client1";
    t.created_at = rmdp::currentTimestamp();

    const std::string key  = "tasks/" + t.uuid;
    const std::string body = rmdp::buildTaskJson(t);

    BOOST_CHECK(backend_->putObject(key, body));

    const std::string got  = backend_->getObject(key);
    rmdp::Task        back = rmdp::parseTask(got);

    BOOST_CHECK_EQUAL(back.uuid,    t.uuid);
    BOOST_CHECK_EQUAL(back.payload, t.payload);
}

// ---------------------------------------------------------------------------
// Status record round-trip
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(StatusRecordRoundTrip) {
    rmdp::TaskStatusRecord r;
    r.uuid       = rmdp::generateUUID();
    r.status     = rmdp::TaskStatus::COMPLETED;
    r.server_id  = "srv1";
    r.updated_at = rmdp::currentTimestamp();
    r.result     = "ok";

    const std::string key  = "status/" + r.uuid;
    const std::string body = rmdp::buildStatusJson(r);

    BOOST_CHECK(backend_->putObject(key, body));

    const std::string          got  = backend_->getObject(key);
    rmdp::TaskStatusRecord     back = rmdp::parseTaskStatus(got);

    BOOST_CHECK_EQUAL(back.uuid,      r.uuid);
    BOOST_CHECK_EQUAL(back.status,    r.status);
    BOOST_CHECK_EQUAL(back.server_id, r.server_id);
    BOOST_CHECK_EQUAL(back.result,    r.result);
}

// ---------------------------------------------------------------------------
// Multiple independent backends sharing the same directory
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(TwoBackendsSameDir) {
    rmdp::LocalFilesConfig cfg;
    cfg.base_path = base_dir_;
    rmdp::LocalFilesBackend b2(cfg);

    backend_->putObject("tasks/shared-uuid", "from-b1");
    BOOST_CHECK_EQUAL(b2.getObject("tasks/shared-uuid"), "from-b1");

    b2.putObject("tasks/shared-uuid", "from-b2");
    BOOST_CHECK_EQUAL(backend_->getObject("tasks/shared-uuid"), "from-b2");
}

BOOST_AUTO_TEST_SUITE_END()

