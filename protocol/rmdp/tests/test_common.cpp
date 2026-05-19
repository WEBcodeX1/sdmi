// test_common.cpp – Unit tests for rmdp_common utilities
//
// Tests cover:
//  - taskStatusToString / stringToTaskStatus round-trips
//  - currentTimeMs returns a plausible epoch value
//  - generateUUID produces well-formed v4 UUIDs
//  - buildTaskJson / parseTask round-trip
//  - buildStatusJson / parseTaskStatus round-trip
//  - extractJsonField edge cases

#include <boost/test/unit_test.hpp>

#include "rmdp_common.hpp"

#include <regex>
#include <string>

BOOST_AUTO_TEST_SUITE(CommonTest)

// ---------------------------------------------------------------------------
// TaskStatus conversions
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(TaskStatusRoundTrip) {
    using rmdp::TaskStatus;
    BOOST_CHECK_EQUAL(rmdp::stringToTaskStatus(rmdp::taskStatusToString(TaskStatus::PENDING)),
                      TaskStatus::PENDING);
    BOOST_CHECK_EQUAL(rmdp::stringToTaskStatus(rmdp::taskStatusToString(TaskStatus::EXECUTING)),
                      TaskStatus::EXECUTING);
    BOOST_CHECK_EQUAL(rmdp::stringToTaskStatus(rmdp::taskStatusToString(TaskStatus::COMPLETED)),
                      TaskStatus::COMPLETED);
    BOOST_CHECK_EQUAL(rmdp::stringToTaskStatus(rmdp::taskStatusToString(TaskStatus::FAILED)),
                      TaskStatus::FAILED);
}

BOOST_AUTO_TEST_CASE(TaskStatusUnknown) {
    BOOST_CHECK_EQUAL(rmdp::stringToTaskStatus("bogus"), rmdp::TaskStatus::UNKNOWN);
    BOOST_CHECK_EQUAL(rmdp::taskStatusToString(rmdp::TaskStatus::UNKNOWN), "unknown");
}

// ---------------------------------------------------------------------------
// Time helpers
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(CurrentTimeMsIsReasonable) {
    // Should be somewhere after 2024-01-01 (epoch ~1704067200000 ms)
    BOOST_CHECK_GT(rmdp::currentTimeMs(), static_cast<int64_t>(1704067200000LL));
}

BOOST_AUTO_TEST_CASE(CurrentTimestampFormat) {
    std::string ts = rmdp::currentTimestamp();
    // Expected: "YYYY-MM-DDTHH:MM:SSZ"
    std::regex re(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)");
    BOOST_CHECK_MESSAGE(std::regex_match(ts, re), "timestamp was: " + ts);
}

// ---------------------------------------------------------------------------
// UUID generation
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(GenerateUUIDFormat) {
    const std::string uuid = rmdp::generateUUID();
    BOOST_CHECK_EQUAL(uuid.size(), 36u);
    // Pattern: 8-4-4-4-12 hex digits
    std::regex re("[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}");
    BOOST_CHECK_MESSAGE(std::regex_match(uuid, re), "uuid was: " + uuid);
}

BOOST_AUTO_TEST_CASE(GenerateUUIDUnique) {
    std::string a = rmdp::generateUUID();
    std::string b = rmdp::generateUUID();
    BOOST_CHECK_NE(a, b);
}

// ---------------------------------------------------------------------------
// Task JSON round-trip
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(TaskJsonRoundTrip) {
    rmdp::Task t;
    t.uuid       = rmdp::generateUUID();
    t.payload    = "hello world";
    t.created_by = "test-client";
    t.created_at = rmdp::currentTimestamp();

    const std::string json = rmdp::buildTaskJson(t);
    const rmdp::Task  back = rmdp::parseTask(json);

    BOOST_CHECK_EQUAL(back.uuid,       t.uuid);
    BOOST_CHECK_EQUAL(back.payload,    t.payload);
    BOOST_CHECK_EQUAL(back.created_by, t.created_by);
    BOOST_CHECK_EQUAL(back.created_at, t.created_at);
}

BOOST_AUTO_TEST_CASE(TaskJsonSpecialChars) {
    rmdp::Task t;
    t.uuid       = rmdp::generateUUID();
    t.payload    = "line1\nline2\ttab\"quote\\backslash";
    t.created_by = "client";
    t.created_at = rmdp::currentTimestamp();

    const std::string json = rmdp::buildTaskJson(t);
    const rmdp::Task  back = rmdp::parseTask(json);

    BOOST_CHECK_EQUAL(back.payload, t.payload);
}

// ---------------------------------------------------------------------------
// TaskStatusRecord JSON round-trip
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(StatusJsonRoundTrip) {
    rmdp::TaskStatusRecord r;
    r.uuid       = rmdp::generateUUID();
    r.status     = rmdp::TaskStatus::COMPLETED;
    r.server_id  = "srv1";
    r.updated_at = rmdp::currentTimestamp();
    r.result     = "ok";

    const std::string        json = rmdp::buildStatusJson(r);
    const rmdp::TaskStatusRecord back = rmdp::parseTaskStatus(json);

    BOOST_CHECK_EQUAL(back.uuid,       r.uuid);
    BOOST_CHECK_EQUAL(back.status,     r.status);
    BOOST_CHECK_EQUAL(back.server_id,  r.server_id);
    BOOST_CHECK_EQUAL(back.updated_at, r.updated_at);
    BOOST_CHECK_EQUAL(back.result,     r.result);
}

// ---------------------------------------------------------------------------
// extractJsonField edge cases
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(ExtractJsonFieldMissing) {
    BOOST_CHECK_EQUAL(rmdp::extractJsonField("{}", "missing"), "");
}

BOOST_AUTO_TEST_CASE(ExtractJsonFieldNumeric) {
    const std::string json = R"({"count":42})";
    BOOST_CHECK_EQUAL(rmdp::extractJsonField(json, "count"), "42");
}

BOOST_AUTO_TEST_CASE(ExtractJsonFieldBool) {
    const std::string json = R"({"ok":true})";
    BOOST_CHECK_EQUAL(rmdp::extractJsonField(json, "ok"), "true");
}

BOOST_AUTO_TEST_SUITE_END()

