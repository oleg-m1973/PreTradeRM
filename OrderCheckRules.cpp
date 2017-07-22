#include "Common.h"
#include "RiskManager.h"

#include "Common/FramedQueue.h"

#include <unordered_map>


using namespace RM;
namespace
{
//////////////////////////////////////////////////////////////////////////////////////////////////////
//1) New Trade Moratorium
//Apply : by Investor
//Check : a new order comes in <60* seconds after the previous one
//Action if true : order is rejected, alarm is sent, moratorium starts
#define TS_CFG TS_CFG_(NewOrderMoratorium)
#define TS_CONFIG_ITEMS \
	TS_ITEM(timeout, std::chrono::milliseconds, 1s) \

#include "Common/Config.inl"

class CNewOrderMoratorium
: public COrderCheckRule
{
public:
	NewOrderMoratorium::CConfig m_cfg;

	CNewOrderMoratorium(CRiskManager &rm, const TS::CConfigFile &cfg)
	: COrderCheckRule(rm, cfg)
	, m_cfg(cfg)
	{
		RegisterCallback<SOrder>(&CNewOrderMoratorium::CheckOrder, this);
	}

	void CheckOrder(const SOrder &order)
	{
		auto key = std::make_pair(order.m_user_id, order.m_symbol);
		auto it = Sys::Locked::Emplace(key, m_investors, m_mx, [&order]()
		{
			auto sp = std::make_unique<CInvestor>();
			sp->m_order_time = order.m_time;
			return std::move(sp);
		});

		if (it.second) //New Investor
			return;

		auto &investor = *it.first->second;

		SYS_LOCK(investor.m_mx);
		if (investor.m_order_time > order.m_time)
			return;

		const auto tm = investor.m_order_time + m_cfg.timeout;
		if (tm > order.m_time)
			RejectOrder(order, "NewOrderMoratorium", tm - order.m_time);

		investor.m_order_time = order.m_time;
	}

	struct CInvestor
	{
		std::mutex m_mx;
		TDateTime m_order_time;
	};

	std::shared_mutex m_mx;
	std::map<std::pair<TUserID, TSymbol>, std::unique_ptr<CInvestor>> m_investors; //Время последней заявки для инвестора
};


//////////////////////////////////////////////////////////////////////////////////////////////////////
//4) Price check (only for limit orders)
//Apply : by Instrument
//Check :
//	for a buy order, if the price is higher than trailing 3*hour average price by more than 5*%
//	for a sell order, if the price is lower than trailing 3*hour average price by more than 5*%
//Action if true : order is rejected, alarm is sent, moratorium starts

#define TS_CFG TS_CFG_(PriceCheck)
#define TS_CONFIG_ITEMS \
	TS_ITEM(timeframe, std::chrono::seconds, 3h) \
	TS_ITEM(price_dev, double, 5.0 / 100.0) \

#include "Common/Config.inl"

class CPriceCheck
: public COrderCheckRule
{
public:
	PriceCheck::CConfig m_cfg;

	CPriceCheck(CRiskManager &rm, const TS::CConfigFile &cfg)
	: COrderCheckRule(rm, cfg)
	, m_cfg(cfg)
	{
		RegisterCallback<SQuote>(&CPriceCheck::ProcessQuote, this);
		RegisterCallback<SOrder>(&CPriceCheck::CheckOrder, this);
	}

	void ProcessQuote(const SQuote &quote)
	{
		auto &instr = GetInstrument(quote.m_symbol);
		instr.PutValue(quote.m_time, quote.m_price);
	}

	void CheckOrder(const SOrder &order)
	{
		if (order.m_type != TOrderType::Limit)
			return;

		SYS_LOCK_READ(m_mx);
		auto it = m_instrs.find(order.m_symbol);
		if (it == m_instrs.end())
			RejectOrder(order, "InstrumentNotFound", order.m_symbol);

		auto &instr = *it->second;
		const auto avg = instr.GetAverage(order.m_time);

		const bool reject = order.m_side == TSide::Buy?
			order.m_price > avg * (1.0 + m_cfg.price_dev):
			-order.m_price < -avg * (1.0 - m_cfg.price_dev); //Raise when avg == 0

		if (reject)
			RejectOrder(order, "PriceCheck", avg);
	}

protected:
	typedef TS::CMovingSum<TPrice> TInstrument;

	TInstrument &GetInstrument(const TSymbol &id)
	{
		auto res = Sys::Locked::Emplace(id, m_instrs, m_mx, [this]()
		{
			return std::make_unique<TInstrument>(m_cfg.timeframe);
		});
		return *res.first->second;
	}

	std::shared_mutex m_mx;
	std::unordered_map<TSymbol, std::unique_ptr<TInstrument>> m_instrs;
};

//////////////////////////////////////////////////////////////////////////////////////////////////////
//3) Sequence of bad trades
//Apply : by Investor by Instrument
//Check : every 5* consequent pairs of trades – buy & sell** – during 60* seconds is lossmaking Action if true : order is rejected, alarm is sent

#define TS_CFG TS_CFG_(SeqBadTrades)
#define TS_CONFIG_ITEMS \
	TS_ITEM(timeframe, std::chrono::seconds, 60s) \
	TS_ITEM(cnt, size_t, 5) \

#include "Common/Config.inl"

class CSeqBadTrades
: public COrderCheckRule
{
public:
	SeqBadTrades::CConfig m_cfg;

	CSeqBadTrades(CRiskManager &rm, const TS::CConfigFile &cfg)
	: COrderCheckRule(rm, cfg)
	, m_cfg(cfg)
	{
		RegisterCallback<STrade>(&CSeqBadTrades::ProcessTrade, this);
		RegisterCallback<SOrder>(&CSeqBadTrades::CheckOrder, this);
	}

	void ProcessTrade(const STrade &trade)
	{
		const auto id = std::make_pair(trade.m_symbol, trade.m_user_id);
		auto &trades = *Sys::Locked::Emplace(id, m_trades, m_mx, [this]()
		{
			return std::make_unique<CTradesPair>(m_cfg.timeframe);
		}).first->second;

		trades.ProcessTrade(trade);
	}

	void CheckOrder(const SOrder &order)
	{
		SYS_LOCK_READ(m_mx);
		auto it = m_trades.find({order.m_symbol, order.m_user_id});
		if (it == m_trades.end())
			return;

		const auto n = it->second->GetBadTrades(order.m_time);
		const bool reject = n > m_cfg.cnt;
		if (reject)
			RejectOrder(order, "SeqBadTrades", n);
	}

protected:
	struct CTradesPair
	{
		CTradesPair(auto tm)
		: m_price(tm)
		, m_bads(tm, 0)
		{
		}

		void ProcessTrade(const STrade &trade)
		{
			SYS_LOCK(m_mx);
			if (trade.m_side == m_side)
			{
				m_time = trade.m_time;
				m_price.PutValue(trade.m_time, trade.m_price);
				return;
			}

			const auto price = m_price.GetAverage(trade.m_time);
			if (_IsBadTrade(price))
				m_bads.PutValue(m_time, 1);

			m_price.Clear();

			m_side = trade.m_side;
			m_time = trade.m_time;
			m_price.PutValue(trade.m_time, trade.m_price);
		}

		bool _IsBadTrade(TPrice price)
		{
			if (m_price2 == 0 || price == 0)
				return false;

			return m_side == TSide::Buy? price > m_price2: price < m_price2;
		}

		size_t GetBadTrades(auto tm)
		{
			SYS_LOCK(m_mx)
			const auto n = m_bads.GetSize(tm);
			return _IsBadTrade(m_price.GetAverage())? n + 1: n;
		}

		std::mutex m_mx;

		TSide m_side = TSide::Buy;
		TDateTime m_time;
		TS::CMovingSum<TPrice> m_price;

		TPrice m_price2 = 0;

		TS::CFramedQueue<int> m_bads;
	};

	std::shared_mutex m_mx;
	std::map<std::pair<TSymbol, TUserID>, std::unique_ptr<CTradesPair>> m_trades;
};

}


RM_DECLARE_RULE(NewOrderMoratorium);
RM_DECLARE_RULE(SeqBadTrades);
RM_DECLARE_RULE(PriceCheck);
