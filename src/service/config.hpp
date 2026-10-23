// esca-capabilityd settings: /etc/esca/capability.conf, then the command line.

#pragma once

#include <cstdint>
#include <string>

namespace esca::capability {

struct ServiceConfig {
    /// The name peers see; the host name when empty.
    std::string name;
    uint16_t port = 5683;
    std::string service_type = "_esca-cap._udp";
    /// Static facts about this device merged into "device": platform, model...
    std::string device_file = "/etc/esca/capability/device.json";
    /// Where the last published version and the registered sections are
    /// kept across restarts.
    std::string state_dir = "/var/lib/esca/capability";

    unsigned performance_interval_s = 10;
    unsigned sync_interval_s = 30;
    /// A peer that has not answered for this long is removed.
    unsigned expire_after_s = 95;
    /// Local changes within this window go out as one push.
    unsigned push_delay_ms = 500;

    std::string log_level = "INFO";
    bool session_bus = false;

    bool load_file(const std::string &path, std::string &error);
};

} // namespace esca::capability
