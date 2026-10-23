#include "network_monitor.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "logger.hpp"

namespace esca::capability {

NetworkMonitor::NetworkMonitor(Changed changed) : changed_(std::move(changed)) {}

NetworkMonitor::~NetworkMonitor() { stop(); }

NetworkState NetworkMonitor::read()
{
    NetworkState state;
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0)
        return state;
    for (ifaddrs *i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET)
            continue;
        if ((i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_RUNNING))
            continue;
        char text[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(i->ifa_addr)->sin_addr, text, sizeof(text));
        state.addresses.push_back(text);
        if (std::find(state.interfaces.begin(), state.interfaces.end(), i->ifa_name) == state.interfaces.end())
            state.interfaces.push_back(i->ifa_name);
    }
    freeifaddrs(list);
    std::sort(state.addresses.begin(), state.addresses.end());
    std::sort(state.interfaces.begin(), state.interfaces.end());
    state.online = !state.addresses.empty();
    return state;
}

bool NetworkMonitor::start(std::string &error)
{
    socket_ = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (socket_ < 0) {
        error = std::string("netlink: ") + std::strerror(errno);
        return false;
    }
    sockaddr_nl address {};
    address.nl_family = AF_NETLINK;
    address.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR;
    if (::bind(socket_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 || ::pipe2(wake_, O_CLOEXEC) < 0) {
        error = std::string("netlink: ") + std::strerror(errno);
        ::close(socket_);
        socket_ = -1;
        return false;
    }
    last_ = read();
    changed_(last_);
    running_ = true;
    thread_ = std::thread([this] { loop(); });
    return true;
}

void NetworkMonitor::stop()
{
    if (!thread_.joinable())
        return;
    running_ = false;
    const char c = 0;
    (void)!::write(wake_[1], &c, 1);
    thread_.join();
    ::close(socket_);
    ::close(wake_[0]);
    ::close(wake_[1]);
    socket_ = wake_[0] = wake_[1] = -1;
}

void NetworkMonitor::loop()
{
    char buffer[8192];
    while (running_) {
        pollfd fds[2] = { { socket_, POLLIN, 0 }, { wake_[0], POLLIN, 0 } };
        if (::poll(fds, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (fds[1].revents)
            break;
        // The messages themselves only say "something changed"; reading the
        // whole picture again is simpler than tracking each one, and DHCP
        // tends to send several in a burst, so let it settle first.
        while (::recv(socket_, buffer, sizeof(buffer), MSG_DONTWAIT) > 0) {
        }
        pollfd settle = { wake_[0], POLLIN, 0 };
        if (::poll(&settle, 1, 300) > 0)
            break;
        while (::recv(socket_, buffer, sizeof(buffer), MSG_DONTWAIT) > 0) {
        }
        const NetworkState now = read();
        if (now != last_) {
            std::string list;
            for (const auto &a : now.addresses)
                list += (list.empty() ? "" : " ") + a;
            ESCA_LOG_INFO("network: %s%s%s", now.online ? "online" : "offline", list.empty() ? "" : ", ",
                          list.c_str());
            last_ = now;
            changed_(now);
        }
    }
}

} // namespace esca::capability
