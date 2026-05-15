// test_config.cpp – Unit tests for JSON configuration parsing
//
// Tests cover:
//  - Valid client and server config files
//  - Default values when optional fields are absent
//  - The global.synctype field ("s3" and "local-files")
//  - Error handling for missing / malformed files

#include <gtest/gtest.h>

#include "rdmp_common.hpp"

#include <cstdio>
#include <fstream>
#include <string>

namespace {

// ---------------------------------------------------------------------------
// Helper: write a temporary JSON file and return its path.
// ---------------------------------------------------------------------------

std::string writeTmpJson(const std::string& content) {
    char path[] = "/tmp/rdmp_test_config_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) throw std::runtime_error("mkstemp failed");
    // Write via ofstream for simplicity
    close(fd);
    std::ofstream f(path);
    f << content;
    return std::string(path);
}

// ---------------------------------------------------------------------------
// Client config – full valid JSON
// ---------------------------------------------------------------------------

TEST(ConfigTest, ClientFullConfig) {
    const std::string json = R"({
        "global": { "synctype": "s3" },
        "multicast": {
            "group": "239.9.9.9",
            "port": 6000,
            "ttl": 16,
            "interface": "eth1"
        },
        "s3": {
            "endpoint": "http://mys3:9000",
            "bucket": "my-bucket",
            "access_key": "AKID",
            "secret_key": "SECRET",
            "region": "eu-west-1"
        },
        "local_files": {
            "base_path": "/var/rdmp"
        },
        "timeouts": {
            "task_execution_ms": 1000,
            "s3_poll_interval_ms": 500,
            "degradation_threshold_ms": 800,
            "watchdog_interval_ms": 1200,
            "retry_delay_ms": 2000,
            "multicast_repeat_count": 5,
            "multicast_repeat_interval_ms": 200
        },
        "node": { "id": "myclient" }
    })";

    const std::string path = writeTmpJson(json);
    const rdmp::ClientConfig cfg = rdmp::loadClientConfig(path);
    std::remove(path.c_str());

    EXPECT_EQ(cfg.global.synctype, rdmp::SyncType::S3);
    EXPECT_EQ(cfg.multicast.group, "239.9.9.9");
    EXPECT_EQ(cfg.multicast.port,  6000);
    EXPECT_EQ(cfg.multicast.ttl,   16);
    EXPECT_EQ(cfg.multicast.iface, "eth1");
    EXPECT_EQ(cfg.s3.endpoint,     "http://mys3:9000");
    EXPECT_EQ(cfg.s3.bucket,       "my-bucket");
    EXPECT_EQ(cfg.s3.access_key,   "AKID");
    EXPECT_EQ(cfg.s3.secret_key,   "SECRET");
    EXPECT_EQ(cfg.s3.region,       "eu-west-1");
    EXPECT_EQ(cfg.local_files.base_path, "/var/rdmp");
    EXPECT_EQ(cfg.timeouts.task_execution_ms,        1000u);
    EXPECT_EQ(cfg.timeouts.s3_poll_interval_ms,       500u);
    EXPECT_EQ(cfg.timeouts.degradation_threshold_ms,  800u);
    EXPECT_EQ(cfg.timeouts.watchdog_interval_ms,     1200u);
    EXPECT_EQ(cfg.timeouts.retry_delay_ms,           2000u);
    EXPECT_EQ(cfg.timeouts.multicast_repeat_count,      5u);
    EXPECT_EQ(cfg.timeouts.multicast_repeat_interval_ms, 200u);
    EXPECT_EQ(cfg.node_id, "myclient");
}

// ---------------------------------------------------------------------------
// Client config – local-files synctype
// ---------------------------------------------------------------------------

TEST(ConfigTest, ClientLocalFilesSynctype) {
    const std::string json = R"({
        "global": { "synctype": "local-files" },
        "local_files": { "base_path": "/tmp/rdmp-test" },
        "node": { "id": "lf-client" }
    })";

    const std::string path = writeTmpJson(json);
    const rdmp::ClientConfig cfg = rdmp::loadClientConfig(path);
    std::remove(path.c_str());

    EXPECT_EQ(cfg.global.synctype, rdmp::SyncType::LocalFiles);
    EXPECT_EQ(cfg.local_files.base_path, "/tmp/rdmp-test");
    EXPECT_EQ(cfg.node_id, "lf-client");
}

// ---------------------------------------------------------------------------
// Client config – defaults when optional sections are absent
// ---------------------------------------------------------------------------

TEST(ConfigTest, ClientDefaults) {
    const std::string json = R"({})";

    const std::string path = writeTmpJson(json);
    const rdmp::ClientConfig cfg = rdmp::loadClientConfig(path);
    std::remove(path.c_str());

    EXPECT_EQ(cfg.global.synctype, rdmp::SyncType::S3);
    EXPECT_EQ(cfg.multicast.group, "239.1.2.3");
    EXPECT_EQ(cfg.multicast.port,  5000u);
    EXPECT_EQ(cfg.multicast.ttl,   32u);
    EXPECT_EQ(cfg.multicast.iface, "");
    EXPECT_EQ(cfg.s3.endpoint,     "http://localhost:9000");
    EXPECT_EQ(cfg.s3.bucket,       "rdmp-tasks");
    EXPECT_EQ(cfg.local_files.base_path, "/tmp/rdmp-tasks");
    EXPECT_EQ(cfg.timeouts.task_execution_ms,        5000u);
    EXPECT_EQ(cfg.timeouts.s3_poll_interval_ms,      1000u);
    EXPECT_EQ(cfg.timeouts.degradation_threshold_ms, 2000u);
    EXPECT_EQ(cfg.timeouts.watchdog_interval_ms,     2000u);
    EXPECT_EQ(cfg.timeouts.retry_delay_ms,           3000u);
    EXPECT_EQ(cfg.timeouts.multicast_repeat_count,      3u);
    EXPECT_EQ(cfg.timeouts.multicast_repeat_interval_ms, 100u);
    EXPECT_EQ(cfg.node_id, "client1");
}

// ---------------------------------------------------------------------------
// Server config – full valid JSON
// ---------------------------------------------------------------------------

TEST(ConfigTest, ServerFullConfig) {
    const std::string json = R"({
        "global": { "synctype": "local-files" },
        "multicast": { "group": "239.5.5.5", "port": 7000, "ttl": 8, "interface": "" },
        "s3": { "endpoint": "http://s3:9000", "bucket": "b", "access_key": "", "secret_key": "", "region": "us-east-1" },
        "local_files": { "base_path": "/srv/rdmp" },
        "timeouts": { "task_execution_ms": 2000, "s3_poll_interval_ms": 250 },
        "node": { "id": "myserver" }
    })";

    const std::string path = writeTmpJson(json);
    const rdmp::ServerConfig cfg = rdmp::loadServerConfig(path);
    std::remove(path.c_str());

    EXPECT_EQ(cfg.global.synctype, rdmp::SyncType::LocalFiles);
    EXPECT_EQ(cfg.multicast.group, "239.5.5.5");
    EXPECT_EQ(cfg.multicast.port,  7000u);
    EXPECT_EQ(cfg.local_files.base_path, "/srv/rdmp");
    EXPECT_EQ(cfg.timeouts.task_execution_ms,   2000u);
    EXPECT_EQ(cfg.timeouts.s3_poll_interval_ms,  250u);
    // Unset fields use defaults
    EXPECT_EQ(cfg.timeouts.retry_delay_ms, 3000u);
    EXPECT_EQ(cfg.node_id, "myserver");
}

// ---------------------------------------------------------------------------
// Error cases
// ---------------------------------------------------------------------------

TEST(ConfigTest, MissingFile) {
    EXPECT_THROW(rdmp::loadClientConfig("/nonexistent/path/config.json"),
                 std::runtime_error);
}

TEST(ConfigTest, MalformedJson) {
    const std::string path = writeTmpJson("{ this is not json }");
    EXPECT_THROW(rdmp::loadClientConfig(path), std::runtime_error);
    std::remove(path.c_str());
}

TEST(ConfigTest, UnknownSynctypeFallsBackToS3) {
    const std::string json = R"({ "global": { "synctype": "ftp" } })";
    const std::string path = writeTmpJson(json);
    const rdmp::ClientConfig cfg = rdmp::loadClientConfig(path);
    std::remove(path.c_str());
    EXPECT_EQ(cfg.global.synctype, rdmp::SyncType::S3);
}

} // namespace
