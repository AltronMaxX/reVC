#include "DiscordRpc.h"
#include <common.h>
#include <Script.h>
#include <PlayerInfo.h>
#include <World.h>
#include <Zones.h>
#include "PlayerPed.h"
#include "Ped.h"

#include <codecvt>
#include <locale>

#ifdef USE_DISCORD_RPC
static constexpr auto APP_ID =
    "1456434476057104528";

static void
OnReady(const DiscordUser *user)
{
	debug("Discord RPC READY: %s#%s\n", user->username, user->discriminator);
}
static void
OnDisconnected(int code, const char *msg)
{
	debug("Discord RPC DISCONNECTED: %d %s\n", code, msg ? msg : "");
}
static void
OnErrored(int code, const char *msg)
{
	debug("Discord RPC ERROR: %d %s\n", code, msg ? msg : "");
}

wchar *DiscordRPC::CurMissionName = nullptr;
DiscordRichPresence discordPresence;

void DiscordRPC::Initialize()
{
	debug("Intialising DiscordRPC... \n");
	DiscordEventHandlers handlers = {};
	handlers.ready = OnReady;
	handlers.disconnected = OnDisconnected;
	handlers.errored = OnErrored;
	Discord_Initialize(APP_ID, &handlers, 1, nullptr);
	memset(&discordPresence, 0, sizeof(discordPresence));
	debug("Intialised DiscordRPC... \n");
}

void DiscordRPC::Shutdown()
{
	debug("Shutdowning DiscordRPC... \n");
	Discord_Shutdown();
}

static const char *WideToUtf8(wchar_t *s)
{
	static std::string utf8;

	if(!s)
		return "";

	try {
		std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
		utf8 = conv.to_bytes(s);
	} catch(...) {
		return "";
	}

	return utf8.c_str();
}

void DiscordRPC::Update()
{
	Discord_RunCallbacks();

	//TODO
	/* if(CTheScripts::IsPlayerOnAMission()) {
		if(CurMissionName == nullptr) return;
		discordPresence.state = WideToUtf8(CurMissionName);
		discordPresence.details = "On mission ";
		discordPresence.instance = 1;
	} else {*/
		auto player = CWorld::Players[CWorld::PlayerInFocus];
		auto z1 = CTheZones::FindSmallestNavigationZoneForPosition(&player.GetPos(), true, false);
		auto z2 = CTheZones::FindSmallestNavigationZoneForPosition(&player.GetPos(), false, true);
		if(!z1 && !z2) return;
		CZone *use = z2 ? z2 : z1;
		if(!use) return;
		CPlayerPed* playerPed = player.m_pPed;
		if(playerPed->Driving() && playerPed->m_pMyVehicle) {
			if(playerPed->m_pMyVehicle->IsBoat()) {
				discordPresence.details = "Sailing in ";
			} else if(playerPed->m_pMyVehicle->IsPlane() || playerPed->m_pMyVehicle->IsHeli()
				|| playerPed->m_pMyVehicle->IsRealHeli() || playerPed->m_pMyVehicle->IsRealPlane()) {
				discordPresence.details = "Flying in ";
			} else {
				discordPresence.details = "Driving in ";
			}
		} else {
			discordPresence.details = "Walking in ";
		}
		discordPresence.state = WideToUtf8(reinterpret_cast<wchar_t *>(use->GetTranslatedName()));
		discordPresence.instance = 1;
	//}

	Discord_UpdatePresence(&discordPresence);
}
#endif
