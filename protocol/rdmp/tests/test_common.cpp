// test_common.cpp – Unit tests for rdmp_common utilities
//
// Tests cover:
//  - taskStatusToString / stringToTaskStatus round-trips
//  - currentTimeMs returns a plausible epoch value
//  - generateUUID produces well-formed v4 UUIDs
//  - buildTaskJson / parseTask round-trip
//  - buildStatusJson / parseTaskStatus round-trip
//  - extractJsonField edge cases

#include <gtest/gtest.h>

#include "rdmp_common.hpp"

#include <regex>
#include <string>

namespace {

// ---------------------------------------------------------------------------
// TaskStatus conversions
// ---------------------------------------------------------------------------

TEST(CommonTest, TaskStatusRoundTrip) {
    using rdmp::TaskStatus;
    EXPECT_EQ(rdmp::stringToTaskStatus(rdmp::taskStatusToString(TaskStatus::PENDING)),
              TaskStatus::PENDING);
    EXPECT_EQ(rdmp::stringToTaskStatus(rdmp::taskStatusToString(TaskStatus::EXECUTING)),
              TaskStatus::EXECUTING);
    EXPECT_EQ(rdmp::stringToTaskStatus(rdmp::taskStatusToString(TaskStatus::COMPLETED)),
              TaskStatus::COMPLETED);
    EXPECT_EQ(rdmp::stringToTaskStatus(rdmp::taskStatusToString(TaskStatus::FAILED)),
              TaskStatus::FAILED);
}

TEST(CommonTest, TaskStatusUnknown) {
    EXPECT_EQ(rdmp::stringToTaskStatus("bogus"), rdmp::TaskStatus::UNKNOWN);
    EXPECT_EQ(rdmp::taskStatusToString(rdmp::TaskStatus::UNKNOWN), "unknown");
}

// ---------------------------------------------------------------------------
// Time helpers
// ---------------------------------------------------------------------------

TEST(CommonTest, CurrentTimeMsIsReasonable) {
    // Should be somewhere after 2024-01-01 (epoch ~1704067200000 ms)
    EXPECT_GT(rdmp::currentTimeMs(), static_cast<int64_t>(1704067200000LL));
}

TEST(CommonTest, CurrentTimestampFormat) {
    std::string ts = rdmp::currentTimestamp();
    // Expected: "YYYY-MM-DDTHH:MM:SSZ"
    std::regex re(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)");
    EXPECT_TRUE(std::regex_match(ts, re)) << "timestamp was: " << ts;
}

// ---------------------------------------------------------------------------
// UUID generation
// ---------------------------------------------------------------------------

TEST(CommonTest, GenerateUUIDFormat) {
    const std::string uuid = rdmp::generateUUID();
    EXPECT_EQ(uuid.size(), 36u);
    // Pattern: 8-4-4-4-12 hex digits
    std::regex re("[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}");
    EXPECT_TRUE(std::regex_match(uuid, re)) << "uuid was: " << uuid;
}

TEST(CommonTest, GenerateUUIDUnique) {
    std::string a = rdmp::generateUUID();
    std::string b = rdmp::generateUUID();
    EXPECT_NE(a, b);
}

// ---------------------------------------------------------------------------
// Task JSON round-trip
// ---------------------------------------------------------------------------

TEST(CommonTest, TaskJsonRoundTrip) {
    rdmp::Task t;
    t.uuid       = rdmp::generateUUID();
    t.payload    = "hello world";
    t.created_by = "test-client";
    t.created_at = rdmp::currentTimestamp();

    const std::string json = rdmp::buildTaskJson(t);
    const rdmp::Task  back = rdmp::parseTask(json);

    EXPECT_EQ(back.uuid,       t.uuid);
    EXPECT_EQ(back.payload,    t.payload);
    EXPECT_EQ(back.created_by, t.created_by);
    EXPECT_EQ(back.created_at, t.created_at);
}

TEST(CommonTest, TaskJsonSpecialChars) {
    rdmp::Task t;
    t.uuid       = rdmp::generateUUID();
    t.payload    = "line1\nline2\ttab\"quote\\backslash";
    t.created_by = "client";
    t.created_at = rdmp::currentTimestamp();

    const std::string json = rdmp::buildTaskJson(t);
    const rdmp::Task  back = rdmp::parseTask(json);

    EXPECT_EQ(back.payload, t.payload);
}

// ---------------------------------------------------------------------------
// TaskStatusRecord JSON round-trip
// ---------------------------------------------------------------------------

TEST(CommonTest, StatusJsonRoundTrip) {
    rdmp::TaskStatusRecord r;
    r.uuid       = rdmp::generateUUID();
    r.status     = rdmp::TaskStatus::COMPLETED;
    r.server_id  = "srv1";
    r.updated_at = rdmp::currentTimestamp();
    r.result     = "ok";

    const std::string        json = rdmp::buildStatusJson(r);
    const rdmp::TaskStatusRecord back = rdmp::parseTaskStatus(json);

    EXPECT_EQ(back.uuid,       r.uuid);
    EXPECT_EQ(back.status,     r.status);
    EXPECT_EQ(back.server_id,  r.server_id);
    EXPECT_EQ(back.updated_at, r.updated_at);
    EXPECT_EQ(back.result,     r.result);
}

// ---------------------------------------------------------------------------
// extractJsonField edge cases
// ---------------------------------------------------------------------------

TEST(CommonTest, ExtractJsonFieldMissing) {
    EXPECT_EQ(rdmp::extractJsonField("{}", "missing"), "");
}

TEST(CommonTest, ExtractJsonFieldNumeric) {
    const std::string json = R"({"count":42})";
    EXPECT_EQ(rdmp::extractJsonField(json, "count"), "42");
}

TEST(CommonTest, ExtractJsonFieldBool) {
    const std::string json = R"({"ok":true})";
    EXPECT_EQ(rdmp::extractJsonField(json, "ok"), "true");
}

} // namespace
