#pragma once
#include "Errors.h"
#include <vector>

namespace TS
{
template <typename T>
T Parse(const char *psz)
{
	T val;
	Parse(psz, val);
	return val;
}

template <typename T>
void Parse(const char *src, const char *end, T &dst)
{
	dst = T(src, end);
}

//template <typename T>
//void Parse(const std::string &s, T &dst)
//{
//	Parse(s.c_str(), s.c_str() + s.size(), dst);
//}

inline
void Parse(const char *src, const char *end, std::string &dst)
{
	dst.assign(src, end);
}

inline
void Parse(const char *psz, std::string &dst)
{
	dst = psz;
}

template <typename T, typename TFunc, typename... TT>
const char *ParseNumber(const char *src, T &dst, TFunc &&func, TT&&... args)
{
	char *end = nullptr;
	auto res = std::invoke(std::forward<TFunc>(func), src, &end, std::forward<TT>(args)...);
	if (end != src && end)
	{
		dst = std::move(res);
		return end;
	}

	return nullptr;
}

#define TS_NUMERIC_PARSER(c_type, func) inline \
	const char *Parse(const char *src, c_type &dst, int base = 10) {return ParseNumber(src, dst, func, base);}

TS_NUMERIC_PARSER(int8_t, strtol)
TS_NUMERIC_PARSER(int16_t, strtol)
TS_NUMERIC_PARSER(int32_t, strtol)
TS_NUMERIC_PARSER(int64_t, strtoll)

TS_NUMERIC_PARSER(uint8_t, strtol)
TS_NUMERIC_PARSER(uint16_t, strtoul)
TS_NUMERIC_PARSER(uint32_t, strtoul)
TS_NUMERIC_PARSER(uint64_t, strtoull)

#undef TS_NUMERIC_PARSER

#define TS_NUMERIC_PARSER(c_type, func) inline \
	const char *Parse(const char *src, c_type &dst) {return ParseNumber(src, dst, func);}

TS_NUMERIC_PARSER(float, strtof)
TS_NUMERIC_PARSER(double, strtod)
TS_NUMERIC_PARSER(long double, strtold)

#undef TS_NUMERIC_PARSER

inline
void Parse(const char *psz, char &dst)
{
	dst = *psz;
}

template <typename T> requires std::is_enum<T>::value
void Parse(const char *psz, T &dst)
{
	auto val = static_cast<std::underlying_type_t<T>>(dst);
	Parse(psz, val);
	dst = T(val);
}

inline
void ParseDateTime(const char *psz, const char *fmt, std::chrono::system_clock::time_point &dst)
{
	struct tm tm = {0};
	int ms = 0;

	sscanf(psz, "%04d-%02d-%02d%*[ T]%02d:%02d:%02d.%03d",
		&tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &ms);

	if (tm.tm_year)
		tm.tm_year -= 1900;

	if (tm.tm_mon)
		--tm.tm_mon;

	dst = std::chrono::system_clock::from_time_t(mktime(&tm)) + std::chrono::milliseconds(ms);
}

inline
void Parse(const char *psz, std::chrono::system_clock::time_point &dst, const char *fmt = nullptr)
{
	ParseDateTime(psz, fmt? fmt: "%Y-%m-%d%*T%*H%*:%*M%*:%*S%*.%*l", dst);
}


template <char ch1>
constexpr bool IsOneOf(char ch)
{
	return ch == ch1;
}

template <char ch1, char ch2, char... chs>
constexpr bool IsOneOf(char ch)
{
	return ch == ch1 || IsOneOf<ch2, chs...>(ch);
}

template <typename TResult, char _delim = '\1', char _fin = '\0'>
class CKeyValueParser
{
public:
	static const size_t MaxDataLen = 1024 * 1024 * 1024;
	typedef std::remove_cv_t<typename TResult::value_type::first_type> TKey;
	typedef typename TResult::value_type::second_type TValue;

	CKeyValueParser()
	{
		m_data.reserve(256);
	}

	template <typename TFunc>
	void DoParse(const char *data, size_t sz, TFunc &&func)
	{
    	const char *end = data + sz;
    	for (const char *p = data; p < end; )
		{
			p = (this->*m_fn)(p, end);
			if (p == end)
				break;

			if (*p == _fin)
			{
				func(std::move(m_res));
				m_res.clear();
				m_ln = 0;
				m_fn = &CKeyValueParser::_ParseKey;
			}

			data = ++p;
		}


		if (data < end)
		{
			m_ln += std::distance(data, end);
			if (m_ln > MaxDataLen)
				TS_RAISE_ERROR("Message too long", m_ln);

			m_data.insert(m_data.end(), data, end);
		}
	}

	void reserve(size_t n)
	{
		m_res.reserve(n);
	}

	void Reset()
	{
		m_res.clear();
		m_data.clear();
		m_ln = 0;
		m_fn = &CKeyValueParser::_ParseKey;
	}
protected:
	virtual void ParseKey(const char *psz, const char *end, TKey &key) const
	{
		TS::Parse(psz, end, key);
	}

	virtual void ParseValue(const char *psz, const char *end, TValue &val) const
	{

		TS::Parse(psz, end, val);
	}

	static std::pair<const char *, const char *> GetItemString(auto &data, const char *p, const char *end)
	{
		if (data.empty())
			return {p, end};


		data.insert(data.end(), p, end);
		return {data.data(), data.data() + data.size()};
	}

	void StoreAttr(TValue &&val)
	{
		if (!m_key_init)
			return;

		m_res.insert(m_res.end(), std::make_pair(std::move(m_key), std::move(val)));
		m_key_init = false;
	}

	const char * _ParseKey(const char *data, const char *end)
	{
		const char *p = data;
		for (; p != end; ++p)
		{
			const char ch = *p;
			if (!IsOneOf<'=', _delim, _fin>(ch))
				continue;

			auto buf = std::move(m_data);
			const auto s = GetItemString(buf, data, p);
			m_key_init = ch == '=' || *p != 0;

			ParseKey(s.first, s.second, m_key);

			if (ch == '=')
			{
				m_fn = &CKeyValueParser::_ParseValue;
				break;
			}

			StoreAttr(TValue());
			break;
		}
		return p;
	}

	const char * _ParseValue(const char *data, const char *end)
	{
		const char *p = data;
		for (; p != end; ++p)
		{
			const char ch = *p;
			if (!IsOneOf<_delim, _fin>(ch))
				continue;

			TValue val;
			auto buf = std::move(m_data);
			const auto s = GetItemString(buf, data, p);
			ParseValue(s.first, s.second, val);

			StoreAttr(std::move(val));
			m_fn = &CKeyValueParser::_ParseKey;

			break;
		}
		return p;
	}

	TResult m_res;

	bool m_key_init = false;
	TKey m_key;

	size_t m_ln = 0;
	std::vector<char> m_data;

	decltype(&CKeyValueParser::_ParseKey) m_fn = &CKeyValueParser::_ParseKey;
};

}
