#pragma once
#include "Transport.h"

#include "Common/CallbackManager.h"
#include "Common/Parser.h"
#include "Common/SocketServer.h"
#include "Common/Config.h"

#include <algorithm>

namespace RM
{
class CTransport;

struct CMessage
{
	typedef std::vector<std::pair<std::string, std::string>> TAttrs;

	CMessage(TAttrs &&attrs)
	: m_attrs(std::move(attrs))
	{
		if (m_attrs.empty())
			m_attrs.emplace_back(TAttrs::value_type());
		else
			std::sort(m_attrs.begin() + 1, m_attrs.end(), [](const auto &val1, const auto &val2)
			{
				return val1.first < val2.first;
			});
	}

	const auto &GetID() const
	{
		return m_attrs.front();
	}

	template <typename T>
	T GetAttr(const std::string &name) const
	{
		auto it = FindAttr(name);
		return it == m_attrs.end()? T(): TS::Parse<T>(it->second.c_str());
	}

	template <typename T>
	bool GetAttr(const std::string &name, T &dst) const
	{
		auto it = FindAttr(name);
		if (it == m_attrs.end())
			return false;

		TS::Parse(it->second.c_str(), dst);
		return true;
	}

	auto FindAttr(const std::string &name) const
	{
		auto it = std::lower_bound(m_attrs.begin() + 1, m_attrs.end(), name, [](const auto &item, const auto &name)
		{
			return item.first < name;
		});

		if (it == m_attrs.end() || name < it->first)
			return m_attrs.end();

		return it;
	}

	const std::string &GetAttr(const std::string &name) const
	{
		auto it = FindAttr(name);
		if (it != m_attrs.end())
			return it->second;

		static const std::string _s;
		return _s;
	}

	TAttrs m_attrs;
};

typedef TS::CCallbackManager<void(CTransport &, const std::shared_ptr<const CMessage> &), std::string> TTransportCallbackManager;

class CTransport
: protected TTransportCallbackManager
{
public:
	using TTransportCallbackManager::TCallbackPtr;
	using TTransportCallbackManager::RegisterCallback;

	static std::unique_ptr<CTransport> Create(const std::string &name);

	virtual ~CTransport()
	{
	}

	void DispatchMessage(const std::shared_ptr<const CMessage> &sp)
	{
		auto &msg = *sp;
		ForEachCallback(msg.m_attrs.front().first, [this, &sp](auto &fn)
		{
			fn(*this, sp);
		});
	}

	virtual void SendMessage(const CMessage::TAttrs &attrs)
	{
	};
protected:
};


}
