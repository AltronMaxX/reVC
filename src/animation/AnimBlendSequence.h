#pragma once

#include "Quaternion.h"

#ifdef MoveMemory
#undef MoveMemory	// windows shit
#endif

// TODO: put them somewhere else?
struct KeyFrame {
	CQuaternion rotation;
	float deltaTime{};	// relative to previous key frame
};

struct KeyFrameTrans : KeyFrame {
	CVector translation;
};

struct KeyFrameCompressed {
	int16 rot[4];		// 4096
	int16 deltaTime;	// 60

	void GetRotation(CQuaternion *quat) const {
		constexpr float scale = 1.0f/4096.0f;
		quat->x = rot[0]*scale;
		quat->y = rot[1]*scale;
		quat->z = rot[2]*scale;
		quat->w = rot[3]*scale;
	}
	void SetRotation(const CQuaternion &quat){
		rot[0] = quat.x * 4096.0f;
		rot[1] = quat.y * 4096.0f;
		rot[2] = quat.z * 4096.0f;
		rot[3] = quat.w * 4096.0f;
	}
	[[nodiscard]] float GetDeltaTime() const { return deltaTime/60.0f; }
	void SetTime(const float t) { deltaTime = t*60.0f + 0.5f; }
};

struct KeyFrameTransCompressed : KeyFrameCompressed {
	int16 trans[3];		// 1024

	void GetTranslation(CVector *vec) const {
		constexpr float scale = 1.0f/1024.0f;
		vec->x = trans[0]*scale;
		vec->y = trans[1]*scale;
		vec->z = trans[2]*scale;
	}
	void SetTranslation(const CVector &vec){
		trans[0] = vec.x*1024.0f;
		trans[1] = vec.y*1024.0f;
		trans[2] = vec.z*1024.0f;
	}
};


// The sequence of key frames of one animated node
class CAnimBlendSequence
{
public:
	enum {
		KF_ROT = 1,
		KF_TRANS = 2
	};
	int32 type;
	char name[24]{};
	int32 numFrames;
	int16 boneTag;
	void *keyFrames;
	void *keyFramesCompressed;

	CAnimBlendSequence();
	virtual ~CAnimBlendSequence();
	void SetName(const char *name);
	void SetNumFrames(int numFrames, bool translation, bool compressed);
	void RemoveQuaternionFlips() const;
	[[nodiscard]] KeyFrame *GetKeyFrame(const int n) const {
		return type & KF_TRANS ?
			&static_cast<KeyFrameTrans *>(keyFrames)[n] :
			&static_cast<KeyFrame *>(keyFrames)[n];
	}
	[[nodiscard]] KeyFrameCompressed *GetKeyFrameCompressed(const int n) const {
		return type & KF_TRANS ?
			&static_cast<KeyFrameTransCompressed *>(keyFramesCompressed)[n] :
			&static_cast<KeyFrameCompressed *>(keyFramesCompressed)[n];
	}
	[[nodiscard]] bool HasTranslation() const { return !!(type & KF_TRANS); }
	void Uncompress();
	void CompressKeyframes();
	void RemoveUncompressedData();
#ifdef USE_CUSTOM_ALLOCATOR
	bool MoveMemory(void);
#endif

	void SetBoneTag(const int tag) { boneTag = tag; }
};
VALIDATE_SIZE(CAnimBlendSequence, 0x30);
