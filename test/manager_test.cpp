// The manifest's integrity and compatibility checks, and the device
// manager's decisions: which manifest wins, who is online, who can do what.

#include <thread>

#include "check.hpp"
#include "device_manager.hpp"
#include "device_manifest.hpp"

using namespace esca::capability;
using nlohmann::json;

namespace {

DeviceManifest device(const std::string &id, const std::string &name, uint64_t version = 1)
{
    return DeviceManifest::create(id, json { { "name", name }, { "platform", "test" } }, version);
}

const std::string A = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
const std::string B = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
const Endpoint EB { "192.168.0.100", 5683 };

} // namespace

TEST("FR04 a manifest is standard JSON that round-trips through parse")
{
    DeviceManifest m = device(A, "board");
    m.set_section("ai", json { { "tasks", { "detect" } } });
    const DeviceManifest back = DeviceManifest::parse(m.dump());
    CHECK(back.id() == A);
    CHECK(back.version() == 2);
    CHECK(back.hash() == m.hash());
    CHECK(back.doc()["schema"] == "esca.ai-device-manifest/1.0");
}

TEST("FR09 every change raises the version and changes the hash")
{
    DeviceManifest m = device(A, "board");
    const std::string h1 = m.hash();
    m.set_performance(json { { "cpu", { { "load", 0.5 } } } });
    CHECK(m.version() == 2 && m.hash() != h1);
    CHECK(!m.remove_section("nope"));
    CHECK(m.version() == 2);
}

TEST("integrity: a tampered manifest is refused")
{
    DeviceManifest m = device(A, "board");
    json doc = m.doc();
    doc["device"]["name"] = "evil";
    CHECK_THROWS_AS(DeviceManifest::parse(doc.dump()), ManifestError);
}

TEST("schema: a manifest missing fields or with a bad id is refused")
{
    json doc = device(A, "board").doc();
    doc.erase("network");
    doc["hash"] = DeviceManifest::compute_hash(doc);
    CHECK_THROWS_AS(DeviceManifest::parse(doc.dump()), ManifestError);

    json bad = device("XYZ", "board").doc();
    CHECK_THROWS_AS(DeviceManifest::parse(bad.dump()), ManifestError);
    CHECK_THROWS_AS(DeviceManifest::parse("{"), ManifestError);
}

TEST("QA05 a newer minor version with unknown fields is accepted, another major is not")
{
    json doc = device(A, "board").doc();
    doc["schema"] = "esca.ai-device-manifest/1.7";
    doc["future_field"] = { { "x", 1 } };
    doc["hash"] = DeviceManifest::compute_hash(doc);
    CHECK(DeviceManifest::parse(doc.dump()).doc().contains("future_field"));

    doc["schema"] = "esca.ai-device-manifest/2.0";
    doc["hash"] = DeviceManifest::compute_hash(doc);
    CHECK_THROWS_AS(DeviceManifest::parse(doc.dump()), ManifestError);
}

TEST("FR07 newer manifests replace older ones; a late old copy never rolls back")
{
    DeviceManager dm(device(A, "board"));
    DeviceManifest b = device(B, "server");
    CHECK(dm.accept(b, EB) == RemoteUpdate::Added);
    DeviceManifest b2 = b;
    b2.set_section("ai", json { { "tasks", { "stt" } } });
    CHECK(dm.accept(b2, EB) == RemoteUpdate::Changed);
    CHECK(dm.accept(b, EB) == RemoteUpdate::Stale);
    CHECK(dm.accept(b2, EB) == RemoteUpdate::Unchanged);
    CHECK(dm.manifest(B)->version() == b2.version());
    CHECK(dm.accept(device(A, "me"), EB) == RemoteUpdate::Unchanged); // its own manifest echoed back
}

TEST("FR01 an advertisement asks for a fetch when the device is new or ahead")
{
    DeviceManager dm(device(A, "board"));
    CHECK(dm.advertised(B, "server", EB, 1, ""));
    CHECK(dm.accept(device(B, "server"), EB) == RemoteUpdate::Added);
    const std::string h = dm.manifest(B)->hash().substr(7, 16);
    CHECK(!dm.advertised(B, "server", EB, 1, h));
    CHECK(dm.advertised(B, "server", EB, 2, h));
    CHECK(!dm.advertised(A, "me", EB, 9, "")); // itself
}

TEST("FR08 find answers which devices offer a capability")
{
    DeviceManager dm(device(A, "board"));
    dm.update_local([](DeviceManifest &m) {
        m.set_section("ai", json { { "tasks", { "detect" } } });
        return true;
    });
    DeviceManifest b = device(B, "server");
    b.set_section("ai", json { { "tasks", { "stt", "tts", "anomaly" } }, { "url", "http://x:8099" } });
    dm.accept(b, EB);

    const auto stt = dm.find("/sections/ai/tasks", "stt");
    CHECK(stt.size() == 1 && stt[0].first == B && stt[0].second == "server");
    CHECK(dm.find("/sections/ai/tasks", "detect").size() == 1);
    CHECK(dm.find("/sections/ai", "url").size() == 1); // key of an object
    CHECK(dm.find("/sections/ai/tasks", "fly").empty());
    CHECK(dm.find("not a pointer", "x").empty());
}

TEST("FR02 a device that leaves, or goes silent, is removed")
{
    DeviceManager dm(device(A, "board"));
    dm.accept(device(B, "server"), EB);
    CHECK(dm.devices().size() == 2);
    CHECK(dm.endpoints().size() == 1);
    CHECK(dm.lost(B));
    CHECK(dm.devices().size() == 1);

    dm.accept(device(B, "server"), EB);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    CHECK(dm.expire(std::chrono::seconds(1)) == std::vector<std::string>({ B }));
    CHECK(!dm.manifest(B).has_value());
}

TEST("update_local leaves the version alone when nothing changed")
{
    DeviceManager dm(device(A, "board"));
    const uint64_t v = dm.local().version();
    dm.update_local([](DeviceManifest &) { return false; });
    CHECK(dm.local().version() == v);
}

int main() { return check::run_all(); }
