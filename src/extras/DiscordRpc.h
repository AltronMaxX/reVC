#pragma once

#ifdef USE_DISCORD_RPC
#include "discord_rpc.h"
#include "discord_register.h"
#include "windows.h"
#include "common.h"

class DiscordRPC
{
public:
	static void Initialize();
	static void Shutdown();
	static void Update();
	static wchar *CurMissionName;
};
#endif
