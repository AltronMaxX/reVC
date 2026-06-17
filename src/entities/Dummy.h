#pragma once

#include "Lists.h"
#include "Entity.h"

class CDummy : public CEntity
{
public:
	CEntryInfoList m_entryInfoList;

	CDummy() { m_type = ENTITY_TYPE_DUMMY; }
	void Add() override;
	void Remove() override;

	void *operator new(size_t);

	void operator delete(void*, size_t);
};

bool IsDummyPointerValid(CDummy* pDummy);
