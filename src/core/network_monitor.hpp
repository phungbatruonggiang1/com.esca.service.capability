// Whether this device is on a network, and at which addresses.
//
// The ESCA image runs systemd-networkd, not connman, so this watches
// the kernel directly: an rtnetlink socket subscribed to link and IPv4
// address changes. On every change it re-reads the addresses and reports
// when the picture differs from the last one.

#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace esca::capability {

struct NetworkState {
    bool online = false;
    std::vector<std::string> addresses; ///< IPv4, loopback excluded
    std::vector<std::string> interfaces;
    bool operator==(const NetworkState &o) const
    {
        return online == o.online && addresses == o.addresses && interfaces == o.interfaces;
    }
    bool operator!=(const NetworkState &o) const { return !(*this == o); }
};

class NetworkMonitor {
public:
    /// Runs on the monitor's thread.
    using Changed = std::function<void(const NetworkState &)>;

    explicit NetworkMonitor(Changed changed);
    ~NetworkMonitor();
    NetworkMonitor(const NetworkMonitor &) = delete;
    NetworkMonitor &operator=(const NetworkMonitor &) = delete;

    bool start(std::string &error);
    void stop();

    /// The addresses as they are now.
    static NetworkState read();

private:
    void loop();

    Changed changed_;
    int socket_ = -1;
    int wake_[2] = { -1, -1 };
    std::atomic<bool> running_ { false };
    std::thread thread_;
    NetworkState last_;
};

} // namespace esca::capability
