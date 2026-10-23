// Finding the other devices on the network, and being found.
//
// Discovery, over Avahi: this device is published as an
// mDNS service of type _esca-cap._udp on its CoAP port, with a TXT record
// carrying its id and the version and hash of its manifest; other devices
// are found by browsing the same type. When the manifest changes the TXT
// record is updated, so a peer that only listens still learns of it.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "device_manager.hpp"

namespace esca::capability {

struct Advertisement {
    std::string id;
    std::string name;
    Endpoint endpoint;
    uint64_t version = 0;
    std::string hash; ///< the first 16 hex digits
};

class DiscoveryManager {
public:
    /// Both run on Avahi's thread.
    using Found = std::function<void(const Advertisement &)>;
    using Lost = std::function<void(const std::string &id)>;

    DiscoveryManager(std::string service_type, uint16_t port, Found found, Lost lost);
    ~DiscoveryManager();
    DiscoveryManager(const DiscoveryManager &) = delete;
    DiscoveryManager &operator=(const DiscoveryManager &) = delete;

    /// Connects to avahi-daemon, publishes, starts browsing. If the daemon
    /// is not there yet, keeps waiting for it rather than failing.
    bool start(const std::string &name, const std::string &id, uint64_t version, const std::string &hash,
               std::string &error);
    void stop();

    /// New manifest version: updates the published TXT record.
    void update(uint64_t version, const std::string &hash);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace esca::capability
