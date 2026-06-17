#include "common.h"

#include "Bridge.h"
#include "Pools.h"
#include "ModelIndices.h"
#include "PathFind.h"
#include "Stats.h"

CEntity *CBridge::pLiftRoad;
CEntity *CBridge::pLiftPart;
CEntity *CBridge::pWeight;

int CBridge::State;
int CBridge::OldState;

float CBridge::DefaultZLiftPart;
float CBridge::DefaultZLiftRoad;
float CBridge::DefaultZLiftWeight;

float CBridge::OldLift;

uint32 CBridge::TimeOfBridgeBecomingOperational;

void CBridge::Init()
{
}

void CBridge::Update()
{
}

bool CBridge::ShouldLightsBeFlashing()
{
	return false;
}

void CBridge::FindBridgeEntities()
{
}

bool CBridge::ThisIsABridgeObjectMovingUp(int index)
{
	return false;
}
