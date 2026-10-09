#include "stdafx.h"
#include "sys_net.h"

#include "Emu/IdManager.h"
#include "Emu/system_config.h"
#include "Emu/Cell/PPUThread.h"
#include "Utilities/Thread.h"

#include "sys_sync.h"
#include "sys_event.h"

#ifdef _WIN32
#include <winsock2.h>
#include <WS2tcpip.h>
#else
#ifdef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#ifdef __clang__
#pragma GCC diagnostic pop
#endif
#endif

#include "Emu/NP/np_handler.h"
#include "Emu/NP/np_helpers.h"
#include "Emu/Cell/timers.hpp"
#include <shared_mutex>

#include "sys_net/network_context.h"
#include "sys_net/lv2_socket.h"
#include "sys_net/lv2_socket_native.h"
#include "sys_net/lv2_socket_raw.h"
#include "sys_net/lv2_socket_p2p.h"
#include "sys_net/lv2_socket_p2ps.h"
#include "sys_net/sys_net_helpers.h"

LOG_CHANNEL(sys_net);
LOG_CHANNEL(sys_net_dump);

template <>
void fmt_class_string<sys_net_error>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto error)
		{
			switch (static_cast<s32>(error))
			{
#define SYS_NET_ERROR_CASE(x) \
	case -x: return "-" #x;   \
	case x:                   \
		return #x
				SYS_NET_ERROR_CASE(SYS_NET_ENOENT);
				SYS_NET_ERROR_CASE(SYS_NET_EINTR);
				SYS_NET_ERROR_CASE(SYS_NET_EBADF);
				SYS_NET_ERROR_CASE(SYS_NET_ENOMEM);
				SYS_NET_ERROR_CASE(SYS_NET_EACCES);
				SYS_NET_ERROR_CASE(SYS_NET_EFAULT);
				SYS_NET_ERROR_CASE(SYS_NET_EBUSY);
				SYS_NET_ERROR_CASE(SYS_NET_EINVAL);
				SYS_NET_ERROR_CASE(SYS_NET_EMFILE);
				SYS_NET_ERROR_CASE(SYS_NET_ENOSPC);
				SYS_NET_ERROR_CASE(SYS_NET_EPIPE);
				SYS_NET_ERROR_CASE(SYS_NET_EAGAIN);
				static_assert(SYS_NET_EWOULDBLOCK == SYS_NET_EAGAIN);
				SYS_NET_ERROR_CASE(SYS_NET_EINPROGRESS);
				SYS_NET_ERROR_CASE(SYS_NET_EALREADY);
				SYS_NET_ERROR_CASE(SYS_NET_EDESTADDRREQ);
				SYS_NET_ERROR_CASE(SYS_NET_EMSGSIZE);
				SYS_NET_ERROR_CASE(SYS_NET_EPROTOTYPE);
				SYS_NET_ERROR_CASE(SYS_NET_ENOPROTOOPT);
				SYS_NET_ERROR_CASE(SYS_NET_EPROTONOSUPPORT);
				SYS_NET_ERROR_CASE(SYS_NET_EOPNOTSUPP);
				SYS_NET_ERROR_CASE(SYS_NET_EPFNOSUPPORT);
				SYS_NET_ERROR_CASE(SYS_NET_EAFNOSUPPORT);
				SYS_NET_ERROR_CASE(SYS_NET_EADDRINUSE);
				SYS_NET_ERROR_CASE(SYS_NET_EADDRNOTAVAIL);
				SYS_NET_ERROR_CASE(SYS_NET_ENETDOWN);
				SYS_NET_ERROR_CASE(SYS_NET_ENETUNREACH);
				SYS_NET_ERROR_CASE(SYS_NET_ECONNABORTED);
				SYS_NET_ERROR_CASE(SYS_NET_ECONNRESET);
				SYS_NET_ERROR_CASE(SYS_NET_ENOBUFS);
				SYS_NET_ERROR_CASE(SYS_NET_EISCONN);
				SYS_NET_ERROR_CASE(SYS_NET_ENOTCONN);
				SYS_NET_ERROR_CASE(SYS_NET_ESHUTDOWN);
				SYS_NET_ERROR_CASE(SYS_NET_ETOOMANYREFS);
				SYS_NET_ERROR_CASE(SYS_NET_ETIMEDOUT);
				SYS_NET_ERROR_CASE(SYS_NET_ECONNREFUSED);
				SYS_NET_ERROR_CASE(SYS_NET_EHOSTDOWN);
				SYS_NET_ERROR_CASE(SYS_NET_EHOSTUNREACH);
#undef SYS_NET_ERROR_CASE
			}

			return unknown;
		});
}

template <>
void fmt_class_string<lv2_socket_type>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto value)
		{
			switch (value)
			{
			case SYS_NET_SOCK_STREAM: return "STREAM";
			case SYS_NET_SOCK_DGRAM: return "DGRAM";
			case SYS_NET_SOCK_RAW: return "RAW";
			case SYS_NET_SOCK_DGRAM_P2P: return "DGRAM-P2P";
			case SYS_NET_SOCK_STREAM_P2P: return "STREAM-P2P";
			}

			return unknown;
		});
}

template <>
void fmt_class_string<lv2_socket_family>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto value)
		{
			switch (value)
			{
			case SYS_NET_AF_UNSPEC: return "UNSPEC";
			case SYS_NET_AF_LOCAL: return "LOCAL";
			case SYS_NET_AF_INET: return "INET";
			case SYS_NET_AF_INET6: return "INET6";
			}

			return unknown;
		});
}

template <>
void fmt_class_string<lv2_ip_protocol>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto value)
		{
			switch (value)
			{
			case SYS_NET_IPPROTO_IP: return "IPPROTO_IP";
			case SYS_NET_IPPROTO_ICMP: return "IPPROTO_ICMP";
			case SYS_NET_IPPROTO_IGMP: return "IPPROTO_IGMP";
			case SYS_NET_IPPROTO_TCP: return "IPPROTO_TCP";
			case SYS_NET_IPPROTO_UDP: return "IPPROTO_UDP";
			case SYS_NET_IPPROTO_ICMPV6: return "IPPROTO_ICMPV6";
			}

			return unknown;
		});
}

template <>
void fmt_class_string<lv2_tcp_option>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto value)
		{
			switch (value)
			{
			case SYS_NET_TCP_NODELAY: return "TCP_NODELAY";
			case SYS_NET_TCP_MAXSEG: return "TCP_MAXSEG";
			case SYS_NET_TCP_MSS_TO_ADVERTISE: return "TCP_MSS_TO_ADVERTISE";
			}

			return unknown;
		});
}

template <>
void fmt_class_string<lv2_socket_option>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto value)
		{
			switch (value)
			{
			case SYS_NET_SO_SNDBUF: return "SO_SNDBUF";
			case SYS_NET_SO_RCVBUF: return "SO_RCVBUF";
			case SYS_NET_SO_SNDLOWAT: return "SO_SNDLOWAT";
			case SYS_NET_SO_RCVLOWAT: return "SO_RCVLOWAT";
			case SYS_NET_SO_SNDTIMEO: return "SO_SNDTIMEO";
			case SYS_NET_SO_RCVTIMEO: return "SO_RCVTIMEO";
			case SYS_NET_SO_ERROR: return "SO_ERROR";
			case SYS_NET_SO_TYPE: return "SO_TYPE";
			case SYS_NET_SO_NBIO: return "SO_NBIO";
			case SYS_NET_SO_TPPOLICY: return "SO_TPPOLICY";
			case SYS_NET_SO_REUSEADDR: return "SO_REUSEADDR";
			case SYS_NET_SO_KEEPALIVE: return "SO_KEEPALIVE";
			case SYS_NET_SO_BROADCAST: return "SO_BROADCAST";
			case SYS_NET_SO_LINGER: return "SO_LINGER";
			case SYS_NET_SO_OOBINLINE: return "SO_OOBINLINE";
			case SYS_NET_SO_REUSEPORT: return "SO_REUSEPORT";
			case SYS_NET_SO_ONESBCAST: return "SO_ONESBCAST";
			case SYS_NET_SO_USECRYPTO: return "SO_USECRYPTO";
			case SYS_NET_SO_USESIGNATURE: return "SO_USESIGNATURE";
			case SYS_NET_SOL_SOCKET: return "SOL_SOCKET";
			}

			return unknown;
		});
}

template <>
void fmt_class_string<lv2_ip_option>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto value)
		{
			switch (value)
			{
			case SYS_NET_IP_HDRINCL: return "IP_HDRINCL";
			case SYS_NET_IP_TOS: return "IP_TOS";
			case SYS_NET_IP_TTL: return "IP_TTL";
			case SYS_NET_IP_MULTICAST_IF: return "IP_MULTICAST_IF";
			case SYS_NET_IP_MULTICAST_TTL: return "IP_MULTICAST_TTL";
			case SYS_NET_IP_MULTICAST_LOOP: return "IP_MULTICAST_LOOP";
			case SYS_NET_IP_ADD_MEMBERSHIP: return "IP_ADD_MEMBERSHIP";
			case SYS_NET_IP_DROP_MEMBERSHIP: return "IP_DROP_MEMBERSHIP";
			case SYS_NET_IP_TTLCHK: return "IP_TTLCHK";
			case SYS_NET_IP_MAXTTL: return "IP_MAXTTL";
			case SYS_NET_IP_DONTFRAG: return "IP_DONTFRAG";
			}

			return unknown;
		});
}

template <>
void fmt_class_string<struct in_addr>::format(std::string& out, u64 arg)
{
	const u8* data = reinterpret_cast<const u8*>(&get_object(arg));

	fmt::append(out, "%u.%u.%u.%u", data[0], data[1], data[2], data[3]);
}

template <>
void fmt_class_string<sys_net_sockinfo_t>::format(std::string& out, u64 arg)
{
	const auto& info = get_object(arg);

	fmt::append(out, "{ s=%d, proto=%s, recv_queue_length=%d, send_queue_length=%d, local=%s:%d, remote=%s:%d, state=%d }",
		info.s, static_cast<lv2_ip_protocol>(info.proto.value()), info.recv_queue_length, info.send_queue_length,
		np::ip_to_string(std::bit_cast<u32>(info.local_adr._s_addr)), info.local_port,
		np::ip_to_string(std::bit_cast<u32>(info.remote_adr._s_addr)), info.remote_port,
		info.state);
}

lv2_socket::lv2_socket(utils::serial& ar, lv2_socket_type _type)
	: family(ar)
	, type(_type)
	, protocol(ar)
	, so_nbio(ar)
	, so_error(ar)
	, so_tcp_maxseg(ar)
#ifdef _WIN32
	, so_reuseaddr(ar)
	, so_reuseport(ar)
{
#else
{
	// Try to match structure between different platforms
	ar.pos += 8;
#endif

	[[maybe_unused]] const s32 version = GET_SERIALIZATION_VERSION(lv2_net);

	ar(so_rcvtimeo, so_sendtimeo);

	lv2_id = idm::last_id<lv2_socket>();

	ar(last_bound_addr);
}

std::function<void(void*)> lv2_socket::load(utils::serial& ar)
{
	const lv2_socket_type type{ar};

	shared_ptr<lv2_socket> sock_lv2;

	switch (type)
	{
	case SYS_NET_SOCK_STREAM:
	case SYS_NET_SOCK_DGRAM:
	{
		auto lv2_native = make_shared<lv2_socket_native>(ar, type);
		ensure(lv2_native->create_socket() >= 0);
		sock_lv2 = std::move(lv2_native);
		break;
	}
	case SYS_NET_SOCK_RAW: sock_lv2 = make_shared<lv2_socket_raw>(ar, type); break;
	case SYS_NET_SOCK_DGRAM_P2P: sock_lv2 = make_shared<lv2_socket_p2p>(ar, type); break;
	case SYS_NET_SOCK_STREAM_P2P: sock_lv2 = make_shared<lv2_socket_p2ps>(ar, type); break;
	}

	if (std::memcmp(&sock_lv2->last_bound_addr, std::array<u8, 16>{}.data(), 16))
	{
		// NOTE: It is allowed fail
		sock_lv2->bind(sock_lv2->last_bound_addr);
	}

	return [ptr = sock_lv2](void* storage) { *static_cast<atomic_ptr<lv2_socket>*>(storage) = ptr; };;
}

void lv2_socket::save(utils::serial& ar, bool save_only_this_class)
{
	USING_SERIALIZATION_VERSION(lv2_net);

	if (save_only_this_class)
	{
		ar(family, protocol, so_nbio, so_error, so_tcp_maxseg);
#ifdef _WIN32
		ar(so_reuseaddr, so_reuseport);
#else
		ar(std::array<char, 8>{});
#endif
		ar(so_rcvtimeo, so_sendtimeo);
		ar(last_bound_addr);
		return;
	}

	ar(type);

	switch (type)
	{
	case SYS_NET_SOCK_STREAM:
	case SYS_NET_SOCK_DGRAM:
	{
		static_cast<lv2_socket_native*>(this)->save(ar);
		break;
	}
	case SYS_NET_SOCK_RAW: static_cast<lv2_socket_raw*>(this)->save(ar); break;
	case SYS_NET_SOCK_DGRAM_P2P: static_cast<lv2_socket_p2p*>(this)->save(ar); break;
	case SYS_NET_SOCK_STREAM_P2P: static_cast<lv2_socket_p2ps*>(this)->save(ar); break;
	}
}

void sys_net_dump_data(std::string_view desc, const u8* data, s32 len, const void* addr)
{
	const sys_net_sockaddr_in_p2p* p2p_addr = reinterpret_cast<const sys_net_sockaddr_in_p2p*>(addr);

	if (p2p_addr)
		sys_net_dump.trace("%s(%s:%d:%d): %s", desc, np::ip_to_string(std::bit_cast<u32>(p2p_addr->sin_addr)), p2p_addr->sin_port, p2p_addr->sin_vport, fmt::buf_to_hexstring(data, len));
	else
		sys_net_dump.trace("%s: %s", desc, fmt::buf_to_hexstring(data, len));
}

error_code sys_net_bnet_accept(ppu_thread& ppu, s32 s, vm::ptr<sys_net_sockaddr> addr, vm::ptr<u32> paddrlen)
{
	ppu.state += cpu_flag::wait;

	sys_net.warning("sys_net_bnet_accept(s=%d, addr=*0x%x, paddrlen=*0x%x)", s, addr, paddrlen);

	if (addr.operator bool() != paddrlen.operator bool() || (paddrlen && *paddrlen < addr.size()))
	{
		return -SYS_NET_EINVAL;
	}

	s32 result = 0;
	sys_net_sockaddr sn_addr{};
	shared_ptr<lv2_socket> new_socket{};

	const auto sock = idm::check<lv2_socket>(s, [&, notify = lv2_obj::notify_all_t()](lv2_socket& sock)
		{
			auto [success, res, res_socket, res_addr] = sock.accept();

			if (success)
			{
				result  = res;
				sn_addr = res_addr;
				new_socket = std::move(res_socket);
				return true;
			}

			auto lock = sock.lock();

			sock.poll_queue(idm::get_unlocked<named_thread<ppu_thread>>(ppu.id), lv2_socket::poll_t::read, [&](bs_t<lv2_socket::poll_t> events) -> bool
				{
					if (events & lv2_socket::poll_t::read)
					{
						auto [success, res, res_socket, res_addr] = sock.accept(false);
						if (success)
						{
							result  = res;
							sn_addr = res_addr;
							new_socket = std::move(res_socket);
							lv2_obj::awake(&ppu);
							return success;
						}
					}

					sock.set_poll_event(lv2_socket::poll_t::read);
					return false;
				});

			lv2_obj::prepare_for_sleep(ppu);
			lv2_obj::sleep(ppu);
			return false;
		});

	if (!sock)
	{
		return -SYS_NET_EBADF;
	}

	if (!sock.ret)
	{
		while (auto state = ppu.state.fetch_sub(cpu_flag::signal))
		{
			if (is_stopped(state))
			{
				return {};
			}

			if (state & cpu_flag::signal)
			{
				break;
			}

			ppu.state.wait(state);
		}

		if (ppu.gpr[3] == static_cast<u64>(-SYS_NET_EINTR))
		{
			return -SYS_NET_EINTR;
		}

		if (result < 0)
		{
			return sys_net_error{result};
		}
	}

	if (result < 0)
	{
		return sys_net_error{result};
	}

	s32 id_ps3 = result;

	if (!id_ps3)
	{
		ensure(new_socket);
		id_ps3 = idm::import_existing<lv2_socket>(new_socket);
		if (id_ps3 == id_manager::id_traits<lv2_socket>::invalid)
		{
			return -SYS_NET_EMFILE;
		}
	}

	static_cast<void>(ppu.test_stopped());

	if (addr)
	{
		*paddrlen = sizeof(sys_net_sockaddr_in);
		*addr     = sn_addr;
	}

	// Socket ID
	return not_an_error(id_ps3);
}

error_code sys_net_bnet_bind(ppu_thread& ppu, s32 s, vm::cptr<sys_net_sockaddr> addr, u32 addrlen)
{
	ppu.state += cpu_flag::wait;

	sys_net.warning("sys_net_bnet_bind(s=%d, addr=*0x%x, addrlen=%u)", s, addr, addrlen);

	if (!addr || addrlen < addr.size())
	{
		return -SYS_NET_EINVAL;
	}

	if (!idm::check_unlocked<lv2_socket>(s))
	{
		return -SYS_NET_EBADF;
	}

	const sys_net_sockaddr sn_addr = *addr;

	// 0 presumably defaults to AF_INET(to check?)
	if (sn_addr.sa_family != SYS_NET_AF_INET && sn_addr.sa_family != SYS_NET_AF_UNSPEC)
	{
		sys_net.error("sys_net_bnet_bind: unsupported sa_family (%d)", sn_addr.sa_family);
		return -SYS_NET_EAFNOSUPPORT;
	}

	const auto sock = idm::check<lv2_socket>(s, [&, notify = lv2_obj::notify_all_t()](lv2_socket& sock) -> s32
		{
			return sock.bind(sn_addr);
		});

	if (!sock)
	{
		return -SYS_NET_EBADF;
	}

	if (sock.ret)
	{
		return sys_net_error{sock.ret};
	}

	return CELL_OK;
}

error_code sys_net_bnet_connect(ppu_thread& ppu, s32 s, vm::ptr<sys_net_sockaddr> addr, u32 addrlen)
{
	ppu.state += cpu_flag::wait;

	sys_net.warning("sys_net_bnet_connect(s=%d, addr=*0x%x, addrlen=%u)", s, addr, addrlen);

	if (!addr || addrlen < addr.size())
	{
		return -SYS_NET_EINVAL;
	}

	if (addr->sa_family != SYS_NET_AF_INET)
	{
		sys_net.error("sys_net_bnet_connect(s=%d): unsupported sa_family (%d)", s, addr->sa_family);
		return -SYS_NET_EAFNOSUPPORT;
	}

	if (!idm::check_unlocked<lv2_socket>(s))
	{
		return -SYS_NET_EBADF;
	}

	s32 result               = 0;
	sys_net_sockaddr sn_addr = *addr;

	const auto sock = idm::check<lv2_socket>(s, [&, notify = lv2_obj::notify_all_t()](lv2_socket& sock)
		{
			const auto success = sock.connect(sn_addr);

			if (success)
			{
				result = *success;
				return true;
			}

			auto lock = sock.lock();

			sock.poll_queue(idm::get_unlocked<named_thread<ppu_thread>>(ppu.id), lv2_socket::poll_t::write, [&](bs_t<lv2_socket::poll_t> events) -> bool
				{
					if (events & lv2_socket::poll_t::write)
					{
						result = sock.connect_followup();

						lv2_obj::awake(&ppu);
						return true;
					}
					sock.set_poll_event(lv2_socket::poll_t::write);
					return false;
				});

			lv2_obj::prepare_for_sleep(ppu);
			lv2_obj::sleep(ppu);
			return false;
		});

	if (!sock)
	{
		return -SYS_NET_EBADF;
	}

	if (sock.ret)
	{
		if (result < 0)
		{
			return sys_net_error{result};
		}

		return not_an_error(result);
	}

	while (auto state = ppu.state.fetch_sub(cpu_flag::signal))
	{
		if (is_stopped(state))
		{
			return {};
		}

		if (state & cpu_flag::signal)
		{
			break;
		}

		ppu.state.wait(state);
	}

	if (ppu.gpr[3] == static_cast<u64>(-SYS_NET_EINTR))
	{
		return -SYS_NET_EINTR;
	}

	if (result)
	{
		if (result < 0)
		{
			return sys_net_error{result};
		}

		return not_an_error(result);
	}

	return CELL_OK;
}

error_code sys_net_bnet_getpeername(ppu_thread& ppu, s32 s, vm::ptr<sys_net_sockaddr> addr, vm::ptr<u32> paddrlen)
{
	ppu.state += cpu_flag::wait;

	sys_net.warning("sys_net_bnet_getpeername(s=%d, addr=*0x%x, paddrlen=*0x%x)", s, addr, paddrlen);

	// Note: paddrlen is both an input and output argument
	if (!addr || !paddrlen || *paddrlen < addr.size())
	{
		return -SYS_NET_EINVAL;
	}

	const auto sock = idm::check<lv2_socket>(s, [&, notify = lv2_obj::notify_all_t()](lv2_socket& sock) -> s32
		{
			auto [res, sn_addr] = sock.getpeername();

			if (res == CELL_OK)
			{
				*paddrlen = sizeof(sys_net_sockaddr);
				*addr     = sn_addr;
			}

			return res;
		});

	if (!sock)
	{
		return -SYS_NET_EBADF;
	}

	if (sock.ret < 0)
	{
		return sys_net_error{sock.ret};
	}

	return CELL_OK;
}

error_code sys_net_bnet_getsockname(ppu_thread& ppu, s32 s, vm::ptr<sys_net_sockaddr> addr, vm::ptr<u32> paddrlen)
{
	ppu.state += cpu_flag::wait;

	sys_net.warning("sys_net_bnet_getsockname(s=%d, addr=*0x%x, paddrlen=*0x%x)", s, addr, paddrlen);

	// Note: paddrlen is both an input and output argument
	if (!addr || !paddrlen || *paddrlen < addr.size())
	{
		return -SYS_NET_EINVAL;
	}

	const auto sock = idm::check<lv2_socket>(s, [&, notify = lv2_obj::notify_all_t()](lv2_socket& sock) -> s32
		{
			auto [res, sn_addr] = sock.getsockname();

			if (res == CELL_OK)
			{
				*paddrlen = sizeof(sys_net_sockaddr);
				*addr     = sn_addr;
			}

			return res;
		});

	if (!sock)
	{
		return -SYS_NET_EBADF;
	}

	if (sock.ret < 0)
	{
		return sys_net_error{sock.ret};
	}

	return CELL_OK;
}

error_code sys_net_bnet_getsockopt(ppu_thread& ppu, s32 s, s32 level, s32 optname, vm::ptr<void> optval, vm::ptr<u32> optlen)
{
	ppu.state += cpu_flag::wait;

	switch (level)
	{
	case SYS_NET_SOL_SOCKET:
		sys_net.warning("sys_net_bnet_getsockopt(s=%d, level=SYS_NET_SOL_SOCKET, optname=%s, optval=*0x%x, optlen=%u)", s, static_cast<lv2_socket_option>(optname), optval, optlen);
		break;
	case SYS_NET_IPPROTO_TCP:
		sys_net.warning("sys_net_bnet_getsockopt(s=%d, level=SYS_NET_IPPROTO_TCP, optname=%s, optval=*0x%x, optlen=%u)", s, static_cast<lv2_tcp_option>(optname), optval, optlen);
		break;
	case SYS_NET_IPPROTO_IP:
		sys_net.warning("sys_net_bnet_getsockopt(s=%d, level=SYS_NET_IPPROTO_IP, optname=%s, optval=*0x%x, optlen=%u)", s, static_cast<lv2_ip_option>(optname), optval, optlen);
		break;
	default:
		sys_net.warning("sys_net_bnet_getsockopt(s=%d, level=0x%x, optname=0x%x, optval=*0x%x, optlen=%u)", s, level, optname, optval, optlen);
		break;
	}

	if (!optval || !optlen)
	{
		return -SYS_NET_EINVAL;
	}

	const u32 len = *optlen;

	if (!len)
	{
		return -SYS_NET_EINVAL;
	}

	const auto sock = idm::check<lv2_socket>(s, [&, notify = lv2_obj::notify_all_t()](lv2_socket& sock) -> s32
		{
			if (len < sizeof(s32))
			{
				return -SYS_NET_EINVAL;
			}

			const auto& [res, out_val, out_len] = sock.getsockopt(level, optname, *optlen);

			if (res == CELL_OK)
			{
				std::memcpy(optval.get_ptr(), out_val.ch, out_len);
				*optlen = out_len;
			}

			return res;
		});

	if (!sock)
	{
		return -SYS_NET_EBADF;
	}

	if (sock.ret < 0)
	{
		return sys_net_error{sock.ret};
	}

	return CELL_OK;
}

error_code sys_net_bnet_listen(ppu_thread& ppu, s32 s, s32 backlog)
{
	ppu.state += cpu_flag::wait;

	sys_net.warning("sys_net_bnet_listen(s=%d, backlog=%d)", s, backlog);

	if (backlog <= 0)
	{
		return -SYS_NET_EINVAL;
	}

	const auto sock = idm::check<lv2_socket>(s, [&, notify = lv2_obj::notify_all_t()](lv2_socket& sock) -> s32
		{
			return sock.listen(backlog);
		});

	if (!sock)
	{
		return -SYS_NET_EBADF;
	}

	if (sock.ret < 0)
	{
		return sys_net_error{sock.ret};
	}

	return CELL_OK;
}

error_code sys_net_bnet_recvfrom(ppu_thread& ppu, s32 s, vm::ptr<void> buf, u32 len, s32 flags, vm::ptr<sys_net_sockaddr> addr, vm::ptr<u32> paddrlen)
{
	ppu.state += cpu_flag::wait;

	sys_net.trace("sys_net_bnet_recvfrom(s=%d, buf=*0x%x, len=%u, flags=0x%x, addr=*0x%x, paddrlen=*0x%x)", s, buf, len, flags, addr, paddrlen);

	// If addr is null, paddrlen must be null as well
	if (!buf || !len || addr.operator bool() != paddrlen.operator bool())
	{
		return -SYS_NET_EINVAL;
	}

	if (flags & ~(SYS_NET_MSG_PEEK | SYS_NET_MSG_DONTWAIT | SYS_NET_MSG_WAITALL | SYS_NET_MSG_USECRYPTO | SYS_NET_MSG_USESIGNATURE))
	{
		fmt::throw_exception("sys_net_bnet_recvfrom(s=%d): unknown flags (0x%x)", flags);
	}

	s32 result = 0;
	sys_net_sockaddr sn_addr{};

	const auto sock = idm::check<lv2_socket>(s, [&, notify = lv2_obj::notify_all_t()](lv2_socket& sock)
		{
			auto lock = sock.lock();

			const auto success = sock.recvfrom(flags, len, false);

			if (success)
			{
				const auto& [res, vec, res_addr] = *success;
				if (res > 0)
				{
					sn_addr = res_addr;
					std::memcpy(buf.get_ptr(), vec.data(), res);
					sys_net_dump_data("recvfrom", vec.data(), res, &res_addr);
				}

				result = res;
				return true;
			}

			sock.poll_queue(idm::get_unlocked<named_thread<ppu_thread>>(ppu.id), lv2_socket::poll_t::read, [&](bs_t<lv2_socket::poll_t> events) -> bool
				{
					if (events & lv2_socket::poll_t::read)
					{
						const auto success = sock.recvfrom(flags, len, false);

						if (success)
						{
							const auto& [res, vec, res_addr] = *success;
							if (res > 0)
							{
								sn_addr = res_addr;
								std::memcpy(buf.get_ptr(), vec.data(), res);
								sys_net_dump_data("recvfrom", vec.data(), res, &res_addr);
							}
							result = res;
							lv2_obj::awake(&ppu);
							return true;
						}
					}

					if (sock.so_rcvtimeo && get_guest_system_time() - ppu.start_time > sock.so_rcvtimeo)
					{
						result = -SYS_NET_EWOULDBLOCK;
						lv2_obj::awake(&ppu);
						return true;
					}

					sock.set_poll_event(lv2_socket::poll_t::read);
					return false;
				});

			lv2_obj::prepare_for_sleep(ppu);
			lv2_obj::sleep(ppu);
			return false;
		});

	if (!sock)
	{
		return -SYS_NET_EBADF;
	}

	if (!sock.ret)
	{

		while (auto state = ppu.state.fetch_sub(cpu_flag::signal))
		{
			if (is_stopped(state))
			{
				return {};
			}

			if (state & cpu_flag::signal)
			{
				break;
			}

			ppu.state.wait(state);
		}

		if (ppu.gpr[3] == static_cast<u64>(-SYS_NET_EINTR))
		{
			return -SYS_NET_EINTR;
		}
	}

	static_cast<void>(ppu.test_stopped());

	if (result == -SYS_NET_EWOULDBLOCK)
	{
		return not_an_error(result);
	}

	if (result >= 0)
	{
		if (addr)
		{
			*paddrlen = sizeof(sys_net_sockaddr_in);
			*addr     = sn_addr;
		}

		return not_an_error(result);
	}

	return sys_net_error{result};
}

error_code sys_net_bnet_recvmsg(ppu_thread& ppu, s32 s, vm::ptr<sys_net_msghdr> msg, s32 flags)
{
	ppu.state += cpu_flag::wait;

	sys_net.todo("sys_net_bnet_recvmsg(s=%d, msg=*0x%x, flags=0x%x)", s, msg, flags);
	return CELL_OK;
}

error_code sys_net_bnet_sendmsg(ppu_thread& ppu, s32 s, vm::cptr<sys_net_msghdr> msg, s32 flags)
{
	ppu.state += cpu_flag::wait;

	sys_net.warning("sys_net_bnet_sendmsg(s=%d, msg=*0x%x, flags=0x%x)", s, msg, flags);

	if (flags & ~(SYS_NET_MSG_DONTWAIT | SYS_NET_MSG_WAITALL | SYS_NET_MSG_USECRYPTO | SYS_NET_MSG_USESIGNATURE))
	{
		fmt::throw_exception("sys_net_bnet_sendmsg(s=%d): unknown flags (0x%x)", flags);
	}

	s32 result{};

	const auto sock = idm::check<lv2_socket>(s, [&](lv2_socket& sock)
		{
			auto netmsg = msg.get_ptr();
			const auto success = sock.sendmsg(flags, *netmsg);

			if (success)
			{
				result = *success;

				return true;
			}

			sock.poll_queue(idm::get_unlocked<named_thread<ppu_thread>>(ppu.id), lv2_socket::poll_t::write, [&](bs_t<lv2_socket::poll_t> events) -> bool
				{
					if (events & lv2_socket::poll_t::write)
					{
						const auto success = sock.sendmsg(flags, *netmsg, false);

						if (success)
						{
							result = *success;
							lv2_obj::awake(&ppu);
							return true;
						}
					}

					sock.set_poll_event(lv2_socket::poll_t::write);
					return false;
				});

			lv2_obj::sleep(ppu);
			return false;
		});

	if (!sock)
	{
		return -SYS_NET_EBADF;
	}

	if (!sock.ret)
	{
		while (true)
		{
			const auto state = ppu.state.fetch_sub(cpu_flag::signal);
			if (is_stopped(state) || state & cpu_flag::signal)
			{
				break;
			}
			thread_ctrl::wait_on(ppu.state, state);
		}

		if (ppu.gpr[3] == static_cast<u64>(-SYS_NET_EINTR))
		{
			return -SYS_NET_EINTR;
		}
	}

	if (result >= 0 || result == -SYS_NET_EWOULDBLOCK)
	{
		return not_an_error(result);
	}


	return sys_net_error{result};
}

error_code sys_net_bnet_sendto(ppu_thread& ppu, s32 s, vm::cptr<void> buf, u32 len, s32 flags, vm::cptr<sys_net_sockaddr> addr, u32 addrlen)
{
	ppu.state += cpu_flag::wait;

	sys_net.trace("sys_net_bnet_sendto(s=%d, buf=*0x%x, len=%u, flags=0x%x, addr=*0x%x, addrlen=%u)", s, buf, len, flags, addr, addrlen);

	if (flags & ~(SYS_NET_MSG_DONTWAIT | SYS_NET_MSG_WAITALL | SYS_NET_MSG_USECRYPTO | SYS_NET_MSG_USESIGNATURE))
	{
		fmt::throw_exception("sys_net_bnet_sendto(s=%d): unknown flags (0x%x)", flags);
	}

	if (addr && addrlen < sizeof(sys_net_sockaddr))
	{
		sys_net.error("sys_net_bnet_sendto(s=%d): bad addrlen (%u)", s, addrlen);
		return -SYS_NET_EINVAL;
	}

	if (addr && addr->sa_family != SYS_NET_AF_INET)
	{
		sys_net.error("sys_net_bnet_sendto(s=%d): unsupported sa_family (%d)", s, addr->sa_family);
		return -SYS_NET_EAFNOSUPPORT;
	}

	sys_net_dump_data("sendto", static_cast<const u8*>(buf.get_ptr()), len, addr ? addr.get_ptr() : nullptr);

	const std::optional<sys_net_sockaddr> sn_addr = addr ? std::optional<sys_net_sockaddr>(*addr) : std::nullopt;
	const std::vector<u8> buf_copy(vm::_ptr<const char>(buf.addr()), vm::_ptr<const char>(buf.addr()) + len);
	s32 result{};

	const auto sock = idm::check<lv2_socket>(s, [&, notify = lv2_obj::notify_all_t()](lv2_socket& sock)
		{
			auto success = sock.sendto(flags, buf_copy, sn_addr);

			if (success)
			{
				result = *success;
				return true;
			}

			auto lock = sock.lock();

			// Enable write event
			sock.poll_queue(idm::get_unlocked<named_thread<ppu_thread>>(ppu.id), lv2_socket::poll_t::write, [&](bs_t<lv2_socket::poll_t> events) -> bool
				{
					if (events & lv2_socket::poll_t::write)
					{
						auto success = sock.sendto(flags, buf_copy, sn_addr, false);
						if (success)
						{
							result = *success;
							lv2_obj::awake(&ppu);
							return true;
						}
					}

					if (sock.so_sendtimeo && get_guest_system_time() - ppu.start_time > sock.so_sendtimeo)
					{
						result = -SYS_NET_EWOULDBLOCK;
						lv2_obj::awake(&ppu);
						return true;
					}

					sock.set_poll_event(lv2_socket::poll_t::write);
					return false;
				});

			lv2_obj::prepare_for_sleep(ppu);
			lv2_obj::sleep(ppu);
			return false;
		});

	if (!sock)
	{
		return -SYS_NET_EBADF;
	}

	if (!sock.ret)
	{
		while (true)
		{
			const auto state = ppu.state.fetch_sub(cpu_flag::signal);
			if (is_stopped(state) || state & cpu_flag::signal)
			{
				break;
			}
			ppu.state.wait(state);
		}

		if (ppu.gpr[3] == static_cast<u64>(-SYS_NET_EINTR))
		{
			return -SYS_NET_EINTR;
		}
	}

	if (result >= 0 || result == -SYS_NET_EWOULDBLOCK)
	{
		return not_an_error(result);
	}

	return sys_net_error{result};
}

error_code sys_net_bnet_setsockopt(ppu_thread& ppu, s32 s, s32 level, s32 optname, vm::cptr<void> optval, u32 optlen)
{
	ppu.state += cpu_flag::wait;

	switch (level)
	{
	case SYS_NET_SOL_SOCKET:
		sys_net.warning("sys_net_bnet_setsockopt(s=%d, level=SYS_NET_SOL_SOCKET, optname=%s, optval=*0x%x, optlen=%u)", s, static_cast<lv2_socket_option>(optname), optval, optlen);
		break;
	case SYS_NET_IPPROTO_TCP:
		sys_net.warning("sys_net_bnet_setsockopt(s=%d, level=SYS_NET_IPPROTO_TCP, optname=%s, optval=*0x%x, optlen=%u)", s, static_cast<lv2_tcp_option>(optname), optval, optlen);
		break;
	case SYS_NET_IPPROTO_IP:
		sys_net.warning("sys_net_bnet_setsockopt(s=%d, level=SYS_NET_IPPROTO_IP, optname=%s, optval=*0x%x, optlen=%u)", s, static_cast<lv2_ip_option>(optname), optval, optlen);
		break;
	default:
		sys_net.warning("sys_net_bnet_setsockopt(s=%d, level=0x%x, optname=0x%x, optval=*0x%x, optlen=%u)", s, level, optname, optval, optlen);
		break;
	}

	switch (optlen)
	{
	case 1:
		sys_net.warning("optval: 0x%02X", *static_cast<const u8*>(optval.get_ptr()));
		break;
	case 2:
		sys_net.warning("optval: 0x%04X", *static_cast<const be_t<u16>*>(optval.get_ptr()));
		break;
	case 4:
		sys_net.warning("optval: 0x%08X", *static_cast<const be_t<u32>*>(optval.get_ptr()));
		break;
	case 8:
		sys_net.warning("optval: 0x%016X", *static_cast<const be_t<u64>*>(optval.get_ptr()));
		break;
	}

	// BSD takes u_char for IP_MULTICAST_TTL / IP_MULTICAST_LOOP (the firmware DHCP client passes a single byte)
	std::vector<u8> optval_copy;

	if (optlen == 1 && level == SYS_NET_IPPROTO_IP && (optname == SYS_NET_IP_MULTICAST_TTL || optname == SYS_NET_IP_MULTICAST_LOOP))
	{
		const be_t<s32> widened = *vm::_ptr<u8>(optval.addr());
		optval_copy.assign(reinterpret_cast<const u8*>(&widened), reinterpret_cast<const u8*>(&widened) + sizeof(widened));
		optlen = sizeof(s32);
	}

	if (optlen < sizeof(s32))
	{
		return -SYS_NET_EINVAL;
	}

	if (optval_copy.empty())
	{
		optval_copy.assign(vm::_ptr<u8>(optval.addr()), vm::_ptr<u8>(optval.addr() + optlen));
	}

	const auto sock = idm::check<lv2_socket>(s, [&, notify = lv2_obj::notify_all_t()](lv2_socket& sock) -> s32
		{
			return sock.setsockopt(level, optname, optval_copy);
		});

	if (!sock)
	{
		return -SYS_NET_EBADF;
	}

	if (sock.ret < 0)
	{
		return sys_net_error{sock.ret};
	}

	return not_an_error(sock.ret);
}

error_code sys_net_bnet_shutdown(ppu_thread& ppu, s32 s, s32 how)
{
	ppu.state += cpu_flag::wait;

	sys_net.warning("sys_net_bnet_shutdown(s=%d, how=%d)", s, how);

	if (how < 0 || how > 2)
	{
		return -SYS_NET_EINVAL;
	}

	const auto sock = idm::check<lv2_socket>(s, [&, notify = lv2_obj::notify_all_t()](lv2_socket& sock) -> s32
		{
			return sock.shutdown(how);
		});

	if (!sock)
	{
		return -SYS_NET_EBADF;
	}

	if (sock.ret < 0)
	{
		return sys_net_error{sock.ret};
	}

	return CELL_OK;
}

error_code sys_net_bnet_socket(ppu_thread& ppu, lv2_socket_family family, lv2_socket_type type, lv2_ip_protocol protocol)
{
	ppu.state += cpu_flag::wait;

	sys_net.warning("sys_net_bnet_socket(family=%s, type=%s, protocol=%s)", family, type, protocol);

	if (family != SYS_NET_AF_INET)
	{
		sys_net.error("sys_net_bnet_socket(): unknown family (%d)", family);
	}

	// Type 7 is created by the firmware DHCP client next to its UDP socket (interface notifications); no data is ever delivered
	const bool is_firmware_type7 = static_cast<s32>(type) == 7;

	if (!is_firmware_type7 && type != SYS_NET_SOCK_STREAM && type != SYS_NET_SOCK_DGRAM && type != SYS_NET_SOCK_RAW && type != SYS_NET_SOCK_DGRAM_P2P && type != SYS_NET_SOCK_STREAM_P2P)
	{
		sys_net.error("sys_net_bnet_socket(): unsupported type (%d)", type);
		sys_net.error("  caller: %s", ppu.dump_callstack());
		return -SYS_NET_EPROTONOSUPPORT;
	}

	shared_ptr<lv2_socket> sock_lv2;

	if (is_firmware_type7)
	{
		sock_lv2 = make_shared<lv2_socket_raw>(family, type, protocol);
	}
	else switch (type)
	{
	case SYS_NET_SOCK_STREAM:
	case SYS_NET_SOCK_DGRAM:
	{
		auto lv2_native = make_shared<lv2_socket_native>(family, type, protocol);
		if (s32 result = lv2_native->create_socket(); result < 0)
		{
			return sys_net_error{result};
		}

		sock_lv2 = std::move(lv2_native);
		break;
	}
	case SYS_NET_SOCK_RAW: sock_lv2 = make_shared<lv2_socket_raw>(family, type, protocol); break;
	case SYS_NET_SOCK_DGRAM_P2P: sock_lv2 = make_shared<lv2_socket_p2p>(family, type, protocol); break;
	case SYS_NET_SOCK_STREAM_P2P: sock_lv2 = make_shared<lv2_socket_p2ps>(family, type, protocol); break;
	}

	const s32 s = idm::import_existing<lv2_socket>(sock_lv2);

	// Can't allocate more than 1000 sockets
	if (s == id_manager::id_traits<lv2_socket>::invalid)
	{
		return -SYS_NET_EMFILE;
	}

	sock_lv2->set_lv2_id(s);

	return not_an_error(s);
}

error_code sys_net_bnet_close(ppu_thread& ppu, s32 s)
{
	ppu.state += cpu_flag::wait;

	sys_net.warning("sys_net_bnet_close(s=%d)", s);

	auto sock = idm::withdraw<lv2_socket>(s);

	if (!sock)
	{
		return -SYS_NET_EBADF;
	}

	if (sock->get_queue_size())
	{
		sock->abort_socket(0);
	}

	sock->close();

	{
		// Ensures the socket has no lingering copy from the network threads
		std::scoped_lock threads_lock(g_fxo->get<network_context>().mutex_thread_loop, g_fxo->get<p2p_context>().mutex_thread_loop);
		sock.reset();
	}

	return CELL_OK;
}

error_code sys_net_bnet_poll(ppu_thread& ppu, vm::ptr<sys_net_pollfd> fds, s32 nfds, s32 ms)
{
	ppu.state += cpu_flag::wait;

	sys_net.trace("sys_net_bnet_poll(fds=*0x%x, nfds=%d, ms=%d)", fds, nfds, ms);

	if (nfds <= 0)
	{
		return not_an_error(0);
	}

	atomic_t<s32> signaled{0};
	bool has_sockets = false;
	u64 timeout = ms < 0 ? 0 : ms * 1000ull;

	std::vector<sys_net_pollfd> fds_buf;

	{
		fds_buf.assign(fds.get_ptr(), fds.get_ptr() + nfds);

		lv2_obj::prepare_for_sleep(ppu);

		std::unique_lock nw_lock(g_fxo->get<network_context>().mutex_thread_loop);
		std::unique_lock p2p_lock(g_fxo->get<p2p_context>().mutex_thread_loop);
		std::shared_lock lock(id_manager::g_mutex);

		std::vector<::pollfd> _fds(nfds);
#ifdef _WIN32
		std::vector<bool> connecting(nfds);
#endif

		for (s32 i = 0; i < nfds; i++)
		{
			_fds[i].fd         = -1;
			fds_buf[i].revents = 0;

			if (fds_buf[i].fd < 0)
			{
				continue;
			}

			if (auto sock = idm::check_unlocked<lv2_socket>(fds_buf[i].fd))
			{
				has_sockets = true;
				sock->poll(fds_buf[i], _fds[i]);
#ifdef _WIN32
				connecting[i] = sock->is_connecting();
#endif
			}
			else
			{
				fds_buf[i].revents |= SYS_NET_POLLNVAL;
			}
		}

#ifdef _WIN32
		windows_poll(_fds, nfds, 0, connecting);
#else
		::poll(_fds.data(), nfds, 0);
#endif
		for (s32 i = 0; i < nfds; i++)
		{
			if (_fds[i].revents & (POLLIN | POLLHUP))
				fds_buf[i].revents |= SYS_NET_POLLIN;
			if (_fds[i].revents & POLLOUT)
				fds_buf[i].revents |= SYS_NET_POLLOUT;
			if (_fds[i].revents & POLLERR)
				fds_buf[i].revents |= SYS_NET_POLLERR;

			if (fds_buf[i].revents)
			{
				signaled++;
			}
		}

		if (ms == 0 || signaled)
		{
			lock.unlock();
			p2p_lock.unlock();
			nw_lock.unlock();
			std::memcpy(fds.get_ptr(), fds_buf.data(), nfds * sizeof(sys_net_pollfd));
			return not_an_error(signaled);
		}

		for (s32 i = 0; i < nfds; i++)
		{
			if (fds_buf[i].fd < 0)
			{
				continue;
			}

			if (auto sock = idm::check_unlocked<lv2_socket>(fds_buf[i].fd))
			{
				auto lock = sock->lock();

#ifdef _WIN32
				sock->set_connecting(connecting[i]);
#endif

				bs_t<lv2_socket::poll_t> selected = +lv2_socket::poll_t::error;

				if (fds_buf[i].events & SYS_NET_POLLIN)
					selected += lv2_socket::poll_t::read;
				if (fds_buf[i].events & SYS_NET_POLLOUT)
					selected += lv2_socket::poll_t::write;
				// if (fds_buf[i].events & SYS_NET_POLLPRI) // Unimplemented
				//	selected += lv2_socket::poll::error;

				sock->poll_queue(idm::get_unlocked<named_thread<ppu_thread>>(ppu.id), selected, [sock, selected, &fds_buf, i, &signaled, &ppu](bs_t<lv2_socket::poll_t> events)
					{
						if (events & selected)
						{
							if (events & selected & lv2_socket::poll_t::read)
								fds_buf[i].revents |= SYS_NET_POLLIN;
							if (events & selected & lv2_socket::poll_t::write)
								fds_buf[i].revents |= SYS_NET_POLLOUT;
							if (events & selected & lv2_socket::poll_t::error)
								fds_buf[i].revents |= SYS_NET_POLLERR;

							signaled++;
							sock->queue_wake(&ppu);
							return true;
						}

						sock->set_poll_event(selected);
						return false;
					});
			}
		}

		lv2_obj::sleep(ppu, timeout);
	}

	bool has_timedout = false;

	while (auto state = ppu.state.fetch_sub(cpu_flag::signal))
	{
		if (is_stopped(state))
		{
			return {};
		}

		if (state & cpu_flag::signal)
		{
			break;
		}

		if (timeout)
		{
			if (lv2_obj::wait_timeout(timeout, &ppu))
			{
				// Wait for rescheduling
				if (ppu.check_state())
				{
					return {};
				}

				has_timedout = network_clear_queue(ppu) || !has_sockets;
				clear_ppu_to_awake(ppu);
				ppu.state -= cpu_flag::signal;
				break;
			}
		}
		else
		{
			ppu.state.wait(state);
		}
	}

	if (!has_timedout && !signaled)
	{
		return -SYS_NET_EINTR;
	}

	std::memcpy(fds.get_ptr(), fds_buf.data(), nfds * sizeof(fds[0]));

	return not_an_error(signaled);
}

error_code sys_net_bnet_select(ppu_thread& ppu, s32 nfds, vm::ptr<sys_net_fd_set> readfds, vm::ptr<sys_net_fd_set> writefds, vm::ptr<sys_net_fd_set> exceptfds, vm::ptr<sys_net_timeval> _timeout)
{
	ppu.state += cpu_flag::wait;

	sys_net.trace("sys_net_bnet_select(nfds=%d, readfds=*0x%x, writefds=*0x%x, exceptfds=*0x%x, timeout=*0x%x(%d:%d))", nfds, readfds, writefds, exceptfds, _timeout, _timeout ? _timeout->tv_sec.value() : 0, _timeout ? _timeout->tv_usec.value() : 0);

	atomic_t<s32> signaled{0};

	if (exceptfds)
	{
		struct log_t
		{
			atomic_t<bool> logged = false;
		};

		if (!g_fxo->get<log_t>().logged.exchange(true))
		{
			sys_net.error("sys_net_bnet_select(): exceptfds not implemented");
		}
	}

	sys_net_fd_set rread{}, _readfds{};
	sys_net_fd_set rwrite{}, _writefds{};
	sys_net_fd_set rexcept{}, _exceptfds{};
	u64 timeout = !_timeout ? 0 : _timeout->tv_sec * 1000000ull + _timeout->tv_usec;
	bool has_sockets = false;

	if (nfds > 0 && nfds <= 1024)
	{
		if (readfds)
			_readfds = *readfds;
		if (writefds)
			_writefds = *writefds;
		if (exceptfds)
			_exceptfds = *exceptfds;

		std::lock_guard nw_lock(g_fxo->get<network_context>().mutex_thread_loop);
		std::lock_guard p2p_lock(g_fxo->get<p2p_context>().mutex_thread_loop);
		reader_lock lock(id_manager::g_mutex);

		std::vector<::pollfd> _fds(nfds);
#ifdef _WIN32
		std::vector<bool> connecting(nfds);
#endif

		for (s32 i = 0; i < nfds; i++)
		{
			_fds[i].fd = -1;
			bs_t<lv2_socket::poll_t> selected{};

			if (readfds && _readfds.bit(i))
				selected += lv2_socket::poll_t::read;
			if (writefds && _writefds.bit(i))
				selected += lv2_socket::poll_t::write;
			// if (exceptfds && _exceptfds.bit(i))
			//	selected += lv2_socket::poll::error;

			if (selected)
			{
				selected += lv2_socket::poll_t::error;
				has_sockets = true;
			}
			else
			{
				continue;
			}

			if (auto sock = idm::check_unlocked<lv2_socket>((lv2_socket::id_base & -1024) + i))
			{
				auto [read_set, write_set, except_set] = sock->select(selected, _fds[i]);

				if (read_set || write_set || except_set)
				{
					signaled++;
				}

				if (read_set)
				{
					rread.set(i);
				}

				if (write_set)
				{
					rwrite.set(i);
				}

				if (except_set)
				{
					rexcept.set(i);
				}

#ifdef _WIN32
				connecting[i] = sock->is_connecting();
#endif
			}
			else
			{
				return -SYS_NET_EBADF;
			}
		}

#ifdef _WIN32
		windows_poll(_fds, nfds, 0, connecting);
#else
		::poll(_fds.data(), nfds, 0);
#endif
		for (s32 i = 0; i < nfds; i++)
		{
			bool sig = false;
			if ((_fds[i].revents & (POLLIN | POLLHUP | POLLERR)) && _readfds.bit(i))
				sig = true, rread.set(i);
			if ((_fds[i].revents & (POLLOUT | POLLERR)) && _writefds.bit(i))
				sig = true, rwrite.set(i);

			if (sig)
			{
				signaled++;
			}
		}

		if ((_timeout && !timeout) || signaled)
		{
			if (readfds)
				*readfds = rread;
			if (writefds)
				*writefds = rwrite;
			if (exceptfds)
				*exceptfds = rexcept;
			return not_an_error(signaled);
		}

		for (s32 i = 0; i < nfds; i++)
		{
			bs_t<lv2_socket::poll_t> selected{};

			if (readfds && _readfds.bit(i))
				selected += lv2_socket::poll_t::read;
			if (writefds && _writefds.bit(i))
				selected += lv2_socket::poll_t::write;
			// if (exceptfds && _exceptfds.bit(i))
			//	selected += lv2_socket::poll_t::error;

			if (selected)
			{
				selected += lv2_socket::poll_t::error;
			}
			else
			{
				continue;
			}

			if (auto sock = idm::check_unlocked<lv2_socket>((lv2_socket::id_base & -1024) + i))
			{
				auto lock = sock->lock();
#ifdef _WIN32
				sock->set_connecting(connecting[i]);
#endif

				sock->poll_queue(idm::get_unlocked<named_thread<ppu_thread>>(ppu.id), selected, [sock, selected, i, &rread, &rwrite, &rexcept, &signaled, &ppu](bs_t<lv2_socket::poll_t> events)
					{
						if (events & selected)
						{
							if (selected & lv2_socket::poll_t::read && events & (lv2_socket::poll_t::read + lv2_socket::poll_t::error))
								rread.set(i);
							if (selected & lv2_socket::poll_t::write && events & (lv2_socket::poll_t::write + lv2_socket::poll_t::error))
								rwrite.set(i);
							// if (events & (selected & lv2_socket::poll::error))
						    //	rexcept.set(i);

							signaled++;
							sock->queue_wake(&ppu);
							return true;
						}

						sock->set_poll_event(selected);
						return false;
					});
			}
			else
			{
				return -SYS_NET_EBADF;
			}
		}

		lv2_obj::sleep(ppu, timeout);
	}
	else
	{
		return -SYS_NET_EINVAL;
	}

	bool has_timedout = false;

	while (auto state = ppu.state.fetch_sub(cpu_flag::signal))
	{
		if (is_stopped(state))
		{
			return {};
		}

		if (state & cpu_flag::signal)
		{
			break;
		}

		if (timeout)
		{
			if (lv2_obj::wait_timeout(timeout, &ppu))
			{
				// Wait for rescheduling
				if (ppu.check_state())
				{
					return {};
				}

				has_timedout = network_clear_queue(ppu) || !has_sockets;
				clear_ppu_to_awake(ppu);
				ppu.state -= cpu_flag::signal;
				break;
			}
		}
		else
		{
			ppu.state.wait(state);
		}
	}

	if (!has_timedout && !signaled)
	{
		return -SYS_NET_EINTR;
	}

	if (readfds)
		*readfds = rread;
	if (writefds)
		*writefds = rwrite;
	if (exceptfds)
		*exceptfds = rexcept;

	return not_an_error(signaled);
}

error_code _sys_net_open_dump(ppu_thread& ppu, s32 len, s32 flags)
{
	ppu.state += cpu_flag::wait;

	sys_net.todo("_sys_net_open_dump(len=%d, flags=0x%x)", len, flags);
	return CELL_OK;
}

error_code _sys_net_read_dump(ppu_thread& ppu, s32 id, vm::ptr<void> buf, s32 len, vm::ptr<s32> pflags)
{
	ppu.state += cpu_flag::wait;

	sys_net.todo("_sys_net_read_dump(id=0x%x, buf=*0x%x, len=%d, pflags=*0x%x)", id, buf, len, pflags);
	return CELL_OK;
}

error_code _sys_net_close_dump(ppu_thread& ppu, s32 id, vm::ptr<s32> pflags)
{
	ppu.state += cpu_flag::wait;

	sys_net.todo("_sys_net_close_dump(id=0x%x, pflags=*0x%x)", id, pflags);
	return CELL_OK;
}

error_code _sys_net_write_dump(ppu_thread& ppu, s32 id, vm::cptr<void> buf, s32 len, u32 unknown)
{
	ppu.state += cpu_flag::wait;

	sys_net.todo("_sys_net_write_dump(id=0x%x, buf=*0x%x, len=%d, unk=0x%x)", id, buf, len, unknown);
	return CELL_OK;
}

error_code lv2_socket::abort_socket(s32 flags)
{
	decltype(queue) qcopy;
	{
		std::lock_guard lock(mutex);

		if (queue.empty())
		{
			if (flags & SYS_NET_ABORT_STRICT_CHECK)
			{
				// Strict error checking: ENOENT if nothing happened
				return -SYS_NET_ENOENT;
			}

			// TODO: Abort the subsequent function called on this socket (need to investigate correct behaviour)
			return CELL_OK;
		}

		qcopy = std::move(queue);
		queue = {};
		events.store({});
	}

	for (auto& [ppu, _] : qcopy)
	{
		if (!ppu)
			continue;

		// Avoid possible double signaling
		network_clear_queue(*ppu);
		clear_ppu_to_awake(*ppu);

		sys_net.warning("lv2_socket::abort_socket(): waking up \"%s\": (func: %s, r3=0x%x, r4=0x%x, r5=0x%x, r6=0x%x)", ppu->get_name(), ppu->current_function, ppu->gpr[3], ppu->gpr[4], ppu->gpr[5], ppu->gpr[6]);
		ppu->gpr[3] = static_cast<u64>(-SYS_NET_EINTR);
		lv2_obj::append(ppu.get());
	}

	const u32 num_waiters = ::size32(qcopy);
	if (num_waiters && (type == SYS_NET_SOCK_STREAM || type == SYS_NET_SOCK_DGRAM))
	{
		auto& nc = g_fxo->get<network_context>();
		const u32 prev_value = nc.num_polls.fetch_sub(num_waiters);
		ensure(prev_value >= num_waiters);
	}

	lv2_obj::awake_all();
	return CELL_OK;
}

error_code sys_net_abort(ppu_thread& ppu, s32 type, u64 arg, s32 flags)
{
	ppu.state += cpu_flag::wait;

	sys_net.warning("sys_net_abort(type=%d, arg=0x%x, flags=0x%x)", type, arg, flags);

	enum abort_type : s32
	{
		_socket,
		resolver,
		type_2, // ??
		type_3, // ??
		all,
	};

	switch (type)
	{
	case _socket:
	{
		std::scoped_lock threads_lock(g_fxo->get<network_context>().mutex_thread_loop, g_fxo->get<p2p_context>().mutex_thread_loop);

		const auto sock = idm::get_unlocked<lv2_socket>(static_cast<u32>(arg));

		if (!sock)
		{
			return -SYS_NET_EBADF;
		}

		return sock->abort_socket(flags);
	}
	case all:
	{
		std::vector<u32> sockets;

		idm::select<lv2_socket>([&](u32 id, lv2_socket&)
		{
			sockets.emplace_back(id);
		});

		s32 failed = 0;

		for (u32 id : sockets)
		{
			const auto sock = idm::withdraw<lv2_socket>(id);

			if (!sock)
			{
				failed++;
				continue;
			}

			if (sock->get_queue_size())
				sys_net.error("ABORT 4");

			sock->close();

			sys_net.success("lv2_socket::handle_abort(): Closed socket %d", id);
		}

		// Ensures the socket has no lingering copy from the network threads
		g_fxo->get<network_context>().mutex_thread_loop.lock_unlock();
		g_fxo->get<p2p_context>().mutex_thread_loop.lock_unlock();

		return not_an_error(::narrow<s32>(sockets.size()) - failed);
	}
	case resolver:
	case type_2:
	case type_3:
	{
		break;
	}
	default: return -SYS_NET_EINVAL;
	}

	return CELL_OK;
}

struct net_infoctl_cmd_6_t
{
	be_t<s32> sock_id;
	be_t<u32> zero_0;
	be_t<u32> zero_1;
	vm::bptr<sys_net_sockinfo_t> sock_info;
	be_t<s32> n;
	be_t<u32> zero_2;
};

struct net_infoctl_cmd_9_t
{
	be_t<u32> zero;
	vm::bptr<char> server_name;
	// More (TODO)
};

static void net_write_sockinfo(s32 s, lv2_socket& sock, sys_net_sockinfo_t& info)
{
	info = {};
	info.s = s;

	switch (sock.get_type())
	{
	case SYS_NET_SOCK_DGRAM:
	case SYS_NET_SOCK_DGRAM_P2P:
		info.proto = SYS_NET_IPPROTO_UDP;
		break;
	case SYS_NET_SOCK_STREAM:
	case SYS_NET_SOCK_STREAM_P2P:
		info.proto = SYS_NET_IPPROTO_TCP;
		break;
	default:
		info.proto = static_cast<s32>(sock.get_protocol());
		break;
	}

	if (const auto [res, sn_addr] = sock.getsockname(); res == CELL_OK)
	{
		const auto* addr_in = reinterpret_cast<const sys_net_sockaddr_in*>(&sn_addr);
		info.local_adr._s_addr = addr_in->sin_addr;
		info.local_port = addr_in->sin_port;
	}

	if (const auto [res, sn_addr] = sock.getpeername(); res == CELL_OK)
	{
		const auto* addr_in = reinterpret_cast<const sys_net_sockaddr_in*>(&sn_addr);
		info.remote_adr._s_addr = addr_in->sin_addr;
		info.remote_port = addr_in->sin_port;
	}

	sock.get_sockinfo(info);

	sys_net.trace("sys_net_infoctl(cmd=6): %s", info);
}

static bool net_virtual_link_up();

error_code sys_net_infoctl(ppu_thread& ppu, s32 cmd, vm::ptr<void> arg)
{
	ppu.state += cpu_flag::wait;

	if (cmd == 6 || cmd == 9)
		sys_net.notice("sys_net_infoctl(cmd=%d, arg=*0x%x)", cmd, arg);
	else
		sys_net.todo("sys_net_infoctl(cmd=%d, arg=*0x%x)", cmd, arg);

	// TODO
	switch (cmd)
	{
	case 6:
	{
		if (!arg)
		{
			return -SYS_NET_EINVAL;
		}

		vm::ptr<net_infoctl_cmd_6_t> cmd_arg = vm::static_ptr_cast<net_infoctl_cmd_6_t>(arg);

		const vm::bptr<sys_net_sockinfo_t> sock_info = cmd_arg->sock_info;
		const s32 sock_id = cmd_arg->sock_id;
		const s32 max_infos = cmd_arg->n;

		sys_net.trace("cmd 6: sock_id: %d, sock_info: *0x%x, max_infos: %d", sock_id, sock_info, max_infos);

		std::scoped_lock threads_lock(g_fxo->get<network_context>().mutex_thread_loop, g_fxo->get<p2p_context>().mutex_thread_loop);

		if (sock_id == -1)
		{
			if (!sock_info)
			{
				return not_an_error(static_cast<s32>(idm::select<lv2_socket>([](u32, lv2_socket&) {})));
			}

			if (max_infos < 0)
			{
				return -SYS_NET_EINVAL;
			}

			s32 num_infos = 0;

			idm::select<lv2_socket>([&](u32 id, lv2_socket& sock)
				{
					if (num_infos >= max_infos)
					{
						return;
					}

					net_write_sockinfo(static_cast<s32>(id), sock, sock_info[num_infos]);
					num_infos++;
				});

			return not_an_error(num_infos);
		}

		if (!sock_info || max_infos < 1)
		{
			return -SYS_NET_EINVAL;
		}

		const auto sock = idm::check<lv2_socket>(sock_id, [&](lv2_socket& sock)
			{
				net_write_sockinfo(sock_id, sock, sock_info[0]);
			});

		if (!sock)
		{
			return -SYS_NET_EBADF;
		}

		break;
	}
	case 9:
	{
		if (!arg)
		{
			return -SYS_NET_EINVAL;
		}

		auto& nph = g_fxo->get<named_thread<np::np_handler>>();
		std::string nameserver = "nameserver " + np::ip_to_string(nph.get_dns_ip());

		vm::ptr<net_infoctl_cmd_9_t> cmd_arg = vm::static_ptr_cast<net_infoctl_cmd_9_t>(arg);
		cmd_arg->zero = 0;
		std::memcpy(cmd_arg->server_name.get_ptr(), nameserver.c_str(), nameserver.size() + 1);
		break;
	}
	case 55:
	{
		// libnetctl registers the event queue on which the kernel reports link events (netctl_main, event mask 0x20 in data1).
		// Events as consumed by the firmware (FUN_00329140 in vsh.elf): data1 = 0x20, data2 = 4 (link up) with data3 = active media
		// bitmask (see sys_net_control 0x80020001), or data2 = 8 (link down). The firmware waits for such an event in its IP
		// obtaining state, otherwise it never leaves it and reports "no ethernet cable".
		if (!arg)
		{
			return -SYS_NET_EINVAL;
		}

		const auto words = vm::_ptr<be_t<u32>>(arg.addr());
		sys_net.notice("sys_net_infoctl(cmd=55): arg=%s", fmt::buf_to_hexstring(vm::_ptr<u8>(arg.addr()), 32));

		for (u32 i = 0; i < 8; i++)
		{
			if (const auto queue = idm::get_unlocked<lv2_obj, lv2_event_queue>(words[i]))
			{
				const bool up = net_virtual_link_up();
				sys_net.notice("sys_net_infoctl(cmd=55): queue 0x%x (arg word %u) <- link %s event", +words[i], i, up ? "up" : "down");
				queue->send(0, 0x20, up ? 4 : 8, up ? 0x8 : 0);
				break;
			}
		}

		break;
	}
	default: break;
	}

	return CELL_OK;
}

// The firmware's netctl (netctl_main, statically linked in the VSH, FreeBSD-derived libnet) decides about the physical link through
// the interface ioctls and sys_net_control. Everything below follows the call sites in the firmware (see docs in the comments):
//  - SIOCGIFFLAGS (_IOWR('i', 17, ifreq)): ifr_flags is a be16 at ifreq+16
//  - 'i' 140 (_IOWR, 32 bytes): get link-layer (MAC) address of the interface, sockaddr at ifreq+16, address bytes at ifreq+18
//  - sys_net_control(ifname, 0x80020001, u32*, 4): active media of the link as a bitmask (0 = no link / unknown):
//      0x1 10BASE half, 0x2 10BASE full, 0x4 100BASE half, 0x8 100BASE full, 0x10 1000BASE half, 0x20 1000BASE full
//    netctl maps it to CELL_NET_CTL_LINK_TYPE_*; anything else is reported as "no ethernet cable connected"
static bool net_virtual_link_up()
{
	return g_cfg.net.net_active == np_internet_status::enabled;
}

error_code sys_net_control(ppu_thread& ppu, u32 arg1, s32 arg2, vm::ptr<void> arg3, s32 arg4)
{
	ppu.state += cpu_flag::wait;

	sys_net.todo("sys_net_control(0x%x, %d, *0x%x, %d)", arg1, arg2, arg3, arg4);

	if (static_cast<u32>(arg2) == 0x80020001u)
	{
		// Active media of the interface
		if (arg3 && arg4 == 4)
		{
			// Same link type the HLE cellNetCtl reports (100BASE full duplex)
			*vm::_ptr<be_t<u32>>(arg3.addr()) = net_virtual_link_up() ? 0x8 : 0x0;
			sys_net.notice("sys_net_control(): reporting link media 0x%x for interface \"%s\"", *vm::_ptr<be_t<u32>>(arg3.addr()), arg1 ? vm::_ptr<const char>(arg1) : "");
		}

		return CELL_OK;
	}

	// EXPERIMENT (kept from earlier work): other 4-byte queries (seen: 0x80080005) are answered with 1 until their meaning is known
	if (arg3 && arg4 == 4)
	{
		sys_net.todo("sys_net_control(): faking connected status for cmd=0x%x", arg2);
		*vm::_ptr<u32>(arg3.addr()) = 1;
	}

	return CELL_OK;
}

error_code sys_net_bnet_ioctl(ppu_thread& ppu, s32 arg1, u32 arg2, u32 arg3)
{
	ppu.state += cpu_flag::wait;

	sys_net.todo("sys_net_bnet_ioctl(%d, 0x%x, 0x%x) called from cia=0x%x", arg1, arg2, arg3, ppu.cia);

	// BSD-style ioctl encoding: bits 16-28 = transfer size, bits 8-15 = group, bits 0-7 = command number
	const u32 io_size  = (arg2 >> 16) & 0x1fff;
	const u32 io_group = (arg2 >> 8) & 0xff;
	const u32 io_num   = arg2 & 0xff;
	const bool io_out  = (arg2 & 0x40000000) != 0;

	if (arg3 && io_group == 'i' && io_size >= 32)
	{
		const auto ifr = vm::_ptr<u8>(arg3);
		const std::string name(reinterpret_cast<const char*>(ifr), strnlen(reinterpret_cast<const char*>(ifr), 16));
		const bool is_lo = name.starts_with("lo");

		if (io_out && io_num == 17)
		{
			// SIOCGIFFLAGS: IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_SIMPLEX | IFF_MULTICAST (running only with link)
			// (loopback: IFF_UP | IFF_LOOPBACK | IFF_RUNNING | IFF_MULTICAST)
			u16 flags = is_lo ? 0x8049 : 0x8803;

			if (!is_lo && net_virtual_link_up())
			{
				flags |= 0x40;
			}

			*reinterpret_cast<be_t<u16>*>(ifr + 16) = flags;
		}
		else if (io_out && (io_num == 33 || io_num == 37 || io_num == 35) && !is_lo)
		{
			// SIOCGIFADDR / SIOCGIFNETMASK / SIOCGIFBRDADDR: sockaddr_in at ifreq+16
			const auto cfg = g_fxo->get<named_thread<np::np_handler>>().get_ipv4_config();
			u32 value = cfg.ip;

			if (io_num == 37)
			{
				value = cfg.mask;
			}
			else if (io_num == 35)
			{
				value = cfg.ip | ~cfg.mask;
			}

			ifr[16] = 16; // sin_len
			ifr[17] = 2;  // AF_INET
			std::memset(ifr + 18, 0, 14);
			std::memcpy(ifr + 20, &value, 4);
		}
		else if (io_out && io_num == 140 && !is_lo)
		{
			// Link-layer address of the interface
			const auto mac = g_fxo->get<named_thread<np::np_handler>>().get_or_discover_ether_addr();
			ifr[16] = 8; // sa_len
			ifr[17] = 18; // AF_LINK
			std::memcpy(ifr + 18, mac.data(), 6);
		}
	}

	return CELL_OK;
}

// Interface list as returned by sysctl({CTL_NET, PF_ROUTE, 0, 0, NET_RT_IFLIST, 0}) in the (NetBSD-derived) layout the firmware's getifaddrs()
// parses (FUN_00025f78 in vsh.elf): RTM_IFINFO (0x0f) messages {if_msghdr 16 bytes, if_data 0x88 bytes, sockaddr_dl} followed by
// RTM_NEWADDR (0x0c) messages {ifa_msghdr 20 bytes, netmask, address sockaddr_in}. This is what if_nametoindex() and the netctl code use to find "eth0".
static std::vector<u8> net_build_iflist()
{
	std::vector<u8> out;

	const auto put8 = [&](std::vector<u8>& v, u8 x) { v.push_back(x); };
	const auto put16 = [&](std::vector<u8>& v, u16 x) { v.push_back(x >> 8); v.push_back(x & 0xff); };
	const auto put32 = [&](std::vector<u8>& v, u32 x) { put16(v, x >> 16); put16(v, x & 0xffff); };
	const auto sockaddr_in = [&](std::vector<u8>& v, const u8* ip)
	{
		put8(v, 16); put8(v, 2); put16(v, 0);
		v.insert(v.end(), ip, ip + 4);
		v.insert(v.end(), 8, u8{0});
	};

	struct nic_t { const char* name; u16 index; u8 type; u16 flags; u32 mtu; bool mac; u8 ip[4]; u8 mask[4]; };

	const bool link_up = net_virtual_link_up();
	np::np_handler::ipv4_config host_cfg{};
	std::array<u8, 6> host_mac{};

	if (link_up)
	{
		auto& nph = g_fxo->get<named_thread<np::np_handler>>();
		host_cfg = nph.get_ipv4_config();
		host_mac = nph.get_or_discover_ether_addr();
	}

	nic_t nics[2]{
		{"lo0", 1, 24, 0x8049, 16384, false, {127, 0, 0, 1}, {255, 0, 0, 0}},
		{"eth0", 2, 6, static_cast<u16>(0x8803 | (link_up ? 0x40 : 0)), 1500, true, {}, {255, 255, 255, 0}},
	};

	std::memcpy(nics[1].ip, &host_cfg.ip, 4);
	if (host_cfg.mask) std::memcpy(nics[1].mask, &host_cfg.mask, 4);

	for (const nic_t& nic : nics)
	{
		// RTM_IFINFO
		std::vector<u8> sdl;
		const u8 nlen = static_cast<u8>(std::strlen(nic.name));
		const u8 alen = nic.mac ? 6 : 0;
		put8(sdl, 8 + nlen + alen); put8(sdl, 18); put16(sdl, nic.index); put8(sdl, nic.type); put8(sdl, nlen); put8(sdl, alen); put8(sdl, 0);
		sdl.insert(sdl.end(), nic.name, nic.name + nlen);
		if (nic.mac) sdl.insert(sdl.end(), host_mac.begin(), host_mac.end());
		sdl.resize((sdl.size() + 3) & ~3u);

		std::vector<u8> msg;
		put16(msg, 0); put8(msg, 3); put8(msg, 0x0f); put32(msg, 0x10); put32(msg, nic.flags); put16(msg, nic.index); put16(msg, 0);
		// if_data (0x88 bytes): type, addrlen, hdrlen, pad, mtu, metric, baudrate, counters
		put8(msg, nic.type); put8(msg, alen); put8(msg, nic.mac ? 14 : 0); put8(msg, 0); put32(msg, nic.mtu); put32(msg, 0); put32(msg, 100'000'000);
		msg.resize(16 + 0x88);
		msg.insert(msg.end(), sdl.begin(), sdl.end());
		msg[0] = static_cast<u8>(msg.size() >> 8); msg[1] = static_cast<u8>(msg.size());
		out.insert(out.end(), msg.begin(), msg.end());

		// RTM_NEWADDR (only for configured addresses)
		if (nic.ip[0] || nic.ip[1] || nic.ip[2] || nic.ip[3])
		{
			std::vector<u8> am;
			put16(am, 0); put8(am, 3); put8(am, 0x0c); put32(am, 0x4 | 0x20); put32(am, 0); put16(am, nic.index); put16(am, 0); put32(am, 0);
			sockaddr_in(am, nic.mask);
			sockaddr_in(am, nic.ip);
			am[0] = static_cast<u8>(am.size() >> 8); am[1] = static_cast<u8>(am.size());
			out.insert(out.end(), am.begin(), am.end());
		}
	}

	return out;
}

// Routing table as returned by sysctl({CTL_NET, PF_ROUTE, 0, af, NET_RT_DUMP, 0}): rt_msghdr (120 bytes) followed by sockaddrs
// (dst, gateway, netmask in RTA order). Parsed by the firmware in sys_net_get_routing_table_af (FUN_0001fa74 in vsh.elf).
static std::vector<u8> net_build_route_dump(s32 af)
{
	std::vector<u8> out;

	if ((af != 0 && af != 2) || !net_virtual_link_up())
	{
		return out;
	}

	const auto cfg = g_fxo->get<named_thread<np::np_handler>>().get_ipv4_config();

	if (!cfg.ip)
	{
		return out;
	}

	const auto put16 = [](std::vector<u8>& v, u16 x) { v.push_back(x >> 8); v.push_back(x & 0xff); };
	const auto put32 = [&](std::vector<u8>& v, u32 x) { put16(v, x >> 16); put16(v, x & 0xffff); };
	const auto sockaddr_in = [&](std::vector<u8>& v, u32 net_order_ip)
	{
		v.push_back(16); v.push_back(2); put16(v, 0);
		const u8* b = reinterpret_cast<const u8*>(&net_order_ip);
		v.insert(v.end(), b, b + 4);
		v.insert(v.end(), 8, u8{0});
	};

	const auto add_route = [&](u32 dst, u32 gw, u32 mask, u32 flags, u16 index)
	{
		std::vector<u8> m;
		put16(m, 0); m.push_back(3); m.push_back(4 /* RTM_GET */); put16(m, index); put16(m, 0);
		put32(m, flags); put32(m, 0x1 | 0x2 | 0x4 /* RTA_DST | RTA_GATEWAY | RTA_NETMASK */);
		m.resize(120);
		sockaddr_in(m, dst);
		sockaddr_in(m, gw);
		sockaddr_in(m, mask);
		m[0] = static_cast<u8>(m.size() >> 8); m[1] = static_cast<u8>(m.size());
		out.insert(out.end(), m.begin(), m.end());
	};

	// default route via the gateway: RTF_UP | RTF_GATEWAY | RTF_STATIC
	add_route(0, cfg.gateway, 0, 0x1 | 0x2 | 0x800, 2);
	// directly attached network: RTF_UP | RTF_CLONING
	add_route(cfg.ip & cfg.mask, cfg.ip, cfg.mask, 0x1 | 0x100, 2);
	return out;
}

error_code sys_net_bnet_sysctl(ppu_thread& ppu, u32 arg1, u32 arg2, u32 arg3, vm::ptr<void> arg4, u32 arg5, u32 arg6)
{
	ppu.state += cpu_flag::wait;

	sys_net.todo("sys_net_bnet_sysctl(0x%x, 0x%x, 0x%x, *0x%x, 0x%x, 0x%x)", arg1, arg2, arg3, arg4, arg5, arg6);

	// (name = arg1, namelen = arg2, oldp = arg3, oldlenp = arg4, newp = arg5, newlen = arg6)
	if (arg1 && arg2 == 6)
	{
		const auto mib = vm::_ptr<const be_t<s32>>(arg1);
		sys_net.notice("sys_net_bnet_sysctl(): mib = {%d, %d, %d, %d, %d, %d} oldp=0x%x oldlenp=%llu", mib[0], mib[1], mib[2], mib[3], mib[4], mib[5], arg3, arg4 ? +*vm::static_ptr_cast<be_t<u64>>(arg4) : u64{0});

		if (mib[0] == 4 /* CTL_NET */ && mib[1] == 17 /* PF_ROUTE */ && mib[4] == 4 /* NET_RT_IFLIST (NetBSD numbering: 3 is the old format) */)
		{
			const std::vector<u8> data = net_build_iflist();
			const auto oldlenp = vm::static_ptr_cast<be_t<u64>>(arg4); // size_t is 64-bit on the guest
			sys_net.notice("sys_net_bnet_sysctl(): NET_RT_IFLIST -> %u bytes (oldlenp=0x%x)", data.size(), arg4.addr());

			if (oldlenp)
			{
				const u64 capacity = *oldlenp;
				*oldlenp = static_cast<u64>(data.size());

				if (arg3)
				{
					if (capacity < data.size())
					{
						return -SYS_NET_ENOMEM;
					}

					std::memcpy(vm::base(arg3), data.data(), data.size());
				}
			}

			return CELL_OK;
		}

		if (mib[0] == 4 /* CTL_NET */ && mib[1] == 17 /* PF_ROUTE */ && mib[4] == 1 /* NET_RT_DUMP */)
		{
			const std::vector<u8> data = net_build_route_dump(mib[3]);
			const auto oldlenp = vm::static_ptr_cast<be_t<u64>>(arg4);
			sys_net.notice("sys_net_bnet_sysctl(): NET_RT_DUMP af=%d -> %u bytes", mib[3], data.size());

			if (oldlenp)
			{
				const u64 capacity = *oldlenp;
				*oldlenp = static_cast<u64>(data.size());

				if (arg3)
				{
					if (capacity < data.size())
					{
						return -SYS_NET_ENOMEM;
					}

					std::memcpy(vm::base(arg3), data.data(), data.size());
				}
			}

			return CELL_OK;
		}
	}

	return CELL_OK;
}

error_code sys_net_eurus_post_command(ppu_thread& ppu, s32 arg1, u32 arg2, u32 arg3)
{
	ppu.state += cpu_flag::wait;

	sys_net.todo("sys_net_eurus_post_command(%d, 0x%x, 0x%x)", arg1, arg2, arg3);
	return CELL_OK;
}
