#pragma once

#include "Entity.h"

class CBuilding : public CEntity
{
public:
	CBuilding() {
		m_type = ENTITY_TYPE_BUILDING;
		bUsesCollision = true;
	}

	void *operator new(size_t);

	void operator delete(void*, size_t);

	void ReplaceWithNewModel(int32 id);

	virtual bool GetIsATreadable() { return false; }
};

bool IsBuildingPointerValid(CBuilding*);
