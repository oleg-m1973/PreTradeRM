#include "Common.h"
#include "Storage.h"

#include "Common/Parser.h"
#include "Common/FramedQueue.h"

#include "Transport.h"

#include <fstream>
#include <set>


namespace RM
{

#define RM_SAVE_OBJECT(name) template <> void CStorage::_SaveObject<S##name>(const S##name &obj, std::ostream &out)
#define TS_ITEM(name, type) TS::FormatVals<0>(out, "|" #name, '=', obj.m_##name);

RM_SAVE_OBJECT(Quote){RM_QUOTE;}
RM_SAVE_OBJECT(Trade){RM_TRADE;}
RM_SAVE_OBJECT(Order){RM_ORDER;}

#undef TS_ITEM
#undef RM_SAVE_OBJECT

}

using namespace RM;

void CStorage::Load()
{
	if (!fs::exists(m_cfg.dir) || !fs::is_directory(m_cfg.dir))
		return;

	std::set<fs::path> files;
	for(auto &item: fs::directory_iterator(m_cfg.dir))
	{
		if (fs::is_regular_file(item.status()) && item.path().extension() == ".rm_save")
			files.emplace(item.path());
	}

#define TS_ITEM(name) {#name##s, &CRiskManager::ProcessMessage<S##name>},
	THandlers handlers = {RM_OBJECTS};
#undef TS_ITEM

	Log.Info("Load files...", files.size());
	for (auto &item: files)
		TS_NOEXCEPT(LoadFile(item, handlers));
}

void CStorage::LoadFile(const fs::path &file_name, const THandlers &handlers)
{

	auto name = file_name.native();
	static CTransport _trans;
	const auto tm = std::chrono::system_clock::now();
	std::ifstream in(name, std::ifstream::binary);

	TS::CKeyValueParser<CMessage::TAttrs, '|', '\n'> parser;

	const size_t _sz = 1024;
	char buf[_sz + 1] = {0};
	size_t n = 0;

	auto tm2 = std::chrono::system_clock::now();
	while (in.good())
	{
		in.read(buf, _sz);
		parser.DoParse(buf, in.gcount(), [this, &n, &file_name, &tm2, &handlers](auto &&attrs)
		{
			if (attrs.size() < 2)
				return;

			RM::CMessage msg(std::move(attrs));

			auto it = handlers.find(msg.GetID().first);
			if (it != handlers.end())
				std::invoke(it->second, &m_rm, _trans, msg);

			++n;

//			if (n % 10000 == 0)
//			{
//				const auto now = std::chrono::system_clock::now();
//				Log.Debug(file_name, n, now - tm2);
//				tm2 = now;
//			}
		});
	}

	Log.Info("RiskManager::LoadFile", name, n, std::chrono::system_clock::now() - tm);
	m_remove.PutValue(fs::last_write_time(file_name), file_name);
}

inline
std::string GetFileName(const std::string &dir, auto now)
{
	std::stringstream stm;

	TS::FormatVals<0>(stm, dir, '/', program_invocation_short_name, '.');
	TS::FormatVal(stm, std::chrono::system_clock::time_point(now), "%y%m%d-%H");
	TS::FormatVals<0>(stm, ".rm_save");

	return stm.str();
}

void CStorage::ThreadProc(Sys::CThreadControl &thread)
{
	mkdir(m_cfg.dir.c_str(), 0777);

	typedef std::chrono::hours TPeriod;
	TPeriod tm(0);

	std::ofstream fout;
	std::string file_name;
	while (thread.Wait(1min, m_evSave) != nullptr)
	{
		const auto now = std::chrono::duration_cast<TPeriod>(std::chrono::system_clock::now().time_since_epoch());
		if (tm != now)
		{
			if (!file_name.empty())
			{
				fout.close();
				Log.Debug("Close save file", file_name, std::chrono::system_clock::time_point(tm));
				m_remove.PutValue(TDateTime(now), file_name);
			}
			tm = now;

			file_name = GetFileName(m_cfg.dir, tm);
			fout.open(file_name, std::ofstream::app);

			Log.Info("Open file for save", file_name);
		}

        auto items = Sys::Locked::Move(m_items);
        for (auto &item: items)
			TS_NOEXCEPT(item(fout));

		m_remove.EraseExpired(std::chrono::system_clock::now());
	}
}

