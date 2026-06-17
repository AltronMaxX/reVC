#include "common.h"

#include "AnimBlendSequence.h"
#include "MemoryHeap.h"

CAnimBlendSequence::CAnimBlendSequence() {
	type = 0;
	numFrames = 0;
	keyFrames = nullptr;
	keyFramesCompressed = nullptr;
	boneTag = -1;
}

CAnimBlendSequence::~CAnimBlendSequence()
{
	if(keyFrames)
		RwFree(keyFrames);
	if(keyFramesCompressed)
		RwFree(keyFramesCompressed);
}

void
CAnimBlendSequence::SetName(const char *name)
{
	strncpy(this->name, name, 24);
}

void
CAnimBlendSequence::SetNumFrames(int numFrames, const bool translation, const bool compressed)
{
	if(translation){
		type |= KF_ROT | KF_TRANS;
		if(compressed)
			keyFramesCompressed = RwMalloc(sizeof(KeyFrameTrans) * numFrames);
		else
			keyFrames = RwMalloc(sizeof(KeyFrameTrans) * numFrames);
	}else{
		type |= KF_ROT;
		if(compressed)
			keyFramesCompressed = RwMalloc(sizeof(KeyFrame) * numFrames);
		else
			keyFrames = RwMalloc(sizeof(KeyFrame) * numFrames);
	}
	this->numFrames = numFrames;
}

void
CAnimBlendSequence::RemoveQuaternionFlips() const
{
	if(numFrames < 2)
		return;

	KeyFrame *frame = GetKeyFrame(0);
	CQuaternion last = frame->rotation;
	for(int i = 1; i < numFrames; i++){
		frame = GetKeyFrame(i);
		if(DotProduct(last, frame->rotation) < 0.0f)
			frame->rotation = -frame->rotation;
		last = frame->rotation;
	}
}

void
CAnimBlendSequence::Uncompress()
{
	int i;

	if(numFrames == 0)
		return;

	PUSH_MEMID(MEMID_ANIMATION);

	constexpr float rotScale = 1.0f/4096.0f;
	constexpr float timeScale = 1.0f/60.0f;
	constexpr float transScale = 1.0f/1024.0f;
	if(type & KF_TRANS){
		void *newKfs = RwMalloc(numFrames * sizeof(KeyFrameTrans));
		const auto *ckf = static_cast<KeyFrameTransCompressed *>(keyFramesCompressed);
		auto *kf = static_cast<KeyFrameTrans *>(newKfs);
		for(i = 0; i < numFrames; i++){
			kf->rotation.x = ckf->rot[0]*rotScale;
			kf->rotation.y = ckf->rot[1]*rotScale;
			kf->rotation.z = ckf->rot[2]*rotScale;
			kf->rotation.w = ckf->rot[3]*rotScale;
			kf->deltaTime = ckf->deltaTime*timeScale;
			kf->translation.x = ckf->trans[0]*transScale;
			kf->translation.y = ckf->trans[1]*transScale;
			kf->translation.z = ckf->trans[2]*transScale;
			kf++;
			ckf++;
		}
		keyFrames = newKfs;
	}else{
		void *newKfs = RwMalloc(numFrames * sizeof(KeyFrame));
		const auto *ckf = static_cast<KeyFrameCompressed *>(keyFramesCompressed);
		auto *kf = static_cast<KeyFrame *>(newKfs);
		for(i = 0; i < numFrames; i++){
			kf->rotation.x = ckf->rot[0]*rotScale;
			kf->rotation.y = ckf->rot[1]*rotScale;
			kf->rotation.z = ckf->rot[2]*rotScale;
			kf->rotation.w = ckf->rot[3]*rotScale;
			kf->deltaTime = ckf->deltaTime*timeScale;
			kf++;
			ckf++;
		}
		keyFrames = newKfs;
	}
	REGISTER_MEMPTR(&keyFrames);

	RwFree(keyFramesCompressed);
	keyFramesCompressed = nullptr;

	POP_MEMID();
}

void
CAnimBlendSequence::CompressKeyframes()
{
	int i;

	if(numFrames == 0)
		return;

	PUSH_MEMID(MEMID_ANIMATION);

	constexpr float rotScale = 4096.0f;
	constexpr float timeScale = 60.0f;
	if(type & KF_TRANS){
		void *newKfs = RwMalloc(numFrames * sizeof(KeyFrameTransCompressed));
		auto *ckf = static_cast<KeyFrameTransCompressed *>(newKfs);
		const auto *kf = static_cast<KeyFrameTrans *>(keyFrames);
		for(i = 0; i < numFrames; i++){
			constexpr float transScale = 1024.0f;
			ckf->rot[0] = kf->rotation.x*rotScale;
			ckf->rot[1] = kf->rotation.y*rotScale;
			ckf->rot[2] = kf->rotation.z*rotScale;
			ckf->rot[3] = kf->rotation.w*rotScale;
			ckf->deltaTime = kf->deltaTime*timeScale + 0.5f;
			ckf->trans[0] = kf->translation.x*transScale;
			ckf->trans[1] = kf->translation.y*transScale;
			ckf->trans[2] = kf->translation.z*transScale;
			kf++;
			ckf++;
		}
		keyFramesCompressed = newKfs;
	}else{
		void *newKfs = RwMalloc(numFrames * sizeof(KeyFrameCompressed));
		auto *ckf = static_cast<KeyFrameCompressed *>(newKfs);
		const auto *kf = static_cast<KeyFrame *>(keyFrames);
		for(i = 0; i < numFrames; i++){
			ckf->rot[0] = kf->rotation.x*rotScale;
			ckf->rot[1] = kf->rotation.y*rotScale;
			ckf->rot[2] = kf->rotation.z*rotScale;
			ckf->rot[3] = kf->rotation.w*rotScale;
			ckf->deltaTime = kf->deltaTime*timeScale + 0.5f;
			kf++;
			ckf++;
		}
		keyFramesCompressed = newKfs;
	}
	REGISTER_MEMPTR(&keyFramesCompressed);

	POP_MEMID();
}

void
CAnimBlendSequence::RemoveUncompressedData()
{
	if(numFrames == 0)
		return;
	CompressKeyframes();
	RwFree(keyFrames);
	keyFrames = nullptr;
}

#ifdef USE_CUSTOM_ALLOCATOR
bool
CAnimBlendSequence::MoveMemory(void)
{
	if(keyFrames){
		void *newaddr = gMainHeap.MoveMemory(keyFrames);
		if(newaddr != keyFrames){
			keyFrames = newaddr;
			return true;
		}
	}else if(keyFramesCompressed){
		void *newaddr = gMainHeap.MoveMemory(keyFramesCompressed);
		if(newaddr != keyFramesCompressed){
			keyFramesCompressed = newaddr;
			return true;
		}
	}
	return false;
}
#endif
