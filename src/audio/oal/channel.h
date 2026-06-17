#pragma once

#ifdef AUDIO_OAL
#include <AL/al.h>
#include <AL/alext.h>


class CChannel
{
	uint32 id{};
	float  Pitch{}, Gain{};
	float  Mix{};
	void  *Data;
	size_t DataSize;
	int32  Frequency{};
	float  Position[3]{};
	float  Distances[2]{};
	int32  LoopCount{};
	ALint  LoopPoints[2]{};
	ALint  LastProcessedOffset{};
public:
	static int32 channelsThatNeedService;

	static void InitChannels();
	static void DestroyChannels();

	CChannel();
	void SetDefault();
	void Reset();
	void Init(uint32 _id, bool Is2D = false);
	void Term();
	void Start() const;
	void Stop();

	[[nodiscard]] bool HasSource() const;

	[[nodiscard]] bool IsUsed() const;
	void SetPitch(float pitch) const;
	void SetGain(float gain) const;
	void SetVolume(int32 vol) const;
	void SetSampleData(void *_data, size_t _DataSize, int32 freq);
	void SetCurrentFreq(uint32 freq) const;
	void SetLoopCount(int32 count);
	void SetLoopPoints(ALint start, ALint end);
	void SetPosition(float x, float y, float z) const;
	void SetDistances(float max, float min) const;
	void SetPan(int32 pan) const;
	void ClearBuffer();
	void SetReverbMix(ALuint slot, float mix);
	void UpdateReverb(ALuint slot) const;
	bool Update();
};

#endif