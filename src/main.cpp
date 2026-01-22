#include <SKSE/SKSE.h>
#include "SF/Plugin.h"

extern "C"
{
	__declspec(dllexport) bool SKSEPlugin_Query(const SKSE::QueryInterface* skse, SKSE::PluginInfo* info)
	{
		info->infoVersion = SKSE::PluginInfo::kVersion;
		info->name = "Sunderandforged";
		info->version = 1;

		if (skse->IsEditor()) {
			return false;
		}

		return true;
	}

	__declspec(dllexport) bool SKSEPlugin_Load(const SKSE::LoadInterface* skse)
	{
		SF::Plugin::Init(skse);
		return true;
	}
}
