#include "common.h"

#include "rpworld.h"
#include "Treadable.h"
#include "Pools.h"

void *CTreadable::operator new(size_t) { return CPools::GetTreadablePool()->New();  }
void CTreadable::operator delete(void *p, size_t) { CPools::GetTreadablePool()->Delete(static_cast<CTreadable *>(p)); }
