#include "device_manager.hpp"

#include <algorithm>

namespace esca::capability {

using nlohmann::json;

const char *to_string(RemoteUpdate update)
{
    switch (update) {
    case RemoteUpdate::Added: return "added";
    case RemoteUpdate::Changed: return "changed";
    case RemoteUpdate::Unchanged: return "unchanged";
    case RemoteUpdate::Stale: return "stale";
    }
    return "unchanged";
}

DeviceManager::DeviceManager(DeviceManifest local) : local_(std::move(local)) {}

DeviceManifest DeviceManager::local() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return local_;
}

std::string DeviceManager::local_id() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return local_.id();
}

DeviceManifest DeviceManager::update_local(const std::function<bool(DeviceManifest &)> &change)
{
    std::lock_guard<std::mutex> lock(mutex_);
    DeviceManifest copy = local_;
    if (change(copy))
        local_ = std::move(copy);
    return local_;
}

bool DeviceManager::advertised(const std::string &id, const std::string &name, const Endpoint &endpoint,
                               uint64_t version, const std::string &hash)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (id == local_.id())
        return false;
    Entry &e = remote_[id];
    e.record.id = id;
    if (e.record.name.empty())
        e.record.name = name;
    e.record.endpoint = endpoint;
    e.record.online = true;
    e.record.advertised_version = version;
    e.record.advertised_hash = hash;
    e.record.last_seen = std::chrono::steady_clock::now();
    if (!e.manifest)
        return true;
    // A short hash in the advertisement that differs from the held one also
    // calls for a fetch, whatever the versions say.
    return version > e.manifest->version()
           || (!hash.empty() && e.manifest->hash().find(hash) == std::string::npos);
}

RemoteUpdate DeviceManager::accept(const DeviceManifest &m, const Endpoint &from)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (m.id() == local_.id())
        return RemoteUpdate::Unchanged;

    Entry &e = remote_[m.id()];
    const bool known = e.manifest.has_value();
    RemoteUpdate result = RemoteUpdate::Added;
    if (known) {
        const DeviceManifest &held = *e.manifest;
        if (m.version() < held.version())
            result = RemoteUpdate::Stale;
        else if (m.version() == held.version())
            result = m.hash() == held.hash() ? RemoteUpdate::Unchanged : RemoteUpdate::Stale;
        else
            result = RemoteUpdate::Changed;
    }

    // Whatever the verdict, the device just spoke: it is online at this
    // address.
    e.record.id = m.id();
    e.record.endpoint = from;
    e.record.online = true;
    e.record.last_seen = std::chrono::steady_clock::now();
    if (result == RemoteUpdate::Added || result == RemoteUpdate::Changed) {
        e.manifest = m;
        e.record.name = m.name();
        e.record.version = m.version();
    }
    return result;
}

bool DeviceManager::lost(const std::string &id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return remote_.erase(id) != 0;
}

std::vector<std::string> DeviceManager::expire(std::chrono::seconds silence)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> gone;
    for (auto it = remote_.begin(); it != remote_.end();) {
        if (now - it->second.record.last_seen > silence) {
            gone.push_back(it->first);
            it = remote_.erase(it);
        } else {
            ++it;
        }
    }
    return gone;
}

std::vector<std::string> DeviceManager::clear_remote()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> gone;
    for (const auto &[id, e] : remote_)
        gone.push_back(id);
    remote_.clear();
    return gone;
}

std::optional<DeviceManifest> DeviceManager::manifest(const std::string &id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (id.empty() || id == local_.id())
        return local_;
    const auto it = remote_.find(id);
    if (it == remote_.end())
        return std::nullopt;
    return it->second.manifest;
}

std::vector<DeviceRecord> DeviceManager::devices() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DeviceRecord> out;
    DeviceRecord self;
    self.id = local_.id();
    self.name = local_.name();
    self.local = true;
    self.online = true;
    self.version = local_.version();
    self.last_seen = std::chrono::steady_clock::now();
    out.push_back(self);

    std::vector<DeviceRecord> others;
    for (const auto &[id, e] : remote_)
        others.push_back(e.record);
    std::sort(others.begin(), others.end(),
              [](const DeviceRecord &a, const DeviceRecord &b) { return a.name < b.name; });
    out.insert(out.end(), others.begin(), others.end());
    return out;
}

std::vector<Endpoint> DeviceManager::endpoints() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Endpoint> out;
    for (const auto &[id, e] : remote_)
        if (e.record.online && !e.record.endpoint.address.empty())
            out.push_back(e.record.endpoint);
    return out;
}

std::optional<Endpoint> DeviceManager::endpoint(const std::string &id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = remote_.find(id);
    if (it == remote_.end())
        return std::nullopt;
    return it->second.record.endpoint;
}

namespace {

bool matches(const json &node, const json &value)
{
    if (node == value)
        return true;
    if (node.is_array())
        return std::any_of(node.begin(), node.end(), [&](const json &v) { return v == value; });
    if (node.is_object() && value.is_string())
        return node.contains(value.get<std::string>());
    return false;
}

} // namespace

std::vector<std::pair<std::string, std::string>> DeviceManager::find(const std::string &pointer,
                                                                     const json &value) const
{
    json::json_pointer ptr;
    try {
        ptr = json::json_pointer(pointer);
    } catch (const json::exception &) {
        return {};
    }
    auto hit = [&](const DeviceManifest &m) {
        const json &doc = m.doc();
        return doc.contains(ptr) && matches(doc.at(ptr), value);
    };

    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<std::string, std::string>> out;
    if (hit(local_))
        out.emplace_back(local_.id(), local_.name());
    for (const auto &[id, e] : remote_)
        if (e.manifest && e.record.online && hit(*e.manifest))
            out.emplace_back(id, e.manifest->name());
    return out;
}

} // namespace esca::capability
