#include "Common.h"
#include "RiskManager.h"

#include "Common/FramedQueue.h"

#include <unordered_map>
#include <map>

using namespace RM;
namespace
{

//////////////////////////////////////////////////////////////////////////////////////////////////////
//2) 24H (trailing) drawdown
//Apply : by Investor
//Check : if drawdown ( = max 24hour cumulative P&L – current 24hour cumulative P&L) > EUR100*
//Action if true : order is rejected, alarm is sent
#define TS_CFG TS_CFG_(DrawDownRule)
#define TS_CONFIG_ITEMS \
	TS_ITEM(pnl_time, std::chrono::seconds, 24h) \
 	TS_ITEM(drawdown, TPrice, 100) \

#include "Common/Config.inl"

class CDrawDown;

//Сделки по позиции для расчёта доходности
struct CTrade
{
	CTrade()
	{
	}

	CTrade(const STrade &src)
	: m_price(src.m_price)
	, m_qty(src.m_side == TSide::Sell? -src.m_qty: src.m_qty)
	{
	}

	TPrice m_price = 0;
	TQty m_qty = 0; //Для сделок на покупку - положительное, на продажу - отрицательное
};

struct CPositionYield
{
	//Вызывается при добавлении сделки
	CPositionYield &operator +=(const CTrade &trade)
	{
		m_sum += trade.m_price * trade.m_qty;
		m_qty += trade.m_qty;
		return *this;
	}

	//Вызывается при выходе сделки из диапазона по времени
	CPositionYield &operator -=(const CTrade &trade)
	{
		m_sum -= trade.m_price * trade.m_qty;
		m_qty -= trade.m_qty;
		return *this;
	}

	TPrice GetYield(const TPrice &price) const
	{
		return price * m_qty - m_sum;
	}

	TPrice m_sum = 0; //Сумма всех сделок по позиции
	TQty m_qty = 0; //Количество по всем сделкам
};

struct CPosition
{
	template <typename Rep, typename Period>
	CPosition(const TPriceTime &price, std::chrono::duration<Rep, Period> dt)
	: m_price(price)
	, m_trades(dt)
	{
	}

	void PutQuote(const SQuote &quote)
	{
		if (quote.m_time < m_price.second)
			return;

		m_price = TPriceTime(quote.m_price, quote.m_time);
	}

	void PutTrade(const STrade &trade)
	{
		m_trades.PutValue(trade.m_time, trade);
	}

	void UpdateYield()
	{
		m_yield = m_trades.GetSum(m_price.second).GetYield(m_price.first);
	}


	TPriceTime m_price; //Current instrument price
	TPrice m_yield = 0;
	TS::CMovingSum<CTrade, CPositionYield> m_trades;
};

struct CInvestor
{
	template <typename Rep, typename Period>
	CInvestor(CDrawDown &rule, std::chrono::duration<Rep, Period> dt)
	: m_rule(rule)
	, m_pnl_max(dt)
	{
	}

	void PutQuote(const SQuote &quote)
	{
		SYS_LOCK(m_mx);
		auto it = m_positions.find(quote.m_symbol);
		if (it == m_positions.end())
			return;

		auto &pos = *it->second;
		if (m_time < quote.m_time)
			m_time = quote.m_time;

		pos.PutQuote(quote);
		UpdatePnL(pos);
	}

	void PutTrade(const STrade &trade)
	{
		SYS_LOCK(m_mx);
        auto &pos = GetPosition(trade.m_symbol);
        if (pos.m_price.first == 0)
			return;

		pos.PutTrade(trade);
		UpdatePnL(pos);
	}

	void UpdatePnL(CPosition &pos)
	{
		//Обновляем значение суммарного P&L при изменении позиции
		const auto yield = pos.m_yield;
		pos.UpdateYield();
		m_pnl += pos.m_yield - yield;

		const auto pnl_max = m_pnl_max.GetMax(m_time);

		m_drawdown = pnl_max - m_pnl;
		m_pnl_max.PutValue(m_time, m_pnl);
	}

	CPosition &GetPosition(auto &id);

	CDrawDown &m_rule;
	std::mutex m_mx;
	TPrice m_pnl = 0; //Cumulative P&L
	TS::CMovingMinMax<TPrice> m_pnl_max;

	std::atomic<TPrice> m_drawdown{0};

	TDateTime m_time;
	std::unordered_map<TSymbol, std::unique_ptr<CPosition>> m_positions;
};


class CDrawDown
: public COrderCheckRule
{
public:
	DrawDownRule::CConfig m_cfg;

	CDrawDown(CRiskManager &rm, const TS::CConfigFile &cfg)
	: COrderCheckRule(rm, cfg)
	, m_cfg(cfg)
	{
		RegisterCallback<SQuote>(&CDrawDown::ProcessQuote, this);
		RegisterCallback<STrade>(&CDrawDown::ProcessTrade, this);
		RegisterCallback<SOrder>(&CDrawDown::CheckOrder, this);
	}

	void ProcessQuote(const SQuote &quote)
	{
		if (!UpdateLastPrice(quote))
			return;


		SYS_LOCK_READ(m_investors);
		auto it = m_positions.lower_bound(std::pair<const TSymbol &, const TUserID &>(quote.m_symbol, TUserID()));
		for (auto end = m_positions.end(); it != end && it->first.first == quote.m_symbol; ++it)
			it->second->PutQuote(quote);
	}

	void ProcessTrade(const STrade &trade)
	{
		GetInvestor(trade.m_user_id, trade.m_symbol).PutTrade(trade);
	}

	void CheckOrder(const SOrder &order)
	{
		SYS_LOCK_READ(m_investors);
		auto it = m_investors.find(order.m_user_id);
		if (it == m_investors.end())
			return;

		const auto &inv = *it->second;
		if (inv.m_drawdown > m_cfg.drawdown)
			RejectOrder(order, "TrailingDrowdown", inv.m_drawdown);
	}

	TPriceTime GetLastPrice(auto &symbol) const
	{
		SYS_LOCK(m_prices);
		auto it = m_prices.find(symbol);
		return it == m_prices.end()? TPriceTime(0, TDateTime()): it->second;
	}
protected:
	bool UpdateLastPrice(const SQuote &quote)
	{
		SYS_LOCK(m_prices);
		auto it = m_prices.find(quote.m_symbol);
		if (it == m_prices.end())
		{
			m_prices.emplace(quote.m_symbol, TPriceTime(quote.m_price, quote.m_time));
			return true;
		}

		if (quote.m_time < it->second.second)
			return false;

		it->second = TPriceTime(quote.m_price, quote.m_time);
		return true;
	}

	CInvestor &GetInvestor(const TUserID &id, const TSymbol &symbol)
	{
		auto res = Sys::Locked::Emplace(id, m_investors, [this]()
		{
			auto sp = std::make_shared<CInvestor>(*this, m_cfg.pnl_time);
			return sp;
		});

		auto *p = res.first->second.get();

		const std::pair<const TSymbol &, const TUserID &> pos_id(symbol, id);
		Sys::Locked::Emplace(pos_id, m_positions, m_investors.mutex(), [p]()
		{
			return p;
		});

		return *p;
	}

	Sys::CLockedObject<std::unordered_map<TUserID, std::shared_ptr<CInvestor>>, std::shared_mutex> m_investors;
	std::map<std::pair<TSymbol, TUserID>, CInvestor *> m_positions;
	mutable Sys::CLockedObject<std::unordered_map<TSymbol, TPriceTime>> m_prices;
};

CPosition &CInvestor::GetPosition(auto &id)
{
	auto it = m_positions.find(id);
	if (it != m_positions.end())
		return *it->second;

	auto sp = std::make_unique<CPosition>(m_rule.GetLastPrice(id), m_pnl_max.GetFrame());
	return *m_positions.emplace(id, std::move(sp)).first->second;
}

}

RM_DECLARE_RULE(DrawDown);
