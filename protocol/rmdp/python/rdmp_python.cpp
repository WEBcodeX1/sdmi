// rdmp_python.cpp – Boost.Python bindings for the RDMP protocol library.
//
// Exposed to Python as the 'rdmp' module.  Provides access to:
//   - Configuration structures and loaders
//   - RDMPClient  – send tasks via UDP multicast
//   - RDMPServer  – receive and process tasks
//   - Utility functions (generateUUID, currentTimestamp, …)
//   - JSON helpers (buildTaskJson, parseTask, buildStatusJson, parseTaskStatus)
//
// Example (Python):
//   import rdmp
//   cfg = rdmp.load_client_config("client.json")
//   client = rdmp.RDMPClient("client.json")
//   uuid = client.add_new_task("scale-out node-7")
//   client.run_once()

#include <boost/python.hpp>
#include <boost/python/suite/indexing/vector_indexing_suite.hpp>

#include "rdmp_common.hpp"
#include "rdmp_client.hpp"
#include "rdmp_server.hpp"

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
static void server_set_task_handler(rdmp::RDMPServer& srv,
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

BOOST_PYTHON_MODULE(rdmp) {
    using namespace boost::python;

    // ------------------------------------------------------------------
    // std::vector<std::string> → Python list
    // ------------------------------------------------------------------
    class_<std::vector<std::string>>("StringList")
        .def(vector_indexing_suite<std::vector<std::string>>());

    // ------------------------------------------------------------------
    // SyncType enum
    // ------------------------------------------------------------------
    enum_<rdmp::SyncType>("SyncType")
        .value("S3",         rdmp::SyncType::S3)
        .value("LocalFiles", rdmp::SyncType::LocalFiles)
        ;

    // ------------------------------------------------------------------
    // TaskStatus enum
    // ------------------------------------------------------------------
    enum_<rdmp::TaskStatus>("TaskStatus")
        .value("UNKNOWN",   rdmp::TaskStatus::UNKNOWN)
        .value("PENDING",   rdmp::TaskStatus::PENDING)
        .value("EXECUTING", rdmp::TaskStatus::EXECUTING)
        .value("COMPLETED", rdmp::TaskStatus::COMPLETED)
        .value("FAILED",    rdmp::TaskStatus::FAILED)
        ;

    // ------------------------------------------------------------------
    // GlobalConfig
    // ------------------------------------------------------------------
    class_<rdmp::GlobalConfig>("GlobalConfig")
        .def_readwrite("synctype", &rdmp::GlobalConfig::synctype)
        ;

    // ------------------------------------------------------------------
    // MulticastConfig
    // ------------------------------------------------------------------
    class_<rdmp::MulticastConfig>("MulticastConfig")
        .def_readwrite("group", &rdmp::MulticastConfig::group)
        .def_readwrite("port",  &rdmp::MulticastConfig::port)
        .def_readwrite("ttl",   &rdmp::MulticastConfig::ttl)
        .def_readwrite("iface", &rdmp::MulticastConfig::iface)
        ;

    // ------------------------------------------------------------------
    // S3Config
    // ------------------------------------------------------------------
    class_<rdmp::S3Config>("S3Config")
        .def_readwrite("endpoint",            &rdmp::S3Config::endpoint)
        .def_readwrite("bucket",              &rdmp::S3Config::bucket)
        .def_readwrite("access_key",          &rdmp::S3Config::access_key)
        .def_readwrite("secret_key",          &rdmp::S3Config::secret_key)
        .def_readwrite("region",              &rdmp::S3Config::region)
        .def_readwrite("max_answer_timeout_ms",
                       &rdmp::S3Config::max_answer_timeout_ms)
        ;

    // ------------------------------------------------------------------
    // LocalFilesConfig
    // ------------------------------------------------------------------
    class_<rdmp::LocalFilesConfig>("LocalFilesConfig")
        .def_readwrite("base_path", &rdmp::LocalFilesConfig::base_path)
        ;

    // ------------------------------------------------------------------
    // TimeoutConfig
    // ------------------------------------------------------------------
    class_<rdmp::TimeoutConfig>("TimeoutConfig")
        .def_readwrite("task_execution_ms",
                       &rdmp::TimeoutConfig::task_execution_ms)
        .def_readwrite("s3_poll_interval_ms",
                       &rdmp::TimeoutConfig::s3_poll_interval_ms)
        .def_readwrite("retry_delay_ms",
                       &rdmp::TimeoutConfig::retry_delay_ms)
        .def_readwrite("multicast_repeat_count",
                       &rdmp::TimeoutConfig::multicast_repeat_count)
        .def_readwrite("multicast_repeat_interval_ms",
                       &rdmp::TimeoutConfig::multicast_repeat_interval_ms)
        ;

    // ------------------------------------------------------------------
    // ClientConfig
    // ------------------------------------------------------------------
    class_<rdmp::ClientConfig>("ClientConfig")
        .def_readwrite("global_cfg",  &rdmp::ClientConfig::global)
        .def_readwrite("multicast",   &rdmp::ClientConfig::multicast)
        .def_readwrite("s3",          &rdmp::ClientConfig::s3)
        .def_readwrite("local_files", &rdmp::ClientConfig::local_files)
        .def_readwrite("timeouts",    &rdmp::ClientConfig::timeouts)
        .def_readwrite("node_id",     &rdmp::ClientConfig::node_id)
        ;

    // ------------------------------------------------------------------
    // ServerConfig
    // ------------------------------------------------------------------
    class_<rdmp::ServerConfig>("ServerConfig")
        .def_readwrite("global_cfg",           &rdmp::ServerConfig::global)
        .def_readwrite("multicast",            &rdmp::ServerConfig::multicast)
        .def_readwrite("s3",                   &rdmp::ServerConfig::s3)
        .def_readwrite("local_files",          &rdmp::ServerConfig::local_files)
        .def_readwrite("timeouts",             &rdmp::ServerConfig::timeouts)
        .def_readwrite("node_id",              &rdmp::ServerConfig::node_id)
        .def_readwrite("bypass_pending_check", &rdmp::ServerConfig::bypass_pending_check)
        ;

    // ------------------------------------------------------------------
    // Task
    // ------------------------------------------------------------------
    class_<rdmp::Task>("Task")
        .def_readwrite("uuid",       &rdmp::Task::uuid)
        .def_readwrite("payload",    &rdmp::Task::payload)
        .def_readwrite("created_by", &rdmp::Task::created_by)
        .def_readwrite("created_at", &rdmp::Task::created_at)
        ;

    // ------------------------------------------------------------------
    // TaskStatusRecord
    // ------------------------------------------------------------------
    class_<rdmp::TaskStatusRecord>("TaskStatusRecord")
        .def_readwrite("uuid",       &rdmp::TaskStatusRecord::uuid)
        .def_readwrite("status",     &rdmp::TaskStatusRecord::status)
        .def_readwrite("server_id",  &rdmp::TaskStatusRecord::server_id)
        .def_readwrite("updated_at", &rdmp::TaskStatusRecord::updated_at)
        .def_readwrite("result",     &rdmp::TaskStatusRecord::result)
        ;

    // ------------------------------------------------------------------
    // Config loaders
    // ------------------------------------------------------------------
    def("load_client_config", &rdmp::loadClientConfig);
    def("load_server_config", &rdmp::loadServerConfig);

    // ------------------------------------------------------------------
    // Utility functions
    // ------------------------------------------------------------------
    def("generate_uuid",       &rdmp::generateUUID);
    def("current_timestamp",   &rdmp::currentTimestamp);
    def("current_time_ms",     &rdmp::currentTimeMs);
    def("task_status_to_string", &rdmp::taskStatusToString);
    def("string_to_task_status", &rdmp::stringToTaskStatus);

    // ------------------------------------------------------------------
    // JSON helpers
    // ------------------------------------------------------------------
    def("build_task_json",      &rdmp::buildTaskJson);
    def("build_status_json",    &rdmp::buildStatusJson);
    def("parse_task",           &rdmp::parseTask);
    def("parse_task_status",    &rdmp::parseTaskStatus);
    def("extract_json_field",   &rdmp::extractJsonField);

    // ------------------------------------------------------------------
    // RDMPClient
    // ------------------------------------------------------------------
    class_<rdmp::RDMPClient, boost::noncopyable>("RDMPClient",
            "RDMP protocol client: sends tasks via UDP multicast.",
            init<std::string>(arg("config_path")))
        .def("add_new_task",    &rdmp::RDMPClient::addNewTask,
             "Store a task and schedule multicast burst. Returns UUID or ''.")
        .def("add_new_message", &rdmp::RDMPClient::addNewMessage,
             "Alias for add_new_task().")
        .def("run_once",        &rdmp::RDMPClient::runOnce,
             "Process one event-loop iteration (non-blocking).")
        .def("run",             &rdmp::RDMPClient::run,
             "Blocking event loop; runs until stop() is called.")
        .def("stop",            &rdmp::RDMPClient::stop,
             "Request a clean exit from run().")
        ;

    // ------------------------------------------------------------------
    // RDMPServer
    // ------------------------------------------------------------------
    class_<rdmp::RDMPServer, boost::noncopyable>("RDMPServer",
            "RDMP protocol server: receives and executes tasks.",
            init<std::string>(arg("config_path")))
        .def("set_task_handler", &server_set_task_handler,
             "Register a Python callable as the task handler.\n"
             "Signature: handler(uuid: str, payload: str) -> str")
        .def("run_once",  &rdmp::RDMPServer::runOnce,
             "Process one event-loop iteration (non-blocking).")
        .def("run",       &rdmp::RDMPServer::run,
             "Blocking event loop; runs until stop() is called.")
        .def("stop",      &rdmp::RDMPServer::stop,
             "Request a clean exit from run().")
        ;
}
