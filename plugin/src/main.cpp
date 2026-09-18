#include "PCH.h"

#include "AlwaysScan.h"

#include <atomic>

// 注意：SFSE_PLUGIN_VERSION 由 commonlibsf.plugin 规则生成的 commonlibsf-plugin.cpp
// 提供（配置见 plugin/xmake.lua 的 add_rules 参数），这里不要再定义一份，
// 否则会报 LNK2005 重复定义。

namespace
{
	std::atomic_bool g_installed{ false };

	// 插件加载时可能过早（SFSE 的任务系统还没起来），所以在
	// PostPostLoad / PostDataLoad 消息里再兜底试一次。
	void TryInstall()
	{
		if (g_installed.load()) {
			return;
		}
		if (SAS::Install()) {
			g_installed.store(true);
		}
	}

	void OnMessage(SFSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		switch (a_msg->type) {
		case SFSE::MessagingInterface::kPostPostLoad:
		case SFSE::MessagingInterface::kPostDataLoad:
			TryInstall();
			break;
		default:
			break;
		}
	}
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	SFSE::Init(a_sfse, { .logName = "SAS_AlwaysScan" });

	REX::INFO("SAS_AlwaysScan v4.3.0 loading (SFSE build {})", SFSE::GetSFSEVersion());

	if (auto* messaging = SFSE::GetMessagingInterface()) {
		messaging->RegisterListener(OnMessage);
	} else {
		REX::WARN("messaging interface unavailable");
	}

	TryInstall();
	return true;
}
