#pragma once

#include "Building.h"

class CTreadable : public CBuilding
{
public:
	void *operator new(size_t);

	void operator delete(void*, size_t);

	bool GetIsATreadable() override { return true; }
};
