#include "config.hpp"

#include <fstream>

#include "logger.hpp"

namespace esca::capability {

namespace {

std::string trim(const std::string &s)
{
    const size_t a = s.find_first_not_of(" \t\r");
    const size_t b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

} // namespace

bool ServiceConfig::load_file(const std::string &path, std::string &error)
{
    std::ifstream in(path);
    // A missing file means "defaults", which is the normal case.
    if (!in)
        return true;

    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        line = trim(line.substr(0, line.find('#')));
        if (line.empty())
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            error = path + ":" + std::to_string(number) + ": expected key = value";
            return false;
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        try {
            if (key == "name") name = value;
            else if (key == "port") port = static_cast<uint16_t>(std::stoul(value));
            else if (key == "service_type") service_type = value;
            else if (key == "device_file") device_file = value;
            else if (key == "state_dir") state_dir = value;
            else if (key == "performance_interval") performance_interval_s = static_cast<unsigned>(std::stoul(value));
            else if (key == "sync_interval") sync_interval_s = static_cast<unsigned>(std::stoul(value));
            else if (key == "expire_after") expire_after_s = static_cast<unsigned>(std::stoul(value));
            else if (key == "push_delay_ms") push_delay_ms = static_cast<unsigned>(std::stoul(value));
            else if (key == "log_level") log_level = value;
            else ESCA_LOG_WARN("%s:%d: unknown key \"%s\"", path.c_str(), number, key.c_str());
        } catch (const std::exception &) {
            error = path + ":" + std::to_string(number) + ": bad value for " + key;
            return false;
        }
    }
    return true;
}

} // namespace esca::capability
