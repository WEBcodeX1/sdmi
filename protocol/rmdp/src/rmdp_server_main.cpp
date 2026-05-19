#include "rmdp_server.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------

static rmdp::RMDPServer* g_server = nullptr;

static void signalHandler(int /*sig*/) {
    if (g_server) g_server->stop();
}

// ---------------------------------------------------------------------------
// Default task handler – prints the task and returns "ok".
// Replace or augment this in production deployments.
// ---------------------------------------------------------------------------

static std::string defaultHandler(const std::string& uuid,
                                  const std::string& payload) {
    std::cout << "[RMDP/Server] Executing task " << uuid
              << "  payload=" << payload << "\n";
    return "ok";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: rmdp_server <config-file>\n"
                  << "\n"
                  << "  config-file  Path to the INI-style server configuration file.\n";
        return EXIT_FAILURE;
    }

    const std::string config_path = argv[1];

    try {
        rmdp::RMDPServer server(config_path);
        g_server = &server;

        std::signal(SIGINT,  signalHandler);
        std::signal(SIGTERM, signalHandler);

        server.setTaskHandler(defaultHandler);

        std::cout << "RMDP server started. Press Ctrl-C to stop.\n";
        server.run();
    } catch (const std::exception& ex) {
        std::cerr << "Fatal: " << ex.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
