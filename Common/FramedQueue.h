#pragma once
#include <chrono>
#include <deque>
#include <algorithm>
#include <map>

namespace TS
{
template <typename _TValue, typename _TTimestamp = std::chrono::system_clock::time_point>
class CFramedQueue
{
public:
	typedef _TValue TValue;
	typedef _TTimestamp TTimestamp;
	typedef decltype(TTimestamp() - TTimestamp()) TFrame;


	CFramedQueue(const TFrame &frame, size_t rem = 1)
	: m_frame(frame)
	, m_rem(rem)
	{
	}

	bool PutValue(const TTimestamp &tm, const TValue &val)
	{
		SYS_LOCK_GUARD(m_mx, lock);
		return _PutValue(tm, val, lock);
	}

	template <typename TFunc, typename... TT>
	void ForEachItem(TFunc &&func, TT&&... args)
	{
		SYS_LOCK_READ(m_mx);
		for (auto &item: m_items)
			func(item.first, item.second, args...);
	}

	bool EraseExpired(const TTimestamp &tm)
	{
		SYS_SHARED_LOCK(m_mx, lock);
		return _EraseExpired(tm, lock);
	}

	size_t GetSize() const
	{
		SYS_LOCK_READ(m_mx);
		return m_items.size();
	}

	size_t GetSize(const TTimestamp &tm)
	{
		SYS_SHARED_LOCK(m_mx, lock);
		_EraseExpired(tm, lock);
		return m_items.size();
	}

	const TFrame &GetFrame() const
	{
		return m_frame;
	}

	void Clear()
	{
		SYS_LOCK_WRITE(m_mx);
		while (!m_items.empty())
		{
			auto &item = m_items.front();
			EraseItem(item.first, item.second);
			m_items.pop_front();
		}
	}

protected:
	virtual void EraseItem(const TTimestamp &tm, const TValue &val)
	{
	}

	bool _PutValue(const TTimestamp &tm, const TValue &val, auto &&lock)
	{
		if (m_items.empty())
		{
			m_items.emplace_back(tm, val);
			return true;
		}

		const auto &tm1 = m_items.front().first;
		const auto &tm2 = m_items.back().first;

		if (tm2 < tm || !(tm < tm2))
		{
			if ((tm1 + m_frame) < tm)
				_EraseExpired(tm, lock);

			m_items.emplace_back(tm, val);
			return true;
		}

		if (tm1 < tm)
		{
			auto it = std::upper_bound(m_items.begin(), m_items.end(), tm, [](const TTimestamp &tm, const auto &item)
			{
				 return tm < item.first;
			});
			m_items.insert(it, std::make_pair(tm, val));
			return true;
		}

		if (tm2 < tm + m_frame)
		{
			m_items.emplace_front(tm, val);
			return true;
		}

		return false;
	}

	bool _EraseExpired(const TTimestamp &tm, auto &&lock)
	{
		if (m_items.empty() || tm <= m_items.front().first)
			return false;

		lock.upgrade();

		bool res = false;
		while (m_items.size() > m_rem)
		{
			auto &item = m_items.front();
			if (item.first + m_frame < tm)
			{
				EraseItem(item.first, item.second);
				m_items.pop_front();
				res = true;
			}
			else
				break;
		}
		return res;
	}

	mutable std::shared_mutex m_mx;
	const TFrame m_frame;
	const size_t m_rem;

	std::deque<std::pair<TTimestamp, TValue>> m_items;
};


template<typename TValue, typename TSum = TValue, typename... TT>
class CMovingSum
: public CFramedQueue<TValue, TT...>
{
public:
	typedef CFramedQueue<TValue, TT...> TBase;
	using typename TBase::TTimestamp;
	using TBase::CFramedQueue;


	bool PutValue(const TTimestamp &tm, const TValue &val)
	{
		SYS_LOCK_GUARD(m_mx, lock);
		if (!TBase::_PutValue(tm, val, lock))
			return false;

		m_sum += val;
		return true;
	}

	TValue GetAverage(const TTimestamp &tm)
	{
		SYS_SHARED_LOCK(m_mx, lock);
		TBase::_EraseExpired(tm, lock);
		return !m_items.empty()? m_sum / m_items.size(): 0;
	}

	TValue GetAverage() const
	{
		SYS_LOCK_READ(m_mx);
		return !m_items.empty()? m_sum / m_items.size(): 0;
	}

	TSum GetSum(const TTimestamp &tm)
	{
		SYS_SHARED_LOCK(m_mx, lock);
		TBase::_EraseExpired(tm, lock);
		return m_sum;
	}
protected:
	using TBase::m_items;
	using TBase::m_mx;

	virtual void EraseItem(const TTimestamp &tm, const TValue &val) override
	{
		m_sum -= val;
	}

	TSum m_sum{TSum()};
};

template<typename TValue, typename... TT>
class CMovingMinMax
: public CFramedQueue<TValue, TT...>
{
protected:

public:
	typedef CFramedQueue<TValue, TT...> TBase;
	using typename TBase::TTimestamp;
	using TBase::CFramedQueue;

	bool PutValue(const TTimestamp &tm, const TValue &val)
	{
		SYS_LOCK_GUARD(m_mx, lock);
		if (!TBase::_PutValue(tm, val, lock))
			return false;

		auto it = m_items2.find(val);
		if (it == m_items2.end())
			m_items2.emplace(val, 1);
		else
			++it->second;

		return true;
	}

	TValue GetMin(const TTimestamp &tm)
	{
		SYS_SHARED_LOCK(m_mx, lock);
		TBase::_EraseExpired(tm, lock);
		return m_items2.empty()? TValue(): m_items2.begin()->first;
	}

	TValue GetMax(const TTimestamp &tm)
	{
		SYS_SHARED_LOCK(m_mx, lock);
		TBase::_EraseExpired(tm, lock);
		return m_items2.empty()? TValue(): m_items2.rbegin()->first;
	}

	TValue GetMin() const
	{
		SYS_LOCK_READ(m_mx);
		return m_items2.empty()? TValue(): m_items2.begin()->first;
	}

	TValue GetMax() const
	{
		SYS_LOCK_READ(m_mx);
		return m_items2.empty()? TValue(): m_items2.rbegin()->first;
	}

protected:
	using TBase::m_items;
	using TBase::m_mx;

	virtual void EraseItem(const TTimestamp &tm, const TValue &val) override
	{
		auto it = m_items2.find(val);
		if (it != m_items2.end() && --it->second == 0)
			m_items2.erase(it);
	}

	std::map<TValue, size_t> m_items2;
};


}
