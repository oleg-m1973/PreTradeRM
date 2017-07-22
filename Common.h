#pragma once
#include "Common/Common.h"
#include "Common/Format.h"
#include "Common/SyncObjs.h"

#include <iostream>


template <typename... TT>
void WriteLog(char ltr, TT&&... vals) noexcept
{
	static std::mutex _mx;
	static size_t _n = 0;
	try
	{
		SYS_LOCK(_mx);
		++_n;
		TS::FormatVal(std::cout, std::chrono::system_clock::now(), "%y%m%d %H:%M:%S.%l");
		TS::FormatVals<0>(std::cout, " >", ltr, " ");
		TS::FormatVals<TS::CommaSpace>(std::cout, std::forward<TT>(vals)...);
		std::cout << std::endl;
	}
	catch(...)
	{
	}
}

#define TS_LOG_TYPES \
	TS_ITEM(Info, ' ') \
	TS_ITEM(Error, 'E') \
	TS_ITEM(Warning, 'W') \
	TS_ITEM(Debug, 'D') \

struct
{
#define TS_ITEM(name, ltr) 	template <typename... TT> void name(TT&&... vals) const noexcept {WriteLog(ltr, std::forward<TT>(vals)...);}
	TS_LOG_TYPES
#undef TS_ITEM
} Log;
