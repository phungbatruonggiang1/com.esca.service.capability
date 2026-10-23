// CapabilityClient: the provider's methods as C++ calls.

#include "esca/capability/client.hpp"

#include <sdbus-c++/sdbus-c++.h>

namespace esca::capability {

using nlohmann::json;

namespace {

template <typename F>
auto call(F &&f) -> decltype(f())
{
    try {
        return f();
    } catch (const sdbus::Error &e) {
        throw CapabilityError(e.getName(), e.getMessage());
    }
}

} // namespace

struct CapabilityClient::Impl {
    std::unique_ptr<sdbus::IProxy> proxy;
};

CapabilityClient::CapabilityClient(bool session_bus) : impl_(std::make_unique<Impl>())
{
    call([&] {
        auto connection = session_bus ? sdbus::createSessionBusConnection() : sdbus::createSystemBusConnection();
        // The proxy owns the connection and runs its event loop on a thread
        // of its own, which is what delivers the signals.
        impl_->proxy = sdbus::createProxy(std::move(connection), sdbus::ServiceName { kBusName },
                                          sdbus::ObjectPath { kObjectPath });
    });
}

CapabilityClient::~CapabilityClient() = default;

std::string CapabilityClient::local_id()
{
    return call([&] {
        std::string id;
        impl_->proxy->callMethod("GetLocalId").onInterface(kInterface).storeResultsTo(id);
        return id;
    });
}

json CapabilityClient::manifest(const std::string &id)
{
    return call([&] {
        std::string text;
        impl_->proxy->callMethod("GetDeviceManifest").onInterface(kInterface).withArguments(id).storeResultsTo(text);
        return json::parse(text);
    });
}

std::vector<DeviceInfo> CapabilityClient::devices()
{
    return call([&] {
        std::vector<sdbus::Struct<std::string, std::string, bool, bool, std::string, uint16_t, uint64_t>> rows;
        impl_->proxy->callMethod("ListDevices").onInterface(kInterface).storeResultsTo(rows);
        std::vector<DeviceInfo> out;
        for (const auto &r : rows)
            out.push_back({ r.get<0>(), r.get<1>(), r.get<2>(), r.get<3>(), r.get<4>(), r.get<5>(), r.get<6>() });
        return out;
    });
}

std::vector<std::pair<std::string, std::string>> CapabilityClient::find(const std::string &pointer,
                                                                        const json &value)
{
    return call([&] {
        std::vector<sdbus::Struct<std::string, std::string>> rows;
        impl_->proxy->callMethod("FindDevices")
            .onInterface(kInterface)
            .withArguments(pointer, value.dump())
            .storeResultsTo(rows);
        std::vector<std::pair<std::string, std::string>> out;
        for (const auto &r : rows)
            out.emplace_back(r.get<0>(), r.get<1>());
        return out;
    });
}

uint64_t CapabilityClient::register_info(const std::string &section, const json &value)
{
    return call([&] {
        uint64_t version = 0;
        impl_->proxy->callMethod("RegisterManifestInfo")
            .onInterface(kInterface)
            .withArguments(section, value.dump())
            .storeResultsTo(version);
        return version;
    });
}

uint64_t CapabilityClient::remove_info(const std::string &section)
{
    return call([&] {
        uint64_t version = 0;
        impl_->proxy->callMethod("RemoveManifestInfo").onInterface(kInterface).withArguments(section).storeResultsTo(version);
        return version;
    });
}

json CapabilityClient::network()
{
    return call([&] {
        std::string text;
        impl_->proxy->callMethod("GetNetworkInfo").onInterface(kInterface).storeResultsTo(text);
        return json::parse(text);
    });
}

void CapabilityClient::sync()
{
    call([&] { impl_->proxy->callMethod("Sync").onInterface(kInterface); });
}

void CapabilityClient::on_device_added(std::function<void(const std::string &, const std::string &)> callback)
{
    impl_->proxy->uponSignal("DeviceAdded").onInterface(kInterface).call(std::move(callback));
}

void CapabilityClient::on_device_removed(std::function<void(const std::string &)> callback)
{
    impl_->proxy->uponSignal("DeviceRemoved").onInterface(kInterface).call(std::move(callback));
}

void CapabilityClient::on_manifest_changed(std::function<void(const std::string &, uint64_t)> callback)
{
    impl_->proxy->uponSignal("ManifestChanged").onInterface(kInterface).call(std::move(callback));
}

} // namespace esca::capability
