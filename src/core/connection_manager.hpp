// The peer-to-peer channel manifests travel on.
//
// The connection manager, over CoAP (libcoap): every device serves one
// resource, /manifest, on UDP 5683. GET returns its manifest; PUT hands it a
// peer's. A device fetches a peer's manifest when discovery finds it or
// advertises a newer version, and pushes its own to every peer when it
// changes. No server anywhere: each device is both ends (FR03, TC03).
//
// Manifests run to several kilobytes, beyond one datagram, so libcoap's
// block-wise transfer carries them and hands over whole bodies.
//
// libcoap is not thread-safe, so every libcoap call happens on this
// manager's own thread; fetch() and push() only queue work for it.

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "device_manager.hpp"

namespace esca::capability {

class ConnectionManager {
public:
    /// The local manifest, as served on GET.
    using Provider = std::function<std::string()>;
    /// A manifest from a peer, fetched or pushed. Returns "" if accepted,
    /// otherwise why not; a PUT is answered with that.
    using Receiver = std::function<std::string(const std::string &text, const Endpoint &from)>;
    /// A request got no answer at all.
    using Unreachable = std::function<void(const Endpoint &peer)>;

    ConnectionManager(uint16_t port, Provider provider, Receiver receiver, Unreachable unreachable);
    ~ConnectionManager();
    ConnectionManager(const ConnectionManager &) = delete;
    ConnectionManager &operator=(const ConnectionManager &) = delete;

    bool start(std::string &error);
    void stop();

    /// GET the peer's manifest; the reply goes to the Receiver.
    void fetch(const Endpoint &peer);
    /// PUT this manifest to the peer.
    void push(const Endpoint &peer, std::string manifest);

    uint16_t port() const { return port_; }

private:
    struct Job {
        bool put;
        Endpoint peer;
        std::string body;
    };

    struct Impl;
    void loop();

    uint16_t port_;
    Provider provider_;
    Receiver receiver_;
    Unreachable unreachable_;

    std::mutex mutex_;
    std::deque<Job> jobs_;
    std::atomic<bool> running_ { false };
    std::thread thread_;
    Impl *impl_ = nullptr; ///< owned by the loop thread
};

} // namespace esca::capability
