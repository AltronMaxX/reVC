#pragma once

#ifdef AUDIO_OAL
#include <AL/al.h>

#define NUM_STREAMBUFFERS 8

class IDecoder
{
public:
	virtual ~IDecoder() = default;
	
	virtual bool   IsOpened() = 0;
	
	virtual uint32 GetSampleSize() = 0;
	virtual uint32 GetSampleCount() = 0;
	virtual uint32 GetSampleRate() = 0;
	virtual uint32 GetChannels() = 0;
	
	uint32 GetAvgSamplesPerSec()
	{
		return GetChannels() * GetSampleRate();
	}
	
	uint32 ms2samples(const uint32 ms)
	{
		return static_cast<float>(ms) / 1000.0f * static_cast<float>(GetSampleRate());
	}
	
	uint32 samples2ms(const uint32 sm)
	{
		return static_cast<float>(sm) * 1000.0f / static_cast<float>(GetSampleRate());
	}
	
	uint32 GetBufferSamples()
	{
		//return (GetAvgSamplesPerSec() >> 2) - (GetSampleCount() % GetChannels());
		return (GetAvgSamplesPerSec() / 4); // 250ms
	}
	
	uint32 GetBufferSize()
	{
		return GetBufferSamples() * GetSampleSize();
	}
	
	virtual void   Seek(uint32 milliseconds) = 0;
	virtual uint32 Tell() = 0;
	
	uint32 GetLength()
	{
		return static_cast<float>(GetSampleCount()) * 1000.0f / static_cast<float>(GetSampleRate());
	}
	
	virtual uint32 Decode(void *buffer) = 0;
};

class CStream
{
	char     m_aFilename[128];
	ALuint  *m_pAlSources;
	ALuint (&m_alBuffers)[NUM_STREAMBUFFERS];
	
	bool     m_bPaused;
	bool     m_bActive;
	
	void    *m_pBuffer;
	
	bool     m_bReset;
	uint32   m_nVolume;
	uint8    m_nPan;
	uint32   m_nPosBeforeReset;
	int32   m_nLoopCount;
	
	IDecoder *m_pSoundFile;
	
	[[nodiscard]] bool HasSource() const;
	void SetPosition(int i, float x, float y, float z) const;
	void SetPitch(float pitch) const;
	void SetGain(float gain) const;
	void   Pause() const;
	void   SetPlay(bool state);
	
	bool   FillBuffer(const ALuint *alBuffer) const;
	[[nodiscard]] int32  FillBuffers() const;
	void   ClearBuffers() const;
public:
	static void Initialise();
	static void Terminate();
	
	CStream(const char *filename, ALuint *sources, ALuint (&buffers)[NUM_STREAMBUFFERS], uint32 overrideSampleRate = 32000);
	~CStream();
	void   Delete();
	
	[[nodiscard]] bool   IsOpened() const;
	[[nodiscard]] bool   IsPlaying() const;
	void   SetPause (bool bPause);
	void   SetVolume(uint32 nVol);
	void   SetPan   (uint8 nPan);
	void   SetPosMS (uint32 nPos); 
	[[nodiscard]] uint32 GetPosMS() const;
	[[nodiscard]] uint32 GetLengthMS() const;
	
	bool Setup(bool imSureQueueIsEmpty = false);
	void Start();
	void Stop();
	void Update();
	void SetLoopCount(int32);

	
	void ProviderInit();
	void ProviderTerm();
};

#endif
