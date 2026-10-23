#include "discovery_manager.hpp"

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/thread-watch.h>

#include <map>

#include "logger.hpp"

namespace esca::capability {

struct DiscoveryManager::Impl {
    std::string type;
    uint16_t port = 0;
    Found found;
    Lost lost;

    std::string name;
    std::string id;
    uint64_t version = 0;
    std::string hash;

    AvahiThreadedPoll *poll = nullptr;
    AvahiClient *client = nullptr;
    AvahiEntryGroup *group = nullptr;
    AvahiServiceBrowser *browser = nullptr;
    /// One resolver per instance, kept alive so TXT changes keep arriving.
    std::map<std::string, AvahiServiceResolver *> resolvers;
    /// Instance name to device id: a REMOVE event carries only the name.
    std::map<std::string, std::string> ids;

    std::string short_hash() const
    {
        // "sha256:" then the digest; sixteen digits tell versions apart.
        return hash.size() > 7 ? hash.substr(7, 16) : hash;
    }

    AvahiStringList *txt() const
    {
        return avahi_string_list_new(("id=" + id).c_str(), ("v=" + std::to_string(version)).c_str(),
                                     ("h=" + short_hash()).c_str(), "s=1.0", nullptr);
    }

    void publish()
    {
        if (!group) {
            group = avahi_entry_group_new(client, &Impl::on_group, this);
            if (!group) {
                ESCA_LOG_ERROR("avahi: cannot create an entry group: %s",
                               avahi_strerror(avahi_client_errno(client)));
                return;
            }
        }
        if (!avahi_entry_group_is_empty(group))
            return;
        AvahiStringList *list = txt();
        const int rc = avahi_entry_group_add_service_strlst(group, AVAHI_IF_UNSPEC, AVAHI_PROTO_INET,
                                                            static_cast<AvahiPublishFlags>(0), name.c_str(),
                                                            type.c_str(), nullptr, nullptr, port, list);
        avahi_string_list_free(list);
        if (rc == AVAHI_ERR_COLLISION) {
            rename();
            return;
        }
        if (rc < 0 || avahi_entry_group_commit(group) < 0)
            ESCA_LOG_ERROR("avahi: cannot publish %s: %s", name.c_str(), avahi_strerror(rc));
        else
            ESCA_LOG_INFO("published %s.%s on port %u", name.c_str(), type.c_str(), port);
    }

    void rename()
    {
        char *next = avahi_alternative_service_name(name.c_str());
        ESCA_LOG_WARN("avahi: \"%s\" is taken, publishing as \"%s\"", name.c_str(), next);
        name = next;
        avahi_free(next);
        if (group)
            avahi_entry_group_reset(group);
        publish();
    }

    void browse()
    {
        if (browser)
            return;
        browser = avahi_service_browser_new(client, AVAHI_IF_UNSPEC, AVAHI_PROTO_INET, type.c_str(), nullptr,
                                            static_cast<AvahiLookupFlags>(0), &Impl::on_browse, this);
        if (!browser)
            ESCA_LOG_ERROR("avahi: cannot browse %s: %s", type.c_str(),
                           avahi_strerror(avahi_client_errno(client)));
    }

    void forget_browsing()
    {
        for (auto &[key, r] : resolvers)
            avahi_service_resolver_free(r);
        resolvers.clear();
        if (browser)
            avahi_service_browser_free(browser);
        browser = nullptr;
        group = nullptr; // freed with the client
    }

    static void on_client(AvahiClient *c, AvahiClientState state, void *self)
    {
        auto *d = static_cast<Impl *>(self);
        d->client = c;
        switch (state) {
        case AVAHI_CLIENT_S_RUNNING:
            d->publish();
            d->browse();
            break;
        case AVAHI_CLIENT_S_COLLISION:
        case AVAHI_CLIENT_S_REGISTERING:
            // The host name changed or collided; publish again once settled.
            if (d->group)
                avahi_entry_group_reset(d->group);
            break;
        case AVAHI_CLIENT_FAILURE:
            ESCA_LOG_WARN("avahi: %s; waiting for the daemon", avahi_strerror(avahi_client_errno(c)));
            // With AVAHI_CLIENT_NO_FAIL the client reconnects by itself once
            // the daemon is back; everything it owned is gone meanwhile.
            d->forget_browsing();
            break;
        case AVAHI_CLIENT_CONNECTING:
            break;
        }
    }

    static void on_group(AvahiEntryGroup *, AvahiEntryGroupState state, void *self)
    {
        auto *d = static_cast<Impl *>(self);
        if (state == AVAHI_ENTRY_GROUP_COLLISION)
            d->rename();
        else if (state == AVAHI_ENTRY_GROUP_FAILURE)
            ESCA_LOG_ERROR("avahi: publishing failed: %s", avahi_strerror(avahi_client_errno(d->client)));
    }

    static void on_browse(AvahiServiceBrowser *, AvahiIfIndex interface, AvahiProtocol protocol,
                          AvahiBrowserEvent event, const char *name, const char *type, const char *domain,
                          AvahiLookupResultFlags, void *self)
    {
        auto *d = static_cast<Impl *>(self);
        // Only NEW and REMOVE name a service; the others pass null.
        const std::string key = name ? std::to_string(interface) + "/" + name : std::string();
        switch (event) {
        case AVAHI_BROWSER_NEW:
            if (!d->resolvers.count(key)) {
                AvahiServiceResolver *r = avahi_service_resolver_new(
                    d->client, interface, protocol, name, type, domain, AVAHI_PROTO_INET,
                    static_cast<AvahiLookupFlags>(0), &Impl::on_resolve, self);
                if (r)
                    d->resolvers[key] = r;
            }
            break;
        case AVAHI_BROWSER_REMOVE: {
            const auto r = d->resolvers.find(key);
            if (r != d->resolvers.end()) {
                avahi_service_resolver_free(r->second);
                d->resolvers.erase(r);
            }
            const auto it = d->ids.find(name);
            if (it != d->ids.end()) {
                const std::string id = it->second;
                d->ids.erase(it);
                d->lost(id);
            }
            break;
        }
        case AVAHI_BROWSER_FAILURE:
            ESCA_LOG_ERROR("avahi: browsing failed: %s", avahi_strerror(avahi_client_errno(d->client)));
            break;
        case AVAHI_BROWSER_ALL_FOR_NOW:
        case AVAHI_BROWSER_CACHE_EXHAUSTED:
            break;
        }
    }

    static std::string txt_value(AvahiStringList *txt, const char *key)
    {
        AvahiStringList *item = avahi_string_list_find(txt, key);
        if (!item)
            return "";
        char *k = nullptr, *v = nullptr;
        std::string value;
        if (avahi_string_list_get_pair(item, &k, &v, nullptr) == 0 && v)
            value = v;
        avahi_free(k);
        avahi_free(v);
        return value;
    }

    static void on_resolve(AvahiServiceResolver *, AvahiIfIndex, AvahiProtocol, AvahiResolverEvent event,
                           const char *name, const char *, const char *, const char *,
                           const AvahiAddress *address, uint16_t port, AvahiStringList *txt,
                           AvahiLookupResultFlags, void *self)
    {
        auto *d = static_cast<Impl *>(self);
        if (event != AVAHI_RESOLVER_FOUND)
            return;
        Advertisement a;
        a.id = txt_value(txt, "id");
        if (a.id.empty() || a.id == d->id)
            return; // ourselves, or not one of ours
        char text[AVAHI_ADDRESS_STR_MAX];
        avahi_address_snprint(text, sizeof(text), address);
        a.name = name;
        a.endpoint = Endpoint { text, port };
        try {
            a.version = std::stoull(txt_value(txt, "v"));
        } catch (const std::exception &) {
            a.version = 0;
        }
        a.hash = txt_value(txt, "h");
        d->ids[name] = a.id;
        d->found(a);
    }
};

DiscoveryManager::DiscoveryManager(std::string service_type, uint16_t port, Found found, Lost lost)
    : impl_(std::make_unique<Impl>())
{
    impl_->type = std::move(service_type);
    impl_->port = port;
    impl_->found = std::move(found);
    impl_->lost = std::move(lost);
}

DiscoveryManager::~DiscoveryManager() { stop(); }

bool DiscoveryManager::start(const std::string &name, const std::string &id, uint64_t version,
                             const std::string &hash, std::string &error)
{
    impl_->name = name;
    impl_->id = id;
    impl_->version = version;
    impl_->hash = hash;

    impl_->poll = avahi_threaded_poll_new();
    if (!impl_->poll) {
        error = "avahi: cannot create a poll loop";
        return false;
    }
    int err = 0;
    // The client callback may already run inside avahi_client_new, on this
    // thread, before the poll thread exists; after that it runs there.
    impl_->client = avahi_client_new(avahi_threaded_poll_get(impl_->poll), AVAHI_CLIENT_NO_FAIL,
                                     &Impl::on_client, impl_.get(), &err);
    if (!impl_->client) {
        error = std::string("avahi: ") + avahi_strerror(err);
        avahi_threaded_poll_free(impl_->poll);
        impl_->poll = nullptr;
        return false;
    }
    if (avahi_threaded_poll_start(impl_->poll) < 0) {
        error = "avahi: cannot start the poll thread";
        return false;
    }
    return true;
}

void DiscoveryManager::stop()
{
    if (!impl_->poll)
        return;
    avahi_threaded_poll_stop(impl_->poll);
    impl_->forget_browsing();
    if (impl_->client)
        avahi_client_free(impl_->client); // frees the entry group too
    impl_->client = nullptr;
    avahi_threaded_poll_free(impl_->poll);
    impl_->poll = nullptr;
}

void DiscoveryManager::update(uint64_t version, const std::string &hash)
{
    if (!impl_->poll)
        return;
    avahi_threaded_poll_lock(impl_->poll);
    impl_->version = version;
    impl_->hash = hash;
    if (impl_->group && !avahi_entry_group_is_empty(impl_->group)) {
        AvahiStringList *list = impl_->txt();
        const int rc = avahi_entry_group_update_service_txt_strlst(
            impl_->group, AVAHI_IF_UNSPEC, AVAHI_PROTO_INET, static_cast<AvahiPublishFlags>(0),
            impl_->name.c_str(), impl_->type.c_str(), nullptr, list);
        avahi_string_list_free(list);
        if (rc < 0)
            ESCA_LOG_WARN("avahi: cannot update TXT: %s", avahi_strerror(rc));
    }
    avahi_threaded_poll_unlock(impl_->poll);
}

} // namespace esca::capability
