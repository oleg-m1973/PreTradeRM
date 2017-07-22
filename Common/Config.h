#pragma once

namespace TS
{
class CConfigFile
{
public:
	template <typename T>
	void ReadValue(const char *name, T &val, bool use_parent = false) const
	{
	}

	template <typename T>
	void ReadValue(const char *name, bool use_parent = false) const
	{
		return T();
	}

	template <typename TFunc, typename... TT>
	void ForEachNode(const char *name, TFunc &&func, TT&&... args) const
	{
		//func(args..., node);
	}
protected:
};

template <typename T>
struct CConfigHolder
{
	typedef CConfigHolder TConfig;

	template <typename... TT>
	CConfigHolder(TT&&... args)
	: m_cfg(std::forward<TT>(args)...)
	{
	}

	T m_cfg;
};

}

