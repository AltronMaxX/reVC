#include "common.h"

#ifdef AUDIO_OAL
#include "channel.h"
#include "sampman.h"
#ifdef FIX_BUGS
#include "Timer.h"
#endif

#ifndef _WIN32
#include <float.h>
#endif

extern bool
IsFXSupported();
extern size_t gPlayerTalkDataSize;

ALuint alSources[NUM_CHANNELS];
ALuint alFilters[NUM_CHANNELS];
ALuint alBuffers[NUM_CHANNELS];
bool bChannelsCreated = false;

uint8 *tempStereoBuffer = nil;

void
CChannel::InitChannels()
{
	alGenSources(NUM_CHANNELS, alSources);
	alGenBuffers(NUM_CHANNELS, alBuffers);
	if(IsFXSupported()) alGenFilters(NUM_CHANNELS, alFilters);

	tempStereoBuffer = new uint8[Max(PED_BLOCKSIZE, gPlayerTalkDataSize) * 2];
	bChannelsCreated = true;
}

void
CChannel::DestroyChannels()
{
	if(bChannelsCreated) {
		alDeleteSources(NUM_CHANNELS, alSources);
		memset(alSources, 0, sizeof(alSources));
		alDeleteBuffers(NUM_CHANNELS, alBuffers);
		memset(alBuffers, 0, sizeof(alBuffers));
		if(IsFXSupported()) {
			alDeleteFilters(NUM_CHANNELS, alFilters);
			memset(alFilters, 0, sizeof(alFilters));
		}
		delete[] tempStereoBuffer;
		bChannelsCreated = false;
	}
}

CChannel::CChannel()
{
	Data = nil;
	DataSize = 0;
	bIs2DDefault = bForce2D = false;
	SetDefault();
}

void
CChannel::SetDefault()
{
	Pan = 63;
	Pitch = 1.0f;
	Gain = 1.0f;
	Mix = 0.0f;

	Position[0] = 0.0f;
	Position[1] = 0.0f;
	Position[2] = 0.0f;
	Distances[0] = 0.0f;
	Distances[1] = FLT_MAX;

	LoopPoints[0] = 0;
	LoopPoints[1] = -1;

	Frequency = MAX_FREQ;
	bForce2D = bIs2DDefault;
#ifdef FIX_BUGS
	m_nStartMs = 0;
#endif
}

void
CChannel::Reset()
{
	ClearBuffer();
	SetDefault();
}

void
CChannel::Init(uint32 _id, bool Is2D)
{
	id = _id;
	if(HasSource()) {
		alSourcei(alSources[id], AL_SOURCE_RELATIVE, AL_TRUE);
		if(IsFXSupported()) alSource3i(alSources[id], AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);

		if(Is2D) {
			bIs2DDefault = bForce2D = true;
			alSource3f(alSources[id], AL_POSITION, 0.0f, 0.0f, 0.0f);
			alSourcef(alSources[id], AL_GAIN, 1.0f);

			if(alIsExtensionPresent("AL_SOFT_source_spatialize")) {
				alSourcei(alSources[id], AL_SOURCE_SPATIALIZE_SOFT, AL_FALSE);
			}
			if(alIsExtensionPresent("AL_SOFT_direct_channels")) {
				alSourcei(alSources[id], AL_DIRECT_CHANNELS_SOFT, AL_TRUE);
			}
		}
	}
}

void
CChannel::Term()
{
	Stop();
	Reset();
	if(HasSource()) {
		if(IsFXSupported()) { alSource3i(alSources[id], AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL); }
	}
}

void
CChannel::Start()
{
	if(!HasSource()) return;
	if(!Data) return;

	if(bForce2D) {
		// convert mono data to stereo
		int LVol = Pan <= 63 ? 128 : (127 - Pan) / 64.0f * 128;
		int RVol = Pan >= 63 ? 128 : Pan / 64.0f * 128;

		int16 *monoData = (int16 *)Data;
		int16 *stereoData = (int16 *)tempStereoBuffer;
		for(size_t i = 0; i < DataSize / 2; i++) {
			*(stereoData++) = (*monoData * LVol) >> 7;
			*(stereoData++) = (*(monoData++) * RVol) >> 7;
		}
		alBufferData(alBuffers[id], AL_FORMAT_STEREO16, tempStereoBuffer, DataSize * 2, Frequency);
	} else
		alBufferData(alBuffers[id], AL_FORMAT_MONO16, Data, DataSize, Frequency);
#ifdef FIX_BUGS
	// Loop points come from sfx.SDT, an end of -1 means "until the end of the sample".
	// Dividing -1 by 2 (DIGITALBITS/8) in SetChannelLoopPoints turned it into 0, which
	// made OpenAL reject the loop points. As a result, looped sounds such as vehicle
	// engines fell back to looping the entire buffer (including any non-loop intro),
	// which on Linux/OpenAL Soft sounded like the sound restarting several times a
	// second — very noticeable when the FPS limiter is off. Preserve -1 here and
	// translate it to the real sample count, and only pass loop points OpenAL accepts.
	if ( LoopPoints[0] > 0 || LoopPoints[1] > 0 )
	{
		ALint numSamples = DataSize / sizeof(int16);
		ALint loopPoints[2] = { LoopPoints[0], LoopPoints[1] < 0 ? numSamples : LoopPoints[1] };
		if ( loopPoints[0] >= 0 && loopPoints[0] < loopPoints[1] && loopPoints[1] <= numSamples )
			alBufferiv(alBuffers[id], AL_LOOP_POINTS_SOFT, loopPoints);
	}
#else
	if(LoopPoints[0] != 0 && LoopPoints[0] != -1) alBufferiv(alBuffers[id], AL_LOOP_POINTS_SOFT, LoopPoints);
#endif
	alSourcei(alSources[id], AL_BUFFER, alBuffers[id]);
	alSourcePlay(alSources[id]);
#ifdef FIX_BUGS
	// Remember when we started so IsUsed() can give OpenAL a grace period
	// before trusting AL_SOURCE_STATE (which can flicker AL_STOPPED for a
	// few ms right after alSourcePlay, especially on Linux/OpenAL Soft).
	m_nStartMs = CTimer::GetTimeInMillisecondsPauseMode();
#endif
}

void
CChannel::Stop()
{
	if(HasSource()) alSourceStop(alSources[id]);
#ifdef FIX_BUGS
	m_nStartMs = 0;
#endif
}

bool
CChannel::HasSource()
{ return alSources[id] != AL_NONE; }

bool
CChannel::IsUsed()
{
	if(HasSource()) {
		ALint sourceState;
		alGetSourcei(alSources[id], AL_SOURCE_STATE, &sourceState);
#ifdef FIX_BUGS
		// OpenAL Soft on Linux can briefly report AL_STOPPED (or AL_INITIAL) for
		// a few ms right after alSourcePlay before the source actually transitions
		// to AL_PLAYING. At high frame rates (FPS limiter off) the audio manager
		// polls IsUsed() many times per second, sees the transient AL_STOPPED,
		// and re-triggers the sound — heard as engine / gear-up sounds restarting
		// several times per second. While we are inside the start grace window,
		// treat any non-AL_PAUSED state as "still playing" so the audio manager
		// does not yank the channel and restart it.
		if (sourceState != AL_PLAYING && sourceState != AL_PAUSED && m_nStartMs != 0) {
			const uint32 now = CTimer::GetTimeInMillisecondsPauseMode();
			if (now - m_nStartMs < START_GRACE_MS)
				return true;
		}
#endif
		return sourceState == AL_PLAYING;
	}
	return false;
}

void
CChannel::SetPitch(float pitch)
{
	if(!HasSource()) return;
	alSourcef(alSources[id], AL_PITCH, pitch);
}

void
CChannel::SetGain(float gain)
{
	if(!HasSource()) return;
	alSourcef(alSources[id], AL_GAIN, gain);
}

void
CChannel::SetVolume(uint32 vol)
{ SetGain(ALfloat(vol) / MAX_VOLUME); }

void
CChannel::SetSampleData(void *_data, size_t _DataSize, uint32 freq)
{
	Data = _data;
	DataSize = _DataSize;
	Frequency = freq;
}

void
CChannel::SetCurrentFreq(uint32 freq)
{ SetPitch(ALfloat(freq) / Frequency); }

void
CChannel::SetLoopCount(uint32 count)
{
	if(!HasSource()) return;

	// 0: loop indefinitely, 1: play one time, 2: play two times etc... in our case controlled by audio manager
	alSourcei(alSources[id], AL_LOOPING, count == 1 ? AL_FALSE : AL_TRUE);
}

void
CChannel::SetLoopPoints(ALint start, ALint end)
{
	LoopPoints[0] = start;
	LoopPoints[1] = end;
}

void
CChannel::SetPosition(float x, float y, float z)
{
	if(!HasSource()) return;
	alSource3f(alSources[id], AL_POSITION, x, y, z);
	bForce2D = false;
}

void
CChannel::SetDistances(float max, float min)
{
	if(!HasSource()) return;
	alSourcef(alSources[id], AL_MAX_DISTANCE, max);
	alSourcef(alSources[id], AL_REFERENCE_DISTANCE, min);
	alSourcef(alSources[id], AL_MAX_GAIN, 1.0f);
	alSourcef(alSources[id], AL_ROLLOFF_FACTOR, 1.0f);
	bForce2D = false;
}

void
CChannel::SetPan(uint8 pan)
{
	if(IsUsed()) debug("Channel %i changes pan during playback, we don't support that yet :(\n", id);

	// this is kinda pointless
	// SetPosition(((int)pan-63)/64.0f, 0.0f, -Sqrt(1.0f-SQR(((int)pan-63)/64.0f)));
	Pan = pan;
	bForce2D = true;
}

void
CChannel::ClearBuffer()
{
	if(!HasSource()) return;
	alSourcei(alSources[id], AL_LOOPING, AL_FALSE);
	alSourcei(alSources[id], AL_BUFFER, AL_NONE);
	Data = nil;
	DataSize = 0;
}

void
CChannel::SetReverbMix(ALuint slot, float mix)
{
	if(!IsFXSupported()) return;
	if(!HasSource()) return;
	if(alFilters[id] == AL_FILTER_NULL) return;

	Mix = mix;
	EAX3_SetReverbMix(alFilters[id], mix);
	alSource3i(alSources[id], AL_AUXILIARY_SEND_FILTER, slot, 0, alFilters[id]);
}

void
CChannel::UpdateReverb(ALuint slot)
{
	if(!IsFXSupported()) return;
	if(!HasSource()) return;
	if(alFilters[id] == AL_FILTER_NULL) return;
	EAX3_SetReverbMix(alFilters[id], Mix);
	alSource3i(alSources[id], AL_AUXILIARY_SEND_FILTER, slot, 0, alFilters[id]);
}

void
CChannel::ProviderTerm()
{
	SavedState = AL_STOPPED;
	SavedOffset = 0;
	SavedLooping = AL_FALSE;
	SavedMix = Mix;
	if(!HasSource()) return;
	alGetSourcei(alSources[id], AL_SOURCE_STATE, &SavedState);
	alGetSourcei(alSources[id], AL_SAMPLE_OFFSET, &SavedOffset);
	alGetSourcei(alSources[id], AL_LOOPING, &SavedLooping);
	alGetSourcef(alSources[id], AL_PITCH, &Pitch);
	alGetSourcef(alSources[id], AL_GAIN, &Gain);
	alGetSourcefv(alSources[id], AL_POSITION, Position);
	alGetSourcef(alSources[id], AL_MAX_DISTANCE, &Distances[0]);
	alGetSourcef(alSources[id], AL_REFERENCE_DISTANCE, &Distances[1]);
}

void
CChannel::ProviderInit(ALuint slot)
{
	if(!HasSource()) return;
	alSourcef(alSources[id], AL_PITCH, Pitch);
	alSourcef(alSources[id], AL_GAIN, Gain);
	alSourcefv(alSources[id], AL_POSITION, Position);
	alSourcef(alSources[id], AL_MAX_DISTANCE, Distances[0]);
	alSourcef(alSources[id], AL_REFERENCE_DISTANCE, Distances[1]);
	alSourcei(alSources[id], AL_LOOPING, SavedLooping);
	SetReverbMix(slot, SavedMix);
	if(SavedState == AL_PLAYING || SavedState == AL_PAUSED) {
		Start();
		alSourcei(alSources[id], AL_SAMPLE_OFFSET, SavedOffset);
		if(SavedState == AL_PAUSED) alSourcePause(alSources[id]);
	}
}

#endif
