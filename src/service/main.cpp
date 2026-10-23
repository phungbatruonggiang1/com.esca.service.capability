// esca-capabilityd: tells the network what this device can do, and this
// device what the network can.

#include <csignal>
#include <cstdio>
#include <string>

#include <sdbus-c++/sdbus-c++.h>

#include "capability_manager.hpp"
#include "esca/capability/client.hpp"
#include "logger.hpp"

namespace {

sdbus::IConnection *g_connection = nullptr;

void handle_signal(int)
{
    if (g_connection)
        g_connection->leaveEventLoop();
}

int usage()
{
    std::fprintf(stderr,
                 "usage: esca-capabilityd [options]\n"
                 "  --config FILE       settings (default /etc/esca/capability.conf)\n"
                 "  --name NAME         the name peers see (default: host name)\n"
                 "  --port N            CoAP port (default 5683)\n"
                 "  --state-dir DIR     where the manifest is kept across restarts\n"
                 "  --device-file FILE  static device facts (JSON)\n"
                 "  --log-level LEVEL   DEBUG, INFO, WARN, ERROR\n"
                 "  --session           use the session bus (development)\n");
    return 2;
}

} // namespace

int main(int argc, char **argv)
{
    using namespace esca::capability;

    std::string config_file = "/etc/esca/capability.conf";
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--config")
            config_file = argv[i + 1];

    ServiceConfig config;
    std::string error;
    if (!config.load_file(config_file, error)) {
        std::fprintf(stderr, "esca-capabilityd: %s\n", error.c_str());
        return 1;
    }

    // The command line wins over the file.
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        try {
            if (arg == "--config") next();
            else if (arg == "--name") config.name = next();
            else if (arg == "--port") config.port = static_cast<uint16_t>(std::stoul(next()));
            else if (arg == "--state-dir") config.state_dir = next();
            else if (arg == "--device-file") config.device_file = next();
            else if (arg == "--log-level") config.log_level = next();
            else if (arg == "--session") config.session_bus = true;
            else return usage();
        } catch (const std::exception &) {
            return usage();
        }
    }
    if (!log::set_threshold(config.log_level))
        ESCA_LOG_WARN("unknown log level \"%s\"", config.log_level.c_str());

    try {
        const sdbus::ServiceName name { kBusName };
        auto connection = config.session_bus ? sdbus::createSessionBusConnection(name)
                                             : sdbus::createSystemBusConnection(name);
        g_connection = connection.get();
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        CapabilityManager manager(*connection, config);
        manager.start();
        ESCA_LOG_INFO("ready on %s, CoAP port %u", kBusName, config.port);
        connection->enterEventLoop();
        ESCA_LOG_INFO("shutting down");
        manager.stop();
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "esca-capabilityd: fatal: %s\n", e.what());
        return 1;
    }
}
