// Minimal BitTorrent DHT client for latest libtorrent master (2025)
// Reports approximate DHT peer count every second with timestamp (logfile style)
// Supports binding to a specific interface and port via command line
// Supports reading multiple BTIHs (--btih or --btih-file) and querying one per interval

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
#include <sstream>
#include <random>
#include <ctime>
#include <string>
#include <stdexcept>
#include <vector>
#include <fstream>
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
        << "  --port <port>      Set listening port for TCP/UDP/DHT (default: 0 = random)\n"
        << "  --btih <btih>      Add a BTIH (40-char hex or 'random') to the query list (can repeat)\n"
        << "  --btih-file <path> Read one or more BTIHs (40-char hex or 'random') from file\n"
        << "  --sleep-print <N>  Print number of DHT peers every N seconds (default: 1)\n"
        << "  --sleep-query <N>  Re-send the DHT query every N seconds (default: 5)\n"
        << "  --stop-nodes <N>   Stop when connected to at least N DHT nodes (default: 0)\n"
        << "  --stop-time <N>    Stop after N seconds (default: 0)\n";
}

libtorrent::sha1_hash random_sha1()
{
    std::array<unsigned char, 20> buf;
    static std::mt19937_64 rng(std::random_device{}());
    for (auto &b : buf)
        b = static_cast<unsigned char>(rng() & 0xFF);
    libtorrent::sha1_hash h;
    std::memcpy(h.data(), buf.data(), 20);
    return h;
}

std::string to_hex(const libtorrent::sha1_hash& h)
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (auto b : h)
        oss << std::setw(2) << int(static_cast<unsigned char>(b));
    return oss.str();
}

// decode 40-char hex string to 20-byte array
std::vector<unsigned char> base16decode(const std::string& hex)
{
    if (hex.size() != 40)
        throw std::invalid_argument("invalid btih length (must be 40 hex chars)");

    std::vector<unsigned char> out(20);
    for (size_t i = 0; i < 20; ++i) {
        unsigned int byte;
        std::stringstream ss;
        ss << std::hex << hex.substr(i * 2, 2);
        ss >> byte;
        if (ss.fail())
            throw std::invalid_argument("invalid hex string");
        out[i] = static_cast<unsigned char>(byte);
    }
    return out;
}

int main(int argc, char* argv[]) {
    std::string bind_iface;
    int listen_port = 0; // default port. 0 = use random port
    int sleep_print = 1;
    int sleep_query = 5;
    int stop_nodes = 0;
    int stop_time = 0;
    std::string btih_file;
    std::vector<std::string> btih_list;

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
            btih_list.push_back(argv[++i]);
        } else if (arg == "--btih-file" && i + 1 < argc) {
            btih_file = argv[++i];
        } else if (arg == "--sleep-print" && i + 1 < argc) {
            sleep_print = std::stoi(argv[++i]);
            if (sleep_print <= 0) {
                std::cout << "error: sleep-print cannot be zero or less\n";
                return 1;
            }
        } else if (arg == "--sleep-query" && i + 1 < argc) {
            sleep_query = std::stoi(argv[++i]);
            if (sleep_query <= 0) {
                std::cout << "error: sleep-query cannot be zero or less\n";
                return 1;
            }
        } else if (arg == "--stop-nodes" && i + 1 < argc) {
            stop_nodes = std::stoi(argv[++i]);
            if (stop_nodes < 0) {
                std::cout << "error: stop-nodes cannot be less than zero\n";
                return 1;
            }
        } else if (arg == "--stop-time" && i + 1 < argc) {
            stop_time = std::stoi(argv[++i]);
            if (stop_time < 0) {
                std::cout << "error: stop-time cannot be less than zero\n";
                return 1;
            }
        } else {
            std::cout << "error: unrecognized argument: " << arg << std::endl;
            return 1;
        }
    }

    // load BTIHs from file if provided
    if (!btih_file.empty()) {
        std::ifstream fin(btih_file);
        if (!fin) {
            std::cerr << "error: cannot open btih-file: " << btih_file << std::endl;
            return 1;
        }
        std::string line;
        while (std::getline(fin, line)) {
            // trim spaces
            line.erase(0, line.find_first_not_of(" \t\r\n"));
            line.erase(line.find_last_not_of(" \t\r\n") + 1);
            // skip empty and comment lines
            if (line.empty() || line[0] == '#') continue;
            btih_list.push_back(line);
        }
        fin.close();
    }

    // if no btihs provided, add default one
    if (btih_list.empty()) {
        btih_list.push_back(DEFAULT_TEST_BTIH);
        std::cout << "No BTIHs provided; using default " << DEFAULT_TEST_BTIH << std::endl;
    }

    std::cout << "Sending DHT queries for " << btih_list.size() << " infohashes" << std::endl;

    libtorrent::settings_pack pack;
    pack.set_bool(libtorrent::settings_pack::enable_lsd, false);
    pack.set_bool(libtorrent::settings_pack::enable_upnp, false);
    pack.set_bool(libtorrent::settings_pack::enable_natpmp, false);
    pack.set_bool(libtorrent::settings_pack::enable_dht, true);
    pack.set_int(libtorrent::settings_pack::alert_mask,
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

    if (!bind_iface.empty()) {
        pack.set_str(libtorrent::settings_pack::listen_interfaces,
                     bind_iface + ":" + std::to_string(listen_port));
        std::cout << std::put_time(&tm, "%F %T")
                  << " Binding DHT client to interface " << bind_iface
                  << " on port " << listen_port << std::endl;
    } else {
        pack.set_str(libtorrent::settings_pack::listen_interfaces,
                     "0.0.0.0:" + std::to_string(listen_port));
        std::cout << std::put_time(&tm, "%F %T")
                  << " Binding DHT client to all interfaces on port " << listen_port << std::endl;
    }

    libtorrent::session ses{pack};

    size_t current_btih_index = 0;
    libtorrent::sha1_hash test_hash;
    const std::string& first_btih = btih_list[current_btih_index];

    if (first_btih == "random") {
        test_hash = random_sha1();
    } else {
        auto bytes = base16decode(first_btih);
        std::memcpy(test_hash.data(), bytes.data(), 20);
    }

    std::cout << std::put_time(&tm, "%F %T")
              << " Sending DHT query every " << sleep_query << " seconds" << std::endl;
    std::cout << std::put_time(&tm, "%F %T")
              << " Sending DHT query for btih " << to_hex(test_hash) << std::endl;

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

        ses.post_dht_stats();

        // FIXME this always returns 0 DHT nodes
        // libtorrent::sha1_hash zero_target;
        // zero_target.clear();
        // ses.dht_live_nodes(zero_target);

        std::vector<libtorrent::alert*> alerts;
        ses.pop_alerts(&alerts);

        int dht_nodes = 0;
        for (auto a : alerts) {
            switch (a->type()) {
                case libtorrent::dht_stats_alert::alert_type: {
                    auto* st = libtorrent::alert_cast<libtorrent::dht_stats_alert>(a);
                    for (auto const& bucket : st->routing_table)
                        dht_nodes += bucket.num_nodes;
                    for (auto const& bucket : st->routing_table)
                    {
                        std::cout << std::put_time(&tm, "%F %T") << " DHT routing table bucket:\n";
                        std::cout << "  " << bucket.live_nodes.size() << " live nodes:\n";
                        for (auto const& n : bucket.live_nodes)
                        {
                            std::cout << "    " << n.ep.address().to_string()
                                    << ":" << n.ep.port()
                                    << " id=" << to_hex(n.id)
                                    << " rtt=" << n.rtt << "ms\n";
                        }
                        std::cout << "  " << bucket.replacement_nodes.size() << " replacement nodes:\n";
                        for (auto const& n : bucket.replacement_nodes)
                        {
                            std::cout << "    " << n.ep.address().to_string()
                                    << ":" << n.ep.port()
                                    << " id=" << to_hex(n.id)
                                    << " rtt=" << n.rtt << "ms\n";
                        }
                    }
                    break;
                }
                // FIXME this always returns 0 DHT nodes
                // case libtorrent::dht_live_nodes_alert::alert_type: {
                //     auto* ln = libtorrent::alert_cast<libtorrent::dht_live_nodes_alert>(a);
                //     auto const& nodes = ln->nodes();   // note: method call!
                //     std::cout << std::put_time(&tm, "%F %T")
                //             << " [DHT live nodes] " << nodes.size() << " nodes:" << std::endl;
                //     for (auto const& n : nodes) {
                //         auto const& id = n.first;
                //         auto const& ep = n.second;
                //         std::cout << "    " << ep.address().to_string()
                //                 << ":" << ep.port()
                //                 << " id=" << id.to_string().substr(0, 8) << "..."
                //                 << std::endl;
                //     }
                //     break;
                // }
                case libtorrent::listen_failed_alert::alert_type: {
                    auto* lf = libtorrent::alert_cast<libtorrent::listen_failed_alert>(a);
                    std::cerr << std::put_time(&tm, "%F %T") << " Failed to bind to "
                              << lf->address.to_string() << ":" << lf->port
                              << " - " << lf->message() << "\n";
                    return 1;
                }
            }
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time).count();

        std::cout << std::put_time(&tm, "%F %T")
                  << " connected to " << dht_nodes
                  << " DHT nodes after " << elapsed << " seconds" << std::endl;

        if (stop_time > 0 && elapsed >= stop_time) {
            int return_code = (dht_nodes == 0) ? 1 : 0;
            std::cout << std::put_time(&tm, "%F %T")
                      << " Stopping after " << stop_time << " seconds"
                      << " with " << dht_nodes << " DHT nodes -> return " << return_code
                      << std::endl;
            return return_code;
        }

        if (stop_nodes > 0 && dht_nodes >= stop_nodes) {
            std::cout << std::put_time(&tm, "%F %T")
                      << " Stopping with " << stop_nodes << " or more DHT nodes" << std::endl;
            return 0;
        }

        auto now_steady = std::chrono::steady_clock::now();
        if (now_steady - last_dht_query >= std::chrono::seconds(sleep_query)) {
            current_btih_index = (current_btih_index + 1) % btih_list.size();
            const std::string& hstr = btih_list[current_btih_index];
            if (hstr == "random") {
                test_hash = random_sha1();
            } else {
                try {
                    auto bytes = base16decode(hstr);
                    std::memcpy(test_hash.data(), bytes.data(), 20);
                } catch (const std::exception& e) {
                    std::cerr << "warning: invalid btih at index "
                              << current_btih_index << ": " << e.what() << std::endl;
                    test_hash = random_sha1();
                }
            }

            std::cout << std::put_time(&tm, "%F %T")
                      << " Sending DHT query for btih " << to_hex(test_hash)
                      << " (index " << current_btih_index << ")" << std::endl;

            ses.dht_get_peers(test_hash);
            last_dht_query = now_steady;
        }

        std::this_thread::sleep_for(std::chrono::seconds(sleep_print));
    }

    return 0;
}
