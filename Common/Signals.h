#pragma once
#include <signal.h>

class CWaitSignals
{
public:

	template <typename... TT>
	CWaitSignals(int sig1, TT... sigN)
	{
		m_act = {0};
		m_act.sa_handler = SignalsHandler;
		::sigemptyset(&m_signals);

		AddSignals(sig1, sigN...);
	}

	int Wait() noexcept
	{
		int signal = -1;
		::sigwait(&m_signals, &signal);
		return signal;
	}

protected:
	static void SignalsHandler(int) {;}

	void AddSignals(int sig)
	{
		::sigaddset(&m_signals, sig);
		::sigaction(sig, &m_act, nullptr);
	}

	template <typename... TT>
	void AddSignals(int sig1, int sig2, TT... sigN)
	{
		AddSignals(sig1);
		AddSignals(sig2, sigN...);
	}

	sigset_t m_signals;
	struct sigaction m_act;
};

inline
void WaitStop()
{
	CWaitSignals signals(SIGINT, SIGTERM, SIGHUP, SIGTSTP);
	signals.Wait();
}

extern void CatchSegmentationsFault();
