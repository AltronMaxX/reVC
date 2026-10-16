#ifndef __GTA_CHANNEL_H__
#define __GTA_CHANNEL_H__

#ifdef AUDIO_OAL
#include "oal/oal_utils.h"
#include <AL/al.h>
#include <AL/alext.h>
#include <AL/efx.h>


class CChannel
{
	uint8  id;
	bool   bIs2DDefault;
	bool   bForce2D;
	uint8  Pan;
	float  Pitch, Gain;
	float  Mix;
	void  *Data;
	size_t DataSize;
	uint32 Frequency;
	float  Position[3];
	float  Distances[2];
	ALint  LoopPoints[2];
	ALint  SavedState, SavedOffset, SavedLooping;
	float  SavedMix;
#ifdef FIX_BUGS
	// Wall-clock timestamp (ms, from CTimer::GetTimeInMillisecondsPauseMode) of the
	// last alSourcePlay on this channel. Used to give OpenAL a grace period after
	// Start(): alGetSourcei(AL_SOURCE_STATE) can briefly return AL_STOPPED /
	// AL_INITIAL for a few ms right after alSourcePlay before the source actually
	// transitions to AL_PLAYING (especially on Linux/OpenAL Soft at high frame
	// rates). Without the grace window, ProcessActiveQueues and
	// ProcessPlayersVehicleEngine think the sound already finished and re-trigger
	// it, which is heard as the sound restarting several times per second.
	uint32 m_nStartMs;
	static constexpr uint32 START_GRACE_MS = 50;
#endif
public:
	static void InitChannels();
	static void DestroyChannels();

	CChannel();
	void SetDefault();
	void Reset();
	void Init(uint32 _id, bool Is2D = false);
	void Term();
	void Start();
	void Stop();
	bool HasSource();
	bool IsUsed();
	void SetPitch(float pitch);
	void SetGain(float gain);
	void SetVolume(uint32 vol);
	void SetSampleData(void *_data, size_t _DataSize, uint32 freq);
	void SetCurrentFreq(uint32 freq);
	void SetLoopCount(uint32 count);
	void SetLoopPoints(ALint start, ALint end);
	void SetPosition(float x, float y, float z);
	void SetDistances(float max, float min);
	void SetPan(uint8 pan);
	void ClearBuffer();
	void SetReverbMix(ALuint slot, float mix);
	void UpdateReverb(ALuint slot);
	void ProviderTerm();
	void ProviderInit(ALuint slot);
};

#endif

#endif // __GTA_CHANNEL_H__
