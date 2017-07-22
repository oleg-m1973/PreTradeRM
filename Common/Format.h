#pragma once
#include <ostream>
#include <typeinfo>
#include <memory>
#include <sstream>

#include <cxxabi.h>


namespace TS
{
typedef std::ostream TFormatOutput;

template <typename T>
TFormatOutput &FormatVal(TFormatOutput &out, const T &val)
{
	out << val;
	return out;
}

template <typename T>
TFormatOutput &&FormatVal(TFormatOutput &&out, const T &val)
{
	FormatVal(out, val);
	return std::move(out);
}

template <typename T>
TFormatOutput &FormatVal(TFormatOutput &out, const std::atomic<T> &val)
{
	return FormatVal(out, T(val));
}

template <typename T> requires std::is_enum<T>::value
TFormatOutput &FormatVal(TFormatOutput &out, const T &val)
{
	return FormatVal(out, static_cast<std::underlying_type_t<T>>(val));
}

template <typename TStream>
TStream &&FormatTypeName(TStream &&out, const char *mangled)
{
	int status = 0;
	std::unique_ptr<char, void(*)(void *)> sp(abi::__cxa_demangle(mangled, nullptr, nullptr, &status), std::free);
	out << (sp && status == 0? sp.get(): mangled);
	return std::forward<TStream>(out);
}

#define TS_FILE_LINE TS::FileLine(__FILE__, __LINE__)
struct FileLine
{
	FileLine()
	{
	}

	template <size_t sz>
	FileLine(const char (&__file__)[sz], int __line__)
	: m_file(__file__, __file__ + (sz - 1))
	, m_line(__line__)
	{
	}

	void FormatVal(TFormatOutput &out) const
	{
		if (!m_file.first)
			return;

		auto psz = *m_file.second == '/'? m_file.second - 1: m_file.second;
		while (*psz != '/' && psz != m_file.first)
			--psz;

		out << psz << ":" << m_line;
	}

	std::pair<const char *, const char *> m_file{nullptr, nullptr};
	int m_line = 0;
};


inline
TFormatOutput &FormatDateTime(TFormatOutput &out, const std::chrono::system_clock::time_point &val, const char *fmt, bool utc)
{
	auto format = [&out](int digs, int val)
	{
		static const size_t _sz = std::numeric_limits<int>::digits +1;
		char buf[_sz + 1];
		const int n = snprintf(buf, _sz + 1, "%0*d", digs, val);
		if (n > 0)
			out.write(buf, n);
	};

	const time_t t = std::chrono::system_clock::to_time_t(val);
	struct tm tm = {0};
	if (utc)
		gmtime_r(&t, &tm);
	else
		localtime_r(&t, &tm);

	for (const char *psz = fmt; *psz; )
	{
		const char ch = *(psz++);

		if (ch != '%')
			out.put(ch);
		else
		{
			const char ch2 = *(psz++);
			switch (ch2)
			{
			case 'y': format(2, tm.tm_year % 100); break;
			case 'Y': format(4, tm.tm_year + 1900); break;
			case 'm': format(2, tm.tm_mon + 1); break;
			case 'd': format(2, tm.tm_mday); break;
			case 'H': format(2, tm.tm_hour); break;
			case 'M': format(2, tm.tm_min); break;
			case 'S': format(2, tm.tm_sec); break;
			case 'l': format(3, std::chrono::duration_cast<std::chrono::milliseconds>(val.time_since_epoch()).count() % 1000); break;
			case 's':
				{
					format(2, tm.tm_sec); break;
					const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(val.time_since_epoch()).count() % 1000;
					if (ms)
					{
						out.put('.');
						format(3, ms);
					}

					break;
				}
			case 'c': format(6, std::chrono::duration_cast<std::chrono::microseconds>(val.time_since_epoch()).count() % std::chrono::microseconds::period::den); break;
			case '%': out.put('%'); break;
			case 0: break;
			default:
				out.put('%').put(ch2);
			}
		}
	}
	return out;
}

inline
TFormatOutput &FormatVal(TFormatOutput &out, const std::chrono::system_clock::time_point &val, const char *fmt = nullptr)
{
	TS::FormatDateTime(out, val, fmt? fmt: "%Y-%m-%d %H:%M:%S.%l", false);
	return out;
}

template <typename T> constexpr int duration_digits() {return 0;}
template <> constexpr int duration_digits<std::chrono::milliseconds>() {return 3;}
template <> constexpr int duration_digits<std::chrono::microseconds>() {return 6;}
template <> constexpr int duration_digits<std::chrono::nanoseconds>() {return 9;}

template <typename Rep, typename Period>
TFormatOutput &FormatVal(TFormatOutput &out, std::chrono::duration<Rep, Period> val)
{
	const bool neg = val.count() < 0;
	if (neg)
	{
		val = -val;
		out << "-";
	}

	const auto s = std::chrono::duration_cast<std::chrono::seconds>(val);

	const size_t sz = 255;
	char buf[sz + 1];
	buf[sz] = 0;

	int res = 0;
	if (s.count() >= 3600)
		res = snprintf(buf, sz, "%ld:%02ld:%02ld", std::chrono::duration_cast<std::chrono::hours>(val).count(), std::chrono::duration_cast<std::chrono::minutes>(val).count() % 60, s.count() % 60);
	else if (s.count() >= 60)
		res = snprintf(buf, sz, "%ld:%02ld", std::chrono::duration_cast<std::chrono::minutes>(val).count() % 60, s.count() % 60);
	else if (s.count())
		res = snprintf(buf, sz, "%ld", s.count() % 60);
	else
		res = snprintf(buf, sz, "0");

	if (res < 0)
	{
		out << val.count();
		return out;
	}

	const std::chrono::duration<Rep, Period> rest = val - std::chrono::duration_cast<std::chrono::duration<Rep, Period>>(s);
	if (rest.count())
	{
		res += snprintf(buf + res, sz - res, ".%0*ld", duration_digits<std::chrono::duration<Rep, Period>>(), intmax_t(rest.count()));
		for (--res; buf[res] == '0'; --res)
			buf[res] = '\0';
	}

	out << buf;
	return out;
}

template <char ch = 0, char... chs>
struct Delim
{
	static void Put(TFormatOutput &out)
	{
		static const char _chs[] = {ch, chs...};
		out.write(_chs, sizeof(_chs));
	}
};

template <>
struct Delim<0>
{
	static void Put(TFormatOutput &out)
	{
	}
};

typedef Delim<',', ' '> CommaSpace;

template <typename _Delim = CommaSpace, typename TStream> inline
TStream &&FormatVals(TStream &&out)
{
	return std::forward<TStream>(out);
}

template <typename _Delim = CommaSpace, typename TStream, typename T> inline
TStream &&FormatVals(TStream &&out, T &&val)
{
	FormatVal(out, std::forward<T>(val));
	return std::forward<TStream>(out);
}

template <typename TDelim = CommaSpace, typename TStream, typename T, typename... TT>
TStream &&FormatVals(TStream &&out, T &&val, TT&&... vals)
{
	FormatVal(out, val);
	TDelim::Put(out);
	return FormatVals<TDelim>(std::forward<TStream>(out), std::forward<TT>(vals)...);
}

template <char delim, typename TStream, typename T, typename... TT>
TStream &&FormatVals(TStream &&out, T &&val, TT&&... vals)
{
	return FormatVals<Delim<delim>>(std::forward<TStream>(out), std::forward<T>(val), std::forward<TT>(vals)...);
}

template <typename TDelim = CommaSpace, typename... TT>
std::string FormatStr(TT&&... vals)
{
	return FormatVals<TDelim>(std::stringstream(), std::forward<TT>(vals)...).str();
}

template <char delim, typename... TT>
std::string FormatStr(TT&&... vals)
{
	return FormatVals<delim>(std::stringstream(), std::forward<TT>(vals)...).str();
}

template <typename T>
std::string FormatStr(T &&val)
{
	std::stringstream stm;
	FormatVal(stm, std::forward<T>(val));
	return stm.str();
}

}

template <typename T> inline
std::ostream &operator <<(std::ostream &out, const std::atomic<T> &val)
{
    return out << T(val);
}

inline
std::ostream &operator <<(std::ostream &out, const std::type_info &val)
{
    return TS::FormatTypeName(out, val.name());
}

inline
std::ostream &operator <<(std::ostream &out, const TS::FileLine &val)
{
	val.FormatVal(out);
    return out;
}
//
//inline
//std::ostream &operator <<(std::ostream &out, const std::chrono::system_clock::time_point &val)
//{
//	TS::FormatDateTime(out, val, "%Y-%m-%d %H:%M:%S.%l", false);
//    return out;
//}
