// Every device this one knows about, itself included.
//
// The device manager: stores each device's manifest, decides whether a
// manifest that arrives is newer than the one held, keeps track of which
// devices are reachable, and answers "which devices can do X". It does no
// I/O; discovery and the CoAP channel feed it, the D-Bus face reads it.

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "device_manifest.hpp"

namespace esca::capability {

struct Endpoint {
    std::string address;
    uint16_t port = 0;
    bool operator==(const Endpoint &o) const { return address == o.address && port == o.port; }
};

struct DeviceRecord {
    std::string id;
    std::string name;
    bool local = false;
    bool online = false;
    Endpoint endpoint;
    /// Version of the manifest held; 0 while only advertised.
    uint64_t version = 0;
    /// What discovery last said the device is at, to know when to fetch.
    uint64_t advertised_version = 0;
    std::string advertised_hash;
    std::chrono::steady_clock::time_point last_seen;
};

enum class RemoteUpdate { Added, Changed, Unchanged, Stale };
const char *to_string(RemoteUpdate update);

class DeviceManager {
public:
    explicit DeviceManager(DeviceManifest local);

    DeviceManifest local() const;
    std::string local_id() const;

    /// Applies `change` to the local manifest. `change` returns false when
    /// it changed nothing; the version then stays where it is. Returns the
    /// manifest as it is afterwards.
    DeviceManifest update_local(const std::function<bool(DeviceManifest &)> &change);

    /// A peer announced itself. Returns true when its manifest should be
    /// fetched: it is new, or it advertises a version newer than the one held.
    bool advertised(const std::string &id, const std::string &name, const Endpoint &endpoint,
                    uint64_t version, const std::string &hash);

    /// A manifest received from `from`. Newer versions replace older ones; an
    /// older or equal one is ignored, so a late-arriving copy never rolls a
    /// device back (FR07).
    RemoteUpdate accept(const DeviceManifest &manifest, const Endpoint &from);

    /// The peer left the network (FR02). Returns whether it was known.
    bool lost(const std::string &id);
    /// Removes peers not heard from for `silence`; returns their ids.
    std::vector<std::string> expire(std::chrono::seconds silence);
    /// Every remote device, for when this one goes offline itself.
    std::vector<std::string> clear_remote();

    /// "" or the local id for this device.
    std::optional<DeviceManifest> manifest(const std::string &id) const;
    /// This device first, then the others by name.
    std::vector<DeviceRecord> devices() const;
    /// Endpoints of every online peer, for pushing a change.
    std::vector<Endpoint> endpoints() const;
    std::optional<Endpoint> endpoint(const std::string &id) const;

    /// Devices whose manifest has `value` at the JSON pointer `pointer`:
    /// equal to it, an array containing it, or an object with it as a key.
    /// FR08: "which devices offer speech to text" is
    /// find("/sections/ai/tasks", "stt").
    std::vector<std::pair<std::string, std::string>> find(const std::string &pointer,
                                                          const nlohmann::json &value) const;

private:
    struct Entry {
        DeviceRecord record;
        std::optional<DeviceManifest> manifest;
    };

    mutable std::mutex mutex_;
    DeviceManifest local_;
    std::map<std::string, Entry> remote_;
};

} // namespace esca::capability
