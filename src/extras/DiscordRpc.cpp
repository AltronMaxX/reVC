#include "DiscordRpc.h"

#ifdef USE_DISCORD_RPC
#define APP_ID "1456434476057104528"

void DiscordRPC::Initialize()
{
	DiscordEventHandlers handlers;
	memset(&handlers, 0, sizeof(handlers));
	Discord_Initialize(APP_ID, &handlers, 1, nullptr);
}

void DiscordRPC::Shutdown()
{
	Discord_Shutdown();
}

void DiscordRPC::Update()
{
	DiscordRichPresence discordPresence;
	memset(&discordPresence, 0, sizeof(discordPresence));
	discordPresence.state = "I am the RPC Title.";
	discordPresence.details = "I am just a simple bio.";
	discordPresence.startTimestamp = 0;
	discordPresence.endTimestamp = NULL;
	discordPresence.largeImageKey = "LargeImage";
	discordPresence.largeImageText = "LargeImageText";
	discordPresence.smallImageKey = "SmallImage";
	discordPresence.smallImageText = "SmallImageText";
	discordPresence.instance = 1;

	Discord_UpdatePresence(&discordPresence);
}
#endif