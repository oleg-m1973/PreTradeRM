#pragma once
//#include "Common.h"

#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <thread>
#include <list>


#define SYS_LOCKER(obj, lock) Sys::CLock<lock<std::decay_t<decltype(obj)>>>

#define SYS_UNIQUE_LOCK(obj, name, ...) SYS_LOCKER(obj, std::unique_lock) name(obj, ##__VA_ARGS__);
#define SYS_LOCK_GUARD(obj, name, ...) SYS_LOCKER(obj, std::lock_guard) name(obj, ##__VA_ARGS__);

#define SYS_LOCK(obj, ...) SYS_LOCK_GUARD(obj, CONCAT(lock_, __COUNTER__), ##__VA_ARGS__);

#define SYS_SHARED_LOCK(obj, name, ...) SYS_LOCKER(obj, Sys::CSharedLockGuard) name(obj, ##__VA_ARGS__);
#define SYS_LOCK_READ(obj, ...) SYS_SHARED_LOCK(obj, CONCAT(lock_, __COUNTER__), ##__VA_ARGS__);
#define SYS_LOCK_WRITE(obj, ...) SYS_LOCK(obj, ##__VA_ARGS__)

//TS_LOCKER(obj, lock_guard) CONCAT(lock_, __COUNTER__)(obj, ##__VA_ARGS__);

namespace Sys
{
template <typename TSharedMutex> class CSharedLockGuard;
template <typename TLock> class CLockable;


template <typename T> concept bool IsLockable = requires(T a) {a.GetMutex();};
template <typename T> concept bool IsSharedMutex = requires(T a) {{a.lock_shared()}; {a.unlock_shared()};};
template <typename T> concept bool IsUpgradable = requires(T a) {a.upgrade();};
template <typename T> concept bool IsUpgradableMutex = requires(T a) {a.upgrade_lock();};

template <typename TLockGuard>
class CLock
: public TLockGuard
{
public:
	using TLockGuard::TLockGuard;
	using typename TLockGuard::mutex_type;

	CLock(const IsLockable &mx)
	: TLockGuard(const_cast<mutex_type &>(mx))
	{
	}

	static constexpr bool upgrade() requires !IsUpgradable<TLockGuard>
	{
		return true;
	}

	bool upgrade() requires IsUpgradable<TLockGuard>
	{
		return TLockGuard::upgrade();
	}

};

template <typename T, typename... TT>
auto UniqueLock(T &mx, TT&&... args)
{
	return CLock<std::unique_lock<T>>(mx, std::forward<TT>(args)...);
}

template <typename TMutex = std::mutex>
class CLockable
: public TMutex
{
public:
	typedef TMutex mutex_type;

	template <typename... TT>
	explicit CLockable(TT&&... args)
	: mutex_type(std::forward<TT>(args)...)
	{
	}

	CLockable(CLockable &&) {;}
	CLockable(const CLockable &) {;}

	CLockable &operator =(CLockable &&) {return *this;}
	CLockable &operator =(const CLockable &) {return *this;}

	mutex_type &GetMutex() const
	{
		return *this;
	}
};

template <typename T>
class CSharedMutexWrapper
{
public:
	TS_MOVABLE(CSharedMutexWrapper, default);

	CSharedMutexWrapper(T &mx)
	: m_mx(mx)
	{
	}

	~CSharedMutexWrapper()
	{
	}

	void lock_shared() noexcept
	{
		m_mx.lock_shared();
	}

	void unlock_shared() noexcept
	{
		if (m_upgrade)
			m_mx.unlock();
		else
			m_mx.unlock_shared();
	}

	bool upgrade() noexcept
	{
		if (m_upgrade)
			return true;

		const auto res = _upgrade();
		if (!res)
		{
			m_mx.unlock_shared();
			m_mx.lock();
		}
		m_upgrade = true;
		return res;
	}

protected:
	static constexpr bool _upgrade() requires !IsUpgradableMutex<T>
	{
		return false;
	}

	bool _upgrade() requires IsUpgradableMutex<T>
	{
		return m_mx.upgrade_lock();
	}

	T &m_mx;
	bool m_upgrade = false;
};

template <IsSharedMutex TSharedMutex>
class CSharedLockGuard<TSharedMutex>
{
public:
	typedef TSharedMutex mutex_type;

	TS_COPYABLE(CSharedLockGuard, delete);

	CSharedLockGuard(TSharedMutex &mx)
	: m_mx(mx)
	{
		m_mx.lock_shared();
	}

	~CSharedLockGuard()
	{
		m_mx.unlock_shared();
	}

	bool upgrade()
	{
		return m_mx.upgrade();
	}

protected:
	mutable CSharedMutexWrapper<TSharedMutex> m_mx;
};

template <IsSharedMutex TSharedMutex>
class CSharedLock
: protected CSharedMutexWrapper<TSharedMutex>
, public std::shared_lock<CSharedMutexWrapper<TSharedMutex>>
{
	TS_MOVABLE(CSharedLock, default);

	typedef std::shared_lock<CSharedMutexWrapper<TSharedMutex>> TBase;

	CSharedLock(TSharedMutex &mx)
	: CSharedMutexWrapper<TSharedMutex>(mx)
	, TBase(*static_cast<CSharedMutexWrapper<TSharedMutex> *>(this))
	{
	}

	bool upgrade()
	{
		if (!TBase::mutex())
			throw std::system_error(std::make_error_code(std::errc::operation_not_permitted));

		if (!TBase::owns_lock())
			throw std::system_error(std::make_error_code(std::errc::resource_deadlock_would_occur));

		return TBase::mutex()->upgrade();
	}

};

class CSpinSharedMutex
{
public:
	typedef uint32_t TLockCounter;
	static const TLockCounter WriteLockFlag = TLockCounter(0x1) << ((sizeof(TLockCounter) << 3) - 1);
	static const TLockCounter MaxReaders = ~WriteLockFlag;
	static const size_t SpinCount = 0xFF;

	CSpinSharedMutex()
	{
	}

	TS_MOVABLE(CSpinSharedMutex, delete);
	TS_COPYABLE(CSpinSharedMutex, delete);

	void lock() const noexcept
	{
		auto cnt = m_cnt.load(std::memory_order_relaxed);
		for (size_t i = 0;; Yield(i))
		{
			cnt &= ~WriteLockFlag;
			if (m_cnt.compare_exchange_weak(cnt, cnt | WriteLockFlag, std::memory_order_acquire))
				break;
		}

		for (size_t i = 1; m_cnt.load(std::memory_order_relaxed) != WriteLockFlag; Yield(i))
			;
	}

	void unlock() const noexcept
	{
		m_cnt.store(0, std::memory_order_release);
	}

	void lock_shared() const noexcept
	{
		auto cnt = m_cnt.load(std::memory_order_relaxed);
		for (size_t i = 0;; Yield(i))
		{
			cnt &= ~WriteLockFlag;
			if ((cnt != MaxReaders) && m_cnt.compare_exchange_weak(cnt, cnt + 1, std::memory_order_acquire))
				break;
		}
	}

	void unlock_shared() const noexcept
	{
		m_cnt.fetch_sub(1, std::memory_order_release);
	}

	bool upgrade_lock() const noexcept
	{
		return false;
	}
protected:
	static void Yield(size_t &i)
	{
		if (!((++i) & SpinCount))
			std::this_thread::yield();
	}

	mutable volatile std::atomic<TLockCounter> m_cnt{0};
};


class CBarrierLock
{
public:
	CBarrierLock()
	{
	}

	TS_MOVABLE(CBarrierLock, default);
	TS_COPYABLE(CBarrierLock, delete);

	void lock() noexcept
	{
		m_cnt.fetch_add(1, std::memory_order_acquire);
	}

	void unlock() noexcept
	{
		m_cnt.fetch_sub(1, std::memory_order_release);
	}

	void Enter() const noexcept
	{
		while (m_cnt.load(std::memory_order_acquire) != 0)
			std::this_thread::yield();
	}
protected:
	volatile std::atomic<uintmax_t> m_cnt{0};
};

template <typename TLock, typename T>
class CObjectLock
{
public:
	CObjectLock(TLock &lock, T &obj)
	: m_obj(obj)
	, m_lock(lock)
	{
	}

	CObjectLock(const TLock &lock, T &obj)
	: m_obj(obj)
	, m_lock(const_cast<TLock &>(lock))
	{
	}

	CObjectLock(CObjectLock &&src)
	: m_obj(src.m_obj)
	, m_lock(std::move(src.m_lock))
	{
	}

	~CObjectLock()
	{
	}

	T *operator ->()
	{
		return &m_obj;
	}

	const T *operator ->() const
	{
		return &m_obj;
	}

	T &operator *()
	{
		return m_obj;
	}

	const T &operator *() const
	{
		return m_obj;
	}

	operator std::unique_lock<TLock> &()
	{
		return m_lock;
	}
protected:
	T &m_obj;
	std::unique_lock<TLock> m_lock;
};

template <typename TLock, typename T> inline
CObjectLock<TLock, T> DoLocked(TLock &lock, T &obj)
{
	return CObjectLock<TLock, T>(lock, obj);
}

template <typename T> inline
CObjectLock<T, T> DoLocked(T &obj)
{
	return DoLocked(obj, obj);
}

template <typename T, typename TLock = std::recursive_mutex>
class CLockedObject
: public T
, public TLock
{
public:
	typedef TLock mutex_type;

	CLockedObject()
	{
	}

	template <typename... TT>
	explicit CLockedObject(TT&&... vals)
	: T(std::forward<TT>(vals)...)
	{
	}

	T &operator *()
	{
		return *static_cast<T *>(this);
	}

	const T &operator *() const
	{
		return *static_cast<const T *>(this);
	}

	CObjectLock<TLock, T> DoLocked()
	{
		return CObjectLock<TLock, T>(*this, *this);
	}

	const CObjectLock<TLock, const T> DoLocked() const
	{
		return CObjectLock<TLock, const T>(*this, *this);
	}

	template <typename T2>
	void SpliceFront(std::list<T2> &src)
	{
		SYS_LOCK(*this);
		splice(this->begin(), src);
	}

	template <typename T2>
	void SpliceBack(std::list<T2> &src)
	{
		SYS_LOCK(*this);
		splice(this->end(), src);
	}

	mutex_type &mutex()
	{
		return *this;
	}
};

namespace Locked
{
template <typename T, typename TLock>
T Move(T &val, TLock &lock)
{
	SYS_LOCK(lock);
	return std::move(val);
}

template <typename T, typename TLock>
T Move(CLockedObject<T, TLock> &val)
{
	return Move(*val, val);
}

template <typename T, typename TLock>
void SpliceFront(std::list<T> &dst, CLockedObject<std::list<T>, TLock> &src)
{
	SYS_LOCK(src);
	dst.splice(dst.begin(), src);
}

template <typename T, typename TLock>
void SpliceBack(std::list<T> &dst, CLockedObject<std::list<T>, TLock> &src)
{
	SYS_LOCK(src);
	dst.splice(dst.end(), src);
}

template <typename TKey, typename TItems, typename TMutex, typename TFunc, typename... TT>
auto Emplace(TKey &&key, TItems &items, TMutex &mx, TFunc &&func, TT&&... args)
{
	SYS_LOCK(mx);
	auto it = items.find(key);
	if (it != items.end())
		return std::make_pair(it, false);

	return items.emplace(std::forward<TKey>(key), func(std::forward<TT>(args)...));
}

template <typename TKey, typename TItems, typename TFunc, typename... TT>
auto Emplace(TKey &&key, TItems &items, std::shared_mutex &mx, TFunc &&func, TT&&... args)
{
	SYS_SHARED_LOCK(mx, lock);
	auto it = items.find(key);
	if (it == items.end() && !lock.upgrade())
		it = items.find(key);

	if (it != items.end())
		return std::make_pair(it, false);

	return items.emplace(std::forward<TKey>(key), func(std::forward<TT>(args)...));
}

template <typename TKey, typename TItems, typename TMutex,  typename TFunc, typename... TT>
auto Emplace(TKey &&key, CLockedObject<TItems, TMutex> &items, TFunc &&func, TT&&... args)
{
	return Emplace(std::forward<TKey>(key), *items, items.mutex(), std::forward<TFunc>(func), std::forward<TT>(args)...);
}

} //namespace Locked
} //namespace Sys

