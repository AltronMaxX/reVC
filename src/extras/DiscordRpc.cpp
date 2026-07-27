#include "DiscordRpc.h"
#include "Ped.h"
#include "PlayerPed.h"
#include "Replay.h"
#include "Script.h"
#include <PlayerInfo.h>
#include <World.h>
#include <Zones.h>
#include <common.h>

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
OnDisconnected(const int code, const char *msg)
{
	debug("Discord RPC DISCONNECTED: %d %s\n", code, msg ? msg : "");
}
static void
OnErrored(const int code, const char *msg)
{
	debug("Discord RPC ERROR: %d %s\n", code, msg ? msg : "");
}

wchar *DiscordRPC::CurMissionName = nullptr;
DiscordRichPresence discordPresence;

void DiscordRPC::Initialize()
{
	debug("Initialising DiscordRPC... \n");
	DiscordEventHandlers handlers = {};
	handlers.ready = OnReady;
	handlers.disconnected = OnDisconnected;
	handlers.errored = OnErrored;
	Discord_Initialize(APP_ID, &handlers, 1, nullptr);
	memset(&discordPresence, 0, sizeof(discordPresence));
	debug("Initialized DiscordRPC... \n");
}

void DiscordRPC::Shutdown()
{
	debug("Shutdown DiscordRPC... \n");
	Discord_Shutdown();
}

static const char *WideToUtf8(const wchar *s)
{
	static std::string utf8;
	utf8.clear();
	if(!s) return "";

	for(const wchar *p = s; *p; p++) {

		if(const uint32_t cp = *p; cp < 0x80) {
			utf8 += static_cast<char>(cp);
		} else if(cp < 0x800) {
			utf8 += static_cast<char>(0xC0 | (cp >> 6));
			utf8 += static_cast<char>(0x80 | (cp & 0x3F));
		} else {
			utf8 += static_cast<char>(0xE0 | (cp >> 12));
			utf8 += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			utf8 += static_cast<char>(0x80 | (cp & 0x3F));
		}
	}
	return utf8.c_str();
}

void DiscordRPC::Update()
{
	Discord_RunCallbacks();

	if (CReplay::IsPlayingBack())
		return;

	static std::string missionName;
	static bool wasOnMission = false;
	bool isOnMission = CTheScripts::IsPlayerOnAMission();

	if(!wasOnMission && isOnMission)
		CurMissionName = nullptr;

	if(isOnMission) {
		if(missionName.empty() && CurMissionName != nullptr)
			missionName = WideToUtf8(CurMissionName);
		discordPresence.details = "On mission";
		discordPresence.state = missionName.c_str();
		discordPresence.instance = 1;
	} else {
		missionName.clear();
		auto player = CWorld::Players[CWorld::PlayerInFocus];
		const auto z1 = CTheZones::FindSmallestNavigationZoneForPosition(&player.GetPos(), true, false);
		const auto z2 = CTheZones::FindSmallestNavigationZoneForPosition(&player.GetPos(), false, true);
		if(!z1 && !z2) return;
		CZone *use = z2 ? z2 : z1;
		if(!use) return;
		if(CPlayerPed *playerPed = player.m_pPed; playerPed->Driving() && playerPed->m_pMyVehicle) {
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
		discordPresence.state = WideToUtf8(use->GetTranslatedName());
		discordPresence.instance = 1;
	}
	wasOnMission = isOnMission;

	Discord_UpdatePresence(&discordPresence);
}
#endif
