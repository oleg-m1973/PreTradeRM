#pragma once
#include "SyncObjs.h"

#include <unistd.h>

#include <sys/select.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <poll.h>

#include <chrono>


inline
std::ostream &operator <<(std::ostream &out, const sockaddr_in &val)
{
	out << '{' << ::inet_ntoa(val.sin_addr) << ':' << ntohs(val.sin_port) << '}';
    return out;
}

inline
std::ostream &operator <<(std::ostream &out, const in_addr &val)
{
	out << ::inet_ntoa(val);
    return out;
}

namespace Sys
{
#define SOCK_VERIFY(expr, ...) SockVerify(expr, TS_FILE_LINE, #expr, ##__VA_ARGS__)

template <typename... TT> inline
int SockVerify(const int res, TS::FileLine file_line, TT&&... args)
{
	const int err = errno;
	if (res < 0)
		Sys::RaiseError("Socket Error", Sys::Error(err), std::forward<TT>(args)..., file_line);

	return res;
}

class CSocket
{
public:
	typedef std::pair<std::string, u_short> THostPort;

	CSocket(int type = SOCK_STREAM)
	: m_type(type)
	{
	}

	CSocket(const char *addr, int type = SOCK_STREAM)
	: m_type(type)
	{
		MakeAddress(addr, 0, m_addr);
	}

	CSocket(int type, int sock, const sockaddr_in &addr)
	: m_addr(addr)
	, m_type(type)
	, m_sock(sock)
	{
	}

	CSocket(CSocket &&src) noexcept
	: m_addr(src.m_addr)
	, m_addr2(src.m_addr2)
	, m_type(src.m_type)
	, m_sock(src.m_sock)
	{
		src.m_sock = -1;
	}

	~CSocket()
	{
		if (m_sock != -1)
			::close(m_sock);
	}

	static THostPort GetHostPort(const char *host, u_short port = 0)
	{
		const char *p = strchr(host, ':');
		CSocket::THostPort res(std::string(host, std::distance(host, p)), port);
		if (*p)
		{
			port = strtol(p + 1, nullptr, 10);
			if (port)
				res.second = port;
		}

		return std::move(res);
	}

	static void MakeAddress(const THostPort &host, sockaddr_in &addr, const int type)
	{
		addr = {0};

		addr.sin_family = AF_INET;
		addr.sin_port = htons(host.second);
		addr.sin_addr.s_addr = GetHostAddress(host.first.c_str(), type);
	}

	static void MakeAddress(const char *host, u_short port, sockaddr_in &addr, const int type)
	{
		auto hp = GetHostPort(host, port);
		MakeAddress(hp, addr, type);
	}

	void MakeAddress(const THostPort &host, sockaddr_in &addr)
	{
		MakeAddress(host, addr, m_type);
	}

	void MakeAddress(const char *host, u_short port, sockaddr_in &addr)
	{
		MakeAddress(host, port, addr, m_type);
	}

	void MakeAddress(const std::string &host, u_short port, sockaddr_in &addr)
	{
		MakeAddress(host.c_str(), port, addr, m_type);
	}

	void Close() noexcept
	{
        SYS_LOCK(m_mxSend);
        SYS_LOCK(m_mxRecv);

        _Close();
	}

    CSocket &Connect(const sockaddr_in &addr)
	{
		return Open(addr, [this, &addr](const int sock)
		{
			const auto connect_ = ::connect(sock, (const sockaddr *)&addr, sizeof(addr));
			if (connect_ >= 0 || errno != EINPROGRESS)
				SOCK_VERIFY(connect_, addr, sock);

			socklen_t sz = sizeof(m_addr2);
			::getsockname(sock, (sockaddr *)&m_addr2, &sz);
		});
	}

    CSocket &Connect(const char *host, u_short port = 0)
    {
        sockaddr_in addr = {0};
        MakeAddress(host, port, addr, m_type);
        return Connect(addr);
    }

	CSocket &Connect()
	{
		return Connect(m_addr);
	}

	CSocket &Bind(const u_short port, const char *host = nullptr)
	{
		sockaddr_in addr = {0};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = GetHostAddress(host, m_type);
		addr.sin_port = htons(port);

		return Open(addr, [&addr](const int sock)
		{
			SOCK_VERIFY(::bind(sock, (const sockaddr *)&addr, sizeof(addr)), addr, sock);
		});
	}

	CSocket &AddMembership(const char *multiaddr, const char *interface = nullptr)
	{
		ip_mreq mreq = {0};

		mreq.imr_multiaddr.s_addr = GetHostAddress(multiaddr, m_type);
		mreq.imr_interface.s_addr = GetHostAddress(interface, m_type);

		SetSockOpt(IPPROTO_IP, IP_ADD_MEMBERSHIP, mreq);
		return *this;
	}

	CSocket &Listen()
	{
		SOCK_VERIFY(::listen(GetHandle(), SOMAXCONN), m_addr);
		return *this;
	}

	CSocket &Listen(const u_short port)
	{
		Bind(port);
		Listen();
		return *this;
	}

	static int Poll(pollfd *fds, size_t n, const std::chrono::milliseconds &tm)
	{
		const int res = SOCK_VERIFY(::poll(fds, n, tm.count()), n);
		return res;
	}


	CSocket Accept()
	{
		sockaddr_in addr = {0};
		socklen_t len = sizeof(addr);
		int sock = SOCK_VERIFY(::accept(GetHandle(), (sockaddr *)&addr, &len), m_sock);
		return CSocket(m_type, sock, addr);
	}


	bool WaitSend(const std::chrono::milliseconds &tm)
	{
		SYS_LOCK(m_mxSend);
		return Wait(POLLOUT, tm) & POLLOUT;
	}

	bool WaitRecv(const std::chrono::milliseconds &tm)
	{
		SYS_LOCK(m_mxRecv);
		return Wait(POLLIN, tm) & POLLIN;
	}

	short int Wait(short int events, const std::chrono::milliseconds &tm, bool RaiseError = true)
	{
		pollfd fd = {0};
		fd.fd = GetHandle();
		fd.events = events;

		const int res = SOCK_VERIFY(::poll(&fd, 1, tm.count()), m_addr, m_sock, fd.events);
		if (!res)
			return 0;

		if (fd.revents & (POLLERR | POLLHUP | POLLNVAL) && RaiseError)
		{
			int err = 0;
			if (fd.revents | POLLERR)
			{
				socklen_t ln = sizeof(err);
				getsockopt(fd.fd, SOL_SOCKET, SO_ERROR, (void *)&err, &ln);
			}

			TS_RAISE_ERROR("Socket Error", __FUNCTION__, m_addr, Sys::Error(err));
		}

		return fd.revents;
	}


    CSocket &operator =(CSocket &&src) noexcept
    {
    	if (&src != this)
		{
			std::swap(m_addr, src.m_addr);
			std::swap(m_addr2, src.m_addr2);
			std::swap(m_type, src.m_type);
			std::swap(m_sock, src.m_sock);
		}
        return *this;
    }

    size_t Send(const void *data, size_t sz)
	{
		SYS_LOCK(m_mxSend);
		const int send = ::send(GetHandle(), data, sz, MSG_NOSIGNAL);
		if (send >= 0)
			return size_t(send);

		if (errno == EAGAIN)
			return 0;

		SOCK_VERIFY(send, sz, m_addr, m_sock);
		return size_t(-1);
	}

    size_t Recv(void *buf, size_t sz)
	{
		SYS_LOCK(m_mxRecv);

		const int res = SOCK_VERIFY(::recv(GetHandle(), buf, sz, MSG_NOSIGNAL), sz, m_addr, m_sock);
		return size_t(res);
	}

	size_t SendTo(const sockaddr_in &addr, const void *data, size_t sz, int flags = 0)
	{
		SYS_LOCK(m_mxSend);
		const int res = SOCK_VERIFY(::sendto(GetHandle(), data, sz, flags, (const sockaddr *)&addr, sizeof(addr)), sz, addr, m_sock, flags);
		return size_t(res);
	}

	size_t SendTo(const void *data, size_t sz, int flags = 0)
	{
		return SendTo(m_addr, data, sz, flags);
	}

	size_t RecvFrom(sockaddr_in &addr, void *buf, size_t sz, int flags = 0)
	{
		SYS_LOCK(m_mxRecv);
		socklen_t addr_len = sizeof(addr);
		const size_t res = SOCK_VERIFY(::recvfrom(GetHandle(), buf, sz, flags, (sockaddr *)&addr, &addr_len), m_addr, m_sock, sz, flags);
		return res;
	}

    size_t Recv2(void *buf, size_t sz)
    {
    	const size_t n = GetRecvSize();
    	if (!n)
			return 0;

		return Recv(buf, std::min(sz, n));
    }

	template <typename T>
	size_t Send(T &&obj)
	{
		return Send(&obj, sizeof(obj));
	}

	template <typename T>
	size_t SendTo(const sockaddr_in &addr, T &&obj)
	{
		return SendTo(addr, &obj, sizeof(obj));
	}

	template <typename T>
	size_t SendTo(T &&obj)
	{
		return SendTo(m_addr, obj);
	}

	size_t RecvFrom(void *buf, size_t sz, int flags = 0)
	{
		sockaddr_in addr = {0};
		return RecvFrom(addr, buf, sz, flags);
	}

    size_t GetRecvSize() const
	{
		SYS_LOCK(m_mxSend);
		SYS_LOCK(m_mxRecv);
		return _GetRecvSize();
	}

    CSocket &SetSockOpt(const int level, const int optname, const void *val, const socklen_t sz)
	{
		SYS_LOCK(m_mxSend);
		SYS_LOCK(m_mxRecv);
		SOCK_VERIFY(::setsockopt(GetHandle(), level, optname, val, sz), m_addr, m_sock, level, optname, sz);
		return *this;
	}

	template <class T>
	CSocket &SetSockOpt(const int level, const int optname, const T &val)
	{
		return SetSockOpt(level, optname, &val, sizeof(val));
	}

	void SetTimeout(const std::chrono::microseconds &tm)
	{
		const timeval tv = {std::chrono::duration_cast<std::chrono::seconds>(tm).count(), tm.count() % std::chrono::microseconds::period::den};
		SetSockOpt(SOL_SOCKET, SO_RCVTIMEO, tv);
		SetSockOpt(SOL_SOCKET, SO_SNDTIMEO, tv);
	}

	void GetSockOpt(const int level, const int optname, void *val, socklen_t sz)
	{
		SYS_LOCK(m_mxSend);
		SYS_LOCK(m_mxRecv);
		SOCK_VERIFY(::getsockopt(GetHandle(), level, optname, val, &sz), m_addr, m_sock, level, optname, sz);
	}

	template <class T>
	T GetSockOpt(const int level, const int optname)
	{
		T val;
		GetSockOpt(level, optname, &val, sizeof(val));
		return val;
	}

	explicit operator bool() const
	{
		return m_sock != -1;
	}

    explicit operator int() const
    {
        return m_sock;
    }

    int fd() const
    {
        return m_sock;
    }

    const sockaddr_in &GetAddr() const
    {
        return m_addr;
    }

    const sockaddr_in &GetAddr2() const
    {
        return m_addr2;
    }

    const int &GetType() const
    {
        return m_type;
    }

    size_t ClearRecvBuffer()
    {
    	const size_t _sz = 1024;
    	char buf[_sz];

		size_t res = 0;
    	size_t n = std::min(GetRecvSize(), _sz);
    	while (n)
		{
			res += Recv(buf, n);
			n = std::min(GetRecvSize(), _sz);
		}
		return res;
    }

   	CSocket &SetRecvBufferSize(uint32_t sz)
	{
		return SetSockOpt(SOL_SOCKET, SO_RCVBUF, sz);
	}

	CSocket &SetSendBufferSize(uint32_t sz)
	{
		return SetSockOpt(SOL_SOCKET, SO_SNDBUF, sz);
	}

	uint32_t GetRecvBufferSize()
	{
		return GetSockOpt<uint32_t>(SOL_SOCKET, SO_RCVBUF);
	}

	uint32_t GetSendBufferSize()
	{
		return GetSockOpt<uint32_t>(SOL_SOCKET, SO_SNDBUF);
	}

	bool CheckClosed()
	{
		SYS_LOCK(m_mxRecv);
		return m_sock == -1 || (Wait(POLLIN, std::chrono::milliseconds(0)) && _GetRecvSize() == 0);
	}


protected:
	template <typename T>
	static int _SetSockOpt(int sock, const int level, const int optname, const T &val)
	{
		return ::setsockopt(sock, level, optname, &val, sizeof(val));
	}

	std::pair<int, sockaddr_in> _Accept();
	int GetHandle() const
	{
		if (m_sock == -1 && m_type != 0)
			m_sock = SOCK_VERIFY(::socket(AF_INET, m_type, 0), m_type);
		return m_sock;
	}

	static in_addr_t GetHostAddress(const char *host, const int type)
	{
		if (!host)
			return INADDR_ANY;

		if (std::isdigit(*host, std::locale::classic()))
			return ::inet_addr(host);

		static std::mutex _mx;
		SYS_LOCK(_mx);
		addrinfo *pai = NULL;

		addrinfo hint = {0};

		hint.ai_socktype = type & 0xFF;
		hint.ai_family = AF_INET;

		{
			const int res = ::getaddrinfo(host, NULL, &hint, &pai);
			if (res != 0)
				TS_RAISE_ERROR("getaddrinfo", res, ::gai_strerror(res), host, type);
		}

		const in_addr_t res = ((sockaddr_in *)pai->ai_addr)->sin_addr.s_addr;

		::freeaddrinfo(pai);

		return res;
	}

    void _Close() noexcept
	{
		try
		{
			if (m_sock != -1)
			{
				::shutdown(m_sock, SHUT_RDWR);
				::close(m_sock);
				m_sock = -1; //m_type != 0? ::socket(AF_INET, m_type, 0): -1;
			}
		}
		catch(...)
		{
		}
	}

	template <class TFunc> CSocket &Open(const sockaddr_in &addr, TFunc func)
	{
		SYS_LOCK(m_mxSend);
		SYS_LOCK(m_mxRecv);

		func(GetHandle());

		m_addr = addr;

		return *this;
	}

	size_t _GetRecvSize() const
	{
		int n = 0;
		SOCK_VERIFY(::ioctl(GetHandle(), FIONREAD, &n), m_addr, m_sock);
		return size_t(n);
	}


	mutable std::mutex m_mxSend;
	mutable std::mutex m_mxRecv;

	sockaddr_in m_addr{0};
	sockaddr_in m_addr2{0};

    int m_type = 0;
	mutable int m_sock = -1;

public:
	void FormatVal(TS::TFormatOutput &out) const
	{
		if (!m_addr2.sin_port)
			TS::FormatVal(out, m_addr);
		else
			TS::FormatVals<0>(out, m_addr, "<-", m_addr2);
	}
};


}

inline
std::ostream &operator <<(std::ostream &out, const Sys::CSocket &sock)
{
	sock.FormatVal(out);
    return out;
}
