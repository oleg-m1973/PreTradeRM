#pragma once
#include "Errors.h"

#include <tuple>
#include <functional>
#include <memory>

namespace TS
{

template <typename> struct CFunctionInvoker;
template <size_t... Indexes>
struct CFunctionInvoker<std::index_sequence<Indexes...>>
{
	template <typename TFunc, typename TArgs, typename... TT>
	static auto Invoke(TFunc &&func, TArgs &&args, TT&&... args2)
	{
		return std::invoke(std::forward<TFunc>(func), std::get<Indexes>(std::forward<TArgs>(args))..., std::forward<TT>(args2)...);
	}

	template <typename TFunc, typename TArgs, typename... TT>
	static auto InvokeOnce(TFunc &&func, TArgs &&args, TT&&... args2)
	{
		return std::invoke(std::move(func), std::move(std::get<Indexes>(std::forward<TArgs>(args)))..., std::forward<TT>(args2)...);
	}
};

template <typename> class CFunctionHolder;

template <typename TRes, typename... TArgs>
class CFunctionHolder<TRes(TArgs...)>
{
public:
	typedef std::unique_ptr<CFunctionHolder> TPtr;

	virtual ~CFunctionHolder() {;}

	virtual TRes Call(TArgs... args) = 0;

	TRes operator()(TArgs... args)
	{
		return Call(args...);
	}

};

template <typename THolder, typename TFunc, typename... TT>
class CFunctionImplBase
: public THolder
{
public:
	CFunctionImplBase(TFunc &&fn, TT&&... args)
	: m_fn(std::forward<TFunc>(fn))
	, m_args(std::forward<TT>(args)...)
	{
	}

protected:
	TFunc m_fn;
	std::tuple<std::decay_t<TT>...> m_args;
};

template <typename> class CFunctionBase;

template <typename _TRes, typename... TArgs>
class CFunctionBase<_TRes(TArgs...)>
{
public:
	typedef _TRes TRes;
	typedef std::tuple<TArgs...> TArgsArray;
	typedef std::index_sequence_for<TArgs...> TArgsIndexes;

	typedef CFunctionHolder<TRes(TArgs...)> THolder;
	typedef typename THolder::TPtr TPtr;

	CFunctionBase()
	{
	}

	TS_MOVABLE(CFunctionBase, default);
	TS_COPYABLE(CFunctionBase, delete);

	TRes operator()(TArgs... args) const
	{
		return m_sp->Call(args...);
	}

	void reset() noexcept
	{
		m_sp.reset();
	}

	TPtr release() noexcept
	{
		return std::move(m_sp);
	}

	explicit operator bool() const noexcept
	{
		return bool(m_sp);
	}

	THolder &operator *() const
	{
		return *m_sp;
	}

	void swap(CFunctionBase &obj)
	{
		std::swap(m_sp, obj.m_sp);
	}
protected:
	explicit CFunctionBase(TPtr &&sp)
	: m_sp(std::move(sp))
	{
	}

	TPtr m_sp;
};


template <typename> class CFunction;

template <typename TRes, typename... TArgs>
class CFunction<TRes(TArgs...)>
: public CFunctionBase<TRes(TArgs...)>
{
public:
	typedef CFunctionBase<TRes(TArgs...)> TBase;
	using TBase::reset;

	CFunction()
	{
	}

	template <typename TFunc, typename... TT>
	explicit CFunction(TFunc &&func, TT&&... args)
	: TBase(std::make_unique<CFunctionImpl<TFunc, TT...>>(std::forward<TFunc>(func), std::forward<TT>(args)...))
	{
	}

	template <typename TFunc, typename... TT>
	void reset(TFunc &&func, TT&&... args)
	{
		CFunction(std::forward<TFunc>(func), std::forward<TT>(args)...).swap(*this);
	}

protected:
	template <typename TFunc, typename... TT>
	class CFunctionImpl
	: public CFunctionImplBase<typename TBase::THolder, TFunc, TT...>
	{
	public:
		using CFunctionImplBase<typename TBase::THolder, TFunc, TT...>::CFunctionImplBase;

		virtual TRes Call(TArgs... args) override
		{
			return CFunctionInvoker<std::index_sequence_for<TT...>>::Invoke(this->m_fn, this->m_args, args...);
		}
	};
};

template <typename> class CThreadProc;

template <typename TRes, typename... TArgs>
class CThreadProc<TRes(TArgs...)>
: public CFunctionBase<TRes(TArgs...)>
{
public:
	typedef CFunctionBase<TRes(TArgs...)> TBase;
	using TBase::reset;

	CThreadProc()
	{
	}

	template <typename TFunc, typename... TT>
	explicit CThreadProc(TFunc &&func, TT&&... args)
	: TBase(std::make_unique<CFunctionImpl<TFunc, TT...>>(std::forward<TFunc>(func), std::forward<TT>(args)...))
	{
	}

	template <typename TFunc, typename... TT>
	void reset(TFunc &&func, TT&&... args)
	{
		CThreadProc(std::forward<TFunc>(func), std::forward<TT>(args)...).swap(*this);
	}

//	void operator()(TT&&... args) noexcept
//	{
//		TS_NOEXCEPT(this->m_sp->Call(std::forward<TT>(args)...));
//	}

protected:
	template <typename TFunc, typename... TT>
	class CFunctionImpl
	: public CFunctionImplBase<typename TBase::THolder, TFunc, TT...>
	{
	public:
		using CFunctionImplBase<typename TBase::THolder, TFunc, TT...>::CFunctionImplBase;

		virtual TRes Call(TArgs... args) override
		{
			return CFunctionInvoker<std::index_sequence_for<TT...>>::InvokeOnce(std::move(this->m_fn), std::move(this->m_args), args...);
		}
	};
};


}
