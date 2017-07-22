#include "Common.h"
#include "RiskManager.h"

using namespace RM;


CRiskManager::CRiskManager(const TS::CConfigFile &cfg)
{
	cfg.ForEachNode("rule", [this](auto &&cfg)
	{
		auto name = cfg.template ReadValue<std::string>("id");
		AddRule(name, cfg);
	});
}

CRiskManager::~CRiskManager()
{

}


#define TS_ITEM(name) struct _##name;
namespace RM {RM_CHECK_ORDER_RULES}
#undef TS_ITEM

std::unique_ptr<COrderCheckRule> CRiskManager::CreateRule(const std::string &rule, const TS::CConfigFile &cfg)
{
#define TS_ITEM(name) if (rule == #name) return RM::CreateRule<RM::_##name>(*this, cfg);
	RM_CHECK_ORDER_RULES
#undef TS_ITEM

	Log.Error("Unknown CheckOrderRule", rule);
	return nullptr;
}

