// rmdp_python.cpp – Boost.Python bindings for the RMDP protocol library.
//
// Exposed to Python as the 'rmdp' module.  Provides access to:
//   - Configuration structures and loaders
//   - RMDPClient  – send tasks via UDP multicast
//   - RMDPServer  – receive and process tasks
//   - Utility functions (generateUUID, currentTimestamp, …)
//   - JSON helpers (buildTaskJson, parseTask, buildStatusJson, parseTaskStatus)
//
// Example (Python):
//   import rmdp
//   cfg = rmdp.load_client_config("client.json")
//   client = rmdp.RMDPClient("client.json")
//   uuid = client.add_new_task("scale-out node-7")
//   client.run_once()

#include <boost/python.hpp>
#include <boost/python/suite/indexing/vector_indexing_suite.hpp>

#include "rmdp_common.hpp"
#include "rmdp_client.hpp"
#include "rmdp_server.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace py = boost::python;

// ---------------------------------------------------------------------------
// Helper – expose std::vector<std::string> as a Python list-like object.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Thin wrappers where needed
// ---------------------------------------------------------------------------

// Expose the task-handler as a Python callable.
// The server's setTaskHandler receives a std::function; we wrap it so Python
// can pass a callable object (function or lambda).
static void server_set_task_handler(rmdp::RMDPServer& srv,
                                    py::object handler) {
    srv.setTaskHandler(
        [handler](const std::string& uuid,
                  const std::string& payload) -> std::string {
            try {
                py::object result = handler(uuid, payload);
                return py::extract<std::string>(result);
            } catch (const py::error_already_set&) {
                PyErr_Print();
                return "python-handler-exception";
            }
        });
}

// ---------------------------------------------------------------------------
// Module definition
// ---------------------------------------------------------------------------

BOOST_PYTHON_MODULE(rmdp) {
    using namespace boost::python;

    // ------------------------------------------------------------------
    // std::vector<std::string> → Python list
    // ------------------------------------------------------------------
    class_<std::vector<std::string>>("StringList")
        .def(vector_indexing_suite<std::vector<std::string>>());

    // ------------------------------------------------------------------
    // SyncType enum
    // ------------------------------------------------------------------
    enum_<rmdp::SyncType>("SyncType")
        .value("S3",         rmdp::SyncType::S3)
        .value("LocalFiles", rmdp::SyncType::LocalFiles)
        ;

    // ------------------------------------------------------------------
    // TaskStatus enum
    // ------------------------------------------------------------------
    enum_<rmdp::TaskStatus>("TaskStatus")
        .value("UNKNOWN",   rmdp::TaskStatus::UNKNOWN)
        .value("PENDING",   rmdp::TaskStatus::PENDING)
        .value("EXECUTING", rmdp::TaskStatus::EXECUTING)
        .value("COMPLETED", rmdp::TaskStatus::COMPLETED)
        .value("FAILED",    rmdp::TaskStatus::FAILED)
        ;

    // ------------------------------------------------------------------
    // GlobalConfig
    // ------------------------------------------------------------------
    class_<rmdp::GlobalConfig>("GlobalConfig")
        .def_readwrite("synctype", &rmdp::GlobalConfig::synctype)
        ;

    // ------------------------------------------------------------------
    // MulticastConfig
    // ------------------------------------------------------------------
    class_<rmdp::MulticastConfig>("MulticastConfig")
        .def_readwrite("group", &rmdp::MulticastConfig::group)
        .def_readwrite("port",  &rmdp::MulticastConfig::port)
        .def_readwrite("ttl",   &rmdp::MulticastConfig::ttl)
        .def_readwrite("iface", &rmdp::MulticastConfig::iface)
        ;

    // ------------------------------------------------------------------
    // S3Config
    // ------------------------------------------------------------------
    class_<rmdp::S3Config>("S3Config")
        .def_readwrite("endpoint",            &rmdp::S3Config::endpoint)
        .def_readwrite("bucket",              &rmdp::S3Config::bucket)
        .def_readwrite("access_key",          &rmdp::S3Config::access_key)
        .def_readwrite("secret_key",          &rmdp::S3Config::secret_key)
        .def_readwrite("region",              &rmdp::S3Config::region)
        .def_readwrite("max_answer_timeout_ms",
                       &rmdp::S3Config::max_answer_timeout_ms)
        ;

    // ------------------------------------------------------------------
    // LocalFilesConfig
    // ------------------------------------------------------------------
    class_<rmdp::LocalFilesConfig>("LocalFilesConfig")
        .def_readwrite("base_path", &rmdp::LocalFilesConfig::base_path)
        ;

    // ------------------------------------------------------------------
    // TimeoutConfig
    // ------------------------------------------------------------------
    class_<rmdp::TimeoutConfig>("TimeoutConfig")
        .def_readwrite("task_execution_ms",
                       &rmdp::TimeoutConfig::task_execution_ms)
        .def_readwrite("s3_poll_interval_ms",
                       &rmdp::TimeoutConfig::s3_poll_interval_ms)
        .def_readwrite("retry_delay_ms",
                       &rmdp::TimeoutConfig::retry_delay_ms)
        .def_readwrite("multicast_repeat_count",
                       &rmdp::TimeoutConfig::multicast_repeat_count)
        .def_readwrite("multicast_repeat_interval_ms",
                       &rmdp::TimeoutConfig::multicast_repeat_interval_ms)
        ;

    // ------------------------------------------------------------------
    // ClientConfig
    // ------------------------------------------------------------------
    class_<rmdp::ClientConfig>("ClientConfig")
        .def_readwrite("global_cfg",  &rmdp::ClientConfig::global)
        .def_readwrite("multicast",   &rmdp::ClientConfig::multicast)
        .def_readwrite("s3",          &rmdp::ClientConfig::s3)
        .def_readwrite("local_files", &rmdp::ClientConfig::local_files)
        .def_readwrite("timeouts",    &rmdp::ClientConfig::timeouts)
        .def_readwrite("node_id",     &rmdp::ClientConfig::node_id)
        ;

    // ------------------------------------------------------------------
    // ServerConfig
    // ------------------------------------------------------------------
    class_<rmdp::ServerConfig>("ServerConfig")
        .def_readwrite("global_cfg",           &rmdp::ServerConfig::global)
        .def_readwrite("multicast",            &rmdp::ServerConfig::multicast)
        .def_readwrite("s3",                   &rmdp::ServerConfig::s3)
        .def_readwrite("local_files",          &rmdp::ServerConfig::local_files)
        .def_readwrite("timeouts",             &rmdp::ServerConfig::timeouts)
        .def_readwrite("node_id",              &rmdp::ServerConfig::node_id)
        .def_readwrite("bypass_pending_check", &rmdp::ServerConfig::bypass_pending_check)
        ;

    // ------------------------------------------------------------------
    // Task
    // ------------------------------------------------------------------
    class_<rmdp::Task>("Task")
        .def_readwrite("uuid",       &rmdp::Task::uuid)
        .def_readwrite("payload",    &rmdp::Task::payload)
        .def_readwrite("created_by", &rmdp::Task::created_by)
        .def_readwrite("created_at", &rmdp::Task::created_at)
        ;

    // ------------------------------------------------------------------
    // TaskStatusRecord
    // ------------------------------------------------------------------
    class_<rmdp::TaskStatusRecord>("TaskStatusRecord")
        .def_readwrite("uuid",       &rmdp::TaskStatusRecord::uuid)
        .def_readwrite("status",     &rmdp::TaskStatusRecord::status)
        .def_readwrite("server_id",  &rmdp::TaskStatusRecord::server_id)
        .def_readwrite("updated_at", &rmdp::TaskStatusRecord::updated_at)
        .def_readwrite("result",     &rmdp::TaskStatusRecord::result)
        ;

    // ------------------------------------------------------------------
    // Config loaders
    // ------------------------------------------------------------------
    def("load_client_config", &rmdp::loadClientConfig);
    def("load_server_config", &rmdp::loadServerConfig);

    // ------------------------------------------------------------------
    // Utility functions
    // ------------------------------------------------------------------
    def("generate_uuid",       &rmdp::generateUUID);
    def("current_timestamp",   &rmdp::currentTimestamp);
    def("current_time_ms",     &rmdp::currentTimeMs);
    def("task_status_to_string", &rmdp::taskStatusToString);
    def("string_to_task_status", &rmdp::stringToTaskStatus);

    // ------------------------------------------------------------------
    // JSON helpers
    // ------------------------------------------------------------------
    def("build_task_json",      &rmdp::buildTaskJson);
    def("build_status_json",    &rmdp::buildStatusJson);
    def("parse_task",           &rmdp::parseTask);
    def("parse_task_status",    &rmdp::parseTaskStatus);
    def("extract_json_field",   &rmdp::extractJsonField);

    // ------------------------------------------------------------------
    // RMDPClient
    // ------------------------------------------------------------------
    class_<rmdp::RMDPClient, boost::noncopyable>("RMDPClient",
            "RMDP protocol client: sends tasks via UDP multicast.",
            init<std::string>(arg("config_path")))
        .def("add_new_task",    &rmdp::RMDPClient::addNewTask,
             "Store a task and schedule multicast burst. Returns UUID or ''.")
        .def("add_new_message", &rmdp::RMDPClient::addNewMessage,
             "Alias for add_new_task().")
        .def("run_once",        &rmdp::RMDPClient::runOnce,
             "Process one event-loop iteration (non-blocking).")
        .def("run",             &rmdp::RMDPClient::run,
             "Blocking event loop; runs until stop() is called.")
        .def("stop",            &rmdp::RMDPClient::stop,
             "Request a clean exit from run().")
        ;

    // ------------------------------------------------------------------
    // RMDPServer
    // ------------------------------------------------------------------
    class_<rmdp::RMDPServer, boost::noncopyable>("RMDPServer",
            "RMDP protocol server: receives and executes tasks.",
            init<std::string>(arg("config_path")))
        .def("set_task_handler", &server_set_task_handler,
             "Register a Python callable as the task handler.\n"
             "Signature: handler(uuid: str, payload: str) -> str")
        .def("run_once",  &rmdp::RMDPServer::runOnce,
             "Process one event-loop iteration (non-blocking).")
        .def("run",       &rmdp::RMDPServer::run,
             "Blocking event loop; runs until stop() is called.")
        .def("stop",      &rmdp::RMDPServer::stop,
             "Request a clean exit from run().")
        ;
}
