#pragma once
#include "SyncObjs.h"

#include <atomic>
#include <type_traits>

namespace Sys
{
template <typename T> class CSharedPtr;
template <typename T> class CWeakPtr;
template <typename T> class CEnableSharedFromThis;

template <typename TDerived, typename TBase> concept bool IsConvertiblePtr =
	std::is_same<std::remove_cv_t<TBase>, std::remove_cv_t<TDerived>>::value ||
	std::is_convertible<TDerived *, TBase *>::value && std::has_virtual_destructor<TBase>::value
	//std::is_base_of<TBase, TDerived>::value && std::has_virtual_destructor<TBase>::value
	;

template <typename T> concept bool IsSharedFromThis = std::is_base_of<CEnableSharedFromThis<T>, T>::value;

class CDestructableBase
{
public:
	virtual ~CDestructableBase() {;}
};

class CRefCounter
{
public:
	bool Capture() noexcept
	{
		auto cnt = m_cnt.load(std::memory_order_acquire);
		while(cnt)
		{
			if (m_cnt.compare_exchange_weak(cnt, cnt + 1, std::memory_order_acquire))
				return true;
		}

		return false;
	}

	bool Release() noexcept
	{
		return m_cnt.fetch_sub(1, std::memory_order_release) == 1; //, std::memory_order_acquire) == 1;
	}

	auto GetCount() const
	{
		return m_cnt.load(std::memory_order_acquire);
	}

	explicit operator bool() const noexcept
	{
		return GetCount() != 0;
	}

protected:
	volatile std::atomic<uint32_t> m_cnt{1};
};


struct CSharedCounter
{
	CRefCounter m_refs;
	CRefCounter m_weaks;
};

template <typename T>
class CSharedPtrBase
{
template <typename T_> friend class CSharedPtrBase;
public:
	CSharedPtrBase()
	{
	}

	template <typename T_>
	CSharedPtrBase(std::unique_ptr<T_> &sp)
	: m_cnt(sp? new CSharedCounter(): nullptr)
	, m_p(sp.get())
	{
	}

	CSharedPtrBase(CSharedCounter *cnt, T *p) noexcept
	: m_cnt(cnt)
	, m_p(cnt? p: nullptr)
	{
	}

	template <typename T_>
	CSharedPtrBase(std::pair<CSharedCounter *, T_ *> &&src) noexcept
	: m_cnt(src.first)
	, m_p(src.second)
	{
	}

	template <typename T_> requires IsConvertiblePtr<T_, T>
	CSharedPtrBase(CSharedPtrBase<T_> &&sp) noexcept
	: CSharedPtrBase(sp.m_cnt, sp.m_p)
	{
		sp.m_cnt = nullptr;
	}

	~CSharedPtrBase() noexcept
	{
		if (m_cnt && m_cnt->m_weaks.Release())
			delete m_cnt;
	}

	void reset(CSharedPtrBase &&sp) noexcept
	{
		SYS_LOCK_WRITE(m_mx);
		std::swap(m_cnt, sp.m_cnt);
		std::swap(m_p, sp.m_p);
	}

	std::pair<CSharedCounter *, T *> CaptureShared() const noexcept
	{
		SYS_LOCK_READ(m_mx);

		if (!m_cnt || !m_p || !m_cnt->m_refs.Capture())
			return {nullptr, nullptr};

		if (!m_cnt->m_weaks.Capture())
			abort();

		return {m_cnt, m_p};
	}

	std::pair<CSharedCounter *, T *> CaptureWeak() const noexcept
	{
		SYS_LOCK_READ(m_mx);
		if (m_cnt && m_p && m_cnt->m_weaks.Capture())
			return {m_cnt, m_p};

		return {nullptr, nullptr};
	}

	T *get() const noexcept
	{
		return m_p;
	}

	uint32_t use_count() const noexcept
	{
		SYS_LOCK_READ(m_mx);
		return m_cnt? m_cnt->m_refs.GetCount(): 0;
	}

	bool expired() const noexcept
	{
		SYS_LOCK_READ(m_mx);
		return !m_cnt || !m_cnt->m_refs;
	}

	void DestroyObject() noexcept
	{
		if (m_p && m_cnt && m_cnt->m_refs.Release())
			delete m_p;
	}

protected:
	mutable Sys::CSpinSharedMutex m_mx;
	mutable CSharedCounter *m_cnt = nullptr;
	mutable T *m_p = nullptr;
};

template <typename T>
class CSharedPtr
: protected CSharedPtrBase<T>
{
template <typename T_> friend class CSharedPtr;
template <typename T_> friend class CWeakPtr;
protected:
	typedef CSharedPtrBase<T> TBase;
public:
	typedef T element_type;

	using TBase::get;
	using TBase::use_count;

	template <typename... TT>
	static CSharedPtr Make(TT&&... args)
	{
		return CSharedPtr(std::make_unique<T>(std::forward<TT>(args)...));
	};

	CSharedPtr() noexcept
	{
		static_assert(!std::is_base_of<std::enable_shared_from_this<T>, T>::value);
	}

	CSharedPtr(nullptr_t) noexcept
	{
	}

	template <typename T_> requires IsConvertiblePtr<T_, T> && !std::is_array<T_>::value
	CSharedPtr(std::unique_ptr<T_> &&sp)
	: TBase(sp)
	{
		static_assert(!std::is_base_of<std::enable_shared_from_this<T_>, T_>::value);
		SetSharedFromThis(sp.get());
		sp.release();
	}

	template <typename T_>
	explicit CSharedPtr(T_ *p)
	: CSharedPtr(std::unique_ptr<T_>(p))
	{
	}

	CSharedPtr(const CSharedPtr &sp) noexcept
	: TBase(sp.CaptureShared())
	{
	}

	CSharedPtr(CSharedPtr &&sp) noexcept
	: TBase(std::move(sp))
	{
	}

	template <typename T_> requires IsConvertiblePtr<T_, T>
	CSharedPtr(const CSharedPtr<T_> &sp) noexcept
	: TBase(sp.CaptureShared())
	{
	}

	template <typename T_> requires IsConvertiblePtr<T_, T>
	CSharedPtr(CSharedPtr<T_> &&sp) noexcept
	: TBase(std::move(sp))
	{
	}

	~CSharedPtr()
	{
		TBase::DestroyObject();
	}

	void reset(CSharedPtr &&sp) noexcept
	{
		TBase::reset(std::move(sp));
	}

	void reset(const CSharedPtr &sp) noexcept
	{
		if (this != &sp)
			TBase::reset(sp.CaptureShared());
	}

	template <typename... TT>
	void reset(TT&&... args)
	{
		reset(CSharedPtr(std::forward<TT>(args)...));
	}

	void Destroy();

	bool unique() const noexcept
	{
		return use_count() == 1;
	}

	explicit operator bool() const noexcept
	{
		return get() != nullptr;
	}

	T *operator->() const noexcept
	{
		return get();
	}

	T &operator *() const noexcept
	{
		return *get();
	}

	CSharedPtr &operator =(const CSharedPtr &src)
	{
		reset(src);
		return *this;
	}

	CSharedPtr &operator =(CSharedPtr &&src)
	{
		reset(std::move(src));
		return *this;
	}

	template <typename T_>
	CSharedPtr &operator =(T_ &&src)
	{
		reset(std::forward<T_>(src));
		return *this;
	}

	CWeakPtr<T> GetWeakPtr() const
	{
		return CWeakPtr<T>(*this);
	}

protected:
	explicit CSharedPtr(const TBase &sp)
	: TBase(sp.CaptureShared())
	{
	}

	template <typename T_>
	void SetSharedFromThis(T_ *p)
	{
	}

	void SetSharedFromThis(IsSharedFromThis *p);
};

template <typename T, typename... TT>
CSharedPtr<T> MakeShared(TT&&... args)
{
	return CSharedPtr<T>::Make(std::forward<TT>(args)...);
}

template <typename T>
class CWeakPtr
: protected CSharedPtrBase<T>
{
template <typename T_> friend class CSharedPtr;
template <typename T_> friend class CWeakPtr;
protected:
	typedef CSharedPtrBase<T> TBase;
public:
	using TBase::expired;
	using TBase::use_count;

	CWeakPtr() noexcept
	{
	}

	CWeakPtr(nullptr_t) noexcept
	{
	}

	CWeakPtr(CWeakPtr &&sp) noexcept
	: TBase(std::move(sp))
	{
	}

	CWeakPtr(const CWeakPtr &sp) noexcept
	: TBase(sp.CaptureWeak())
	{
	}

	template <typename T_> requires IsConvertiblePtr<T_, T>
	CWeakPtr(CWeakPtr<T_> &&sp) noexcept
	: TBase(std::move(sp))
	{
	}

	template <typename T_> requires IsConvertiblePtr<T_, T>
	CWeakPtr(const CWeakPtr<T_> &sp) noexcept
	: TBase(sp.CaptureWeak())
	{
	}

	template <typename T_> requires IsConvertiblePtr<T_, T>
	CWeakPtr(const CSharedPtr<T_> &sp) noexcept
	: TBase(sp.CaptureWeak())
	{
	}

	~CWeakPtr()
	{
	}

	CSharedPtr<T> lock() const noexcept
	{
		return CSharedPtr<T>(*this);
	}

	void reset(const CWeakPtr &sp) noexcept
	{
		if (this != &sp)
			TBase::reset(sp.CaptureWeak());
	}

	void reset(CWeakPtr &&sp) noexcept
	{
		TBase::reset(std::move(sp));
	}

	template <typename... TT>
	void reset(TT&&... args)
	{
		reset(CWeakPtr(std::forward<TT>(args)...));
	}

	explicit operator bool() const noexcept
	{
		return !expired();
	}

	CWeakPtr &operator =(const CWeakPtr &src)
	{
		reset(src);
		return *this;
	}

	CWeakPtr &operator =(CWeakPtr &&src)
	{
		reset(std::move(src));
		return *this;
	}

	template <typename T_>
	CWeakPtr &operator =(T_ &&src)
	{
		reset(std::forward<T_>(src));
		return *this;
	}
};

template <typename T>
class CEnableSharedFromThis
{
friend class CSharedPtr<T>;
public:
	CSharedPtr<T> GetSharedFromThis()
	{
		return m_spSharedFromThis.lock();
	}
protected:
	CWeakPtr<T> m_spSharedFromThis;
};

template <typename T> inline
void CSharedPtr<T>::SetSharedFromThis(IsSharedFromThis *p)
{
	if (p)
		p->m_spSharedFromThis.reset(*this);
}

template <typename T> inline
void CSharedPtr<T>::Destroy()
{
	CWeakPtr<T> sp(*this);
	reset();
	while (!sp.expired())
		std::this_thread::yield();
}

}
