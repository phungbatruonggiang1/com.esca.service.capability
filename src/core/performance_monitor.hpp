// What this device can give right now: CPU load and temperature, memory.
//
// Live performance is kept in the manifest so a peer deciding where to
// offload sees it. Every sample would change the version, and every version
// is pushed to every peer, so only a significant change is published.

#pragma once

#include <cstdint>

#include <nlohmann/json.hpp>

namespace esca::capability {

class PerformanceMonitor {
public:
    /// {"cpu": {"cores", "model", "usage_percent", "load1", "temperature_c"},
    ///  "memory": {"total_mb", "available_mb"}, "gpu": null, "npu": null}
    nlohmann::json sample();

    /// Worth publishing: CPU use moved 20 points, available memory 10% of
    /// the total, temperature 5 degrees, or a field appeared or went away.
    static bool significant(const nlohmann::json &published, const nlohmann::json &now);

private:
    uint64_t last_busy_ = 0;
    uint64_t last_total_ = 0;
};

} // namespace esca::capability
