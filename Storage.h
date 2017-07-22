#pragma once
#include "RiskManager.h"
#include "Common/Thread.h"
#include "Common/FramedQueue.h"

#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;

#include <sys/stat.h>
#include <sys/types.h>

#define TS_CFG TS_CFG_(Storage)
#define TS_CONFIG_ITEMS \
	TS_ITEM(dir, fs::path, TS::FormatStr<0>("./", program_invocation_short_name, ".data")) \
	TS_ITEM(period, std::chrono::minutes, 24h) \

#include "Common/Config.inl"

namespace RM
{
class CStorage
: protected CObjectHandler<>
{
public:
	Storage::CConfig m_cfg;

	CStorage(CRiskManager &rm, const TS::CConfigFile &cfg)
	: CObjectHandler<>(rm)
	, m_cfg(cfg)
	{
		Load();
		m_thread.Start(&CStorage::ThreadProc, this);

		RegisterCallback<SQuote>(&CStorage::Save<SQuote>, this);
		RegisterCallback<STrade>(&CStorage::Save<STrade>, this);
	}

	~CStorage()
	{
		CObjectHandler<>::Reset();
		m_thread.Stop();
	}

protected:
	typedef std::map<std::string, decltype(&CRiskManager::ProcessMessage<SQuote>)> THandlers;

	struct CRemoveFile
	{
		auto &operator +=(const std::string &)
		{
			return *this;
		}

		auto &operator -=(const std::string &name)
		{
			auto res = ::remove(name.c_str());
			Log.Info("Delete expired save file", name, res, errno);

			return *this;
		}

	};

	void Load();
	void LoadFile(const fs::path &file_name, const THandlers &handlers);

	template <typename T> void _SaveObject(const T &obj, std::ostream &out);
	template <typename T> void SaveObject(const T &obj, std::ostream &out)
	{
		out << GetObjectName<T>();
		_SaveObject(obj, out);
		out << std::endl;
	}

	template <typename T>
	void Save(const T &obj)
	{
		SYS_LOCK(m_items);
		m_items.emplace_back(&CStorage::SaveObject<T>, this, obj);
		m_evSave.Set();
	}


	void ThreadProc(Sys::CThreadControl &);

	Sys::CLockedObject<std::list<TS::CFunction<void(std::ostream &)>>> m_items;

	Sys::CThread m_thread;
	Sys::CEvent<false> m_evSave{m_thread};

	TS::CMovingSum<std::string, CRemoveFile> m_remove{m_cfg.period};
};


}



