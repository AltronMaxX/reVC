#pragma once

#include "templates.h"

#ifdef MoveMemory
#undef MoveMemory	// windows shit
#endif

class CAnimBlendSequence;

// A collection of sequences
class CAnimBlendHierarchy
{
public:
	char name[24]{};
	CAnimBlendSequence *sequences;
	int16 numSequences;
	bool compressed;
	bool keepCompressed{};
	float totalLength;
	CLink<CAnimBlendHierarchy*> *linkPtr;

	CAnimBlendHierarchy();
	void Shutdown();
	void SetName(const char *name);
	void CalcTotalTime();
	void CalcTotalTimeCompressed();

	void RemoveQuaternionFlips() const;

	void RemoveAnimSequences();
	void Uncompress();
	void RemoveUncompressedData();
#ifdef USE_CUSTOM_ALLOCATOR
	void MoveMemory(bool onlyone = false);
#endif
	[[nodiscard]] bool IsCompressed() const { return !!compressed; };
};

VALIDATE_SIZE(CAnimBlendHierarchy, 0x28);