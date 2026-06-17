#pragma once

#include "audio_enums.h"

class tStreamedSample
{
public:
	uint32 m_nLength;
	uint32 m_nPosition;
	uint32 m_nLastPosCheckTimer;
};

class CVehicle;
class CPed;

class cMusicManager
{
public:
	bool m_bIsInitialised;
	bool m_bDisabled;
	bool m_bSetNextStation;
	uint8 m_nVolumeLatency;
	uint8 m_nCurrentVolume;
	uint8 m_nMaxVolume;
	uint32 m_nAnnouncement;
	bool m_bAnnouncementInProgress;
	tStreamedSample m_aTracks[TOTAL_STREAMED_SOUNDS];
	bool m_bResetTimers;
	uint32 m_nResetTime;
	bool m_bRadioSetByScript;
	uint8 m_nRadioStationScript;
	int32 m_nRadioPosition;
	uint32 m_nRadioInCar;
	uint32 m_nFrontendTrack;
	uint32 m_nPlayingTrack;
	uint8 m_nUpcomingMusicMode;
	uint8 m_nMusicMode;
	bool m_FrontendLoopFlag;
	bool m_bTrackChangeStarted;
	uint32 m_nNextTrack;
	bool m_nNextLoopFlag;
	bool m_bVerifyNextTrackStartedToPlay;
	bool m_bGameplayAllowsRadio;
	bool m_bRadioStreamReady;
	int8 nFramesSinceCutsceneEnded;
	bool m_bUserResumedGame;
	bool m_bMusicModeChangeStarted;
	uint8 m_nMusicModeToBeSet;
	bool m_bEarlyFrontendTrack;
	float aListenTimeArray[NUM_RADIOS];
	float m_nLastTrackServiceTime;

public:
	cMusicManager();
	[[nodiscard]] bool IsInitialised() const { return m_bIsInitialised; }
	[[nodiscard]] uint8 GetMusicMode() const { return m_nMusicMode; }
	[[nodiscard]] uint32 GetCurrentTrack() const { return m_nPlayingTrack; }

	void ResetMusicAfterReload();
	void SetStartingTrackPositions(uint8 isNewGameTimer);
	bool Initialise();
	void Terminate();

	void ChangeMusicMode(uint8 mode);
	void StopFrontEndTrack();

	bool PlayerInCar();
	void DisplayRadioStationName();

	void PlayAnnouncement(uint32);
	void PlayFrontEndTrack(uint32, uint8);
	void PreloadCutSceneMusic(uint32);
	void PlayPreloadedCutSceneMusic();
	void StopCutSceneMusic();
	uint32 GetRadioInCar();
	void SetRadioInCar(uint32);
	void SetRadioChannelByScript(uint32, int32);

	void ResetTimers(int32);
	void Service();
	void ServiceFrontEndMode();
	void ServiceGameMode();
	void ServiceAmbience();
	void ServiceTrack(CVehicle *veh);

	bool UsesPoliceRadio(CVehicle *veh);
	bool UsesTaxiRadio(const CVehicle *veh);
	uint32 GetTrackStartPos(uint32 track);

	void ComputeAmbienceVol(uint8 reset, uint8& outVolume);
	bool ServiceAnnouncement();

	uint32 GetCarTuning();
	uint32 GetNextCarTuning();
	bool ChangeRadioChannel();
	void RecordRadioStats();
	void SetUpCorrectAmbienceTrack();
	float *GetListenTimeArray();
	uint32 GetRadioPosition(uint32 station);
	[[nodiscard]] uint32 GetFavouriteRadioStation() const;
	void SetMalibuClubTrackPos(uint8 pos);
	void SetStripClubTrackPos(uint8 pos);
	[[nodiscard]] bool CheckForMusicInterruptions() const;

	void Enable();
	void Disable();
};

VALIDATE_SIZE(cMusicManager, 0x95C);

extern cMusicManager MusicManager;
extern bool g_bAnnouncementReadPosAlready; // we have a symbol of this so it was declared in .h
float GetHeightScale();
