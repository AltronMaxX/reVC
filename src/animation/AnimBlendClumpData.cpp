#include "common.h"

#include "AnimBlendClumpData.h"
#include "MemoryMgr.h"

CAnimBlendClumpData::CAnimBlendClumpData()
{
	numFrames = 0;
	velocity2d = nullptr;
	frames = nullptr;
	link.Init();
}

CAnimBlendClumpData::~CAnimBlendClumpData()
{
	link.Remove();
	if(frames)
		RwFreeAlign(frames);
}

void
CAnimBlendClumpData::SetNumberOfFrames(const int n)
{
	if(frames)
		RwFreeAlign(frames);
	numFrames = n;
	frames = static_cast<AnimBlendFrameData *>(RwMallocAlign(numFrames * sizeof(AnimBlendFrameData), 64));
}

void
CAnimBlendClumpData::ForAllFrames(void (*cb)(AnimBlendFrameData*, void*), void *arg) const
{
	for(int i = 0; i < numFrames; i++)
		cb(&frames[i], arg);
}
