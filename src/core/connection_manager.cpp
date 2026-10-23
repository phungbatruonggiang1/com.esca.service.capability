#include "connection_manager.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <coap3/coap.h>

#include <condition_variable>
#include <cstring>
#include <future>

#include "logger.hpp"

namespace esca::capability {

namespace {

constexpr const char *kResource = "manifest";

coap_address_t to_address(const Endpoint &e)
{
    coap_address_t a;
    coap_address_init(&a);
    a.size = sizeof(struct sockaddr_in);
    a.addr.sin.sin_family = AF_INET;
    a.addr.sin.sin_port = htons(e.port);
    inet_pton(AF_INET, e.address.c_str(), &a.addr.sin.sin_addr);
    return a;
}

Endpoint from_address(const coap_address_t *a)
{
    Endpoint e;
    if (!a || a->addr.sa.sa_family != AF_INET)
        return e;
    char text[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a->addr.sin.sin_addr, text, sizeof(text));
    e.address = text;
    e.port = ntohs(a->addr.sin.sin_port);
    return e;
}

std::string key_of(const Endpoint &e) { return e.address + ":" + std::to_string(e.port); }

/// libcoap keeps the payload of a large transfer until it is all sent and
/// then calls this; the payload is a heap copy owned by that transfer.
void release(coap_session_t *, void *payload) { delete static_cast<std::string *>(payload); }

} // namespace

struct ConnectionManager::Impl {
    ConnectionManager *owner = nullptr;
    coap_context_t *context = nullptr;
    std::map<std::string, coap_session_t *> sessions; ///< one client session per peer
    /// What each outstanding request was, by token, to route its answer.
    struct Pending {
        bool put;
        Endpoint peer;
    };
    std::map<std::string, Pending> pending;

    static ConnectionManager::Impl *of(coap_session_t *session)
    {
        return static_cast<Impl *>(coap_context_get_app_data(coap_session_get_context(session)));
    }

    static std::string token_of(const coap_pdu_t *pdu)
    {
        const coap_bin_const_t t = coap_pdu_get_token(pdu);
        return std::string(reinterpret_cast<const char *>(t.s), t.length);
    }

    static std::string body_of(const coap_pdu_t *pdu)
    {
        size_t length = 0, offset = 0, total = 0;
        const uint8_t *data = nullptr;
        // COAP_BLOCK_SINGLE_BODY: the whole body, however many blocks it took.
        if (!coap_get_data_large(pdu, &length, &data, &offset, &total) || !data)
            return "";
        return std::string(reinterpret_cast<const char *>(data), length);
    }

    // --- server --------------------------------------------------------------

    static void on_get(coap_resource_t *resource, coap_session_t *session, const coap_pdu_t *request,
                       const coap_string_t *query, coap_pdu_t *response)
    {
        Impl *self = of(session);
        auto *payload = new std::string(self->owner->provider_());
        coap_pdu_set_code(response, COAP_RESPONSE_CODE_CONTENT);
        coap_add_data_large_response(resource, session, request, response, query, COAP_MEDIATYPE_APPLICATION_JSON,
                                     -1, 0, payload->size(), reinterpret_cast<const uint8_t *>(payload->data()),
                                     release, payload);
    }

    static void on_put(coap_resource_t *, coap_session_t *session, const coap_pdu_t *request,
                       const coap_string_t *, coap_pdu_t *response)
    {
        Impl *self = of(session);
        Endpoint from = from_address(coap_session_get_addr_remote(session));
        // The source port is the peer's client port, not its server port;
        // the address is what identifies it here.
        from.port = 0;
        const std::string error = self->owner->receiver_(body_of(request), from);
        if (error.empty()) {
            coap_pdu_set_code(response, COAP_RESPONSE_CODE_CHANGED);
        } else {
            coap_pdu_set_code(response, COAP_RESPONSE_CODE_BAD_REQUEST);
            coap_add_data(response, error.size(), reinterpret_cast<const uint8_t *>(error.data()));
        }
    }

    // --- client --------------------------------------------------------------

    static coap_response_t on_response(coap_session_t *session, const coap_pdu_t *, const coap_pdu_t *received,
                                       const coap_mid_t)
    {
        Impl *self = of(session);
        const auto it = self->pending.find(token_of(received));
        if (it == self->pending.end())
            return COAP_RESPONSE_OK;
        const Pending p = it->second;
        self->pending.erase(it);

        const coap_pdu_code_t code = coap_pdu_get_code(received);
        if (!p.put && code == COAP_RESPONSE_CODE_CONTENT) {
            const std::string error = self->owner->receiver_(body_of(received), p.peer);
            if (!error.empty())
                ESCA_LOG_WARN("manifest from %s refused: %s", key_of(p.peer).c_str(), error.c_str());
        } else if (p.put && code != COAP_RESPONSE_CODE_CHANGED) {
            ESCA_LOG_WARN("%s refused our manifest: %s", key_of(p.peer).c_str(), body_of(received).c_str());
        } else if (!p.put) {
            ESCA_LOG_WARN("GET %s/manifest answered %d.%02d", key_of(p.peer).c_str(), code >> 5, code & 31);
        }
        return COAP_RESPONSE_OK;
    }

    static void on_nack(coap_session_t *session, const coap_pdu_t *sent, const coap_nack_reason_t reason,
                        const coap_mid_t)
    {
        Impl *self = of(session);
        if (!sent)
            return;
        const auto it = self->pending.find(token_of(sent));
        if (it == self->pending.end())
            return;
        const Endpoint peer = it->second.peer;
        self->pending.erase(it);
        if (reason == COAP_NACK_TOO_MANY_RETRIES || reason == COAP_NACK_NOT_DELIVERABLE
            || reason == COAP_NACK_ICMP_ISSUE) {
            ESCA_LOG_WARN("%s does not answer", key_of(peer).c_str());
            self->owner->unreachable_(peer);
        }
    }

    coap_session_t *session_for(const Endpoint &peer)
    {
        const std::string key = key_of(peer);
        const auto it = sessions.find(key);
        if (it != sessions.end())
            return it->second;
        const coap_address_t address = to_address(peer);
        coap_session_t *s = coap_new_client_session(context, nullptr, &address, COAP_PROTO_UDP);
        if (s)
            sessions[key] = s;
        return s;
    }

    void send(const Job &job)
    {
        coap_session_t *session = session_for(job.peer);
        if (!session) {
            ESCA_LOG_WARN("cannot open a CoAP session to %s", key_of(job.peer).c_str());
            return;
        }
        coap_pdu_t *pdu = coap_new_pdu(COAP_MESSAGE_CON, job.put ? COAP_REQUEST_CODE_PUT : COAP_REQUEST_CODE_GET,
                                       session);
        if (!pdu)
            return;
        uint8_t token[8];
        size_t length = 0;
        coap_session_new_token(session, &length, token);
        coap_add_token(pdu, length, token);
        coap_add_option(pdu, COAP_OPTION_URI_PATH, std::strlen(kResource),
                        reinterpret_cast<const uint8_t *>(kResource));
        if (job.put) {
            uint8_t format[4];
            coap_add_option(pdu, COAP_OPTION_CONTENT_FORMAT,
                            coap_encode_var_safe(format, sizeof(format), COAP_MEDIATYPE_APPLICATION_JSON), format);
            auto *payload = new std::string(job.body);
            coap_add_data_large_request(session, pdu, payload->size(),
                                        reinterpret_cast<const uint8_t *>(payload->data()), release, payload);
        }
        pending[std::string(reinterpret_cast<const char *>(token), length)] = Pending { job.put, job.peer };
        if (coap_send(session, pdu) == COAP_INVALID_MID)
            ESCA_LOG_WARN("cannot send to %s", key_of(job.peer).c_str());
    }
};

ConnectionManager::ConnectionManager(uint16_t port, Provider provider, Receiver receiver, Unreachable unreachable)
    : port_(port), provider_(std::move(provider)), receiver_(std::move(receiver)),
      unreachable_(std::move(unreachable))
{
}

ConnectionManager::~ConnectionManager() { stop(); }

bool ConnectionManager::start(std::string &error)
{
    // Setting up on the loop thread itself keeps every libcoap object on the
    // thread that uses it; the result comes back through a promise.
    std::promise<std::string> ready;
    std::future<std::string> result = ready.get_future();
    running_ = true;
    thread_ = std::thread([this, &ready] {
        coap_startup();
        // Our own handlers report what matters; libcoap's warnings (an ICMP
        // refusal from a peer that just left) would only repeat it.
        coap_set_log_level(COAP_LOG_ERR);
        auto *impl = new Impl;
        impl->owner = this;
        impl->context = coap_new_context(nullptr);
        if (!impl->context) {
            delete impl;
            ready.set_value("cannot create a CoAP context");
            return;
        }
        coap_context_set_app_data(impl->context, impl);
        coap_context_set_block_mode(impl->context, COAP_BLOCK_USE_LIBCOAP | COAP_BLOCK_SINGLE_BODY);
        coap_register_response_handler(impl->context, &Impl::on_response);
        coap_register_nack_handler(impl->context, &Impl::on_nack);

        coap_address_t any;
        coap_address_init(&any);
        any.size = sizeof(struct sockaddr_in);
        any.addr.sin.sin_family = AF_INET;
        any.addr.sin.sin_addr.s_addr = INADDR_ANY;
        any.addr.sin.sin_port = htons(port_);
        if (!coap_new_endpoint(impl->context, &any, COAP_PROTO_UDP)) {
            coap_free_context(impl->context);
            delete impl;
            ready.set_value("cannot listen on UDP port " + std::to_string(port_));
            return;
        }
        coap_resource_t *resource = coap_resource_init(coap_make_str_const(kResource), 0);
        coap_register_request_handler(resource, COAP_REQUEST_GET, &Impl::on_get);
        coap_register_request_handler(resource, COAP_REQUEST_PUT, &Impl::on_put);
        coap_add_resource(impl->context, resource);

        impl_ = impl;
        ready.set_value("");
        loop();

        coap_free_context(impl->context); // releases sessions and resources
        delete impl;
        impl_ = nullptr;
        coap_cleanup();
    });
    error = result.get();
    if (!error.empty()) {
        running_ = false;
        thread_.join();
        return false;
    }
    ESCA_LOG_INFO("CoAP: serving /manifest on UDP %u", port_);
    return true;
}

void ConnectionManager::loop()
{
    while (running_) {
        std::deque<Job> jobs;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs.swap(jobs_);
        }
        for (const Job &job : jobs)
            impl_->send(job);
        // Short, so queued jobs and a stop request are not left waiting.
        coap_io_process(impl_->context, 100);
    }
}

void ConnectionManager::stop()
{
    if (!thread_.joinable())
        return;
    running_ = false;
    thread_.join();
}

void ConnectionManager::fetch(const Endpoint &peer)
{
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.push_back(Job { false, peer, {} });
}

void ConnectionManager::push(const Endpoint &peer, std::string manifest)
{
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.push_back(Job { true, peer, std::move(manifest) });
}

} // namespace esca::capability
