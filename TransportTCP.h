#pragma once
#include "RiskManager.h"

namespace RM
{
class CClientPeer
: public Sys::CSocketConnection
, public CTransport
{
public:
	CClientPeer(Sys::CSocket &&sock, CRiskManager &rm)
	: Sys::CSocketConnection(std::move(sock))
	{
		Log.Debug("Accept", sock);
		m_parser.reserve(32);

#define TS_ITEM(name) m_cb##name = RegisterCallback(#name, &RM::CRiskManager::PutMessage<RM::S##name>, &rm);
		RM_OBJECTS
#undef TS_ITEM
	}

	~CClientPeer()
	{
		Log.Debug("Disconnect", m_sock, std::chrono::steady_clock::now() - m_tm);
	}

	virtual void SendMessage(const CMessage::TAttrs &attrs)
	{
		if (attrs.empty())
			return;

        std::stringstream stm;
		TS::FormatVal(stm, attrs.front().first) << '\1';
		for (auto it = attrs.begin() + 1, end = attrs.end(); it != end; ++it)
			TS::FormatVals<0>(stm, it->first, '=', it->second) << '\1';

		Sys::CSocketConnection::Send(stm);
	};



protected:
	virtual void SendClose() noexcept override
	{
		TS_NOEXCEPT(m_sock.Send("\0", 1));
	}

	virtual bool ParseDataChunk(char *data, size_t sz) override
	{
		m_parser.DoParse(data, sz, [this](auto &&attrs)
		{
			if (attrs.size() < 2)
				return;

			auto sp = std::make_shared<RM::CMessage>(std::move(attrs));
			this->DispatchMessage(std::move(sp));
		});
		return true;
	}

	TS::CKeyValueParser<CMessage::TAttrs, '\1', '\0'> m_parser;

#define TS_ITEM(name) CTransport::TCallbackPtr m_cb##name;
	RM_OBJECTS
#undef TS_ITEM

	const std::chrono::steady_clock::time_point m_tm{std::chrono::steady_clock::now()};
};


class CSocketServer
: public Sys::CSocketServer
{
public:
	CSocketServer(CRiskManager &rm)
	: m_rm(rm)
	{
	}

	void Start(u_short port)
	{
		Log.Info("Listen", port);
		Sys::CSocketServer::Start(port);
	}
protected:
	virtual std::shared_ptr<Sys::CSocketConnection> CreateClientPeer(Sys::CSocket &&sock) override
	{
		auto sp = std::make_shared<CClientPeer>(std::move(sock), m_rm);
		return sp;
	}

	CRiskManager &m_rm;
};

}
