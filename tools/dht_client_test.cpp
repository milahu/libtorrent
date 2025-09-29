// Minimal BitTorrent DHT client for latest libtorrent master (2025)
// Reports approximate DHT peer count every second with timestamp (logfile style)
// Supports binding to a specific interface and port via command line

#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/alert.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <iomanip>
#include <ctime>
#include <string>
#include <stdexcept>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>

// helper: resolve interface name to its first IPv4 address
std::string get_iface_ip(const std::string& ifname) {
    struct ifaddrs* ifaddr;
    if (getifaddrs(&ifaddr) == -1) {
        throw std::runtime_error("getifaddrs failed");
    }

    std::string ip;
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifname != ifa->ifa_name) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            char buf[INET_ADDRSTRLEN];
            auto* sa = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
            if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf))) {
                ip = buf;
                break;
            }
        }
    }
    freeifaddrs(ifaddr);

    if (ip.empty()) {
        throw std::runtime_error("no IPv4 address found for interface " + ifname);
    }
    return ip;
}

void print_help(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n\n"
              << "Options:\n"
              << "  --help            Show this help text and exit\n"
              << "  --bind <iface>    Bind to the given network interface (e.g. eth0)\n"
              << "  --port <port>     Set listening port for TCP/UDP/DHT (default: 6881)\n";
}

int main(int argc, char* argv[]) {
    std::string bind_iface;
    int listen_port = 6881; // default port

    // parse command line
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help") {
            print_help(argv[0]);
            return 0;
        } else if (arg == "--bind" && i + 1 < argc) {
            bind_iface = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            listen_port = std::stoi(argv[++i]);
        }
    }

    libtorrent::settings_pack pack;

    // Disable unneeded features
    pack.set_bool(libtorrent::settings_pack::enable_lsd, false);
    pack.set_bool(libtorrent::settings_pack::enable_upnp, false);
    pack.set_bool(libtorrent::settings_pack::enable_natpmp, false);
    pack.set_bool(libtorrent::settings_pack::enable_dht, true);

    // Restrict to specific interface and port if requested
    if (!bind_iface.empty()) {
        std::string ip = get_iface_ip(bind_iface);
        pack.set_str(libtorrent::settings_pack::listen_interfaces,
                     ip + ":" + std::to_string(listen_port));
        std::cout << "Binding DHT client to interface " << bind_iface
                  << " (IP " << ip << ") on port " << listen_port << std::endl;
    } else {
        // otherwise bind to all interfaces
        pack.set_str(libtorrent::settings_pack::listen_interfaces,
                     "0.0.0.0:" + std::to_string(listen_port));
        std::cout << "Binding DHT client to all interfaces on port " << listen_port << std::endl;
    }

    libtorrent::session ses{pack};

    std::atomic<bool> running{true};

    // Alert handling thread
    std::thread alert_thread([&]{
        while (running.load()) {
            std::vector<libtorrent::alert*> alerts;
            ses.pop_alerts(&alerts);
            for (auto a : alerts) {
                if (auto* ls = dynamic_cast<libtorrent::listen_succeeded_alert*>(a)) {
                    std::cout << "Listening on: " << ls->address.to_string() << ":" << ls->port << std::endl;
                } else if (auto* lf = dynamic_cast<libtorrent::listen_failed_alert*>(a)) {
                    std::cerr << "Listen failed: " << lf->message() << std::endl;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });

    std::cout << "DHT client running. DHT node count:" << std::endl;

    while (true) {
        ses.post_dht_stats(); // request DHT stats

        std::vector<libtorrent::alert*> alerts;
        ses.pop_alerts(&alerts);

        int dht_nodes = 0;
        for (auto a : alerts) {
            if (auto* st = dynamic_cast<libtorrent::dht_stats_alert*>(a)) {
                for (auto const& b : st->routing_table) {
                    dht_nodes += b.num_nodes;
                }
            }
        }

        auto now = std::chrono::system_clock::now();
        std::time_t now_c = std::chrono::system_clock::to_time_t(now);
        std::tm tm = *std::localtime(&now_c);

        std::cout << std::put_time(&tm, "%F %T") << " DHT nodes: " << dht_nodes << std::endl;

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    running = false;
    alert_thread.join();

    return 0;
}
