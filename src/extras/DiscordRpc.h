#pragma once
#include "common.h"

#ifdef USE_DISCORD_RPC
#include "discord_rpc.h"
#include "discord_register.h"
#endif

class DiscordRPC
{
public:
#ifdef USE_DISCORD_RPC
	static void Initialize();
	static void Update();
	static void Shutdown();
#else
	static inline void Initialize() {}
	static inline void Update() {}
	static inline void Shutdown() {}
#endif
	static wchar *CurMissionName;
};

