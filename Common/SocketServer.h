#pragma once
#include "Socket.h"
#include "Thread.h"
#include "SharedPtr.h"

#include <deque>
#include <unordered_map>
#include <vector>
#include <algorithm>

#include <sys/epoll.h>

namespace Sys
{
class CSocketCnnManager;

class CSocketConnection
{
friend class CSocketCnnManager;
public:
	TS_COPYABLE(CSocketConnection, delete);

	CSocketConnection(Sys::CSocket &&sock)
	: m_sock(std::move(sock))
	{
	}

	virtual ~CSocketConnection()
	{
	}

	void Send(const char *data, size_t sz)
	{
		const size_t n = m_sock.Send(data, sz);
		if (n != sz)
			TS_RAISE_ERROR("Send data failed", Sys::Error(ENOBUFS), m_sock, sz, n);
	}

	void SendData(const char *data, const size_t sz)
	{
		static const size_t _sz = 1024;

		const auto tm = std::chrono::system_clock::now() + std::chrono::minutes(5);
		auto sz2 = sz;
 		for (;;)
		{
			const size_t n = m_sock.Send(data, std::min(sz2, _sz));
			if (n == sz2)
				break;
			else if (n)
			{
				sz2 -= n;
				data += n;
				continue;
			}

			if (tm <= std::chrono::system_clock::now())
				TS_RAISE_ERROR("Send data timeout", m_sock, sz, sz2);

			m_sock.WaitSend(1s);
		}
	}

	template <typename TStream> //requires std::is_base_of<std::istream, std::decay_t<TStream>>::value
	void Send(TStream &&stm)
	{
		stm << '\0';
		auto *p = stm.rdbuf();
		if (!p)
			return;

		static const size_t _sz = 1024;
        char buf[_sz];
        for(;;)
		{
            const auto n = p->sgetn(buf, _sz);

            if (!n)
				break;

            SendData(buf, n);
		}
	}

	virtual void SendClose() noexcept
	{
	}
protected:
	virtual bool ParseDataChunk(char *data, size_t sz) = 0;

	bool HasData() const
	{
		return !m_data.empty();
	}

	bool ProcessRecv(size_t n) noexcept
	{
		try
		{
			std::unique_ptr<char[]> sp(new char[n]);
			const auto n2 = m_sock.Recv(sp.get(), n);
			if (n2 != n)
				return false;

			SYS_LOCK(m_data);
			m_data.emplace_back(std::make_pair(std::move(sp), n));
			return true;
		}
		TS_CATCH;
		return false;
	}

	bool ProcessParse(Sys::CThreadControl &thread)
	{
		auto data = Sys::Locked::Move(m_data);
		for (;!data.empty() && !thread.IsStop(); data.pop_front())
		{
			auto item = std::move(data.front());
			if (!ParseDataChunk(item.first.get(), item.second))
				return false;
		}
		return true;
	}

	Sys::CSocket m_sock;
	Sys::CLockedObject<std::deque<std::pair<std::unique_ptr<char[]>, size_t>>> m_data;

	std::mutex m_mxParse;
private:
	std::weak_ptr<CSocketConnection> m_sp;
};

class CSocketCnnManager
{
public:
	CSocketCnnManager()
	{
	}

	~CSocketCnnManager()
	{
		_Stop();
	}

	void Start()
	{
		_Stop();

		m_epoll = SYS_VERIFY(::epoll_create1(0));

		m_thParse.Start(&CSocketCnnManager::ParseThreadProc, this);
		m_thWait.Start(&CSocketCnnManager::WaitThreadProc, this);
	}

	void Stop() noexcept
	{
		_Stop();
	}

	template <typename T>
	auto AddConnection(std::shared_ptr<T> sp)
	{
		SYS_LOCK(m_cnns);
		auto &cnn = *sp;
		cnn.m_sp = sp;
		m_changes.emplace_back(EPOLL_CTL_ADD, sp);
		m_cnns.emplace(&cnn, sp);
		m_evCnns.Set();
		return sp;
	}

	template <typename T, typename... TT>
	auto AddConnection(TT&&... args)
	{
		return AddConnection(std::make_shared<T>(std::forward<TT>(args)...));
	}

	size_t GetCnnsCount() const
	{
		SYS_LOCK(m_cnns);
		return m_cnns.size();
	}

	template <typename... TT>
	void ResetConnection(CSocketConnection &cnn, TS::FileLine file_line, TT&&... args) noexcept
	{
		DestroyConnection(&cnn, file_line);
	}

protected:
	void _Stop() noexcept
	{
		m_thWait.Stop();
		m_thParse.Stop();

		m_thread_pool.Stop();

		SYS_LOCK(m_cnns);
		SYS_LOCK(m_recvs);

		for (auto &item: m_cnns)
			item.second->SendClose();

		m_cnns.clear();
		m_recvs.clear();
		m_changes.clear();

		if (m_epoll != -1)
		{
			::close(m_epoll);
			m_epoll = -1;
		}
	}

	void DestroyConnection(CSocketConnection *cnn, TS::FileLine file_line) noexcept
	{
		SYS_LOCK(m_cnns);
		auto it = m_cnns.find(cnn);
		if (it == m_cnns.end())
			return;

		m_changes.emplace_back(EPOLL_CTL_DEL, std::move(it->second));
		m_cnns.erase(it);
	}

	auto GetConnections() const
	{
		std::list<std::weak_ptr<CSocketConnection>> res;
		SYS_LOCK(m_cnns);
		for (auto &item: m_cnns)
			res.emplace_back(item.second);
		return std::move(res);
	}

	bool ProcessChanges() noexcept
	{
		auto items = Sys::Locked::Move(m_changes, m_cnns);
		if (items.empty())
			return true;

		for (auto &item: items)
		{
			auto *p = item.second.get();

			epoll_event ev = {0};
			ev.events = EPOLLIN;
			ev.data.ptr = p;
			::epoll_ctl(m_epoll, item.first, p->m_sock.fd(), &ev);

			if (item.first == EPOLL_CTL_DEL)
				p->m_sock.Close();
		}

		SYS_LOCK(m_cnns);
		if (!m_cnns.empty())
			return true;

		m_evCnns.Reset();
		return false;
	}

	void ProcessRecv(std::shared_ptr<CSocketConnection> sp, size_t n) noexcept
	{
		if (!sp->ProcessRecv(n))
			ResetConnection(*sp, TS_FILE_LINE);
		else
		{
			SYS_LOCK(m_recvs);
			TS_NOEXCEPT(m_recvs.emplace_back(std::move(sp)));
			m_evRecv.Set();
		}
	}

	void WaitThreadProc(Sys::CThreadControl &thread)
	{
		static const int _n = 64;
		epoll_event events[_n];

		while (thread.Wait(m_evCnns) != nullptr)
		{
			if (!ProcessChanges())
				continue;

			const auto n = epoll_wait(m_epoll, events, _n, 100);

			if (n < 0)
				continue;

			std::list<CSocketConnection *> resets;
			for (int i = 0; i < n; ++i)
			{
				auto &ev = events[i];
				auto &cnn = *reinterpret_cast<CSocketConnection *>(ev.data.ptr);
				if (!(ev.events & EPOLLIN))
				{
					resets.emplace_back(&cnn);
					continue;
				}

				try
				{
					auto sp = cnn.m_sp.lock();
					const size_t n = cnn.m_sock.GetRecvSize();
					if (!n)
						resets.emplace_back(&cnn);
					else
						ProcessRecv(std::move(sp), n);
					continue;
				}
				TS_CATCH;
				resets.emplace_back(&cnn);
			}

			for (auto &item: resets)
				DestroyConnection(item, TS_FILE_LINE);
		}
	}

	void ParseThreadProc(Sys::CThreadControl &thread)
	{
		std::list<std::weak_ptr<CSocketConnection>> recvs;
		while (thread.Wait(recvs.empty(), 10ms, m_evRecv) != nullptr)
		{
			Sys::Locked::SpliceFront(recvs, m_recvs);
			recvs.remove_if([this, &thread](auto &item)
			{
				auto sp = item.lock();
				if (!sp || thread.IsStop())
					return true;

				auto lock = Sys::UniqueLock(sp->m_mxParse, std::defer_lock);
				if (!lock.try_lock())
					return false;

				if (sp->HasData())
					m_thread_pool.RunAnyway([this, sp = std::move(sp), lock = std::move(lock)](Sys::CThreadControl &thread) mutable
					{
						auto lock2 = std::move(lock);
						try
						{
							if (sp->ProcessParse(thread))
								return;
						}
						TS_CATCH;
						this->ResetConnection(*sp, TS_FILE_LINE);
					});

				return true;
			});
		}
	}

	int m_epoll = -1;
	mutable Sys::CLockedObject<std::unordered_map<CSocketConnection *, std::shared_ptr<CSocketConnection>>> m_cnns;
	std::list<std::pair<int, std::shared_ptr<CSocketConnection>>> m_changes;

	Sys::CLockedObject<std::list<std::weak_ptr<CSocketConnection>>> m_recvs;

	Sys::CThread m_thWait;
	Sys::CThread m_thParse;

	Sys::CEvent<true> m_evCnns{m_thWait};
	Sys::CEvent<false> m_evRecv{m_thParse};

	Sys::CThreadPool m_thread_pool;
};

class CSocketServer
: public CSocketCnnManager
{
public:
	CSocketServer()
	{
	}

	virtual ~CSocketServer()
	{
		m_thread.Stop();
	}

	void Start(const u_short port)
	{
		m_port = port;
		if (!m_port)
			return;

		CSocketCnnManager::Start();

		m_sock.Listen(m_port);
		m_thread.Start([this](Sys::CThreadControl &thread)
		{
			while (!thread.IsStop())
				TS_NOEXCEPT(DoListen());
		});
	}

	void Stop() noexcept
	{
		m_thread.Stop();
		CSocketCnnManager::Stop();
	}

protected:
	virtual std::shared_ptr<CSocketConnection> CreateClientPeer(Sys::CSocket &&sock) = 0;

	static void SetSocketOptions(Sys::CSocket &sock)
	{
		sock.SetSockOpt<int>(SOL_SOCKET, SO_REUSEADDR, 1);
		sock.SetSockOpt<int>(SOL_SOCKET, SO_KEEPALIVE, 1);

		sock.SetSockOpt<int>(SOL_TCP, TCP_KEEPCNT, 30);
		sock.SetSockOpt<int>(SOL_TCP, TCP_KEEPIDLE, 30);
		sock.SetSockOpt<int>(SOL_TCP, TCP_KEEPINTVL, 1);
	}

	void DoListen()
	{
		if (m_sock.WaitRecv(std::chrono::milliseconds(100)))
		{
			auto sock2 = m_sock.Accept();
			SetSocketOptions(sock2);
			auto sp = CreateClientPeer(std::move(sock2));
			AddConnection(std::move(sp));
		}
	}

	u_short m_port = 0;
	Sys::CSocket m_sock{SOCK_STREAM};

	Sys::CThread m_thread;
};

template <typename TClientPeer>
class CSocketServer2
: public CSocketServer
{
public:
protected:
	typedef CSocketServer2 TSocketServer;

	virtual std::shared_ptr<CSocketConnection> CreateClientPeer(Sys::CSocket &&sock) override
	{
		return std::make_shared<TClientPeer>(std::move(sock));
	}
};

}

namespace TS
{
class CDataBuffer
{
public:

    static const size_t _MaxDataLen = 1024 * 1024 * 1024;
	CDataBuffer(char fin = 0)
	: m_fin(fin)
	{
		m_data.reserve(1024);
	}

    template <typename TFunc>
    bool ParseDataChunk(char *data, size_t sz, TFunc &&func)
    {
    	auto *end = data + sz;
    	for (auto *p = data; p < end; ++p)
		{
			if (*p != m_fin)
				continue;

			if (m_data.empty())
				func(data, std::distance(data, p));
			else
			{
				m_data.insert(m_data.end(), data, p);
				func(m_data.data(), m_data.size());
				m_data.clear();
			}

			data = p + 1;
		}

		if (data < end)
			m_data.insert(m_data.end(), data, end);

		return true;
    }

    void ResetDataBuffer()
    {
    	m_data.clear();
    }
protected:

	const char m_fin = 0;
    std::vector<char> m_data;
};

class CSocketClientPeer
: public Sys::CSocketConnection
{
public:
	using Sys::CSocketConnection::CSocketConnection;

	virtual void SendClose() noexcept override
	{
		TS_NOEXCEPT(m_sock.Send("\0", 1));
	}

protected:
	virtual void ProcessMessage(char *data, size_t sz) = 0;
	virtual bool ParseDataChunk(char *data, size_t sz) override
	{
		return m_data.ParseDataChunk(data, sz, [this](char *data, const size_t sz)
		{
			ProcessMessage(data, sz);
			return true;
		});
	}

	TS::CDataBuffer m_data;
};

class CSocketClientPeer2
: public Sys::CSocketConnection
{
public:
	template <typename TFunc, typename... TT>
	CSocketClientPeer2(Sys::CSocket &&sock, TFunc &&func, TT&&... args)
	: CSocketConnection(std::move(sock))
	, m_fn(std::bind(std::forward<TFunc>(func), std::forward<TT>(args)..., _1, _2, _3))
	{
	}

	virtual void SendClose() noexcept override
	{
		TS_NOEXCEPT(m_sock.Send("\0", 1));
	}

protected:
	virtual bool ParseDataChunk(char *data, size_t sz) override
	{
		return m_data.ParseDataChunk(data, sz, [this](char *data, const size_t sz)
		{
			m_fn(*this, data, sz);
			return true;
		});
	}

	TS::CDataBuffer m_data;
	TS::CFunction<void(Sys::CSocketConnection &, char *, size_t)> m_fn;
};

}
