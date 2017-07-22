#pragma once
#include "SyncObjs.h"
#include "Function.h"
#include "Errors.h"
#include "SharedPtr.h"

#include <list>
#include <map>
#include <vector>


namespace TS
{
template <typename _TCallback, typename _TKey> class CCallbackManager;

template <typename T>
class CCallback
: public std::shared_ptr<T>
{
template <typename TCallback, typename TKey> friend class CCallbackManager;
public:
	using std::shared_ptr<T>::get;

	struct CWeakRef
	{
	public:
		CWeakRef(std::weak_ptr<T> sp)
		: m_sp(std::move(sp))
		{
		}

		TS_MOVABLE(CWeakRef, default);
		TS_COPYABLE(CWeakRef, delete);

		template <typename... TT>
		bool operator()(TT&&... args) const
		{
			SYS_LOCK(m_mx);
			auto sp = m_sp.lock();
			if (!sp)
				return false;

			(*sp)(std::forward<TT>(args)...);
			return true;
		}

		template <typename TFunc, typename... TT>
		bool Invoke(TFunc &&func, TT&&... args) const
		{
			SYS_LOCK(m_mx);
			auto sp = m_sp.lock();
			if (!sp)
				return false;

			_Invoke(*sp, std::forward<TFunc>(func), std::forward<TT>(args)...);
			return true;
		}

		bool expired() const
		{
			SYS_LOCK(m_mx);
			return m_sp.expired();
		}

		void reset()
		{
			SYS_LOCK(m_mx);
			m_sp.reset();
		}

		std::shared_ptr<T> GetObject() const
		{
			SYS_LOCK(m_mx);
			return m_sp.lock();
		}
	protected:
		template <typename TFunc, typename... TT>
		static void _Invoke(T &obj, TFunc &&func, TT&&... args)
		{
			func(std::forward<TT>(args)..., obj);
		}

		template <typename TFunc, typename... TT> requires std::is_member_function_pointer<TFunc>::value
		static void _Invoke(T &obj, TFunc &&func, TT&&... args)
		{
			(obj.*func)(std::forward<TT>(args)...);
		}

		mutable std::recursive_mutex m_mx;
		std::weak_ptr<T> m_sp;
	};

	template<typename T_, typename... TT>
	static CCallback Create(TT&&... args)
	{
		auto sp = std::make_unique<T_>(std::forward<TT>(args)...);
		return CCallback(std::move(sp));
	}

	template <typename T_> requires std::is_base_of<T, T_>::value
	static CCallback Create(std::unique_ptr<T_> &&sp)
	{
		return CCallback(std::move(sp));
	}

	template<typename... TT>
	static CCallback Create(TT&&... args)
	{
		auto sp = std::make_unique<T>(std::forward<TT>(args)...);
		return CCallback(std::move(sp));
	}

	CCallback()
	{
	}

	explicit CCallback(std::shared_ptr<T> sp)
	: std::shared_ptr<T>(std::move(sp))
	, m_weak(new CWeakRef(*this))
	{
	}

	TS_MOVABLE(CCallback, default);
	TS_COPYABLE(CCallback, delete);

	~CCallback()
	{
		reset();
	}

	void reset()
	{
		auto weak = std::move(m_weak);
		if (weak)
			weak->reset();

		std::shared_ptr<T>::reset();
	}

	bool expired() const
	{
		return !m_weak || m_weak->expired();
	}

	template <typename TFunc, typename... TT>
	bool Invoke(TFunc &&func, TT&&... args) const
	{
		if (!get())
			return false;

		func(std::forward<TT>(args)..., *get());
		return true;
	}

	template <typename TFunc, typename... TT> requires std::is_member_function_pointer<TFunc>::value
	bool Invoke(TFunc &&func, TT&&... args) const
	{
		if (!get())
			return false;

		(get()->*func)(std::forward<TT>(args)...);
		return true;
	}

	template <typename... TT>
	bool operator()(TT&&... args) const
	{
		if (!get())
			return false;

		(*get())(std::forward<TT>(args)...);
		return true;
	}

	Sys::CWeakPtr<CWeakRef> GetWeakRef() const
	{
		return m_weak;
	}

protected:
	Sys::CSharedPtr<CWeakRef> m_weak;
};

template <typename TRes, typename... TT>
class CCallback<CFunction<TRes(TT...)>>
: public CCallback<typename TS::CFunction<TRes(TT...)>::THolder>
{
template <typename TCallback, typename TKey> friend class CCallbackManager;
public:
	typedef CCallback<typename TS::CFunction<TRes(TT...)>::THolder> TBase;

	CCallback()
	{
	}

	template <typename TFunc, typename... TT_>
	static CCallback Create(TFunc &&func, TT_&&... args)
	{
		return CCallback(TS::CFunction<TRes(TT...)>(std::forward<TFunc>(func), std::forward<TT_>(args)...));
	}

	explicit CCallback(typename TS::CFunction<TRes(TT...)>::TPtr sp)
	: TBase(std::move(sp))
	{
	}

	explicit CCallback(TS::CFunction<TRes(TT...)> &&fn)
	: TBase(fn.release())
	{
	}
};

template <typename TRes, typename... TT>
class CCallback<TRes(TT...)>
: public CCallback<TS::CFunction<TRes(TT...)>>
{
public:
	using CCallback<TS::CFunction<TRes(TT...)>>::CCallback;

	template <typename TFunc, typename... TT_>
	static CCallback Create(TFunc &&func, TT_&&... args)
	{
		return CCallback(TS::CFunction<TRes(TT...)>(std::forward<TFunc>(func), std::forward<TT_>(args)...));
	}
};

template <typename _TCallback>
class CCallbackManager<_TCallback, void>
{
public:
	typedef CCallbackManager TCallbackServer;
	typedef _TCallback TCallback;
	typedef CCallback<TCallback> TCallbackHolder;
	typedef TCallbackHolder TCallbackPtr;

	template <typename T, typename... TT>
	static TCallbackPtr CreateCallback(TT&&... args)
	{
		return TCallbackHolder::template Create<T>(std::forward<TT>(args)...);
	}

	const TCallbackPtr &RegisterCallback(const TCallbackPtr &sp)
	{
		SYS_LOCK(m_items);
		m_items.emplace_back(sp.GetWeakRef());
		return sp;
	}

	template <typename T, typename... TT>
	TCallbackPtr RegisterCallback(TT&&... args)
	{
		auto sp = TCallbackHolder::template Create<T>(std::forward<TT>(args)...);
		RegisterCallback(sp);
		return sp;
	}

	template <typename T, typename... TT> requires !std::is_same<TCallbackPtr, std::decay_t<T>>::value
	TCallbackPtr RegisterCallback(T &&arg, TT&&... args)
	{
		auto sp = TCallbackHolder::template Create(std::forward<T>(arg), std::forward<TT>(args)...);
		RegisterCallback(sp);
		return sp;
	}

	template <typename TFunc, typename... TT>
	void ForEachCallback(TFunc &&func, TT&&... args)
	{
		auto items = GetCallbacks();
		for (auto &item: items)
			item->Invoke(func, args...);
	}

	template <typename... TT>
	void ForEachCallback2(TT&&... args)
	{
		auto items = GetCallbacks();
		for (auto &item: items)
			(*item)(args...);
	}

	template <typename TFunc>
	auto GetCallbacks(TFunc &&func)
	{
		std::list<Sys::CSharedPtr<TWeakRef>> res;
		{
			SYS_LOCK(m_items);
			_GetCallbacks(res);
		}

		res.remove_if([&func](auto &item)
		{
			bool remove = false;
			const bool remove2 = !item->Invoke([&func, &remove](auto &cb)
			{
				remove = !func(cb);
			});

			return remove || remove2;
		});

		return res;
	}

	auto GetCallbacks()
	{
		SYS_LOCK(m_items);
		std::vector<Sys::CSharedPtr<TWeakRef>> res;
		res.reserve(m_items.size());
		return _GetCallbacks(std::move(res));
	}

	size_t GetCallbacksCount() const
	{
		SYS_LOCK(m_items);
		return m_items.size();
	}

	bool empty() const
	{
		return GetCallbacksCount() == 0;
	}
protected:
	template <typename TItems>
	TItems &&_GetCallbacks(TItems &&items)
	{
		m_items.remove_if([&items](auto &item)
		{
			auto sp = item.lock();
			if (!sp)
				return true;

			items.emplace_back(std::move(sp));
			return false;
		});

		return std::forward<TItems>(items);
	}

	typedef typename TCallbackHolder::CWeakRef TWeakRef;

	mutable Sys::CLockedObject<std::list<Sys::CWeakPtr<TWeakRef>>> m_items;
};

template <typename _TCallback, typename TKey = void>
class CCallbackManager
{
protected:
	typedef CCallbackManager<_TCallback, void> TManager;
public:
	typedef CCallbackManager TCallbackServer;
	typedef _TCallback TCallback;
	typedef CCallback<TCallback> TCallbackHolder;
	typedef CCallback<TCallback> TCallbackPtr;

	const TCallbackPtr &RegisterCallback(const TKey &key, const TCallbackPtr &sp)
	{
		return GetManager(key, true)->RegisterCallback(sp);
	}

	template <typename T, typename... TT> requires !std::is_same<TCallbackPtr, std::decay_t<T>>::value
	TCallbackPtr RegisterCallback(const TKey &key, T &&arg, TT&&... args)
	{
		return GetManager(key, true)->RegisterCallback(std::forward<T>(arg), std::forward<TT>(args)...);
	}

	template <typename TFunc, typename... TT>
	void ForEachCallback(const TKey &key, TFunc &&func, TT&&... args)
	{
		auto *p = GetManager(key, false);
		if (p)
			p->ForEachCallback(std::forward<TFunc>(func), std::forward<TT>(args)...);
	}

	template <typename... TT>
	auto GetCallbacks(const TKey &key, TT&&... args)
	{
		auto *p = GetManager(key, false);
		if (p)
			return p->GetCallbacks(std::forward<TT>(args)...);

		typedef decltype(p->GetCallbacks(std::forward<TT>(args)...)) TRes;
		return TRes();
	}

	bool HasCallbacks(const TKey &key) const
	{
		SYS_LOCK(m_items);
		auto it = m_items.find(key);
		return it != m_items.end() && !it->second->empty();
	}

protected:
	TManager *GetManager(const TKey &key, bool Create)
	{
		SYS_LOCK(m_items);
		auto it = m_items.find(key);
		if (it != m_items.end())
			return it->second.get();

		if (!Create)
			return nullptr;

		return m_items.emplace(key, std::make_unique<TManager>()).first->second.get();
	}


	mutable Sys::CLockedObject<std::map<TKey, std::unique_ptr<TManager>>> m_items;
};





}
