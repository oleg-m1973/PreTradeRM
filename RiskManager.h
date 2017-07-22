#pragma once
#include "Common/Errors.h"
#include "Common/CallbackManager.h"
#include "Common/Config.h"

#include "Transport.h"

#include <chrono>
#include <unordered_map>

#define RM_CHECK_ORDER_RULES \
	TS_ITEM(NewOrderMoratorium) \
	TS_ITEM(PriceCheck) \
	TS_ITEM(SeqBadTrades) \
	TS_ITEM(DrawDown) \

#define RM_DECLARE_RULE_(name, class_name) namespace RM {struct _##name {}; \
	template <> std::unique_ptr<COrderCheckRule> CreateRule<_##name>(CRiskManager &rm, const TS::CConfigFile &cfg) {return std::make_unique<class_name>(rm, cfg);}}

#define RM_DECLARE_RULE(name) RM_DECLARE_RULE_(name, C##name)

#define RM_QUOTE \
	TS_ITEM(symbol, TSymbol) \
	TS_ITEM(price, TPrice) \
	TS_ITEM(time, TDateTime) \

#define RM_TRADE \
	TS_ITEM(trade_id, TTradeID) \
	TS_ITEM(user_id, TUserID) \
	TS_ITEM(symbol, TSymbol) \
	TS_ITEM(side, TSide) \
	TS_ITEM(price, TPrice) \
	TS_ITEM(qty, TQty) \
	TS_ITEM(time, TDateTime) \

#define RM_ORDER \
	TS_ITEM(order_id, TOrderID) \
	TS_ITEM(user_id, TUserID ) \
	TS_ITEM(type, TOrderType) \
	TS_ITEM(symbol, TSymbol) \
	TS_ITEM(side, TSide) \
	TS_ITEM(price, TPrice) \
	TS_ITEM(qty, TQty) \
	TS_ITEM(time, TDateTime) \

#define RM_OBJECTS \
	TS_ITEM(Quote) \
	TS_ITEM(Trade) \
	TS_ITEM(Order) \

namespace RM
{
class CRiskManager;
class COrderCheckRule;

typedef std::string TSymbol;
typedef std::string TUserID;
typedef std::string TTradeID;
typedef std::string TOrderID;
typedef double TPrice;
typedef double TQty;
typedef std::chrono::system_clock::time_point TDateTime;

typedef uintmax_t TRevNo;

typedef std::pair<TPrice, TDateTime> TPriceTime;

enum class TSide : char
{
	Buy = 'B',
	Sell = 'S',
};

enum class TOrderType : int
{
	Market = 0,
	Limit = 1,
};

#define TS_ITEM(name, type) type m_##name{type()};
struct SQuote {RM_QUOTE};
struct STrade {RM_TRADE};
struct SOrder {RM_ORDER};
#undef TS_ITEM

template <typename T> const char *GetObjectName();
#define TS_ITEM(name) template <> inline const char *GetObjectName<S##name>() {return #name;}
RM_OBJECTS
#undef TS_ITEM


#define RM_PARSE(name)
#define TS_ITEM(name, type) src.GetAttr(#name##s, dst.m_##name);
inline void Parse(const CMessage &src, SQuote &dst) {RM_QUOTE;};
inline void Parse(const CMessage &src, STrade &dst) {RM_TRADE;};
inline void Parse(const CMessage &src, SOrder &dst) {RM_ORDER;};
#undef TS_ITEM

template <typename T>
T Parse(const CMessage &msg)
{
	T res;
	Parse(msg, res);
	return res;
}


template <typename T> using TCallbackManager = TS::CCallbackManager<void(const T &)>;
template <typename T> using TCallbackPtr = typename TCallbackManager<T>::TCallbackPtr;

class CCheckOrderError
: public std::runtime_error
{
public:
	using std::runtime_error::runtime_error;

	template <typename Rep, typename Period, typename T, typename... TT>
	CCheckOrderError(std::chrono::duration<Rep, Period> moratorium, T &&text, TT&&... args)
	: runtime_error(TS::FormatStr(std::forward<T>(text), std::forward<TT>(args)...))
	, m_moratorium(moratorium)
	{

	}

	template <typename... TT>
	static void Raise(TT&&... args)
	{
		throw CCheckOrderError(std::forward<TT>(args)...);
	}

	std::chrono::seconds m_moratorium;
};

template <typename T>
struct CCallbackPtrHolder
{
	TCallbackPtr<T> m_cb;
};


template <typename TRiskManager = CRiskManager>
class CObjectHandler
: public CCallbackPtrHolder<SQuote>
, public CCallbackPtrHolder<STrade>
, public CCallbackPtrHolder<SOrder>
{
public:
	CObjectHandler(TRiskManager &rm)
	: m_rm(rm)
	{
	}

	template <typename T, typename... TT>
	void RegisterCallback(TT&&... args)
	{
		CCallbackPtrHolder<T>::m_cb = m_rm.RegisterCallback<T>(std::forward<TT>(args)...);
	}

	void Reset()
	{
#define TS_ITEM(name) CCallbackPtrHolder<S##name>::m_cb.reset();
		RM_OBJECTS
#undef TS_ITEM
	}

protected:
	TRiskManager &m_rm;
};

#define TS_CFG TS_CFG_(CheckRule)
#define TS_CONFIG_ITEMS \
	TS_ITEM(moratorium, std::chrono::seconds, 1min)

#include "Common/Config.inl"

class COrderCheckRule
: protected CObjectHandler<>
{
public:
	COrderCheckRule(CRiskManager &rm, const TS::CConfigFile &cfg)
	: CObjectHandler(rm)
	, m_cfg(cfg)
	{
	}

	virtual ~COrderCheckRule()
	{
	}

	template <typename... TT>
	void RejectOrder(const SOrder &order, const std::string &reason, TT&&... args)
	{
		CCheckOrderError::Raise(m_cfg.moratorium, reason, std::forward<TT>(args)...);
	}

	CheckRule::CConfig m_cfg;
};

template <typename T>
std::unique_ptr<COrderCheckRule> CreateRule(CRiskManager &, const TS::CConfigFile &);


#define TS_CFG TS_CFG_(RiskManager)
#define TS_CONFIG_ITEMS \

#include "Common/Config.inl"



class CRiskManager
: public TS::CConfigHolder<RiskManager::CConfig>
, protected TCallbackManager<SQuote>
, protected TCallbackManager<STrade>
, protected TCallbackManager<SOrder>
{
public:
	struct CInvestor
	{
		std::mutex m_mx;
		TDateTime m_moratorium{TDateTime::min()};
	};

	CRiskManager(const TS::CConfigFile &cfg);
	~CRiskManager();

	void AddRule(const std::string &name, const TS::CConfigFile &cfg)
	{
		auto sp = CreateRule(name, cfg);
		m_rules.emplace_back(std::move(sp));
	}

	template <typename T, typename TFunc, typename... TT>
	auto RegisterCallback(TFunc &&func, TT&&... args)
	{
		return TCallbackManager<T>::RegisterCallback(std::forward<TFunc>(func), std::forward<TT>(args)...);
	}

	template <typename T>
	void PutObject(const T &obj)
	{
		TCallbackManager<T>::ForEachCallback2(obj);
	}

	template <typename T>
	void ProcessMessage(CTransport &trans, const CMessage &msg) //Quote, Trade
	{
		auto obj = Parse<T>(msg);
		PutObject(obj);
	}

	template <typename T>
	void PutMessage(CTransport &trans, const std::shared_ptr<const CMessage> &sp)
	{
		ProcessMessage<T>(trans, *sp);
	}

	CInvestor &GetInvestor(const TUserID &id)
	{
		auto res = Sys::Locked::Emplace(id, m_investors, []()
		{
			return std::make_unique<CInvestor>();
		});
		return *res.first->second;
	}

protected:

	std::unique_ptr<COrderCheckRule> CreateRule(const std::string &rule, const TS::CConfigFile &cfg);

	std::list<std::unique_ptr<COrderCheckRule>> m_rules;
	Sys::CLockedObject<std::unordered_map<TUserID, std::unique_ptr<CInvestor>>, std::shared_mutex> m_investors;


};

inline
void SendReject(CTransport &trans, const SOrder &order, CMessage::TAttrs attrs, std::string reason)
{
	//Log.Debug(order.m_time, order.m_order_id, order.m_symbol, order.m_user_id, "REJECT", reason);
	attrs.emplace_back("reject"s, std::move(reason));
	trans.SendMessage(attrs);
}

template <> inline
void CRiskManager::ProcessMessage<SOrder>(CTransport &trans, const CMessage &msg)
{
	const auto &order = Parse<SOrder>(msg);


	auto &investor = GetInvestor(order.m_user_id);
//	if (investor.m_moratorium > std::chrono::system_clock::now())
//	{
//		SendReject(trans, order, msg.m_attrs, "Moratorium");
//		return;
//	}

	try
	{
		PutObject(order);

		//Log.Debug(order.m_time, order.m_order_id, order.m_symbol, order.m_user_id);
		trans.SendMessage(msg.m_attrs);
		return;
	}
	catch(const CCheckOrderError &err)
	{
		investor.m_moratorium = std::chrono::system_clock::now() + err.m_moratorium;
		SendReject(trans, order, msg.m_attrs, err.what());
	}
}


}

