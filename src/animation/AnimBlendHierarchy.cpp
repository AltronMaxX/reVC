#include "common.h"

#include "AnimBlendSequence.h"
#include "AnimBlendHierarchy.h"
#include "AnimManager.h"

CAnimBlendHierarchy::CAnimBlendHierarchy() {
	sequences = nullptr;
	numSequences = 0;
	compressed = false;
	totalLength = 0.0f;
	linkPtr = nullptr;
}

void
CAnimBlendHierarchy::Shutdown()
{
	CAnimManager::RemoveFromUncompressedCache(this);
	RemoveAnimSequences();
	totalLength = 0.0f;
	compressed = false;
}

void
CAnimBlendHierarchy::SetName(const char *name)
{
	strncpy(this->name, name, 24);
}

void
CAnimBlendHierarchy::CalcTotalTime()
{
	totalLength = 0.0f;

	for(int i = 0; i < numSequences; i++){
#ifdef FIX_BUGS
		if(sequences[i].numFrames == 0)
			continue;
#endif

		totalLength = Max(totalLength, sequences[i].GetKeyFrame(sequences[i].numFrames-1)->deltaTime);
		for(int j = sequences[i].numFrames - 1; j >= 1; j--){
			KeyFrame *kf1 = sequences[i].GetKeyFrame(j);
			const KeyFrame *kf2 = sequences[i].GetKeyFrame(j-1);
			kf1->deltaTime -= kf2->deltaTime;
		}
	}
}

void
CAnimBlendHierarchy::CalcTotalTimeCompressed()
{
	totalLength = 0.0f;

	for(int i = 0; i < numSequences; i++){
#ifdef FIX_BUGS
		if(sequences[i].numFrames == 0)
			continue;
#endif

		totalLength = Max(totalLength, sequences[i].GetKeyFrameCompressed(sequences[i].numFrames-1)->GetDeltaTime());
		for(int j = sequences[i].numFrames - 1; j >= 1; j--){
			KeyFrameCompressed *kf1 = sequences[i].GetKeyFrameCompressed(j);
			const KeyFrameCompressed *kf2 = sequences[i].GetKeyFrameCompressed(j-1);
			kf1->deltaTime -= kf2->deltaTime;
		}
	}
}

void
CAnimBlendHierarchy::RemoveQuaternionFlips() const
{
	for(int i = 0; i < numSequences; i++)
		sequences[i].RemoveQuaternionFlips();
}

void
CAnimBlendHierarchy::RemoveAnimSequences()
{
	delete[] sequences;
	sequences = nullptr;
	numSequences = 0;
}

void
CAnimBlendHierarchy::Uncompress()
{
#ifdef ANIM_COMPRESSION
	int i;
	assert(compressed);
	for(i = 0; i < numSequences; i++)
		sequences[i].Uncompress();
#endif
	compressed = false;
	if(totalLength == 0.0f){
		RemoveQuaternionFlips();
		CalcTotalTime();
	}
}

void
CAnimBlendHierarchy::RemoveUncompressedData()
{
#ifdef ANIM_COMPRESSION
	int i;
	assert(!compressed);
	for(i = 0; i < numSequences; i++)
		sequences[i].RemoveUncompressedData();
#endif
	compressed = true;
}

#ifdef USE_CUSTOM_ALLOCATOR
void
CAnimBlendHierarchy::MoveMemory(bool onlyone)
{
	int i;
	for(i = 0; i < numSequences; i++)
		if(sequences[i].MoveMemory() && onlyone)
			return;
}
#endif
