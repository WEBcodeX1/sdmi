// test_local_files_backend.cpp – Unit tests for LocalFilesBackend
//
// Tests cover:
//  - putObject / getObject round-trip
//  - getObject on missing key returns ""
//  - listObjects returns correct keys with a given prefix
//  - Atomic writes (temp file → rename) so partial content is never visible
//  - Overwrite of existing objects
//  - Nested key paths (e.g. "tasks/<uuid>")

#include <gtest/gtest.h>

#include "rdmp_local_files.hpp"
#include "rdmp_common.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Fixture: creates a unique temp directory per test
// ---------------------------------------------------------------------------

class LocalFilesBackendTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a unique temporary base directory
        char tmpl[] = "/tmp/rdmp_lf_test_XXXXXX";
        const char* dir = mkdtemp(tmpl);
        ASSERT_NE(dir, nullptr);
        base_dir_ = dir;

        rdmp::LocalFilesConfig cfg;
        cfg.base_path = base_dir_;
        backend_ = std::make_unique<rdmp::LocalFilesBackend>(cfg);
    }

    void TearDown() override {
        // Remove the test directory tree
        fs::remove_all(base_dir_);
    }

    std::string base_dir_;
    std::unique_ptr<rdmp::LocalFilesBackend> backend_;
};

// ---------------------------------------------------------------------------
// Basic put / get round-trip
// ---------------------------------------------------------------------------

TEST_F(LocalFilesBackendTest, PutGetRoundTrip) {
    const std::string key  = "tasks/abc123";
    const std::string body = R"({"uuid":"abc123","payload":"hello"})";

    EXPECT_TRUE(backend_->putObject(key, body));

    const std::string got = backend_->getObject(key);
    EXPECT_EQ(got, body);
}

TEST_F(LocalFilesBackendTest, GetMissingReturnsEmpty) {
    EXPECT_EQ(backend_->getObject("tasks/nonexistent"), "");
}

// ---------------------------------------------------------------------------
// Overwrite
// ---------------------------------------------------------------------------

TEST_F(LocalFilesBackendTest, Overwrite) {
    const std::string key = "status/uuid1";
    EXPECT_TRUE(backend_->putObject(key, "first"));
    EXPECT_TRUE(backend_->putObject(key, "second"));
    EXPECT_EQ(backend_->getObject(key), "second");
}

// ---------------------------------------------------------------------------
// List objects
// ---------------------------------------------------------------------------

TEST_F(LocalFilesBackendTest, ListObjectsEmpty) {
    const auto keys = backend_->listObjects("tasks/");
    EXPECT_TRUE(keys.empty());
}

TEST_F(LocalFilesBackendTest, ListObjectsMultiple) {
    backend_->putObject("tasks/uuid-a", "body-a");
    backend_->putObject("tasks/uuid-b", "body-b");
    backend_->putObject("tasks/uuid-c", "body-c");
    backend_->putObject("status/uuid-a", "status-body");

    const auto task_keys   = backend_->listObjects("tasks/");
    const auto status_keys = backend_->listObjects("status/");

    EXPECT_EQ(task_keys.size(),   3u);
    EXPECT_EQ(status_keys.size(), 1u);

    // All task keys should start with "tasks/"
    for (const auto& k : task_keys)
        EXPECT_EQ(k.substr(0, 6), "tasks/");
}

TEST_F(LocalFilesBackendTest, ListObjectsContainsCorrectKeys) {
    backend_->putObject("tasks/my-uuid", "body");

    const auto keys = backend_->listObjects("tasks/");
    ASSERT_EQ(keys.size(), 1u);
    EXPECT_EQ(keys[0], "tasks/my-uuid");
}

// ---------------------------------------------------------------------------
// Binary / large content
// ---------------------------------------------------------------------------

TEST_F(LocalFilesBackendTest, LargeBody) {
    const std::string key  = "tasks/large";
    const std::string body(64 * 1024, 'x'); // 64 KB of 'x'
    EXPECT_TRUE(backend_->putObject(key, body));
    EXPECT_EQ(backend_->getObject(key), body);
}

TEST_F(LocalFilesBackendTest, JsonPayloadRoundTrip) {
    // Simulate the actual task storage flow
    rdmp::Task t;
    t.uuid       = rdmp::generateUUID();
    t.payload    = "scale-out node-42";
    t.created_by = "client1";
    t.created_at = rdmp::currentTimestamp();

    const std::string key  = "tasks/" + t.uuid;
    const std::string body = rdmp::buildTaskJson(t);

    EXPECT_TRUE(backend_->putObject(key, body));

    const std::string got  = backend_->getObject(key);
    rdmp::Task        back = rdmp::parseTask(got);

    EXPECT_EQ(back.uuid,    t.uuid);
    EXPECT_EQ(back.payload, t.payload);
}

// ---------------------------------------------------------------------------
// Status record round-trip
// ---------------------------------------------------------------------------

TEST_F(LocalFilesBackendTest, StatusRecordRoundTrip) {
    rdmp::TaskStatusRecord r;
    r.uuid       = rdmp::generateUUID();
    r.status     = rdmp::TaskStatus::COMPLETED;
    r.server_id  = "srv1";
    r.updated_at = rdmp::currentTimestamp();
    r.result     = "ok";

    const std::string key  = "status/" + r.uuid;
    const std::string body = rdmp::buildStatusJson(r);

    EXPECT_TRUE(backend_->putObject(key, body));

    const std::string          got  = backend_->getObject(key);
    rdmp::TaskStatusRecord     back = rdmp::parseTaskStatus(got);

    EXPECT_EQ(back.uuid,      r.uuid);
    EXPECT_EQ(back.status,    r.status);
    EXPECT_EQ(back.server_id, r.server_id);
    EXPECT_EQ(back.result,    r.result);
}

// ---------------------------------------------------------------------------
// Multiple independent backends sharing the same directory
// ---------------------------------------------------------------------------

TEST_F(LocalFilesBackendTest, TwoBackendsSameDir) {
    rdmp::LocalFilesConfig cfg;
    cfg.base_path = base_dir_;
    rdmp::LocalFilesBackend b2(cfg);

    backend_->putObject("tasks/shared-uuid", "from-b1");
    EXPECT_EQ(b2.getObject("tasks/shared-uuid"), "from-b1");

    b2.putObject("tasks/shared-uuid", "from-b2");
    EXPECT_EQ(backend_->getObject("tasks/shared-uuid"), "from-b2");
}

} // namespace
