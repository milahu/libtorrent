/*

Copyright (c) 2007-2021, Arvid Norberg
Copyright (c) 2015, Thomas Yuan
Copyright (c) 2016-2018, 2020, Alden Torres
Copyright (c) 2016, Andrei Kurushin
Copyright (c) 2016, Steven Siloti
Copyright (c) 2022, Andrei Borzenkov
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include "libtorrent/config.hpp"
#include "libtorrent/udp_socket.hpp"
#include "libtorrent/socket_io.hpp"
#include "libtorrent/settings_pack.hpp"
#include "libtorrent/error.hpp"
#include "libtorrent/time.hpp"
#include "libtorrent/debug.hpp"
#include "libtorrent/deadline_timer.hpp"
#include "libtorrent/aux_/numeric_cast.hpp"
#include "libtorrent/aux_/ip_helpers.hpp" // for is_v4
#include "libtorrent/aux_/alert_manager.hpp"
#include "libtorrent/socks5_stream.hpp" // for socks_error
#include "libtorrent/aux_/keepalive.hpp"
#include "libtorrent/aux_/resolver_interface.hpp"

#include <cstdlib>
#include <functional>

#include <iostream>

#include "libtorrent/aux_/disable_warnings_push.hpp"
#include <boost/asio/ip/v6_only.hpp>
#include "libtorrent/aux_/disable_warnings_pop.hpp"

#include "libtorrent/aux_/ip_helpers.hpp" // libtorrent::aux::is_global

#ifdef _WIN32
// for SIO_KEEPALIVE_VALS
#include <mstcpip.h>
#endif



// aux::bind_to_device
#include <boost/asio.hpp>
#include <string>
#include <system_error>
#include <cstring>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>



// aux::bind_to_device
#include <boost/asio.hpp>
#include <string>
#include <system_error>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>



#include <boost/system/error_code.hpp>
#include <iostream>



namespace libtorrent {



#if false
// aux::bind_to_device
namespace aux {
inline void bind_to_device(
	boost::asio::ip::udp::socket& sock,
	const std::string& device,
	boost::system::error_code& ec
)
{
    int fd = sock.native_handle();
    // SO_BINDTODEVICE expects a null-terminated C string
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE
                   , device.c_str(), device.size() + 1) != 0)
    {
        int err = errno;
        ec = boost::system::error_code(err, boost::system::generic_category());
        return;
    }
    ec.clear();
}
} // namespace aux
#elif false
namespace aux {

// aux::bind_to_device
inline void bind_to_device(boost::asio::ip::udp::socket& sock
                           , const std::string& device
                           , boost::system::error_code& ec)
{
#ifdef SO_BINDTODEVICE
    int fd = sock.native_handle();
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, device.c_str(), device.size() + 1) != 0)
    {
        ec = boost::system::error_code(errno, boost::system::generic_category());
        return;
    }
#endif
    ec.clear();
}

inline bool is_private(const boost::asio::ip::address& addr)
{
    if (addr.is_v4())
    {
        auto bytes = addr.to_v4().to_bytes();
        return (bytes[0] == 10) ||
               (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31) ||
               (bytes[0] == 192 && bytes[1] == 168);
    }
    // For IPv6, we can check for unique-local addresses fc00::/7
    if (addr.is_v6())
    {
        auto bytes = addr.to_v6().to_bytes();
        return (bytes[0] & 0xfe) == 0xfc;
    }
    return false;
}

inline void udp_bind(boost::asio::ip::udp::socket& sock
                     , const boost::asio::ip::udp::endpoint& ep
                     , const std::string& device
					 , error_code& ec
)
{
    // boost::system::error_code ec;

    // Only bind to device if loopback or private address
    if (ep.address().is_loopback() || is_private(ep.address()))
    {
        bind_to_device(sock, device, ec);
        if (ec)
        {
            // fallback: ignore bind-to-device, just bind normally
            sock.bind(ep, ec);
        }
        else
        {
            sock.bind(ep, ec);
        }
    }
    else
    {
        // public IP: do not bind to device, may fail otherwise
        sock.bind(ep, ec);
    }

    if (ec)
    {
        throw boost::system::system_error(ec);
    }
}

} // namespace aux
#elif true
namespace aux {
void bind_to_device(boost::asio::ip::udp::socket& sock, const std::string& device)
{
#ifdef __linux__
    if (!device.empty()) {
        if (setsockopt(sock.native_handle(), SOL_SOCKET, SO_BINDTODEVICE,
                       device.c_str(), device.size()) != 0)
        {
            perror("SO_BINDTODEVICE failed");
        }
    }
#endif
}
inline bool is_private(const boost::asio::ip::address& addr)
{
    if (addr.is_v4())
    {
        auto bytes = addr.to_v4().to_bytes();
        return (bytes[0] == 10) ||
               (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31) ||
               (bytes[0] == 192 && bytes[1] == 168);
    }
    // For IPv6, we can check for unique-local addresses fc00::/7
    if (addr.is_v6())
    {
        auto bytes = addr.to_v6().to_bytes();
        return (bytes[0] & 0xfe) == 0xfc;
    }
    return false;
}
void bind_udp_socket(boost::asio::ip::udp::socket& sock
                    , const boost::asio::ip::udp::endpoint& ep
                    , const std::string& iface_device
                    , boost::system::error_code& ec)
{
    auto ip = ep.address();
    boost::asio::ip::udp::endpoint bind_ep;

    // Local/private IP → bind directly, maybe bind to device
    if (ip.is_loopback() || is_private(ip)) {
        bind_ep = ep;
        bind_to_device(sock, iface_device);
    }
    // Public/global IP → bind to wildcard, let OS pick interface
    else {
		// FIXME error: operands to '?:' have different types 'boost::asio::ip::address_v4' and 'boost::asio::ip::address_v6'
        // bind_ep = boost::asio::ip::udp::endpoint(ip.is_v4()
        //                                          ? boost::asio::ip::address_v4::any()
        //                                          : boost::asio::ip::address_v6::any()
        //                                          , ep.port());
        bind_ep = boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4::any(), ep.port());
        // Do NOT bind SO_BINDTODEVICE for public IP unless root
    }

    sock.open(bind_ep.protocol(), ec);
    if (ec) return;

    sock.bind(bind_ep, ec);
    if (ec) return;

    std::cout << "[udp_socket::bind] bound "
              << sock.local_endpoint().address().to_string()
              << ":" << sock.local_endpoint().port()
              << " device=" << iface_device << std::endl;
}
} // namespace aux
#endif



using namespace std::placeholders;

// used to build SOCKS messages in
std::size_t const tmp_buffer_size = 270;

// used for SOCKS5 UDP wrapper header
std::size_t const max_header_size = 255;

static std::string hex_escape(span<char const> data, std::size_t max_len = 32)
{
    std::string out;
    std::size_t n = std::min<std::size_t>(max_len, static_cast<std::size_t>(data.size()));
    out.reserve(n * 4); // enough room for \xNN

    for (std::size_t i = 0; i < n; ++i)
    {
        unsigned char c = static_cast<unsigned char>(data[i]);
        if (c >= 0x20 && c <= 0x7e && c != '\\')  // printable ASCII
        {
            out += static_cast<char>(c);
        }
        else if (c == '\\')
        {
            out += "\\\\";
        }
        else
        {
            char buf[5];
            std::snprintf(buf, sizeof(buf), "\\x%02x", c);
            out += buf;
        }
    }
    return out;
}

// this class hold the state of the SOCKS5 connection to maintain the UDP
// ASSOCIATE tunnel. It's instantiated on the heap for two reasons:
//
// 1. since its asynchronous functions may refer to it after the udp_socket has
//    been destructed, it needs to be held by a shared_ptr
// 2. since using a socks proxy is assumed to be a less common case, it makes
//    the common case cheaper by not allocating this space unconditionally
struct socks5 : std::enable_shared_from_this<socks5>
{
	explicit socks5(io_context& ios, aux::listen_socket_handle ls
		, aux::alert_manager& alerts, aux::resolver_interface& res, bool const send_local_ep)
		: m_socks5_sock(ios)
		, m_resolver(res)
		, m_timer(ios)
		, m_retry_timer(ios)
		, m_alerts(alerts)
		, m_listen_socket(std::move(ls))
		, m_send_local_ep(send_local_ep)
	{}

	void start(aux::proxy_settings const& ps);
	void close();

	bool active() const { return m_active; }
	udp::endpoint target() const { return m_udp_proxy_addr; }

private:

	std::shared_ptr<socks5> self() { return shared_from_this(); }

	void on_name_lookup(error_code const& e, std::vector<address> const& ips);
	void on_connect_timeout(error_code const& e);
	void on_connected(error_code const& e);
	void handshake1(error_code const& e);
	void handshake2(error_code const& e);
	void handshake3(error_code const& e);
	void handshake4(error_code const& e);
	void socks_forward_udp();
	void connect1(error_code const& e);
	void connect2(error_code const& e);
	void read_bindaddr(error_code const& e);
	void read_domainname(error_code const& e);
	void hung_up(error_code const& e);
	void on_retry_socks_connect(error_code const& e);

	void retry_connection();

	tcp::socket m_socks5_sock;
	aux::resolver_interface& m_resolver;
	deadline_timer m_timer;
	deadline_timer m_retry_timer;
	aux::alert_manager& m_alerts;
	aux::listen_socket_handle m_listen_socket;
	std::array<char, tmp_buffer_size> m_tmp_buf;

	aux::proxy_settings m_proxy_settings;

	// this is the endpoint the proxy server lives at.
	// when performing a UDP associate, we get another
	// endpoint (presumably on the same IP) where we're
	// supposed to send UDP packets.
	tcp::endpoint m_proxy_addr;

	// this is where UDP packets that are to be forwarded
	// are sent. The result from UDP ASSOCIATE is stored
	// in here.
	udp::endpoint m_udp_proxy_addr;

	// count failures to increase the retry timer
	int m_failures = 0;

	// include our local IP and listen port in the UDP associate command
	// Doing so may be risky in case we're talking to the proxy via NAT, and we
	// don't actually know our IP from the proxy's point of view
	bool m_send_local_ep = false;

	// set to true when we've been asked to shut down
	bool m_abort = false;

	// set to true once the tunnel is established
	bool m_active = false;
};

#ifdef TORRENT_HAS_DONT_FRAGMENT
struct set_dont_frag
{
	set_dont_frag(udp::socket& sock, bool const df)
		: m_socket(sock)
		, m_df(df)
	{
		if (!m_df) return;
		error_code ignore_errors;
		m_socket.set_option(libtorrent::dont_fragment(true), ignore_errors);
		TORRENT_ASSERT_VAL(!ignore_errors, ignore_errors.message());
	}

	~set_dont_frag()
	{
		if (!m_df) return;
		error_code ignore_errors;
		m_socket.set_option(libtorrent::dont_fragment(false), ignore_errors);
		TORRENT_ASSERT_VAL(!ignore_errors, ignore_errors.message());
	}

private:
	udp::socket& m_socket;
	bool const m_df;
};
#else
struct set_dont_frag
{ set_dont_frag(udp::socket&, int) {} };
#endif

udp_socket::udp_socket(io_context& ios, aux::listen_socket_handle ls)
	: m_socket(ios)
	, m_ioc(ios)
	, m_buf(new receive_buffer())
	, m_listen_socket(std::move(ls))
	, m_bind_port(0)
	, m_abort(true)
{}

int udp_socket::read(span<packet> pkts, error_code& ec)
{
	auto const num = int(pkts.size());
	int ret = 0;
	packet p;

	while (ret < num)
	{
		int const len = int(m_socket.receive_from(boost::asio::buffer(*m_buf)
			, p.from, 0, ec));

		if (!ec)
		{
			std::cout << "[udp_socket::read] got " << len << " bytes from " << p.from << "\n";
			std::cout << "[udp_socket::read] payload (first 32 bytes): "
				<< hex_escape({m_buf->data(), static_cast<ptrdiff_t>(std::min<std::size_t>(32, static_cast<std::size_t>(len)))})
				<< "\n";
		}
		else
		{
			std::cout << "[udp_socket::read] receive_from error: " << ec.message() << "\n";
		}

		if (ec == error::would_block
			|| ec == error::try_again
			|| ec == error::operation_aborted
			|| ec == error::bad_descriptor)
		{
			return ret;
		}

		if (ec == error::interrupted)
		{
			continue;
		}

		if (ec)
		{
			// SOCKS5 cannot wrap ICMP errors. And even if it could, they certainly
			// would not arrive as unwrapped (regular) ICMP errors. If we're using
			// a proxy we must ignore these
			if (m_proxy_settings.type != settings_pack::none) continue;

			p.error = ec;
			p.data = span<char>();
		}
		else
		{
			p.data = {m_buf->data(), len};

			// support packets coming from the SOCKS5 proxy
			if (active_socks5())
			{
				// if the source IP doesn't match the proxy's, ignore the packet
				if (p.from != m_socks5_connection->target()) continue;
				// if we failed to unwrap, silently ignore the packet
				if (!unwrap(p)) continue;
			}
			else
			{
				// if we don't proxy trackers or peers, we may be receiving unwrapped
				// packets and we must let them through.
				bool const proxy_only
					= m_proxy_settings.proxy_peer_connections
					&& m_proxy_settings.proxy_tracker_connections
					;

				// if we proxy everything, block all packets that aren't coming from
				// the proxy
				if (m_proxy_settings.type != settings_pack::none && proxy_only) continue;
			}
		}

		pkts[ret] = p;
		++ret;

		// we only have a single buffer for now, so we can only return a
		// single packet. In the future though, we could attempt to drain
		// the socket here, or maybe even use recvmmsg()
		break;
	}

	return ret;
}

bool udp_socket::active_socks5() const
{
	return (m_socks5_connection && m_socks5_connection->active());
}

void udp_socket::send_hostname(char const* hostname, int const port
	, span<char const> p, error_code& ec, udp_send_flags_t const flags)
{
	TORRENT_ASSERT(is_single_thread());

	// if the sockets are closed, the udp_socket is closing too
	if (!is_open())
	{
		ec = error_code(boost::system::errc::bad_file_descriptor, generic_category());
		return;
	}

	bool const use_proxy
		= ((flags & peer_connection) && m_proxy_settings.proxy_peer_connections)
		|| ((flags & tracker_connection) && m_proxy_settings.proxy_tracker_connections)
		|| !(flags & (tracker_connection | peer_connection))
		;

	if (use_proxy && m_proxy_settings.type != settings_pack::none)
	{
		if (active_socks5())
		{
			// send udp packets through SOCKS5 server
			wrap(hostname, port, p, ec, flags);
		}
		else
		{
			ec = error_code(boost::system::errc::permission_denied, generic_category());
		}
		return;
	}

	// the overload that takes a hostname is really only supported when we're
	// using a proxy
	address const target = make_address(hostname, ec);
	if (!ec) send(udp::endpoint(target, std::uint16_t(port)), p, ec, flags);
}

void udp_socket::send(udp::endpoint const& ep, span<char const> p
	, error_code& ec, udp_send_flags_t const flags)
{
	TORRENT_ASSERT(is_single_thread());

	// if the sockets are closed, the udp_socket is closing too
	if (!is_open())
	{
		ec = error_code(boost::system::errc::bad_file_descriptor, generic_category());
		return;
	}

	bool const use_proxy
		= ((flags & peer_connection) && m_proxy_settings.proxy_peer_connections)
		|| ((flags & tracker_connection) && m_proxy_settings.proxy_tracker_connections)
		|| !(flags & (tracker_connection | peer_connection))
		;

	if (use_proxy && m_proxy_settings.type != settings_pack::none)
	{
		if (active_socks5())
		{
			// send udp packets through SOCKS5 server
			wrap(ep, p, ec, flags);
		}
		else
		{
			ec = error_code(boost::system::errc::permission_denied, generic_category());
		}
		return;
	}

	// set the DF flag for the socket and clear it again in the destructor
	set_dont_frag df(m_socket, (flags & dont_fragment)
		&& aux::is_v4(ep));

	// m_socket.send_to(boost::asio::buffer(p.data(), static_cast<std::size_t>(p.size())), ep, 0, ec);
	int bytes_sent = 0;
	std::cout << "[udp_socket::send] sending " << p.size() << " bytes to " << ep << "\n";
	std::cout << "[udp_socket::send] payload (first 32 bytes): "
		<< hex_escape({p.data(), static_cast<ptrdiff_t>(std::min<std::size_t>(32, static_cast<std::size_t>(p.size())))})
		<< "\n";
	bytes_sent = m_socket.send_to(boost::asio::buffer(p.data(), p.size()), ep, 0, ec);
	if (ec)
	{
		std::cout << "[udp_socket::send] send_to failed: " << ec.message() << "\n";
	}
	else
	{
		std::cout << "[udp_socket::send] send_to succeeded: " << bytes_sent << " bytes\n";
	}
	if (ec)
	{
		using boost::asio::ip::udp;
		// Operation not permitted / EPERM on send: try fallback using an ephemeral wildcard socket
		// This is a userspace-only workaround for hosts that have public IPs bound to lo.
		#if defined(EPERM)
		if (ec.value() == EPERM)
		#else
		if (false)
		#endif
		{
			std::cout << "[udp_socket::send] send_to failed with EPERM, trying ephemeral wildcard socket fallback\n";

			// create a temporary socket on the same io_context as m_socket
			boost::system::error_code ec2;
			boost::asio::io_context& ios = static_cast<boost::asio::io_context&>(
				m_socket.get_executor().context()); // get the io_context from the socket's executor

			// choose protocol based on destination endpoint
			udp::socket fallback_sock(ios);
			fallback_sock.open(m_socket.local_endpoint(ec2).protocol(), ec2);
			if (!ec2)
			{
				// bind to unspecified address (0.0.0.0 or ::) so kernel can select the correct outgoing interface
				if (aux::is_v4(ep))
				{
					fallback_sock.bind(udp::endpoint(udp::v4(), 0), ec2);
				}
				else
				{
					fallback_sock.bind(udp::endpoint(udp::v6(), 0), ec2);
				}
			}

			if (!ec2)
			{
				std::size_t n = fallback_sock.send_to(boost::asio::buffer(p.data(), static_cast<std::size_t>(p.size()))
					, ep, 0, ec2);
				if (ec2)
				{
					std::cout << "[udp_socket::send] ephemeral fallback send_to failed: " << ec2.message() << "\n";
				}
				else
				{
					std::cout << "[udp_socket::send] ephemeral fallback send_to succeeded: " << n << " bytes\n";
				}
			}
			else
			{
				std::cout << "[udp_socket::send] ephemeral fallback socket setup failed: " << ec2.message() << "\n";
			}
		}
	}
}

void udp_socket::wrap(udp::endpoint const& ep, span<char const> p
	, error_code& ec, udp_send_flags_t const flags)
{
	TORRENT_UNUSED(flags);
	using namespace libtorrent::aux;

	std::array<char, max_header_size> header;
	char* h = header.data();

	write_uint16(0, h); // reserved
	write_uint8(0, h); // fragment
	write_uint8(aux::is_v4(ep) ? 1 : 4, h); // atyp
	write_endpoint(ep, h);

	std::array<boost::asio::const_buffer, 2> iovec;
	iovec[0] = boost::asio::const_buffer(header.data(), aux::numeric_cast<std::size_t>(h - header.data()));
	iovec[1] = boost::asio::const_buffer(p.data(), static_cast<std::size_t>(p.size()));

	// set the DF flag for the socket and clear it again in the destructor
	set_dont_frag df(m_socket, (flags & dont_fragment) && aux::is_v4(ep));

	// m_socket.send_to(iovec, m_socks5_connection->target(), 0, ec);
	int bytes_sent = 0;
	std::cout << "[udp_socket::wrap 1] sending " << p.size() << " bytes to " << m_socks5_connection->target() << "\n";
	std::cout << "[udp_socket::wrap 1] payload (first 32 bytes): "
		<< hex_escape({p.data(), static_cast<ptrdiff_t>(std::min<std::size_t>(32, static_cast<std::size_t>(p.size())))})
		<< "\n";
	bytes_sent = m_socket.send_to(iovec, m_socks5_connection->target(), 0, ec);
	if (ec)
	{
		std::cout << "[udp_socket::wrap 1] send_to failed: " << ec.message() << "\n";
	}
	else
	{
		std::cout << "[udp_socket::wrap 1] send_to succeeded: " << bytes_sent << " bytes\n";
	}
}

void udp_socket::wrap(char const* hostname, int const port, span<char const> p
	, error_code& ec, udp_send_flags_t const flags)
{
	using namespace libtorrent::aux;

	std::array<char, max_header_size> header;
	char* h = header.data();

	write_uint16(0, h); // reserved
	write_uint8(0, h); // fragment
	write_uint8(3, h); // atyp
	std::size_t const hostlen = std::min(std::strlen(hostname), max_header_size - 7);
	write_uint8(hostlen, h); // hostname len
	std::memcpy(h, hostname, hostlen);
	h += hostlen;
	write_uint16(port, h);

	std::array<boost::asio::const_buffer, 2> iovec;
	iovec[0] = boost::asio::const_buffer(header.data(), aux::numeric_cast<std::size_t>(h - header.data()));
	iovec[1] = boost::asio::const_buffer(p.data(), static_cast<std::size_t>(p.size()));

	// set the DF flag for the socket and clear it again in the destructor
	set_dont_frag df(m_socket, (flags & dont_fragment)
		&& aux::is_v4(m_socket.local_endpoint(ec)));

	// m_socket.send_to(iovec, m_socks5_connection->target(), 0, ec);
	int bytes_sent = 0;
	std::cout << "[udp_socket::wrap 2] sending " << p.size() << " bytes to " << m_socks5_connection->target() << "\n";
	std::cout << "[udp_socket::wrap 2] payload (first 32 bytes): "
		<< hex_escape({p.data(), static_cast<ptrdiff_t>(std::min<std::size_t>(32, static_cast<std::size_t>(p.size())))})
		<< "\n";
	bytes_sent = m_socket.send_to(iovec, m_socks5_connection->target(), 0, ec);
	if (ec)
	{
		std::cout << "[udp_socket::wrap 2] send_to failed: " << ec.message() << "\n";
	}
	else
	{
		std::cout << "[udp_socket::wrap 2] send_to succeeded: " << bytes_sent << " bytes\n";
	}
}

// unwrap the UDP packet from the SOCKS5 header
// buf is an in-out parameter. It will be updated
// return false if the packet should be ignored. It's not a valid Socks5 UDP
// forwarded packet
bool udp_socket::unwrap(udp_socket::packet& pack)
{
	using namespace libtorrent::aux;

	// the minimum socks5 header size
	auto const size = aux::numeric_cast<int>(pack.data.size());
	if (size <= 10) return false;

	char* p = pack.data.data();
	p += 2; // reserved
	int const frag = read_uint8(p);
	// fragmentation is not supported
	if (frag != 0) return false;

	int const atyp = read_uint8(p);
	if (atyp == 1)
	{
		// IPv4
		pack.from = read_v4_endpoint<udp::endpoint>(p);
	}
	else if (atyp == 4)
	{
		// IPv6
		pack.from = read_v6_endpoint<udp::endpoint>(p);
	}
	else
	{
		std::uint8_t const len = read_uint8(p);
		if (len > pack.data.end() - p) return false;
		string_view hostname(p, len);
		p += len;

		error_code ec;
		address addr = make_address(std::string(hostname), ec);
		std::uint16_t const port = read_uint16(p);
		if (!ec)
			pack.from = udp::endpoint(addr, port);
		else
			pack.hostname = hostname;
	}

	pack.data = span<char>{p, size - (p - pack.data.data())};
	return true;
}

#if !defined BOOST_ASIO_ENABLE_CANCELIO && defined TORRENT_WINDOWS
#error BOOST_ASIO_ENABLE_CANCELIO needs to be defined when building libtorrent to enable cancel() in asio on windows
#endif

void udp_socket::close()
{
	TORRENT_ASSERT(is_single_thread());

	error_code ec;
	m_socket.close(ec);
	TORRENT_ASSERT_VAL(!ec || ec == error::bad_descriptor, ec);
	if (m_socks5_connection)
	{
		m_socks5_connection->close();
		m_socks5_connection.reset();
	}
	m_abort = true;
}

void udp_socket::open(udp const& protocol, error_code& ec)
{
	TORRENT_ASSERT(is_single_thread());

	m_abort = false;

	if (m_socket.is_open()) m_socket.close(ec);
	ec.clear();

	m_socket.open(protocol, ec);
	if (ec) return;
	if (protocol == udp::v6())
	{
		error_code err;
		m_socket.set_option(boost::asio::ip::v6_only(true), err);

#ifdef TORRENT_WINDOWS
		// enable Teredo on windows
		m_socket.set_option(v6_protection_level(PROTECTION_LEVEL_UNRESTRICTED), err);
#endif // TORRENT_WINDOWS
	}

	// this is best-effort. ignore errors
#ifdef TORRENT_WINDOWS
	error_code err;
	m_socket.set_option(exclusive_address_use(true), err);
#endif
}

#if false
// void udp_socket::bind(udp::endpoint const& ep, error_code& ec)
void udp_socket::bind(udp::endpoint const& ep, error_code& ec, std::string const& device_name)
{
	if (!m_socket.is_open()) open(ep.protocol(), ec);
	if (ec) return;
	// m_socket.bind(ep, ec);
	boost::system::error_code tmp_ec;
	auto route = boost::asio::ip::udp::endpoint(ep.address(), ep.port());
	// If this address is assigned only to 'lo' and not used for outgoing routes
	if (libtorrent::aux::is_global(ep.address()) && device_name == "lo") {
		std::cout << "[udp_socket::bind] Detected global address on lo, using wildcard bind workaround\n";
		boost::asio::ip::udp::endpoint any(boost::asio::ip::udp::v4(), ep.port());
		m_socket.bind(any, tmp_ec);
	} else {
		m_socket.bind(ep, tmp_ec);
	}
	if (ec) return;
	m_socket.non_blocking(true, ec);
	if (ec) return;

	error_code err;
	m_bind_port = m_socket.local_endpoint(err).port();
	if (err) m_bind_port = ep.port();
}
#elif false
void udp_socket::bind(udp::endpoint const& ep, error_code& ec)
{
    if (!m_socket.is_open()) open(ep.protocol(), ec);
    if (ec) return;

    boost::system::error_code tmp_ec;
    auto ip = ep.address();

    // Only bind to loopback / private addresses
    if (ip.is_loopback() || ip.is_private())
    {
        // Optionally bind to device if needed
        m_socket.bind(ep, tmp_ec);
    }
    else
    {
        // Bind normally to global IP without SO_BINDTODEVICE
        m_socket.bind(ep, tmp_ec);
    }

    if (tmp_ec) { ec = tmp_ec; return; }

    m_socket.non_blocking(true, ec);
    if (ec) return;

    m_bind_port = m_socket.local_endpoint(tmp_ec).port();
}
#elif false
void udp_socket::bind(udp::endpoint const& ep, error_code& ec)
{
    if (!m_socket.is_open()) open(ep.protocol(), ec);
    if (ec) return;

    boost::system::error_code tmp_ec;
	auto ip = ep.address();

    // For loopback addresses, bind directly
    if (ip.is_loopback())
    {
        m_socket.bind(ep, tmp_ec);
        if (tmp_ec) { ec = tmp_ec; return; }
    }
    // For global addresses
    else if (libtorrent::aux::is_global(ip))
    {
        // Instead of binding to the "lo" device, bind normally to the endpoint
        m_socket.bind(ep, tmp_ec);
        if (tmp_ec)
        {
            std::cerr << "[udp_socket::bind] failed to bind global IP "
                      << ip.to_string() << ":" << ep.port()
                      << ", error: " << tmp_ec.message() << "\n";

            // fallback to wildcard bind
            boost::asio::ip::udp::endpoint any(ip.is_v4() ? boost::asio::ip::udp::v4()
                                                           : boost::asio::ip::udp::v6(),
                                               ep.port());
            m_socket.bind(any, tmp_ec);
            if (tmp_ec) { ec = tmp_ec; return; }
        }
    }
    else
    {
        // Fallback: bind to endpoint normally
        m_socket.bind(ep, tmp_ec);
        if (tmp_ec) { ec = tmp_ec; return; }
    }

    // Set non-blocking
    m_socket.non_blocking(true, ec);
    if (ec) return;

    // Store local port
    error_code err;
    m_bind_port = m_socket.local_endpoint(err).port();
    if (err) m_bind_port = ep.port();

    std::cout << "[udp_socket::bind] bound "
              << m_socket.local_endpoint().address().to_string()
              << ":" << m_bind_port
              << "\n";
}
#elif false
void udp_socket::bind(udp::endpoint const& ep, error_code& ec)
{
    if (!m_socket.is_open()) open(ep.protocol(), ec);
    if (ec) return;

    boost::system::error_code tmp_ec;
    auto ip = ep.address();

    // Only bind to loopback / private addresses
	if (ip.is_loopback() || ip.is_private())
	{
		// FIXME SO_BINDTODEVICE only works on Linux. On other OSes, this code should be skipped.
		std::cout << "[udp_socket::bind] binding to device "
			<< m_socket.local_endpoint().address().to_string()
			<< ":" << m_bind_port
			<< "\n";
		// Bind to the endpoint *and* the device/interface
		aux::bind_to_device(m_socket, ep.device(), ec); // ep.device() should return the interface name
		if (ec)
		{
			// fallback: just bind without device
			m_socket.bind(ep, ec);
		}
	}
	else
	{
		std::cout << "[udp_socket::bind] binding to global IP without SO_BINDTODEVICE "
			<< m_socket.local_endpoint().address().to_string()
			<< ":" << m_bind_port
			<< "\n";
		// Bind normally, without trying to force a device
		m_socket.bind(ep, tmp_ec);
	}

    if (tmp_ec) { ec = tmp_ec; return; }

    m_socket.non_blocking(true, ec);
    if (ec) return;

    m_bind_port = m_socket.local_endpoint(tmp_ec).port();
}
#elif false
// void udp_socket::bind(const boost::asio::ip::udp::endpoint& ep)
// void udp_socket::bind(udp::endpoint const& ep, error_code& ec)
void udp_socket::bind(udp::endpoint const& ep, error_code& ec, std::string const& device_name)
{
    // aux::udp_bind(m_socket, ep, device_name, ec);
    // Only bind to device if loopback or private address
    if (ep.address().is_loopback() || aux::is_private(ep.address()))
    {
        // bind_to_device(sock, device_name, ec);
		#ifdef SO_BINDTODEVICE
		std::cout << "[udp_socket::bind] binding with setsockopt SO_BINDTODEVICE "
			<< m_socket.local_endpoint().address().to_string()
			<< ":" << m_bind_port
			<< "\n";
		int fd = m_socket.native_handle();
		if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, device_name.c_str(), device_name.size() + 1) != 0)
		{
			ec = boost::system::error_code(errno, boost::system::generic_category());
		}
		else {
			ec.clear();
		}
		#endif
		if (ec)
        {
			std::cout << "[udp_socket::bind] m_socket.bind(ep, ec) v1 "
				<< m_socket.local_endpoint().address().to_string()
				<< ":" << m_bind_port
				<< "\n";
			// FIXME this is the same branch body
            // fallback: ignore bind-to-device, just bind normally
            m_socket.bind(ep, ec);
        }
        else
        {
			std::cout << "[udp_socket::bind] m_socket.bind(ep, ec) v2 "
				<< m_socket.local_endpoint().address().to_string()
				<< ":" << m_bind_port
				<< "\n";
			// FIXME this is the same branch body
            m_socket.bind(ep, ec);
        }
    }
    else
    {
		std::cout << "[udp_socket::bind] m_socket.bind(ep, ec) v3 "
			<< m_socket.local_endpoint().address().to_string()
			<< ":" << m_bind_port
			<< "\n";
        // public IP: do not bind to device, may fail otherwise
		// FIXME this is the same branch body
        m_socket.bind(ep, ec);
    }
}
#elif false
void udp_socket::bind(udp::endpoint const& ep, error_code& ec, std::string const& device_name)
{
    if (m_socket.is_open()) {
        std::cout << "[udp_socket::bind] already open on "
                  << m_socket.local_endpoint(ec).address().to_string() << ":"
                  << m_socket.local_endpoint(ec).port()
                  << " — skipping rebind\n";
        return;
    }
	// bind_udp_socket(m_socket, ep, device_name, ec);

    std::cout << "[udp_socket::bind] binding to " << ep.address().to_string()
              << ":" << ep.port() << "\n";

    if (!m_socket.is_open()) {
        m_socket.open(ep.protocol(), ec);
        if (ec) {
            std::cerr << "[udp_socket::bind] open failed: " << ec.message() << "\n";
            return;
        }
    }

    m_socket.bind(ep, ec);
    if (ec) {
        std::cerr << "[udp_socket::bind] bind(" << ep << ") failed: " << ec.message() << "\n";
    } else {
        auto lep = m_socket.local_endpoint(ec);
        std::cout << "[udp_socket::bind] bound successfully to "
                  << lep.address().to_string() << ":" << lep.port() << "\n";
    }

}
#elif true
void udp_socket::bind(udp::endpoint const& ep, error_code& ec, std::string const& device_name)
{
    if (m_socket.is_open()) {
        boost::asio::ip::udp::endpoint lep = m_socket.local_endpoint(ec);
        if (!ec) {
            if (lep.port() != 0) {
                std::cout << "[udp_socket::bind] already bound to "
                          << lep.address().to_string() << ":" << lep.port()
                          << " — skipping rebind\n";
                return;
            } else {
                std::cout << "[udp_socket::bind] open but only on "
                          << lep.address().to_string() << ":" << lep.port()
                          << " — rebinding to " << ep.address().to_string()
                          << ":" << ep.port() << "\n";
            }
        }
    } else {
        m_socket.open(ep.protocol(), ec);
        if (ec) {
            std::cerr << "[udp_socket::bind] open failed: " << ec.message() << "\n";
            return;
        }
    }

    m_socket.bind(ep, ec);
    if (ec) {
        std::cerr << "[udp_socket::bind] bind(" << ep << ") failed: " << ec.message() << "\n";
    } else {
        boost::asio::ip::udp::endpoint lep = m_socket.local_endpoint(ec);
        std::cout << "[udp_socket::bind] bound successfully to "
                  << lep.address().to_string() << ":" << lep.port() << "\n";
    }
}
#endif

void udp_socket::set_proxy_settings(aux::proxy_settings const& ps
	, aux::alert_manager& alerts, aux::resolver_interface& resolver, bool const send_local_ep)
{
	TORRENT_ASSERT(is_single_thread());

	if (m_socks5_connection)
	{
		m_socks5_connection->close();
		m_socks5_connection.reset();
	}

	m_proxy_settings = ps;

	if (m_abort) return;

	if (ps.type == settings_pack::socks5
		|| ps.type == settings_pack::socks5_pw)
	{
		// connect to socks5 server and open up the UDP tunnel

		m_socks5_connection = std::make_shared<socks5>(m_ioc
			, m_listen_socket, alerts, resolver, send_local_ep);
		m_socks5_connection->start(ps);
	}
}

// ===================== SOCKS 5 =========================

void socks5::start(aux::proxy_settings const& ps)
{
	m_proxy_settings = ps;

	ADD_OUTSTANDING_ASYNC("socks5::on_name_lookup");
	m_proxy_addr.port(ps.port);
	m_resolver.async_resolve(ps.hostname, aux::resolver_interface::abort_on_shutdown
		, std::bind(&socks5::on_name_lookup, self(), _1, _2));
}

void socks5::on_name_lookup(error_code const& e, std::vector<address> const& ips)
{
	COMPLETE_ASYNC("socks5::on_name_lookup");

	if (m_abort) return;

	if (e == boost::asio::error::operation_aborted) return;

	if (e)
	{
		if (m_alerts.should_post<socks5_alert>())
			m_alerts.emplace_alert<socks5_alert>(m_listen_socket.get_local_endpoint()
				, operation_t::hostname_lookup, e);
		++m_failures;
		retry_connection();
		return;
	}

	// only set up a SOCKS5 tunnel for sockets with the same address family
	// as the proxy
	// this is a hack to mitigate excessive SOCKS5 tunnels, until this can get
	// fixed properly.
	auto const i = std::find_if(ips.begin(), ips.end()
		, [&](address const& a) {
			return m_listen_socket.can_route(a);
		});

	if (i == ips.end())
	{
		if (m_alerts.should_post<socks5_alert>())
			m_alerts.emplace_alert<socks5_alert>(m_listen_socket.get_local_endpoint()
				, operation_t::hostname_lookup
				, error_code(boost::system::errc::host_unreachable, generic_category()));
		++m_failures;
		retry_connection();
		return;
	}

	m_proxy_addr.address(*i);

	error_code ec;
	m_socks5_sock.open(aux::is_v4(m_proxy_addr) ? tcp::v4() : tcp::v6(), ec);
	if (ec)
	{
		if (m_alerts.should_post<socks5_alert>())
			m_alerts.emplace_alert<socks5_alert>(m_proxy_addr, operation_t::sock_open, ec);
		return;
	}

	// enable keep-alives
	m_socks5_sock.set_option(boost::asio::socket_base::keep_alive(true), ec);
	if (ec)
	{
		if (m_alerts.should_post<socks5_alert>())
			m_alerts.emplace_alert<socks5_alert>(m_proxy_addr, operation_t::sock_option, ec);
		ec.clear();
	}

#if defined _WIN32 && !defined TORRENT_BUILD_SIMULATOR
	SOCKET sock = m_socks5_sock.native_handle();
	DWORD bytes = 0;
	tcp_keepalive timeout{};
	timeout.onoff = TRUE;
	timeout.keepalivetime = 30;
	timeout.keepaliveinterval = 30;
	auto const ret = WSAIoctl(sock, SIO_KEEPALIVE_VALS, &timeout, sizeof(timeout)
		, nullptr, 0, &bytes, nullptr, nullptr);
	if (ret != 0)
	{
		if (m_alerts.should_post<socks5_alert>())
			m_alerts.emplace_alert<socks5_alert>(m_proxy_addr, operation_t::sock_option
				, error_code(WSAGetLastError(), system_category()));
	}
#else
#if defined TORRENT_HAS_KEEPALIVE_IDLE
	// set keepalive timeouts
	m_socks5_sock.set_option(aux::tcp_keepalive_idle(30), ec);
	if (ec)
	{
		if (m_alerts.should_post<socks5_alert>())
			m_alerts.emplace_alert<socks5_alert>(m_proxy_addr, operation_t::sock_option, ec);
		ec.clear();
	}
#endif
#ifdef TORRENT_HAS_KEEPALIVE_INTERVAL
	m_socks5_sock.set_option(aux::tcp_keepalive_interval(1), ec);
	if (ec)
	{
		if (m_alerts.should_post<socks5_alert>())
			m_alerts.emplace_alert<socks5_alert>(m_proxy_addr, operation_t::sock_option, ec);
		ec.clear();
	}
#endif
#endif

	tcp::endpoint const bind_ep(m_listen_socket.get_local_endpoint().address(), 0);
	m_socks5_sock.bind(bind_ep, ec);
	if (ec)
	{
		if (m_alerts.should_post<socks5_alert>())
			m_alerts.emplace_alert<socks5_alert>(m_proxy_addr, operation_t::sock_bind, ec);
		++m_failures;
		retry_connection();
		return;
	}

	// TODO: perhaps an attempt should be made to bind m_socks5_sock to the
	// device of m_listen_socket

	ADD_OUTSTANDING_ASYNC("socks5::on_connected");
	m_socks5_sock.async_connect(m_proxy_addr
		, std::bind(&socks5::on_connected, self(), _1));

	ADD_OUTSTANDING_ASYNC("socks5::on_connect_timeout");
	m_timer.expires_after(seconds(10));
	m_timer.async_wait(std::bind(&socks5::on_connect_timeout
		, self(), _1));
}

void socks5::on_connect_timeout(error_code const& e)
{
	COMPLETE_ASYNC("socks5::on_connect_timeout");

	if (e == boost::asio::error::operation_aborted) return;

	if (m_abort) return;

	if (m_alerts.should_post<socks5_alert>())
		m_alerts.emplace_alert<socks5_alert>(m_proxy_addr, operation_t::connect, errors::timed_out);

	error_code ignore;
	m_socks5_sock.close(ignore);

	++m_failures;
	retry_connection();
}

void socks5::on_connected(error_code const& e)
{
	COMPLETE_ASYNC("socks5::on_connected");

	m_timer.cancel();

	if (e == boost::asio::error::operation_aborted) return;

	if (m_abort) return;

	// we failed to connect to the proxy
	if (e)
	{
		if (m_alerts.should_post<socks5_alert>())
			m_alerts.emplace_alert<socks5_alert>(m_proxy_addr, operation_t::connect, e);
		++m_failures;
		retry_connection();
		return;
	}

	using namespace libtorrent::aux;

	// send SOCKS5 authentication methods
	char* p = m_tmp_buf.data();
	write_uint8(5, p); // SOCKS VERSION 5
	if (m_proxy_settings.username.empty()
		|| m_proxy_settings.type == settings_pack::socks5)
	{
		write_uint8(1, p); // 1 authentication method (no auth)
		write_uint8(0, p); // no authentication
	}
	else
	{
		write_uint8(2, p); // 2 authentication methods
		write_uint8(0, p); // no authentication
		write_uint8(2, p); // username/password
	}
	TORRENT_ASSERT_VAL(p - m_tmp_buf.data() < int(m_tmp_buf.size()), (p - m_tmp_buf.data()));
	ADD_OUTSTANDING_ASYNC("socks5::on_handshake1");
	boost::asio::async_write(m_socks5_sock, boost::asio::buffer(m_tmp_buf.data()
		, aux::numeric_cast<std::size_t>(p - m_tmp_buf.data()))
		, std::bind(&socks5::handshake1, self(), _1));
}

void socks5::handshake1(error_code const& e)
{
	COMPLETE_ASYNC("socks5::on_handshake1");
	if (m_abort) return;
	if (e)
	{
		if (m_alerts.should_post<socks5_alert>())
			m_alerts.emplace_alert<socks5_alert>(m_proxy_addr, operation_t::handshake, e);
		++m_failures;
		retry_connection();
		return;
	}

	ADD_OUTSTANDING_ASYNC("socks5::on_handshake2");
	boost::asio::async_read(m_socks5_sock, boost::asio::buffer(m_tmp_buf.data(), 2)
		, std::bind(&socks5::handshake2, self(), _1));
}

void socks5::handshake2(error_code const& e)
{
	COMPLETE_ASYNC("socks5::on_handshake2");
	if (m_abort) return;

	if (e)
	{
		if (m_alerts.should_post<socks5_alert>())
			m_alerts.emplace_alert<socks5_alert>(m_proxy_addr, operation_t::handshake, e);
		++m_failures;
		retry_connection();
		return;
	}

	using namespace libtorrent::aux;

	char* p = m_tmp_buf.data();
	int const version = read_uint8(p);
	int const method = read_uint8(p);

	if (version < 5)
	{
		if (m_alerts.should_post<socks5_alert>())
			m_alerts.emplace_alert<socks5_alert>(m_proxy_addr, operation_t::handshake
				, socks_error::unsupported_version);
		error_code ec;
		m_socks5_sock.close(ec);
		return;
	}

	if (method == 0)
	{
		socks_forward_udp(/*l*/);
	}
	else if (method == 2)
	{
		if (m_proxy_settings.username.empty())
		{
			if (m_alerts.should_post<socks5_alert>())
				m_alerts.emplace_alert<socks5_alert>(m_proxy_addr, operation_t::handshake
					, socks_error::username_required);
			error_code ec;
			m_socks5_sock.close(ec);
			return;
		}

		// start sub-negotiation
		p = m_tmp_buf.data();
		write_uint8(1, p);
		TORRENT_ASSERT(m_proxy_settings.username.size() < 0x100);
		write_uint8(uint8_t(m_proxy_settings.username.size()), p);
		write_string(m_proxy_settings.username, p);
		TORRENT_ASSERT(m_proxy_settings.password.size() < 0x100);
		write_uint8(uint8_t(m_proxy_settings.password.size()), p);
		write_string(m_proxy_settings.password, p);
		TORRENT_ASSERT_VAL(p - m_tmp_buf.data() < int(m_tmp_buf.size()), (p - m_tmp_buf.data()));
		ADD_OUTSTANDING_ASYNC("socks5::on_handshake3");
		boost::asio::async_write(m_socks5_sock
			, boost::asio::buffer(m_tmp_buf.data(), aux::numeric_cast<std::size_t>(p - m_tmp_buf.data()))
			, std::bind(&socks5::handshake3, self(), _1));
	}
	else
	{
		if (m_alerts.should_post<socks5_alert>())
			m_alerts.emplace_alert<socks5_alert>(m_proxy_addr, operation_t::handshake
				, socks_error::unsupported_authentication_method);

		error_code ec;
		m_socks5_sock.close(ec);
		return;
	}
}

void socks5::handshake3(error_code const& e)
{
	COMPLETE_ASYNC("socks5::on_handshake3");
	if (m_abort) return;
	if (e)
	{
		if (m_alerts.should_post<socks5_alert>())
			m_alerts.emplace_alert<socks5_alert>(m_proxy_addr, operation_t::handshake, e);
		++m_failures;
		retry_connection();
		return;
	}

	ADD_OUTSTANDING_ASYNC("socks5::on_handshake4");
	boost::asio::async_read(m_socks5_sock, boost::asio::buffer(m_tmp_buf.data(), 2)
		, std::bind(&socks5::handshake4, self(), _1));
}

void socks5::handshake4(error_code const& e)
{
	COMPLETE_ASYNC("socks5::on_handshake4");
	if (m_abort) return;
	if (e)
	{
		if (m_alerts.should_post<socks5_alert>())
			m_alerts.emplace_alert<socks5_alert>(m_proxy_addr, operation_t::handshake, e);
		++m_failures;
		retry_connection();
		return;
	}

	using namespace libtorrent::aux;

	char* p = m_tmp_buf.data();
	int const version = read_uint8(p);
	int const status = read_uint8(p);

	if (version != 1 || status != 0) return;

	socks_forward_udp(/*l*/);
}

void socks5::socks_forward_udp()
{
	using namespace libtorrent::aux;

	// send SOCKS5 UDP command
	char* p = m_tmp_buf.data();
	write_uint8(5, p); // SOCKS VERSION 5
	write_uint8(3, p); // UDP ASSOCIATE command
	write_uint8(0, p); // reserved

	if (m_send_local_ep)
	{
		auto const local_ep = m_listen_socket.get_local_endpoint();
		write_uint8(aux::is_v4(local_ep) ? 1 : 4, p); // atyp
		write_endpoint(local_ep, p);
	}
	else
	{
		write_uint8(1, p); // ATYP = IPv4
		write_uint32(0, p); // 0.0.0.0
		write_uint16(0, p); // :0
	}

	TORRENT_ASSERT_VAL(p - m_tmp_buf.data() < int(m_tmp_buf.size()), (p - m_tmp_buf.data()));
	ADD_OUTSTANDING_ASYNC("socks5::connect1");
	boost::asio::async_write(m_socks5_sock
		, boost::asio::buffer(m_tmp_buf.data(), aux::numeric_cast<std::size_t>(p - m_tmp_buf.data()))
		, std::bind(&socks5::connect1, self(), _1));
}

void socks5::connect1(error_code const& e)
{
	COMPLETE_ASYNC("socks5::connect1");
	if (m_abort) return;
	if (e)
	{
		if (m_alerts.should_post<socks5_alert>())
			m_alerts.emplace_alert<socks5_alert>(m_proxy_addr, operation_t::connect, e);
		++m_failures;
		retry_connection();
		return;
	}

	ADD_OUTSTANDING_ASYNC("socks5::connect2");
	boost::asio::async_read(m_socks5_sock, boost::asio::buffer(m_tmp_buf.data(), 5)
		, std::bind(&socks5::connect2, self(), _1));
}

void socks5::connect2(error_code const& e)
{
	COMPLETE_ASYNC("socks5::connect2");

	if (m_abort) return;
	if (e)
	{
		if (m_alerts.should_post<socks5_alert>())
			m_alerts.emplace_alert<socks5_alert>(m_proxy_addr, operation_t::handshake, e);
		++m_failures;
		retry_connection();
		return;
	}

	using namespace libtorrent::aux;

	char* p = m_tmp_buf.data();
	int const version = read_uint8(p); // VERSION
	int const status = read_uint8(p); // STATUS
	++p; // RESERVED
	int const atyp = read_uint8(p); // address type

	if (version != 5 || status != 0) return;

	if (atyp == 1)
	{
		ADD_OUTSTANDING_ASYNC("socks5::read_bindaddr");
		// save the first byte of the IP address in the buffer. The
		// read_bindaddr() callback will use it
		m_tmp_buf[0] = *p;
		boost::asio::async_read(m_socks5_sock, boost::asio::buffer(m_tmp_buf.data() + 1, 5)
			, std::bind(&socks5::read_bindaddr, self(), _1));
	}
	else if (atyp == 3)
	{
		// we need to skip DOMAINNAME to imitate Python socks client
		std::size_t const len = read_uint8(p);
		ADD_OUTSTANDING_ASYNC("socks5::read_domainname");
		// save the string length in the buffer. The read_domainname() callback
		// will use it
		m_tmp_buf[0] = char(len);
		boost::asio::async_read(m_socks5_sock, boost::asio::buffer(m_tmp_buf.data()+1, len + 2)
			, std::bind(&socks5::read_domainname, self(), _1));
	}
	else
	{
		// in this case we need to read more data from the socket
		// no IPv6 support for UDP socks5
		TORRENT_ASSERT_FAIL();
		return;
	}
}

void socks5::read_bindaddr(error_code const& e)
{
	COMPLETE_ASYNC("socks5::read_bindaddr");

	if (m_abort) return;
	if (e)
	{
		if (m_alerts.should_post<socks5_alert>())
			m_alerts.emplace_alert<socks5_alert>(m_proxy_addr, operation_t::handshake, e);
		++m_failures;
		retry_connection();
		return;
	}

	using namespace libtorrent::aux;

	char* p = m_tmp_buf.data();
	m_udp_proxy_addr.address(address_v4(read_uint32(p)));
	m_udp_proxy_addr.port(read_uint16(p));

	// we're done!
	m_active = true;
	m_failures = 0;

	ADD_OUTSTANDING_ASYNC("socks5::hung_up");
	boost::asio::async_read(m_socks5_sock, boost::asio::buffer(m_tmp_buf.data(), 10)
		, std::bind(&socks5::hung_up, self(), _1));
}

void socks5::read_domainname(error_code const& e)
{
	COMPLETE_ASYNC("socks5::read_domainname");

	if (m_abort) return;
	if (e)
	{
		if (m_alerts.should_post<socks5_alert>())
			m_alerts.emplace_alert<socks5_alert>(m_proxy_addr, operation_t::handshake, e);
		++m_failures;
		retry_connection();
		return;
	}

	using namespace libtorrent::aux;

	char* p = m_tmp_buf.data();
	int const len = read_uint8(p);
	p += len;
	m_udp_proxy_addr.address(m_proxy_addr.address());
	m_udp_proxy_addr.port(read_uint16(p));

	// we're done!
	m_active = true;
	m_failures = 0;

	ADD_OUTSTANDING_ASYNC("socks5::hung_up");
	boost::asio::async_read(m_socks5_sock, boost::asio::buffer(m_tmp_buf.data(), 10)
		, std::bind(&socks5::hung_up, self(), _1));
}

void socks5::hung_up(error_code const& e)
{
	COMPLETE_ASYNC("socks5::hung_up");
	m_active = false;

	if (e == boost::asio::error::operation_aborted || m_abort) return;

	if (e && m_alerts.should_post<socks5_alert>())
		m_alerts.emplace_alert<socks5_alert>(m_proxy_addr, operation_t::sock_read, e);

	retry_connection();
}

void socks5::retry_connection()
{
	// the socks connection was closed, re-open it in a bit
	// back off exponentially
	if (m_failures > 200) m_failures = 200;
	m_retry_timer.expires_after(seconds(std::min(120, m_failures * m_failures / 2) + 5));
	m_retry_timer.async_wait(std::bind(&socks5::on_retry_socks_connect
		, self(), _1));
}

void socks5::on_retry_socks_connect(error_code const& e)
{
	if (e || m_abort) return;
	error_code ignore;
	m_socks5_sock.close(ignore);
	start(m_proxy_settings);
}

void socks5::close()
{
	m_abort = true;
	error_code ec;
	m_socks5_sock.close(ec);
	m_timer.cancel();
	m_retry_timer.cancel();
}

constexpr udp_send_flags_t udp_socket::peer_connection;
constexpr udp_send_flags_t udp_socket::tracker_connection;
constexpr udp_send_flags_t udp_socket::dont_queue;
constexpr udp_send_flags_t udp_socket::dont_fragment;

}
