#pragma once

#ifdef USE_DISCORD_RPC
#include "discord_rpc.h"
#include "discord_register.h"
#include "windows.h"

class DiscordRPC
{
public:
	static void Initialize();
	static void Shutdown();
	static void Update();
};
#endif