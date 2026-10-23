// esca-capabilityd: the AI Capability Provider.
//
// The capability manager: owns this device's manifest and everything
// known about the others, and ties the parts together --
//
//   NetworkMonitor      network section; going offline drops every peer
//   PerformanceMonitor  performance section, when it changes significantly
//   DiscoveryManager    mDNS: find peers, advertise our version
//   ConnectionManager   CoAP: fetch a peer's manifest, push ours on change
//   DeviceManager       which manifest is current, who is online
//
// and puts it on the system bus as vn.sparc.esca.Capability1 for the other
// services: queries (FR08), section registration (FR06), change signals.

#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include <sdbus-c++/sdbus-c++.h>

#include "config.hpp"
#include "connection_manager.hpp"
#include "device_manager.hpp"
#include "discovery_manager.hpp"
#include "network_monitor.hpp"
#include "performance_monitor.hpp"

namespace esca::capability {

class CapabilityManager {
public:
    CapabilityManager(sdbus::IConnection &connection, ServiceConfig config);
    ~CapabilityManager();

    /// Throws std::runtime_error if the CoAP port or netlink cannot be had.
    void start();
    void stop();

private:
    using Clock = std::chrono::steady_clock;

    static DeviceManifest initial_manifest(const ServiceConfig &config);
    void persist(const DeviceManifest &m);

    /// Applies a change to the local manifest and, if it changed, saves it,
    /// re-advertises it and schedules a push. Returns the version after.
    uint64_t commit_local(const std::function<bool(DeviceManifest &)> &change);

    std::string receive(const std::string &text, const Endpoint &from);
    void found(const Advertisement &a);
    void lost(const std::string &id);
    void unreachable(const Endpoint &peer);
    void network_changed(const NetworkState &state);

    void tick_loop();
    void push_now();
    void sync_now();

    void emit_added(const std::string &id, const std::string &name);
    void emit_removed(const std::string &id);
    void emit_changed(const std::string &id, uint64_t version);
    void register_object();

    sdbus::IConnection &connection_;
    ServiceConfig config_;
    DeviceManager devices_;
    PerformanceMonitor performance_;

    std::unique_ptr<sdbus::IObject> object_;
    std::unique_ptr<ConnectionManager> connections_;
    std::unique_ptr<DiscoveryManager> discovery_;
    std::unique_ptr<NetworkMonitor> network_;

    std::mutex mutex_; ///< guards everything below
    std::condition_variable wake_;
    bool running_ = false;
    std::thread ticker_;
    std::optional<Clock::time_point> push_due_;
    /// Every peer discovery reported and has not withdrawn. A peer expired
    /// for silence is fetched again from here, since discovery will not
    /// report it again unless its advertisement changes.
    std::map<std::string, Advertisement> advertised_;
};

} // namespace esca::capability
