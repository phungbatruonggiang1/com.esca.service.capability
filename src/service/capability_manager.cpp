#include "capability_manager.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>

#include "esca/capability/client.hpp"
#include "logger.hpp"

namespace esca::capability {

using nlohmann::json;

namespace {

sdbus::Error error(const char *name, const std::string &message)
{
    return sdbus::Error(sdbus::Error::Name { std::string(kErrorPrefix) + name }, message);
}

std::string os_name()
{
    std::ifstream in("/etc/os-release");
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            std::string v = line.substr(12);
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
                v = v.substr(1, v.size() - 2);
            return v;
        }
    return "Linux";
}

std::string host_name()
{
    char name[256] = {};
    gethostname(name, sizeof(name) - 1);
    return name;
}

json read_json(const std::string &path)
{
    std::ifstream in(path);
    if (!in)
        return nullptr;
    try {
        return json::parse(in);
    } catch (const json::exception &e) {
        ESCA_LOG_WARN("%s: %s", path.c_str(), e.what());
        return nullptr;
    }
}

bool valid_section(const std::string &name)
{
    static const std::regex pattern("^[a-z][a-z0-9_-]{0,31}$");
    return std::regex_match(name, pattern);
}

using DeviceRow = sdbus::Struct<std::string, std::string, bool, bool, std::string, uint16_t, uint64_t>;
using MatchRow = sdbus::Struct<std::string, std::string>;

} // namespace

DeviceManifest CapabilityManager::initial_manifest(const ServiceConfig &config)
{
    const std::string id = device_id_from_machine_id();
    if (id.empty())
        throw std::runtime_error("cannot read /etc/machine-id");

    utsname u {};
    uname(&u);
    json device = { { "name", config.name.empty() ? host_name() : config.name },
                    { "platform", "linux" },
                    { "os", os_name() },
                    { "arch", u.machine } };
    // The image says what the board is: model, camera, anything static.
    const json extra = read_json(config.device_file);
    if (extra.is_object())
        for (const auto &[k, v] : extra.items())
            if (k != "name" || config.name.empty())
                device[k] = v;

    DeviceManifest m = DeviceManifest::create(id, device, 1);
    // Continue from the version last published, so peers holding it take
    // what comes next as newer; restore the sections services registered,
    // so they are there before those services come back to say so again.
    const json saved = read_json(config.state_dir + "/manifest.json");
    if (saved.is_object() && saved.value("id", "") == id) {
        m.set_version(saved.value("version", uint64_t { 1 }));
        if (saved.contains("sections") && saved["sections"].is_object())
            for (const auto &[k, v] : saved["sections"].items())
                m.set_section(k, v);
        // Something changed at least by restarting: the version must move.
        m.set_version(m.version() + 1);
    }
    return m;
}

CapabilityManager::CapabilityManager(sdbus::IConnection &connection, ServiceConfig config)
    : connection_(connection), config_(std::move(config)), devices_(initial_manifest(config_))
{
}

CapabilityManager::~CapabilityManager() { stop(); }

void CapabilityManager::persist(const DeviceManifest &m)
{
    std::error_code ec;
    std::filesystem::create_directories(config_.state_dir, ec);
    const std::string path = config_.state_dir + "/manifest.json";
    const std::string temp = path + ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        out << m.dump(2) << "\n";
        if (!out) {
            ESCA_LOG_WARN("cannot write %s", temp.c_str());
            return;
        }
    }
    std::filesystem::rename(temp, path, ec);
}

uint64_t CapabilityManager::commit_local(const std::function<bool(DeviceManifest &)> &change)
{
    const uint64_t before = devices_.local().version();
    const DeviceManifest m = devices_.update_local(change);
    if (m.version() == before)
        return before;
    persist(m);
    if (discovery_)
        discovery_->update(m.version(), m.hash());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!push_due_)
            push_due_ = Clock::now() + std::chrono::milliseconds(config_.push_delay_ms);
    }
    wake_.notify_all();
    emit_changed(m.id(), m.version());
    return m.version();
}

// --- what arrives from the network -------------------------------------------

std::string CapabilityManager::receive(const std::string &text, const Endpoint &from)
{
    DeviceManifest m;
    try {
        m = DeviceManifest::parse(text);
    } catch (const ManifestError &e) {
        ESCA_LOG_WARN("manifest from %s refused: %s", from.address.c_str(), e.what());
        return e.what();
    }
    Endpoint at = from;
    if (at.port == 0) {
        // Pushed to us: the source port is the sender's client port; where
        // it serves is in its manifest.
        const json &network = m.doc()["network"];
        at.port = network.is_object() ? network.value("coap_port", config_.port) : config_.port;
    }
    switch (devices_.accept(m, at)) {
    case RemoteUpdate::Added:
        ESCA_LOG_INFO("device %s \"%s\" at %s:%u, version %llu", m.id().c_str(), m.name().c_str(),
                      at.address.c_str(), at.port, static_cast<unsigned long long>(m.version()));
        emit_added(m.id(), m.name());
        emit_changed(m.id(), m.version());
        break;
    case RemoteUpdate::Changed:
        ESCA_LOG_INFO("device %s \"%s\" now at version %llu", m.id().c_str(), m.name().c_str(),
                      static_cast<unsigned long long>(m.version()));
        emit_changed(m.id(), m.version());
        break;
    case RemoteUpdate::Stale:
        ESCA_LOG_DEBUG("ignored an older manifest of %s", m.id().c_str());
        break;
    case RemoteUpdate::Unchanged:
        break;
    }
    return "";
}

void CapabilityManager::found(const Advertisement &a)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        advertised_[a.id] = a;
    }
    ESCA_LOG_DEBUG("advertised: %s \"%s\" at %s:%u v%llu", a.id.c_str(), a.name.c_str(),
                   a.endpoint.address.c_str(), a.endpoint.port, static_cast<unsigned long long>(a.version));
    if (devices_.advertised(a.id, a.name, a.endpoint, a.version, a.hash))
        connections_->fetch(a.endpoint);
}

void CapabilityManager::lost(const std::string &id)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        advertised_.erase(id);
    }
    if (devices_.lost(id)) {
        ESCA_LOG_INFO("device %s left", id.c_str());
        emit_removed(id);
    }
}

void CapabilityManager::unreachable(const Endpoint &peer)
{
    // One lost exchange is not a departure: the device stays until it has
    // been silent for expire_after, or discovery says it went away.
    ESCA_LOG_DEBUG("no answer from %s:%u", peer.address.c_str(), peer.port);
}

void CapabilityManager::network_changed(const NetworkState &state)
{
    json network = { { "state", state.online ? "online" : "offline" },
                     { "addresses", state.addresses },
                     { "interfaces", state.interfaces },
                     { "coap_port", config_.port } };
    commit_local([&](DeviceManifest &m) {
        if (m.doc()["network"] == network)
            return false;
        m.set_network(network);
        return true;
    });
    if (!state.online)
        for (const std::string &id : devices_.clear_remote())
            emit_removed(id);
}

// --- the periodic work -----------------------------------------------------

void CapabilityManager::push_now()
{
    const std::string text = devices_.local().dump();
    for (const Endpoint &e : devices_.endpoints())
        connections_->push(e, text);
}

void CapabilityManager::sync_now()
{
    for (const Endpoint &e : devices_.endpoints())
        connections_->fetch(e);
    // Peers still advertised but no longer held: expired while unreachable.
    std::vector<Advertisement> missing;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &[id, a] : advertised_)
            if (!devices_.endpoint(id))
                missing.push_back(a);
    }
    for (const Advertisement &a : missing)
        if (devices_.advertised(a.id, a.name, a.endpoint, a.version, a.hash))
            connections_->fetch(a.endpoint);
}

void CapabilityManager::tick_loop()
{
    auto next_performance = Clock::now();
    auto next_sync = Clock::now() + std::chrono::seconds(config_.sync_interval_s);
    std::unique_lock<std::mutex> lock(mutex_);
    while (running_) {
        wake_.wait_for(lock, std::chrono::milliseconds(250));
        if (!running_)
            break;
        const auto now = Clock::now();
        const bool push = push_due_ && now >= *push_due_;
        if (push)
            push_due_.reset();
        lock.unlock();

        if (push)
            push_now();
        if (now >= next_performance) {
            next_performance = now + std::chrono::seconds(config_.performance_interval_s);
            const json sample = performance_.sample();
            commit_local([&](DeviceManifest &m) {
                if (!PerformanceMonitor::significant(m.doc()["performance"], sample))
                    return false;
                m.set_performance(sample);
                return true;
            });
        }
        if (now >= next_sync) {
            next_sync = now + std::chrono::seconds(config_.sync_interval_s);
            for (const std::string &id : devices_.expire(std::chrono::seconds(config_.expire_after_s))) {
                ESCA_LOG_INFO("device %s silent for %us, removed", id.c_str(), config_.expire_after_s);
                emit_removed(id);
            }
            sync_now();
        }
        lock.lock();
    }
}

// --- the bus ---------------------------------------------------------------

void CapabilityManager::emit_added(const std::string &id, const std::string &name)
{
    object_->emitSignal("DeviceAdded").onInterface(kInterface).withArguments(id, name);
}

void CapabilityManager::emit_removed(const std::string &id)
{
    object_->emitSignal("DeviceRemoved").onInterface(kInterface).withArguments(id);
}

void CapabilityManager::emit_changed(const std::string &id, uint64_t version)
{
    if (object_)
        object_->emitSignal("ManifestChanged").onInterface(kInterface).withArguments(id, version);
}

void CapabilityManager::register_object()
{
    object_ = sdbus::createObject(connection_, sdbus::ObjectPath { kObjectPath });
    object_->addVTable(
        sdbus::registerMethod("GetLocalId").implementedAs([this] { return devices_.local_id(); }),

        sdbus::registerMethod("GetDeviceManifest").implementedAs([this](const std::string &id) {
            const auto m = devices_.manifest(id);
            if (!m)
                throw error("NotFound", "no manifest for device \"" + id + "\"");
            return m->dump();
        }),

        sdbus::registerMethod("ListDevices").implementedAs([this] {
            std::vector<DeviceRow> rows;
            for (const DeviceRecord &d : devices_.devices())
                rows.emplace_back(d.id, d.name, d.local, d.online, d.endpoint.address, d.endpoint.port,
                                  d.version);
            return rows;
        }),

        sdbus::registerMethod("FindDevices").implementedAs([this](const std::string &pointer, const std::string &value) {
            if (pointer.empty() || pointer[0] != '/')
                throw error("InvalidArgument", "a JSON pointer starts with '/'");
            // JSON if it parses, otherwise the plain string: "stt" and
            // "\"stt\"" both work.
            json v = json::parse(value, nullptr, false);
            if (v.is_discarded())
                v = value;
            std::vector<MatchRow> rows;
            for (const auto &[id, name] : devices_.find(pointer, v))
                rows.emplace_back(id, name);
            return rows;
        }),

        sdbus::registerMethod("RegisterManifestInfo")
            .implementedAs([this](const std::string &section, const std::string &value) {
                if (!valid_section(section))
                    throw error("InvalidArgument", "section names are [a-z][a-z0-9_-]*, up to 32 characters");
                json v = json::parse(value, nullptr, false);
                if (v.is_discarded())
                    throw error("InvalidArgument", "the value is not JSON");
                const std::string sender = object_->getCurrentlyProcessedMessage().getSender();
                const uint64_t version = commit_local([&](DeviceManifest &m) {
                    const json &sections = m.doc()["sections"];
                    if (sections.contains(section) && sections[section] == v)
                        return false;
                    m.set_section(section, v);
                    return true;
                });
                ESCA_LOG_INFO("section \"%s\" registered by %s, version %llu", section.c_str(), sender.c_str(),
                              static_cast<unsigned long long>(version));
                return version;
            }),

        sdbus::registerMethod("RemoveManifestInfo").implementedAs([this](const std::string &section) {
            bool existed = false;
            const uint64_t version = commit_local([&](DeviceManifest &m) { return existed = m.remove_section(section); });
            if (!existed)
                throw error("NotFound", "no section \"" + section + "\"");
            return version;
        }),

        sdbus::registerMethod("GetNetworkInfo").implementedAs([this] {
            return devices_.local().doc()["network"].dump();
        }),

        sdbus::registerMethod("Sync").implementedAs([this] { sync_now(); }),

        sdbus::registerSignal("DeviceAdded").withParameters<std::string, std::string>(),
        sdbus::registerSignal("DeviceRemoved").withParameters<std::string>(),
        sdbus::registerSignal("ManifestChanged").withParameters<std::string, uint64_t>())
        .forInterface(kInterface);
}

void CapabilityManager::start()
{
    register_object();
    persist(devices_.local());

    connections_ = std::make_unique<ConnectionManager>(
        config_.port, [this] { return devices_.local().dump(); },
        [this](const std::string &text, const Endpoint &from) { return receive(text, from); },
        [this](const Endpoint &peer) { unreachable(peer); });
    std::string why;
    if (!connections_->start(why))
        throw std::runtime_error(why);

    network_ = std::make_unique<NetworkMonitor>([this](const NetworkState &s) { network_changed(s); });
    if (!network_->start(why))
        throw std::runtime_error(why);

    const DeviceManifest local = devices_.local();
    discovery_ = std::make_unique<DiscoveryManager>(
        config_.service_type, config_.port, [this](const Advertisement &a) { found(a); },
        [this](const std::string &id) { lost(id); });
    // Without avahi-daemon the device is still useful on its own; discovery
    // keeps waiting for the daemon.
    if (!discovery_->start(local.name(), local.id(), local.version(), local.hash(), why))
        ESCA_LOG_ERROR("%s", why.c_str());

    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = true;
    }
    ticker_ = std::thread([this] { tick_loop(); });
    ESCA_LOG_INFO("device %s \"%s\", manifest version %llu", local.id().c_str(), local.name().c_str(),
                  static_cast<unsigned long long>(local.version()));
}

void CapabilityManager::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    wake_.notify_all();
    if (ticker_.joinable())
        ticker_.join();
    if (network_)
        network_->stop();
    if (discovery_)
        discovery_->stop();
    if (connections_)
        connections_->stop();
}

} // namespace esca::capability
