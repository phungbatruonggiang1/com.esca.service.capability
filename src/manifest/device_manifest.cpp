#include "device_manifest.hpp"

#include <openssl/evp.h>

#include <chrono>
#include <cstdio>
#include <fstream>

#include <nlohmann/json-schema.hpp>

namespace esca::capability {

using nlohmann::json;

namespace {

uint64_t now_ms()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

std::string sha256_hex(const std::string &data)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    EVP_Digest(data.data(), data.size(), digest, &length, EVP_sha256(), nullptr);
    std::string hex;
    char byte[3];
    for (unsigned int i = 0; i < length; ++i) {
        std::snprintf(byte, sizeof(byte), "%02x", digest[i]);
        hex += byte;
    }
    return hex;
}

const char *const kSchemaText = R"({
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "ESCA AI device manifest",
  "type": "object",
  "required": ["schema", "id", "version", "updated_ms", "hash", "device", "network", "sections"],
  "properties": {
    "schema": { "type": "string", "pattern": "^esca\\.ai-device-manifest/[0-9]+\\.[0-9]+$" },
    "id": { "type": "string", "pattern": "^[0-9a-f]{16,64}$" },
    "version": { "type": "integer", "minimum": 1 },
    "updated_ms": { "type": "integer", "minimum": 0 },
    "hash": { "type": "string", "pattern": "^sha256:[0-9a-f]{64}$" },
    "device": {
      "type": "object",
      "required": ["name"],
      "properties": {
        "name": { "type": "string", "minLength": 1 },
        "platform": { "type": "string" },
        "os": { "type": "string" },
        "arch": { "type": "string" }
      }
    },
    "network": {
      "type": "object",
      "properties": {
        "state": { "type": "string" },
        "addresses": { "type": "array", "items": { "type": "string" } },
        "coap_port": { "type": "integer", "minimum": 1, "maximum": 65535 }
      }
    },
    "performance": { "type": "object" },
    "sections": { "type": "object" }
  }
})";

} // namespace

const json &manifest_schema()
{
    static const json schema = json::parse(kSchemaText);
    return schema;
}

std::string DeviceManifest::schema_id()
{
    return std::string(kSchemaName) + "/" + std::to_string(kSchemaMajor) + "." + std::to_string(kSchemaMinor);
}

std::string DeviceManifest::compute_hash(const json &doc)
{
    json copy = doc;
    copy.erase("hash");
    // nlohmann::json keeps object keys sorted, so dump() is canonical: two
    // devices hashing the same content get the same digest.
    return "sha256:" + sha256_hex(copy.dump());
}

DeviceManifest DeviceManifest::create(const std::string &id, const json &device, uint64_t version)
{
    json doc = {
        { "schema", schema_id() },
        { "id", id },
        { "version", version < 1 ? 1 : version },
        { "updated_ms", now_ms() },
        { "device", device },
        { "network", json::object() },
        { "performance", json::object() },
        { "sections", json::object() },
    };
    doc["hash"] = compute_hash(doc);
    return DeviceManifest(std::move(doc));
}

DeviceManifest DeviceManifest::parse(const std::string &text)
{
    json doc;
    try {
        doc = json::parse(text);
    } catch (const json::parse_error &e) {
        throw ManifestError(std::string("not JSON: ") + e.what());
    }

    // The schema's major version first: a different major may mean anything,
    // and the validator would only report a confusing field-level error.
    if (doc.is_object() && doc.contains("schema") && doc["schema"].is_string()) {
        const std::string schema = doc["schema"].get<std::string>();
        const std::string prefix = std::string(kSchemaName) + "/";
        if (schema.rfind(prefix, 0) != 0)
            throw ManifestError("not a device manifest: schema \"" + schema + "\"");
        const int major = std::atoi(schema.c_str() + prefix.size());
        if (major != kSchemaMajor)
            throw ManifestError("manifest schema major version " + std::to_string(major)
                                + " is not supported (this device speaks " + schema_id() + ")");
    }

    try {
        nlohmann::json_schema::json_validator validator;
        validator.set_root_schema(manifest_schema());
        validator.validate(doc);
    } catch (const std::exception &e) {
        throw ManifestError(std::string("does not match the manifest schema: ") + e.what());
    }

    const std::string claimed = doc["hash"].get<std::string>();
    const std::string actual = compute_hash(doc);
    if (claimed != actual)
        throw ManifestError("hash mismatch: the manifest says " + claimed.substr(0, 19)
                            + "..., its content hashes to " + actual.substr(0, 19) + "...");
    return DeviceManifest(std::move(doc));
}

std::string DeviceManifest::id() const { return doc_.value("id", ""); }

std::string DeviceManifest::name() const
{
    if (doc_.contains("device") && doc_["device"].is_object())
        return doc_["device"].value("name", "");
    return "";
}

uint64_t DeviceManifest::version() const { return doc_.value("version", uint64_t { 0 }); }

std::string DeviceManifest::hash() const { return doc_.value("hash", ""); }

uint64_t DeviceManifest::updated_ms() const { return doc_.value("updated_ms", uint64_t { 0 }); }

void DeviceManifest::touch()
{
    doc_["version"] = version() + 1;
    doc_["updated_ms"] = now_ms();
    doc_["hash"] = compute_hash(doc_);
}

void DeviceManifest::set_section(const std::string &name, const json &value)
{
    doc_["sections"][name] = value;
    touch();
}

bool DeviceManifest::remove_section(const std::string &name)
{
    if (!doc_["sections"].contains(name))
        return false;
    doc_["sections"].erase(name);
    touch();
    return true;
}

void DeviceManifest::set_network(const json &network)
{
    doc_["network"] = network;
    touch();
}

void DeviceManifest::set_performance(const json &performance)
{
    doc_["performance"] = performance;
    touch();
}

void DeviceManifest::set_version(uint64_t version)
{
    doc_["version"] = version < 1 ? 1 : version;
    doc_["hash"] = compute_hash(doc_);
}

std::string device_id_from_machine_id(const std::string &path)
{
    std::ifstream in(path);
    std::string machine_id;
    if (!(in >> machine_id) || machine_id.empty())
        return "";
    // Salted with the purpose, so the id cannot be matched against the
    // machine id itself if it leaks in another context.
    return sha256_hex("esca-capability:" + machine_id).substr(0, 32);
}

} // namespace esca::capability
