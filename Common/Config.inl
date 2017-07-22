#ifndef TS_CFG
#define TS_CFG
#endif

#define TS_CFG_(name) name
namespace TS_CFG
#undef  TS_CFG_
{
struct CConfig
{
	typedef CConfig TConfig;

	CConfig()
	{
	}

	CConfig(const TS::CConfigFile &cfg, bool use_parent = false)
	{
		Load(cfg, use_parent);
	}

	template <typename TConfig>
	explicit CConfig(TConfig &&src)
	{
		Reset(std::forward<TConfig>(src));
	}

	TS_COPYABLE(CConfig, default);
	TS_MOVABLE(CConfig, default);

	CConfig &Load(const TS::CConfigFile &cfg, bool use_parent = false)
	{
#define TS_ITEM(name, type, def, ...) cfg.ReadValue(#name, name, use_parent);
		TS_CONFIG_ITEMS
#undef TS_ITEM
		return *this;
	}

	template <typename TConfig>
	CConfig &Load(TConfig &&cfg)
	{
		return Reset(std::forward<TConfig>(cfg));
	}

	template<typename TConfig>
	CConfig &Reset(TConfig &&src)
	{
#define TS_ITEM(name, type, def, ...) name = src.name;
		TS_CONFIG_ITEMS
#undef TS_ITEM
		return *this;
	}

#define TS_ITEM(name, type, def, ...) type name = type(def);
	TS_CONFIG_ITEMS
#undef TS_ITEM
};

}

#undef TS_CFG
#undef TS_CONFIG_ITEMS
