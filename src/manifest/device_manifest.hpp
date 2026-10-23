// AIDeviceManifest: what one device says about itself.
//
// The device metadata document: identity, basic device information, network,
// live performance, and named sections other services register -- sensors,
// AI capability, anything else. Standardised JSON, a unique id
// (FR05), a version that increases with every change (FR09) and a hash over
// the content, so a receiver can tell a manifest arrived whole.
//
//     {
//       "schema": "esca.ai-device-manifest/1.0",
//       "id": "3f1c...",            32 hex, from /etc/machine-id
//       "version": 12,
//       "updated_ms": 1760000000000,
//       "hash": "sha256:...",
//       "device": { "name": "...", "platform": "...", "os": "...", "arch": "..." },
//       "network": { "state": "routable", "addresses": ["..."], "coap_port": 5683 },
//       "performance": { "cpu": {...}, "memory": {...}, "gpu": null, "npu": null },
//       "sections": { "ai": {...}, "sensors": {...} }
//     }

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace esca::capability {

class ManifestError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class DeviceManifest {
public:
    static constexpr const char *kSchemaName = "esca.ai-device-manifest";
    static constexpr int kSchemaMajor = 1;
    static constexpr int kSchemaMinor = 0;

    DeviceManifest() = default;

    /// A new local manifest at `version`, hashed.
    static DeviceManifest create(const std::string &id, const nlohmann::json &device, uint64_t version);

    /// A manifest received from another device: parsed, checked against the
    /// schema, the schema major version and its own hash. Throws
    /// ManifestError saying which check failed. Fields this version does not
    /// know are kept, not rejected, so a newer peer stays readable (QA05).
    static DeviceManifest parse(const std::string &text);

    const nlohmann::json &doc() const { return doc_; }
    std::string id() const;
    std::string name() const;
    uint64_t version() const;
    std::string hash() const;
    uint64_t updated_ms() const;

    // --- local changes: each one bumps the version and rehashes -------------
    void set_section(const std::string &name, const nlohmann::json &value);
    /// False if there was no such section (no change made).
    bool remove_section(const std::string &name);
    void set_network(const nlohmann::json &network);
    void set_performance(const nlohmann::json &performance);
    /// Starting point after a restart: the version continues from the last
    /// one this device published, so peers never see it go backwards.
    void set_version(uint64_t version);

    std::string dump(int indent = -1) const { return doc_.dump(indent); }

    /// sha256 over the canonical serialisation without the "hash" field.
    static std::string compute_hash(const nlohmann::json &doc);

    /// "esca.ai-device-manifest/1.0"
    static std::string schema_id();

private:
    explicit DeviceManifest(nlohmann::json doc) : doc_(std::move(doc)) {}
    void touch();

    nlohmann::json doc_;
};

/// The JSON Schema every manifest is checked against.
const nlohmann::json &manifest_schema();

/// 32 hex characters naming this device: sha256 of /etc/machine-id, so the
/// raw machine id never leaves the board. Empty if it cannot be read.
std::string device_id_from_machine_id(const std::string &path = "/etc/machine-id");

} // namespace esca::capability
