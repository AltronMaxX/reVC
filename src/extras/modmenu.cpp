#include "common.h"

#ifdef DEBUGMENU

#include "debugmenu.h"
#include "World.h"
#include "PlayerInfo.h"
#include "Ped.h"
#include "Streaming.h"
#include "Automobile.h"
#include "ModelIndices.h"

namespace ReVCModMenu {

static float health = 100.0f;
static float armour = 100.0f;
static int32 weapon = WEAPONTYPE_COLT45;
static int32 skin = 0;
static int32 &money = CWorld::Players[0].m_nMoney;

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
	money += 10000;
}

static void MaxMoney()
{
	money = 99999999;
}

SETTWEAKPATH("Mod Menu|Player");
TWEAKINT32N(money, 0, 99999999, 1000, "Money");
TWEAKFUNCN(AddMoney, "Give $10,000");
TWEAKFUNCN(MaxMoney, "Max Money");
TWEAKFUNCN(FullHealth, "Full Health");
TWEAKFUNCN(FullArmour, "Full Armour");
TWEAKFLOATN(health, 0.0f, 100.0f, 10.0f, "Health Value");
TWEAKFUNCN(ApplyHealth, "Apply Health");
TWEAKFLOATN(armour, 0.0f, 100.0f, 10.0f, "Armour Value");
TWEAKFUNCN(ApplyArmour, "Apply Armour");
TWEAKINT32N(skin, 0, 7, 1, "Skin Index");
TWEAKFUNCN(ApplySkin, "Apply Skin");

SETTWEAKPATH("Mod Menu|Weapons");
TWEAKINT32N(weapon, WEAPONTYPE_UNARMED, WEAPONTYPE_CAMERA, 1, "Weapon Index");
TWEAKFUNCN(GiveSelectedWeapon, "Give Selected Weapon");
TWEAKFUNCN(GiveAllWeapons, "Give All Weapons");

SETTWEAKPATH("Mod Menu|Spawn");
TWEAKFUNCN(SpawnInfernus, "Spawn Infernus");
TWEAKFUNCN(SpawnRhino, "Spawn Rhino");
TWEAKFUNCN(SpawnPolice, "Spawn Police");

}

#endif
