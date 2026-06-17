#pragma once

#include "Entity.h"

class CSolid : public CEntity
{
public:
	CSolid() {
		m_type = ENTITY_TYPE_BUILDING;
		bUsesCollision = true;
	}
};