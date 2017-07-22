#pragma once
#include "Format.h"

#include <sstream>
#include <errno.h>
#include <string.h>

#define TS_CATCH_(HandleError, ...) \
	catch(const std::exception &e) {HandleError(e.what(), ##__VA_ARGS__);} \
	catch(...) {HandleError("Unhandled exception", ##__VA_ARGS__);}

#define TS_CATCH TS_CATCH_(TS::HandleError, TS_FILE_LINE);

#define TS_RAISE_ERROR(msg, ...) Sys::RaiseError(msg, ##__VA_ARGS__, TS_FILE_LINE)

#define TS_NOEXCEPT(expr, ...) {try {expr;} TS_CATCH;}
#define TS_NOEXCEPT_QUIET(expr) {try {expr;} catch(...) {;}}

#define SYS_VERIFY(expr, ...) Sys::Verify(expr, TS_FILE_LINE, #expr, ##__VA_ARGS__)

namespace TS
{

template <typename TError, typename... TT>
void HandleError(TError &&e, TT&&... args) noexcept
{
	Log.Error(e, std::forward<TT>(args)...);
}

}


namespace Sys
{
template <typename... TT> inline
void RaiseError(TT&&... args)
{
	std::ostringstream ss;
	TS::FormatVals(ss, std::forward<TT>(args)...);
	throw std::runtime_error(ss.str());
}

struct Error
{
	Error(int err)
	: m_err(err)
	{
	}

	int m_err;
};

template <typename... TT> inline
int Verify(const int res, TS::FileLine file_line, TT&&... args)
{
	const int err = errno;
	if (res < 0)
		RaiseError(Error(err), std::forward<TT>(args)..., file_line);

	return res;
}
}

inline
std::ostream &operator <<(std::ostream &out, const Sys::Error &val)
{
	out << "errno=" << val.m_err << ", " << ::strerror(val.m_err);
    return out;
}
