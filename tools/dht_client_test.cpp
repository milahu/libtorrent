// Minimal BitTorrent DHT client for latest libtorrent master (2025)
// Reports approximate DHT peer count every second with timestamp (logfile style)
// Supports binding to a specific interface and port via command line

#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/alert.hpp>
#include <libtorrent/hex.hpp>
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

// https://linuxtracker.org/ # Top Torrents
// https://linuxtracker.org/index.php?page=torrent-details&id=a9ae5333b345d9c66ed09e2f72eef639dec5ad1d
// Linux mint 22 Cinnamon 64bit ISO
#define DEFAULT_TEST_BTIH "a9ae5333b345d9c66ed09e2f72eef639dec5ad1d"

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
    std::cout
        << "Usage: " << prog << " [options]\n\n"
        << "Options:\n"
        << "  --help             Show this help text and exit\n"
        << "  --bind <iface>     Bind to the given network interface (e.g. eth0)\n"
        << "  --port <port>      Set listening port for TCP/UDP/DHT (default: 6881)\n"
        << "  --btih <btih>      Query the DHT for this torrent (default: " << DEFAULT_TEST_BTIH << ")\n"
        << "  --sleep-print <N>  Print number of DHT peers every N seconds (default: 1)\n"
        << "  --sleep-query <N>  Re-send the DHT query every N seconds (default: 30)\n"
        << "  --stop-nodes <N>   Stop when connected to at least N DHT nodes (default: 0)\n"
        << "  --stop-time <N>    Stop after N seconds (default: 0)\n"
    ;
}

int main(int argc, char* argv[]) {
    std::string bind_iface;
    int listen_port = 6881; // default port
    int sleep_print = 1;
    int sleep_query = 30;
    int stop_nodes = 0;
    int stop_time = 0;
    std::string test_btih = DEFAULT_TEST_BTIH;

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
        } else if (arg == "--btih" && i + 1 < argc) {
            test_btih = argv[++i];
        } else if (arg == "--sleep-print" && i + 1 < argc) {
            sleep_print = std::stoi(argv[++i]);
            if (sleep_print <= 0) {
                std::cout << "error: sleep-print cannot be zero or less: " << sleep_print << std::endl;
                return 1;
            }
        } else if (arg == "--sleep-query" && i + 1 < argc) {
            sleep_query = std::stoi(argv[++i]);
            if (sleep_query <= 0) {
                std::cout << "error: sleep-query cannot be zero or less: " << sleep_query << std::endl;
                return 1;
            }
        } else if (arg == "--stop-nodes" && i + 1 < argc) {
            stop_nodes = std::stoi(argv[++i]);
            if (stop_nodes < 0) {
                std::cout << "error: stop-nodes cannot be less than zero: " << stop_nodes << std::endl;
                return 1;
            }
        } else if (arg == "--stop-time" && i + 1 < argc) {
            stop_time = std::stoi(argv[++i]);
            if (stop_time < 0) {
                std::cout << "error: stop-time cannot be less than zero: " << stop_time << std::endl;
                return 1;
            }
        } else {
            std::cout << "error: unrecognized argument: " << arg << std::endl;
            return 1;
        }
    }

    libtorrent::settings_pack pack;

    // Disable unneeded features
    pack.set_bool(libtorrent::settings_pack::enable_lsd, false);
    pack.set_bool(libtorrent::settings_pack::enable_upnp, false);
    pack.set_bool(libtorrent::settings_pack::enable_natpmp, false);
    pack.set_bool(libtorrent::settings_pack::enable_dht, true);

    // enable alerts
    pack.set_int(libtorrent::settings_pack::alert_mask,
        // libtorrent::alert_category::all |
        libtorrent::alert_category::dht |
        libtorrent::alert_category::status |
        libtorrent::alert_category::error |
        libtorrent::alert_category::stats |
        libtorrent::alert_category::session_log |
        libtorrent::alert_category::dht_log |
        libtorrent::alert_category::port_mapping_log
    );

    // Add DHT routers
    // TODO expose CLI option
    pack.set_str(libtorrent::settings_pack::dht_bootstrap_nodes,
        "router.bittorrent.com:6881,"
        "router.utorrent.com:6881,"
        "router.bitcomet.com:6881,"
        "dht.transmissionbt.com:6881"
    );

    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&now_c);

    // Restrict to specific interface and port if requested
    if (!bind_iface.empty()) {
        // no! bind_iface can be a device name (lo, eth0, ...) or an ip address (192.168.178.20, ...)
        // let libtorrent do the resolving
        // because a device name can have multiple ip addresses
        // std::string ip = get_iface_ip(bind_iface);
        pack.set_str(libtorrent::settings_pack::listen_interfaces,
                     // ip + ":" + std::to_string(listen_port));
                     bind_iface + ":" + std::to_string(listen_port));
        std::cout << std::put_time(&tm, "%F %T") << " Binding DHT client to interface " << bind_iface
                  // << " (IP " << ip << ") on port " << listen_port << std::endl;
                  << " on port " << listen_port << std::endl;
    } else {
        // otherwise bind to all interfaces
        pack.set_str(libtorrent::settings_pack::listen_interfaces,
                     "0.0.0.0:" + std::to_string(listen_port));
        std::cout << std::put_time(&tm, "%F %T") << " Binding DHT client to all interfaces on port " << listen_port << std::endl;
    }

    libtorrent::session ses{pack};

    libtorrent::sha1_hash test_hash;
    libtorrent::aux::from_hex(test_btih, test_hash.data());

    // force DHT activity by sending queries
    std::cout << std::put_time(&tm, "%F %T") << " Sending DHT query for btih " << test_btih << " every " << sleep_query << " seconds" << std::endl;
    ses.dht_get_peers(test_hash);
    auto last_dht_query = std::chrono::steady_clock::now();

    std::cout << std::put_time(&tm, "%F %T")
        << " DHT client running. Printing DHT node count every "
        << sleep_print << " seconds" << std::endl;

    static auto start_time = std::chrono::steady_clock::now();

    while (true) {

        auto now = std::chrono::system_clock::now();
        std::time_t now_c = std::chrono::system_clock::to_time_t(now);
        std::tm tm = *std::localtime(&now_c);

        ses.post_dht_stats(); // request DHT stats

        std::vector<libtorrent::alert*> alerts;
        ses.pop_alerts(&alerts);

        int dht_nodes = 0;
        for (auto a : alerts) {
            switch (a->type()) {
                case libtorrent::dht_stats_alert::alert_type: {
                    auto* st = libtorrent::alert_cast<libtorrent::dht_stats_alert>(a);
                    for (auto const& bucket : st->routing_table) {
                        dht_nodes += bucket.num_nodes;
                    }
                    break;
                }
                case libtorrent::listen_failed_alert::alert_type: {
                    auto* lf = libtorrent::alert_cast<libtorrent::listen_failed_alert>(a);
                    std::cerr << std::put_time(&tm, "%F %T") << " Failed to bind to "
                            << lf->address.to_string() << ":" << lf->port
                            << " - " << lf->message() << "\n";
                    return 1;
                    break;
                }
                case libtorrent::listen_succeeded_alert::alert_type: {
                    auto* ls = libtorrent::alert_cast<libtorrent::listen_succeeded_alert>(a);
                    // TODO why is this printed two times? for TCP and UDP?
                    std::cout << std::put_time(&tm, "%F %T") << " Listening succeeded on "
                            << ls->address.to_string() << ":" << ls->port << "\n";
                    break;
                }
                case libtorrent::log_alert::alert_type: {
                    auto* log = libtorrent::alert_cast<libtorrent::log_alert>(a);
                    if (log) {
                        if (log->category() & libtorrent::alert_category::session_log) {
                            std::cout << std::put_time(&tm, "%F %T") << " [session] " << log->message() << std::endl;
                        }
                        else if (log->category() & libtorrent::alert_category::dht_log) {
                            std::cout << std::put_time(&tm, "%F %T") << " [dht] " << log->message() << std::endl;
                        }
                        else if (log->category() & libtorrent::alert_category::port_mapping_log) {
                            std::cout << std::put_time(&tm, "%F %T") << " [portmap] " << log->message() << std::endl;
                        }
                        else {
                            std::cout << std::put_time(&tm, "%F %T") << " [other] " << log->message() << std::endl;
                        }
                    }
                    break;
                }
            }
        }

        // std::cout << std::put_time(&tm, "%F %T") << " DHT nodes: " << dht_nodes << std::endl;

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - start_time).count();

        std::cout << std::put_time(&tm, "%F %T")
                << " connected to " << dht_nodes
                << " DHT nodes after " << elapsed << " seconds"
                << std::endl;

        if (stop_time > 0 && elapsed >= stop_time) {
            // return 1 if no DHT nodes are connected
            int return_code = (dht_nodes == 0) ? 1 : 0;
            std::cout << std::put_time(&tm, "%F %T")
                    << " Stopping after " << stop_time << " seconds"
                    << " with " << dht_nodes << " DHT nodes -> return " << return_code
                    << std::endl;
            return return_code;
        }

        if (stop_nodes > 0 && dht_nodes >= stop_nodes) {
            std::cout << std::put_time(&tm, "%F %T")
                    << " Stopping with " << stop_nodes << " or more DHT nodes"
                    << std::endl;
            return 0;
        }

        // re-send DHT query every N seconds
        auto now_steady = std::chrono::steady_clock::now();
        if (now_steady - last_dht_query >= std::chrono::seconds(sleep_query)) {
            ses.dht_get_peers(test_hash);
            last_dht_query = now_steady;
        }

        std::this_thread::sleep_for(std::chrono::seconds(sleep_print));
    }

    return 0;
}
