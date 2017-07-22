#include <signal.h>

#include <memory>
#include <libgen.h>

#include <dlfcn.h>
#include <cxxabi.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>
#include <execinfo.h>
#include <string.h>

#include <cxxabi.h>
#include <fcntl.h>

namespace
{

typedef struct _sig_ucontext
{
	unsigned long uc_flags;
	struct ucontext *uc_link;
	stack_t uc_stack;
	struct sigcontext uc_mcontext;
	sigset_t uc_sigmask;
} sig_ucontext_t;

}

inline
const char *BaseName(const char *path)
{
	auto psz = path;
	while(*path)
		if (*(path++) == '/')
			psz = path;

	return psz;
}

template <typename TFunc>
void TraceStack(int depth, TFunc &&func)
{
	void *addrs[depth] = {0};
	depth = backtrace(addrs, depth);
	Dl_info info = {0};
	for (int i = 0; i < depth; ++i)
	{
		if (!dladdr(addrs[i], &info))
			continue;

		int status = 0;
		std::unique_ptr<char, void(*)(void *)> sp(abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status), std::free);
		func(i, status == 0? sp.get(): info.dli_sname, info);
	}
}

template <typename... TT>
void WriteLog(FILE *f, const char *fmt, TT... args)
{
	fprintf(f, fmt, args...);
	fprintf(f, "\n");
	fflush(f);

	fprintf(stderr, fmt, args...);
	fprintf(stderr, "\r\n");
}

static
void WriteFile()
{
	char buf[256] = {0};
	sprintf(buf, "%s.SIGSEGV", program_invocation_short_name);

	int fd = open(buf, O_CREAT | O_APPEND | O_WRONLY, 0600);
	write(fd, "\n", 1);

	void *addrs[50] = {0};
	const auto n = backtrace(addrs, sizeof(addrs) / sizeof(*addrs));

	backtrace_symbols_fd(addrs, n, fd);

	close(fd);
}

static
void crit_err_hdlr(int sig_num, siginfo_t *info, void *ucontext)
{
	WriteFile();

	{
		char buf[256] = {0};
		sprintf(buf, "%s.StackTrace.%p", program_invocation_short_name, info);

		FILE *pFile(::fopen(buf, "a"));

		const time_t t = time(nullptr);
		fprintf(pFile, "\n%s\n", ctime(&t));

		WriteLog(pFile, "signal %d", sig_num);
		TraceStack(50, [pFile](int i, const char *name, const auto &info)
		{
			if (i == 0 || !name)
				return;

			WriteLog(pFile, "[bt]: (%d) %s, %s", i, name, BaseName(info.dli_fname));
		});

		fclose(pFile);
	}
	exit(EXIT_FAILURE);
}

void CatchSegmentationsFault()
{
	struct sigaction sigact = {0};

	sigact.sa_sigaction = crit_err_hdlr;
	sigact.sa_flags = SA_RESTART | SA_SIGINFO;

	if (sigaction(SIGSEGV, &sigact, (struct sigaction *)NULL) != 0)
		fprintf(stderr, "error setting signal handler for SIGSEGV\r\n");

	if (sigaction(SIGABRT, &sigact, (struct sigaction *)NULL) != 0)
		fprintf(stderr, "error setting signal handler for SIGABRT\r\n");

}

