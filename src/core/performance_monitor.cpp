#include "performance_monitor.hpp"

#include <unistd.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

namespace esca::capability {

using nlohmann::json;

namespace {

std::string cpu_model()
{
    std::ifstream in("/proc/cpuinfo");
    std::string line;
    while (std::getline(in, line)) {
        // "model name" on x86, "Model" (the board) on the Raspberry Pi.
        if (line.rfind("model name", 0) == 0 || line.rfind("Model", 0) == 0) {
            const size_t colon = line.find(':');
            if (colon != std::string::npos)
                return line.substr(line.find_first_not_of(" \t", colon + 1));
        }
    }
    return "";
}

json temperature()
{
    std::ifstream in("/sys/class/thermal/thermal_zone0/temp");
    long milli = 0;
    if (!(in >> milli))
        return nullptr;
    return std::round(milli / 100.0) / 10.0;
}

double number(const json &j, const char *a, const char *b)
{
    if (!j.contains(a) || !j[a].is_object() || !j[a].contains(b) || !j[a][b].is_number())
        return NAN;
    return j[a][b].get<double>();
}

} // namespace

json PerformanceMonitor::sample()
{
    json cpu = { { "cores", static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN)) }, { "model", cpu_model() } };

    // Usage since the previous sample, from the aggregate line of /proc/stat.
    std::ifstream stat("/proc/stat");
    std::string label;
    uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    stat >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
    const uint64_t busy = user + nice + system + irq + softirq + steal;
    const uint64_t total = busy + idle + iowait;
    if (last_total_ && total > last_total_)
        cpu["usage_percent"] = std::round(1000.0 * (busy - last_busy_) / (total - last_total_)) / 10.0;
    else
        cpu["usage_percent"] = nullptr;
    last_busy_ = busy;
    last_total_ = total;

    std::ifstream loadavg("/proc/loadavg");
    double load1 = 0;
    loadavg >> load1;
    cpu["load1"] = load1;
    cpu["temperature_c"] = temperature();

    std::ifstream meminfo("/proc/meminfo");
    std::string line;
    uint64_t total_kb = 0, available_kb = 0;
    while (std::getline(meminfo, line)) {
        std::istringstream s(line);
        std::string key;
        uint64_t kb = 0;
        s >> key >> kb;
        if (key == "MemTotal:") total_kb = kb;
        else if (key == "MemAvailable:") available_kb = kb;
    }

    return { { "cpu", cpu },
             { "memory", { { "total_mb", total_kb / 1024 }, { "available_mb", available_kb / 1024 } } },
             { "gpu", nullptr },
             { "npu", nullptr } };
}

bool PerformanceMonitor::significant(const json &published, const json &now)
{
    auto moved = [&](const char *a, const char *b, double by) {
        const double x = number(published, a, b), y = number(now, a, b);
        if (std::isnan(x) != std::isnan(y))
            return true;
        return !std::isnan(x) && std::fabs(x - y) >= by;
    };
    const double total = number(now, "memory", "total_mb");
    return moved("cpu", "usage_percent", 20) || moved("cpu", "temperature_c", 5)
           || moved("memory", "available_mb", std::isnan(total) ? 64 : total / 10)
           || published.value("gpu", json()) != now.value("gpu", json())
           || published.value("npu", json()) != now.value("npu", json());
}

} // namespace esca::capability
