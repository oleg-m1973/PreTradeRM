#include "Common.h"
#include "Common/Signals.h"
#include "Common/Config.h"

#include "TransportTCP.h"
#include "RiskManager.h"
#include "Storage.h"

#include <fstream>
#include <experimental/filesystem>


namespace fs = std::experimental::filesystem;
int main(int argc, char *argv[])
{
	u_short port = 0;
	if (argc > 1)
		TS::Parse(argv[1], port);

	if (!port)
		port = 11111;

	CatchSegmentationsFault();
	Log.Info(program_invocation_short_name, "starting...");
	const auto tm = std::chrono::system_clock::now();


	const auto save_dir = TS::FormatStr<0>("./", program_invocation_short_name, ".data");
	try
	{
		TS::CConfigFile cfg;

		RM::CRiskManager rm(cfg);


		rm.AddRule("NewOrderMoratorium", cfg);
		rm.AddRule("PriceCheck", cfg);
		rm.AddRule("SeqBadTrades", cfg);
		rm.AddRule("DrawDown", cfg);

		RM::CStorage storage(rm, cfg);
		RM::CSocketServer trans(rm);

		trans.Start(port);

		Log.Info(program_invocation_short_name, "started", std::chrono::system_clock::now() - tm);

		WaitStop();
		trans.Stop();
	}
	TS_CATCH;

	Log.Info(program_invocation_short_name, "stopped");
    return 0;
}
