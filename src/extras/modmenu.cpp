#include "common.h"

#ifdef DEBUGMENU

#include "debugmenu.h"
#include "World.h"
#include "PlayerInfo.h"
#include "Ped.h"
#include "PlayerPed.h"
#include "Streaming.h"
#include "Automobile.h"
#include "ModelIndices.h"

namespace ReVCModMenu {

static float health = 100.0f;
static float armour = 100.0f;
static int32 weapon = WEAPONTYPE_COLT45;
static int32 skin = 0;

static const char *weaponNames[] = {
	"Unarmed", "Brass Knuckles", "Screwdriver", "Golf Club", "Nightstick",
	"Knife", "Baseball Bat", "Hammer", "Cleaver", "Machete", "Katana",
	"Chainsaw", "Grenade", "Detonator Grenade", "Tear Gas", "Molotov",
	"Rocket", "Colt 45", "Python", "Shotgun", "Spas-12", "Stubby Shotgun",
	"Tec-9", "Uzi", "Silenced Ingram", "MP5", "M4", "Ruger", "Sniper Rifle",
	"Laser Scope", "Rocket Launcher", "Flamethrower", "M60", "Minigun",
	"Detonator", "Helicannon", "Camera"
};

static const char *skinNames[] = {
	"player", "cop", "swat", "fbi", "army", "medic", "fireman", "golf"
};

static void ApplyHealth()
{
	if (CPed *ped = FindPlayerPed())
		ped->m_fHealth = health;
}

static void ApplyArmour()
{
	if (CPed *ped = FindPlayerPed())
		ped->m_fArmour = armour;
}

static void FullHealth()
{
	if (CPed *ped = FindPlayerPed()) {
		health = 100.0f;
		ped->m_fHealth = 100.0f;
	}
}

static void FullArmour()
{
	if (CPed *ped = FindPlayerPed()) {
		armour = 100.0f;
		ped->m_fArmour = 100.0f;
	}
}

static void GiveSelectedWeapon()
{
	if (CPed *ped = FindPlayerPed()) {
		ped->GiveWeapon((eWeaponType)weapon, 9999);
		ped->SetCurrentWeapon((eWeaponType)weapon);
	}
}

static void GiveAllWeapons()
{
	if (CPed *ped = FindPlayerPed()) {
		for (int32 i = WEAPONTYPE_BRASSKNUCKLE; i <= WEAPONTYPE_CAMERA; i++)
			ped->GiveWeapon((eWeaponType)i, 9999);
	}
}

static void ApplySkin()
{
	if (skin >= 0 && skin < (int32)(sizeof(skinNames) / sizeof(skinNames[0])))
		CWorld::Players[0].SetPlayerSkin(skinNames[skin]);
}

static void SpawnVehicle(int32 modelId)
{
	CVector pos = FindPlayerCoors();

	CStreaming::RequestModel(modelId, STREAMFLAGS_DONT_REMOVE);
	CStreaming::LoadAllRequestedModels(true);

	if (!CStreaming::HasModelLoaded(modelId))
		return;

	CAutomobile *vehicle = new CAutomobile(modelId, RANDOM_VEHICLE);
	if (!vehicle)
		return;

	pos.z += 0.5f;
	vehicle->SetPosition(pos);
	vehicle->SetHeading(FindPlayerHeading());
	vehicle->SetStatus(STATUS_PHYSICS);
	vehicle->SetIsStatic(false);
	vehicle->bUsesCollision = true;
	CWorld::Add(vehicle);
}

static void SpawnInfernus()
{
	SpawnVehicle(MI_INFERNUS);
}

static void SpawnRhino()
{
	SpawnVehicle(MI_RHINO);
}

static void SpawnPolice()
{
	SpawnVehicle(MI_POLICE);
}

static void AddMoney()
{
	CWorld::Players[0].m_nMoney += 10000;
}

static void MaxMoney()
{
	CWorld::Players[0].m_nMoney = 99999999;
}

void Init()
{
	DebugMenuAddInt32("Mod Menu|Player", "Money", &CWorld::Players[0].m_nMoney, nil,
		1000, 0, 99999999, nil);
	DebugMenuAddCmd("Mod Menu|Player", "Give $10,000", AddMoney);
	DebugMenuAddCmd("Mod Menu|Player", "Max Money", MaxMoney);

	DebugMenuAddFloat32("Mod Menu|Player", "Health", &health, ApplyHealth,
		10.0f, 0.0f, 100.0f);
	DebugMenuAddCmd("Mod Menu|Player", "Full Health", FullHealth);
	DebugMenuAddFloat32("Mod Menu|Player", "Armour", &armour, ApplyArmour,
		10.0f, 0.0f, 100.0f);
	DebugMenuAddCmd("Mod Menu|Player", "Full Armour", FullArmour);

	DebugMenuEntry *weaponEntry = DebugMenuAddInt32("Mod Menu|Weapons", "Weapon",
		&weapon, GiveSelectedWeapon, 1, WEAPONTYPE_UNARMED, WEAPONTYPE_CAMERA, weaponNames);
	DebugMenuEntrySetWrap(weaponEntry, true);
	DebugMenuAddCmd("Mod Menu|Weapons", "Give Selected Weapon", GiveSelectedWeapon);
	DebugMenuAddCmd("Mod Menu|Weapons", "Give All Weapons", GiveAllWeapons);

	DebugMenuEntry *skinEntry = DebugMenuAddInt32("Mod Menu|Player", "Skin",
		&skin, ApplySkin, 1, 0,
		(int32)(sizeof(skinNames) / sizeof(skinNames[0])) - 1, skinNames);
	DebugMenuEntrySetWrap(skinEntry, true);
	DebugMenuAddCmd("Mod Menu|Player", "Apply Skin", ApplySkin);

	DebugMenuAddCmd("Mod Menu|Spawn", "Spawn Infernus", SpawnInfernus);
	DebugMenuAddCmd("Mod Menu|Spawn", "Spawn Rhino", SpawnRhino);
	DebugMenuAddCmd("Mod Menu|Spawn", "Spawn Police", SpawnPolice);
}

}

void ModMenuInit()
{
	ReVCModMenu::Init();
}

#endif
