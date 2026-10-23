// The AI Capability Provider from an application: what this device and the
// devices around it can do, and how to add to what this one says.
//
//     esca::capability::CapabilityClient cap;
//     cap.register_info("ai", {{"tasks", {"detect"}}});          // FR06
//     for (auto &[id, name] : cap.find("/sections/ai/tasks", "stt"))  // FR08
//         ... offload speech to text to `id`, at cap.manifest(id)["network"]
//
// Talks to esca-capabilityd on the system bus, vn.sparc.esca.Capability1.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace esca::capability {

constexpr const char *kBusName = "vn.sparc.esca.Capability1";
constexpr const char *kObjectPath = "/vn/sparc/esca/Capability1";
constexpr const char *kInterface = "vn.sparc.esca.Capability1";
/// Errors are kErrorPrefix + InvalidArgument, NotFound or Unavailable.
constexpr const char *kErrorPrefix = "vn.sparc.esca.Capability1.Error.";

struct DeviceInfo {
    std::string id;
    std::string name;
    bool local = false;
    bool online = false;
    std::string address;
    uint16_t port = 0;
    uint64_t version = 0;
};

class CapabilityError : public std::runtime_error {
public:
    CapabilityError(std::string name, const std::string &message)
        : std::runtime_error(message), name_(std::move(name))
    {
    }
    /// The D-Bus error name, e.g. vn.sparc.esca.Capability1.Error.NotFound.
    const std::string &name() const { return name_; }

private:
    std::string name_;
};

class CapabilityClient {
public:
    explicit CapabilityClient(bool session_bus = false);
    ~CapabilityClient();
    CapabilityClient(const CapabilityClient &) = delete;
    CapabilityClient &operator=(const CapabilityClient &) = delete;

    std::string local_id();
    /// The manifest of `id`; this device's for "".
    nlohmann::json manifest(const std::string &id = "");
    /// This device first, then every peer it knows.
    std::vector<DeviceInfo> devices();
    /// (id, name) of the devices whose manifest holds `value` at `pointer`.
    std::vector<std::pair<std::string, std::string>> find(const std::string &pointer, const nlohmann::json &value);

    /// Sets a section of this device's manifest; returns the new version.
    uint64_t register_info(const std::string &section, const nlohmann::json &value);
    uint64_t remove_info(const std::string &section);

    /// {"state", "addresses", "interfaces", "coap_port"}
    nlohmann::json network();
    /// Fetches every peer's manifest again now.
    void sync();

    /// Called on the client's own bus thread.
    void on_device_added(std::function<void(const std::string &id, const std::string &name)> callback);
    void on_device_removed(std::function<void(const std::string &id)> callback);
    void on_manifest_changed(std::function<void(const std::string &id, uint64_t version)> callback);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace esca::capability
