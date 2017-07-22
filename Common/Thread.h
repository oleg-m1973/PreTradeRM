#pragma once
#include "SyncObjs.h"
#include "Errors.h"
#include "Function.h"

#include <thread>
#include <future>
#include <functional>
#include <list>

namespace Sys
{
class CSingleEvent;

inline
const void *CheckRaised()
{
	return nullptr;
}

template <typename TEvent, typename... TEvents> inline
const void *CheckRaised(TEvent &&ev, TEvents&&... events)
{
	return ev.CheckRaised() ? &ev : CheckRaised(std::forward<TEvents>(events)...);
}


class CThreadControl
{
friend class CSingleEvent;
public:
	TS_COPYABLE(CThreadControl, delete);
	TS_MOVABLE(CThreadControl, delete)

	CThreadControl()
	{
	}

	~CThreadControl()
	{
	}

	void RaiseStop() noexcept
	{
		SYS_LOCK(m_mx);
		m_stop = true;
		m_cv.notify_all();
	}

	void ResetStop()
	{
		SYS_LOCK(m_mx);
		m_stop = false;
	}

	bool IsStop() const noexcept
	{
		return m_stop;
	}

	const void *Wait() const noexcept
	{
		SYS_UNIQUE_LOCK(m_mx, lock);

		while (!IsStop())
			m_cv.wait(lock);

		return nullptr;
	}


	template <typename Clock, typename Duration>
	const void *Wait(std::chrono::time_point<Clock, Duration> abs_time) const noexcept
	{
		SYS_UNIQUE_LOCK(m_mx, lock);
		while (!IsStop())
		{
			if (m_cv.wait_until(lock, abs_time) == std::cv_status::timeout)
				return this;
		}

		return nullptr;
	}

	template <typename Clock, typename Duration, typename TEvent, typename... TEvents>
	const void *Wait(std::chrono::time_point<Clock, Duration> abs_time, TEvent &&ev, TEvents&&... events) const noexcept
	{
		SYS_UNIQUE_LOCK(m_mx, lock);
		while (!IsStop())
		{
			const void *ptr = CheckRaised(ev, events...);
			if (ptr)
				return ptr;

			const auto res = m_cv.wait_until(lock, abs_time);
			if (IsStop())
				break;

			ptr = CheckRaised(ev, events...);
			if (ptr)
				return ptr;

			if (res == std::cv_status::timeout)
				return this;
		}
		return nullptr;
	}


	template <typename Rep, typename Period, typename... TEvents>
	const void *Wait(std::chrono::duration<Rep, Period> dt, TEvents&&... events) const noexcept
	{
		return Wait(std::chrono::system_clock::now() + dt, std::forward<TEvents>(events)...);
	}

	template <typename Rep, typename Period, typename... TEvents>
	const void *Wait(bool infinite, std::chrono::duration<Rep, Period> dt, TEvents&&... events) const noexcept
	{
		return infinite? Wait(std::forward<TEvents>(events)...): Wait(dt, std::forward<TEvents>(events)...);
	}

	template <typename TEvent, typename... TEvents>
	const void *Wait(TEvent &&ev, TEvents&&... events) const noexcept
	{
		SYS_UNIQUE_LOCK(m_mx, lock);
		while (!IsStop())
		{
			const void *ptr = CheckRaised(ev, events...);
			if (ptr)
				return ptr;

			m_cv.wait(lock);
		}
		return nullptr;
}


	std::mutex &GetMutex() const
	{
		return m_mx;
	}

	std::condition_variable &GetCV() const
	{
		return m_cv;
	}

private:
	volatile bool m_stop = false;
	mutable std::mutex m_mx;
	mutable std::condition_variable m_cv;
};

class CSingleEvent
{
public:
	CSingleEvent()
	{
	}

	CSingleEvent(bool raised)
	{
		if (raised)
			Set();
	}

	void Set() noexcept
	{
		m_ctrl.RaiseStop();
	}

	void Reset() noexcept
	{
		SYS_LOCK(m_ctrl.GetMutex());
		m_ctrl.m_stop = false;
		m_ctrl.GetCV().notify_all();
	}

	bool IsSet() const noexcept
	{
		return m_ctrl.IsStop();
	}

	bool Wait() const noexcept
	{
		return m_ctrl.Wait() == nullptr;
	}

	template <typename Rep, typename Period>
	bool Wait(std::chrono::duration<Rep, Period> tm) const noexcept
	{
		return m_ctrl.Wait(std::move(tm)) == nullptr;
	}

	template <typename Clock, typename Duration>
	bool Wait(std::chrono::time_point<Clock, Duration> abs_time) const noexcept
	{
		return m_ctrl.Wait(std::move(abs_time)) == nullptr;
	}
protected:
	CThreadControl m_ctrl;
};

class CEventBase
{
public:
	CEventBase(CThreadControl &ctrl, bool raised = false)
	: m_raised(raised)
	, m_ctrl(ctrl)
	{
	}

	void Set() noexcept
	{
		SYS_LOCK(m_ctrl.GetMutex());
		if (!m_raised)
		{
			m_raised = true;
			m_ctrl.GetCV().notify_all();
		}
	}

	void Reset() noexcept
	{
		SYS_LOCK(m_ctrl.GetMutex());
		m_raised = false;
	}

	explicit operator bool() const
	{
		return m_raised;
	}

protected:
	volatile bool m_raised;
	CThreadControl &m_ctrl;
};

template <bool ManualReset = true>
class CEvent
: public CEventBase
{
public:
	CEvent(CThreadControl &ctrl, bool raised = false)
	: CEventBase(ctrl, raised)
	{
	}

	bool CheckRaised() noexcept;
};

template <> inline
bool CEvent<true>::CheckRaised() noexcept
{
	return m_raised;
};

template <> inline
bool CEvent<false>::CheckRaised() noexcept
{
	const bool raised = m_raised;
	m_raised = false;
	return raised;
};


class CThread
: public CThreadControl
{
public:
	CThread()
	{
	}

	template <typename TFunc, typename... TT>
	explicit CThread(TFunc &&func, TT&&... args)
	{
		Start(std::forward<TFunc>(func), std::forward<TT>(args)...);
	}

	TS_MOVABLE(CThread, default);
	TS_COPYABLE(CThread, delete);

	~CThread()
	{
		_Stop();
	}

	template <typename TFunc, typename... TT>
	bool Start(TFunc &&func, TT&&... args)
	{
		SYS_LOCK(m_mx);
		_Stop();
		ResetStop();
//		auto fn = std::bind(std::forward<TFunc>(func), std::forward<TT>(args)..., std::ref(*this));
//		m_thread = std::thread([fn = std::move(fn)]()
//		{
//			TS_NOEXCEPT(fn());
//		});

		auto fn = TS::CThreadProc<void(Sys::CThreadControl &)>(std::forward<TFunc>(func), std::forward<TT>(args)...);
		m_thread = std::thread([this, fn = std::move(fn)]() mutable
		{
			TS_NOEXCEPT(fn(*this));
		});

		return true;
	}

	void Stop() noexcept
	{
		SYS_LOCK(m_mx);
		_Stop();
	}

	bool IsCurrentThread() const
	{
		return m_thread.get_id() == std::this_thread::get_id();
	}
protected:
	void _Stop() noexcept
	{
		auto thread = std::move(m_thread);
		if (thread.joinable())
		{
			RaiseStop();
			TS_NOEXCEPT(thread.join());
		}
	}

	std::mutex m_mx;
	std::thread m_thread;
};

class CThreadPool
{
public:
	CThreadPool(size_t max_threads = 128, size_t permanent = 32)
	: m_permanent(permanent)
	, m_max_threads(max_threads ? max_threads : std::numeric_limits<size_t>::max())
	{
		m_thread.Start(&CThreadPool::CleanupThreadProc, this);
	}

	~CThreadPool()
	{
		m_thread.Stop();
		_Stop();
	}

	template <typename Rep, typename Period, typename TFunc, typename... TT>
	bool RunWait(const std::chrono::duration<Rep, Period> &tm, TFunc &&func, TT&&... args)
	{
		auto sp = CaptureThread(tm);
		if (!sp)
			return false;

		sp->Run(std::forward<TFunc>(func), std::forward<TT>(args)...);

		SYS_LOCK(m_threads);
		m_threads.emplace_back(std::move(sp));
		return true;
	}

	template <typename TFunc, typename... TT>
	bool Run(TFunc &&func, TT&&... args)
	{
		return RunWait(0ms, std::forward<TFunc>(func), std::forward<TT>(args)...);
	}

	template <typename TFunc, typename... TT>
	void RunAnyway(TFunc &&func, TT&&... args)
	{
		auto sp = CaptureThread(0ms);
		if (!sp)
		{
			CThreadControl ctrl;
			func(std::forward<TT>(args)..., ctrl);
			return;
		}

		sp->Run(std::forward<TFunc>(func), std::forward<TT>(args)...);

		SYS_LOCK(m_threads);
		m_threads.emplace_back(std::move(sp));
	}

	void Stop() noexcept
	{
		m_thread.Stop();
		_Stop();
		TS_NOEXCEPT(m_thread.Start(&CThreadPool::CleanupThreadProc, this));
	}

	size_t GetThreadCount() const
	{
		return m_cnt;
	}

protected:
	struct XThread
	: public CThreadControl
	{
		XThread(CThreadPool &owner)
		: m_owner(owner)
		{
			++m_owner.m_cnt;
			const int res = pthread_create(&m_thread, nullptr, &XThread::_ThreadProc, this);
			if (res != 0)
				TS_RAISE_ERROR("pthread_create failed", Sys::Error(res));
		}

		~XThread()
		{
			Stop();
			SYS_LOCK(m_owner.m_pool);
			if (--m_owner.m_cnt < m_owner.m_max_threads)
				m_owner.m_evReady.Set();
		}

		template <typename TFunc, typename... TT>
		void Run(TFunc &&func, TT&&... args)
		{
			m_fn.reset(std::forward<TFunc>(func), std::forward<TT>(args)...); //, std::ref(*this));
			m_evReady.Set();
		}

		void Stop() noexcept
		{
			RaiseStop();

			SYS_LOCK(m_mx);
			if (m_thread)
			{
				pthread_join(m_thread, nullptr);
				m_thread = 0;
			}
		}

		bool StopFinished() noexcept
		{
			if (!m_finished)
				return false;

			Stop();
			return true;
		}

		void ThreadProc()
		{
			while (Wait(m_evReady) != nullptr)
			{
				TS_NOEXCEPT((*m_fn)(*this));
				m_fn.reset();
				m_owner.ReleaseThread(*this);
			}

			m_finished = true;
		}

		static void *_ThreadProc(void *ptr)
		{
			reinterpret_cast<XThread *>(ptr)->ThreadProc();
			return nullptr;
		}

		const bool IsRunning() const
		{
			return bool(m_fn);
		}

		CThreadPool &m_owner;

		CEvent<false> m_evReady{*this, false};
		TS::CThreadProc<void(Sys::CThreadControl &)> m_fn;

		bool m_finished = false;
		pthread_t m_thread = 0;

		std::mutex m_mx;
	};

	void _Stop() noexcept
	{
		StopThreads(Sys::Locked::Move(m_threads));
		StopThreads(Sys::Locked::Move(m_pool));
	}

	std::unique_ptr<XThread> GetThreadFromPool()
	{
		SYS_LOCK(m_pool);
		if (!m_pool.empty())
		{
			auto sp = std::move(m_pool.front());
			m_pool.pop_front();
			return std::move(sp);
		}
		else if (m_cnt >= m_max_threads)
			m_evReady.Reset();

		return nullptr;
	}

	template <typename Rep, typename Period>
	std::unique_ptr<XThread> CaptureThread(const std::chrono::duration<Rep, Period> &timeout)
	{
		const auto tm = std::chrono::steady_clock::now() + timeout;
		for(;;)
		{
			auto sp = GetThreadFromPool();
			if (sp)
				return sp;

			if (m_cnt < m_max_threads)
				break;

			if (!m_evReady.Wait(tm))
				return nullptr;
		}

		return std::make_unique<XThread>(*this);
	}

	void ReleaseThread(XThread &thread) noexcept
	{
		m_evCleanup.Set();
	}

	std::unique_ptr<XThread> PushToPool(std::unique_ptr<XThread> sp)
	{
		SYS_LOCK(m_pool);
		if (m_pool.size() < m_permanent)
		{
			m_pool.emplace_front(std::move(sp));
			m_evReady.Set();
		}
		return sp;
	}

	template <typename TThreads> static
	void StopThreads(TThreads &&threads) noexcept
	{
		for (auto &sp : threads)
			sp->RaiseStop();

		TS_NOEXCEPT(threads.clear());
	}

	void CleanupThreadProc(CThreadControl &thread)
	{
		Sys::CLockedObject<std::list<std::unique_ptr<XThread>>> cleanups;
		Sys::CThread thread2;
		CEvent<false> ev(thread2);
		thread2.Start([&cleanups, &ev](auto &thread)
		{
			std::list<std::unique_ptr<XThread>> items;
			while (thread.Wait(items.empty(), 10ms, ev) != nullptr)
			{
				Sys::Locked::SpliceFront(items, cleanups);
				items.remove_if([](auto &item)
				{
					return item->StopFinished();
				});
			};
			StopThreads(std::move(items));
		});


		std::list<std::unique_ptr<XThread>> threads;
		while (thread.Wait(threads.empty(), 10ms, m_evCleanup) != nullptr)
		{
			try
			{
				Sys::Locked::SpliceFront(threads, m_threads);
				threads.remove_if([this, &cleanups, &ev](auto &item)
				{
					if (item->IsRunning())
						return false;

					auto sp = this->PushToPool(std::move(item));
					if (sp)
					{
						sp->RaiseStop();
						SYS_LOCK(cleanups)
						cleanups.emplace_back(std::move(sp));
						ev.Set();
					}

					return true;
				});
			}
			TS_CATCH;
		}

		thread2.Stop();

		StopThreads(std::move(threads));
		StopThreads(std::move(cleanups));
	}


	const size_t m_permanent;
	const size_t m_max_threads;


	Sys::CSingleEvent m_evReady;

	std::atomic<size_t> m_cnt{0};
	Sys::CLockedObject<std::list<std::unique_ptr<XThread>>, std::mutex> m_threads;
	Sys::CLockedObject<std::list<std::unique_ptr<XThread>>, std::mutex> m_pool;

	CThread m_thread;
	CEvent<false> m_evCleanup{m_thread};
};

}

