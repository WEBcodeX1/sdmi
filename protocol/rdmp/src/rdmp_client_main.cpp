#include "rdmp_client.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <chrono>

// ---------------------------------------------------------------------------
// Signal handling – sets a flag so the event loop exits cleanly on SIGINT /
// SIGTERM.  We store a pointer to the client so the handler can call stop().
// ---------------------------------------------------------------------------

static rdmp::RDMPClient* g_client = nullptr;

static void signalHandler(int /*sig*/) {
    if (g_client) g_client->stop();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: rdmp_client <config-file> [message]\n"
                  << "\n"
                  << "  config-file  Path to the INI-style client configuration file.\n"
                  << "  message      Optional: immediately enqueue a task with this\n"
                  << "               message and run until it has been relayed, then exit.\n"
                  << "               Without this argument the client runs in continuous\n"
                  << "               relay mode (Ctrl-C to stop).\n";
        return EXIT_FAILURE;
    }

    const std::string config_path = argv[1];
    const bool        one_shot    = (argc >= 3);

    try {
        rdmp::RDMPClient client(config_path);
        g_client = &client;

        std::signal(SIGINT,  signalHandler);
        std::signal(SIGTERM, signalHandler);

        if (one_shot) {
            // Enqueue the user-supplied message, flush the burst queue, then exit.
            const std::string payload = argv[2];
            std::string uuid = client.addNewTask(payload);
            if (uuid.empty()) {
                std::cerr << "Failed to enqueue task.\n";
                return EXIT_FAILURE;
            }
            std::cout << "Task UUID: " << uuid << "\n";

            // Drain the burst queue (at least multicast_repeat_count sends).
            for (int i = 0; i < 10; ++i) {
                client.runOnce();
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
        } else {
            // Continuous relay mode
            std::cout << "RDMP client started. Press Ctrl-C to stop.\n";
            client.run();
        }
    } catch (const std::exception& ex) {
        std::cerr << "Fatal: " << ex.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
