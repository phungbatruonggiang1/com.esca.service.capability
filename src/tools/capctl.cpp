// esca-capctl: the AI Capability Provider from a terminal.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

#include "esca/capability/client.hpp"

using namespace esca::capability;
using nlohmann::json;

namespace {

int usage()
{
    std::fprintf(stderr,
                 "usage: esca-capctl [--session] COMMAND\n"
                 "  id                       this device's id\n"
                 "  list                     every known device\n"
                 "  show [ID]                a manifest (this device's without ID)\n"
                 "  network                  this device's network state\n"
                 "  find POINTER VALUE       devices with VALUE at POINTER, e.g.\n"
                 "                           find /sections/ai/tasks stt\n"
                 "  register SECTION JSON    set a section; JSON may be @file\n"
                 "  remove SECTION           remove a section\n"
                 "  sync                     fetch every peer's manifest again\n"
                 "  watch                    print device and manifest changes\n");
    return 2;
}

std::string argument_json(const std::string &arg)
{
    if (arg.empty() || arg[0] != '@')
        return arg;
    std::ifstream in(arg.substr(1));
    if (!in)
        throw std::runtime_error("cannot read " + arg.substr(1));
    return std::string(std::istreambuf_iterator<char>(in), {});
}

} // namespace

int main(int argc, char **argv)
{
    int i = 1;
    bool session = false;
    if (i < argc && std::strcmp(argv[i], "--session") == 0) {
        session = true;
        ++i;
    }
    if (i >= argc)
        return usage();
    const std::string command = argv[i++];
    const int rest = argc - i;

    try {
        CapabilityClient cap(session);
        if (command == "id" && rest == 0) {
            std::printf("%s\n", cap.local_id().c_str());
        } else if (command == "list" && rest == 0) {
            std::printf("%-32s  %-20s  %-21s  %7s  %s\n", "ID", "NAME", "ADDRESS", "VERSION", "");
            for (const DeviceInfo &d : cap.devices()) {
                const std::string at =
                    d.local ? "(this device)" : d.address + ":" + std::to_string(d.port);
                std::printf("%-32s  %-20s  %-21s  %7llu  %s\n", d.id.c_str(), d.name.c_str(), at.c_str(),
                            static_cast<unsigned long long>(d.version), d.online ? "" : "offline");
            }
        } else if (command == "show" && rest <= 1) {
            std::printf("%s\n", cap.manifest(rest ? argv[i] : "").dump(2).c_str());
        } else if (command == "network" && rest == 0) {
            std::printf("%s\n", cap.network().dump(2).c_str());
        } else if (command == "find" && rest == 2) {
            json value = json::parse(argv[i + 1], nullptr, false);
            if (value.is_discarded())
                value = argv[i + 1];
            for (const auto &[id, name] : cap.find(argv[i], value))
                std::printf("%s  %s\n", id.c_str(), name.c_str());
        } else if (command == "register" && rest == 2) {
            const json value = json::parse(argument_json(argv[i + 1]));
            std::printf("version %llu\n", static_cast<unsigned long long>(cap.register_info(argv[i], value)));
        } else if (command == "remove" && rest == 1) {
            std::printf("version %llu\n", static_cast<unsigned long long>(cap.remove_info(argv[i])));
        } else if (command == "sync" && rest == 0) {
            cap.sync();
        } else if (command == "watch" && rest == 0) {
            cap.on_device_added([](const std::string &id, const std::string &name) {
                std::printf("added    %s  %s\n", id.c_str(), name.c_str());
                std::fflush(stdout);
            });
            cap.on_device_removed([](const std::string &id) {
                std::printf("removed  %s\n", id.c_str());
                std::fflush(stdout);
            });
            cap.on_manifest_changed([](const std::string &id, uint64_t version) {
                std::printf("changed  %s  version %llu\n", id.c_str(), static_cast<unsigned long long>(version));
                std::fflush(stdout);
            });
            for (;;)
                std::this_thread::sleep_for(std::chrono::hours(1));
        } else {
            return usage();
        }
    } catch (const CapabilityError &e) {
        std::fprintf(stderr, "esca-capctl: %s\n", e.what());
        return 1;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "esca-capctl: %s\n", e.what());
        return 1;
    }
    return 0;
}
