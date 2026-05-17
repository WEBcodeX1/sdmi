// test_config.cpp – Unit tests for JSON configuration parsing
//
// Tests cover:
//  - Valid client and server config files
//  - Default values when optional fields are absent
//  - The global.synctype field ("s3", "local-files", "multicast-reply")
//  - multicast_reply section parsing
//  - Error handling for missing / malformed files

#define BOOST_TEST_MODULE RdmpTests
#include <boost/test/unit_test.hpp>

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
    close(fd);
    std::ofstream f(path);
    f << content;
    return std::string(path);
}

} // namespace

BOOST_AUTO_TEST_SUITE(ConfigTest)

// ---------------------------------------------------------------------------
// Client config – full valid JSON
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(ClientFullConfig) {
    const std::string json = R"({
        "global": { "synctype": "s3" },
        "multicast": {
            "group": "239.9.9.9",
            "port": 6000,
            "ttl": 16,
            "interface": "eth1"
        },
        "multicast_reply": {
            "group": "239.9.9.10",
            "port": 6001,
            "ttl": 8,
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

    BOOST_CHECK_EQUAL(cfg.global.synctype, rdmp::SyncType::S3);
    BOOST_CHECK_EQUAL(cfg.multicast.group, "239.9.9.9");
    BOOST_CHECK_EQUAL(cfg.multicast.port,  6000);
    BOOST_CHECK_EQUAL(cfg.multicast.ttl,   16);
    BOOST_CHECK_EQUAL(cfg.multicast.iface, "eth1");
    BOOST_CHECK_EQUAL(cfg.multicast_reply.group, "239.9.9.10");
    BOOST_CHECK_EQUAL(cfg.multicast_reply.port,  6001);
    BOOST_CHECK_EQUAL(cfg.multicast_reply.ttl,   8);
    BOOST_CHECK_EQUAL(cfg.multicast_reply.iface, "eth1");
    BOOST_CHECK_EQUAL(cfg.s3.endpoint,     "http://mys3:9000");
    BOOST_CHECK_EQUAL(cfg.s3.bucket,       "my-bucket");
    BOOST_CHECK_EQUAL(cfg.s3.access_key,   "AKID");
    BOOST_CHECK_EQUAL(cfg.s3.secret_key,   "SECRET");
    BOOST_CHECK_EQUAL(cfg.s3.region,       "eu-west-1");
    BOOST_CHECK_EQUAL(cfg.local_files.base_path, "/var/rdmp");
    BOOST_CHECK_EQUAL(cfg.timeouts.task_execution_ms,        1000u);
    BOOST_CHECK_EQUAL(cfg.timeouts.s3_poll_interval_ms,       500u);
    BOOST_CHECK_EQUAL(cfg.timeouts.degradation_threshold_ms,  800u);
    BOOST_CHECK_EQUAL(cfg.timeouts.watchdog_interval_ms,     1200u);
    BOOST_CHECK_EQUAL(cfg.timeouts.retry_delay_ms,           2000u);
    BOOST_CHECK_EQUAL(cfg.timeouts.multicast_repeat_count,      5u);
    BOOST_CHECK_EQUAL(cfg.timeouts.multicast_repeat_interval_ms, 200u);
    BOOST_CHECK_EQUAL(cfg.node_id, "myclient");
}

// ---------------------------------------------------------------------------
// Client config – local-files synctype
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(ClientLocalFilesSynctype) {
    const std::string json = R"({
        "global": { "synctype": "local-files" },
        "local_files": { "base_path": "/tmp/rdmp-test" },
        "node": { "id": "lf-client" }
    })";

    const std::string path = writeTmpJson(json);
    const rdmp::ClientConfig cfg = rdmp::loadClientConfig(path);
    std::remove(path.c_str());

    BOOST_CHECK_EQUAL(cfg.global.synctype, rdmp::SyncType::LocalFiles);
    BOOST_CHECK_EQUAL(cfg.local_files.base_path, "/tmp/rdmp-test");
    BOOST_CHECK_EQUAL(cfg.node_id, "lf-client");
}

// ---------------------------------------------------------------------------
// Client config – multicast-reply synctype
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(ClientMulticastReplySynctype) {
    const std::string json = R"({
        "global": { "synctype": "multicast-reply" },
        "multicast_reply": {
            "group": "239.2.3.4",
            "port": 5002,
            "ttl": 16,
            "interface": ""
        },
        "node": { "id": "mr-client" }
    })";

    const std::string path = writeTmpJson(json);
    const rdmp::ClientConfig cfg = rdmp::loadClientConfig(path);
    std::remove(path.c_str());

    BOOST_CHECK_EQUAL(cfg.global.synctype, rdmp::SyncType::MulticastReply);
    BOOST_CHECK_EQUAL(cfg.multicast_reply.group, "239.2.3.4");
    BOOST_CHECK_EQUAL(cfg.multicast_reply.port,  5002u);
    BOOST_CHECK_EQUAL(cfg.multicast_reply.ttl,   16u);
    BOOST_CHECK_EQUAL(cfg.node_id, "mr-client");
}

// ---------------------------------------------------------------------------
// Client config – defaults when optional sections are absent
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(ClientDefaults) {
    const std::string json = R"({})";

    const std::string path = writeTmpJson(json);
    const rdmp::ClientConfig cfg = rdmp::loadClientConfig(path);
    std::remove(path.c_str());

    BOOST_CHECK_EQUAL(cfg.global.synctype, rdmp::SyncType::S3);
    BOOST_CHECK_EQUAL(cfg.multicast.group, "239.1.2.3");
    BOOST_CHECK_EQUAL(cfg.multicast.port,  5000u);
    BOOST_CHECK_EQUAL(cfg.multicast.ttl,   32u);
    BOOST_CHECK_EQUAL(cfg.multicast.iface, "");
    BOOST_CHECK_EQUAL(cfg.multicast_reply.group, "239.1.2.4");
    BOOST_CHECK_EQUAL(cfg.multicast_reply.port,  5001u);
    BOOST_CHECK_EQUAL(cfg.multicast_reply.ttl,   32u);
    BOOST_CHECK_EQUAL(cfg.multicast_reply.iface, "");
    BOOST_CHECK_EQUAL(cfg.s3.endpoint,     "http://localhost:9000");
    BOOST_CHECK_EQUAL(cfg.s3.bucket,       "rdmp-tasks");
    BOOST_CHECK_EQUAL(cfg.local_files.base_path, "/tmp/rdmp-tasks");
    BOOST_CHECK_EQUAL(cfg.timeouts.task_execution_ms,        5000u);
    BOOST_CHECK_EQUAL(cfg.timeouts.s3_poll_interval_ms,      1000u);
    BOOST_CHECK_EQUAL(cfg.timeouts.degradation_threshold_ms, 2000u);
    BOOST_CHECK_EQUAL(cfg.timeouts.watchdog_interval_ms,     2000u);
    BOOST_CHECK_EQUAL(cfg.timeouts.retry_delay_ms,           3000u);
    BOOST_CHECK_EQUAL(cfg.timeouts.multicast_repeat_count,      3u);
    BOOST_CHECK_EQUAL(cfg.timeouts.multicast_repeat_interval_ms, 100u);
    BOOST_CHECK_EQUAL(cfg.node_id, "client1");
}

// ---------------------------------------------------------------------------
// Server config – full valid JSON
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(ServerFullConfig) {
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

    BOOST_CHECK_EQUAL(cfg.global.synctype, rdmp::SyncType::LocalFiles);
    BOOST_CHECK_EQUAL(cfg.multicast.group, "239.5.5.5");
    BOOST_CHECK_EQUAL(cfg.multicast.port,  7000u);
    BOOST_CHECK_EQUAL(cfg.local_files.base_path, "/srv/rdmp");
    BOOST_CHECK_EQUAL(cfg.timeouts.task_execution_ms,   2000u);
    BOOST_CHECK_EQUAL(cfg.timeouts.s3_poll_interval_ms,  250u);
    // Unset fields use defaults
    BOOST_CHECK_EQUAL(cfg.timeouts.retry_delay_ms, 3000u);
    BOOST_CHECK_EQUAL(cfg.node_id, "myserver");
}

// ---------------------------------------------------------------------------
// Error cases
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(MissingFile) {
    BOOST_CHECK_THROW(rdmp::loadClientConfig("/nonexistent/path/config.json"),
                      std::runtime_error);
}

BOOST_AUTO_TEST_CASE(MalformedJson) {
    const std::string path = writeTmpJson("{ this is not json }");
    BOOST_CHECK_THROW(rdmp::loadClientConfig(path), std::runtime_error);
    std::remove(path.c_str());
}

BOOST_AUTO_TEST_CASE(UnknownSynctypeFallsBackToS3) {
    const std::string json = R"({ "global": { "synctype": "ftp" } })";
    const std::string path = writeTmpJson(json);
    const rdmp::ClientConfig cfg = rdmp::loadClientConfig(path);
    std::remove(path.c_str());
    BOOST_CHECK_EQUAL(cfg.global.synctype, rdmp::SyncType::S3);
}

BOOST_AUTO_TEST_SUITE_END()

