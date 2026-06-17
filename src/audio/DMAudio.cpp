#include "common.h"

#include "DMAudio.h"
#include "MusicManager.h"
#include "AudioManager.h"
#include "AudioScriptObject.h"
#include "sampman.h"

cDMAudio DMAudio;

void
cDMAudio::Initialise()
{
	AudioManager.Initialise();
}

void
cDMAudio::Terminate()
{
	AudioManager.Terminate();
}

void
cDMAudio::Service()
{
	AudioManager.Service();
}

int32
cDMAudio::CreateEntity(const eAudioType type, void *UID)
{
	return AudioManager.CreateEntity(type, UID);
}

void
cDMAudio::DestroyEntity(const int32 audioEntity)
{
	AudioManager.DestroyEntity(audioEntity);
}

void
cDMAudio::SetEntityStatus(const int32 audioEntity, const uint8 status)
{
	AudioManager.SetEntityStatus(audioEntity, status);
}

void
cDMAudio::PlayOneShot(const int32 audioEntity, const uint16 oneShot, const float volume)
{
	AudioManager.PlayOneShot(audioEntity, oneShot, volume);
}

void
cDMAudio::DestroyAllGameCreatedEntities()
{
	AudioManager.DestroyAllGameCreatedEntities();
}

void
cDMAudio::SetMonoMode(const uint8 mono)
{
	AudioManager.SetMonoMode(mono);
}

void
cDMAudio::SetMP3BoostVolume(const uint8 volume)
{
	uint8 vol = volume;
	if (vol > MAX_VOLUME) vol = MAX_VOLUME;

	AudioManager.SetMP3BoostVolume(vol);
}

void
cDMAudio::SetEffectsMasterVolume(const uint8 volume)
{
	uint8 vol = volume;
	if ( vol > MAX_VOLUME ) vol = MAX_VOLUME;
	
	AudioManager.SetEffectsMasterVolume(vol);
}

void
cDMAudio::SetMusicMasterVolume(const uint8 volume)
{
	uint8 vol = volume;
	if ( vol > MAX_VOLUME ) vol = MAX_VOLUME;
	
	AudioManager.SetMusicMasterVolume(vol);
}

void
cDMAudio::SetEffectsFadeVol(const uint8 volume)
{
	uint8 vol = volume;
	if ( vol > MAX_VOLUME ) vol = MAX_VOLUME;
	
	AudioManager.SetEffectsFadeVol(vol);
}

void
cDMAudio::SetMusicFadeVol(const uint8 volume)
{
	uint8 vol = volume;
	if ( vol > MAX_VOLUME ) vol = MAX_VOLUME;
	
	AudioManager.SetMusicFadeVol(vol);
}

uint8
cDMAudio::GetNum3DProvidersAvailable()
{
	return AudioManager.GetNum3DProvidersAvailable();
}

char *
cDMAudio::Get3DProviderName(const uint8 id)
{
	return AudioManager.Get3DProviderName(id);
}

int8 cDMAudio::AutoDetect3DProviders()
{
	return AudioManager.AutoDetect3DProviders();
}

int8
cDMAudio::GetCurrent3DProviderIndex()
{
	return AudioManager.GetCurrent3DProviderIndex();
}

int8
cDMAudio::SetCurrent3DProvider(const uint8 which)
{
	return AudioManager.SetCurrent3DProvider(which);
}

void
cDMAudio::SetSpeakerConfig(const int32 config)
{
	AudioManager.SetSpeakerConfig(config);
}

bool
cDMAudio::IsMP3RadioChannelAvailable()
{
	return AudioManager.IsMP3RadioChannelAvailable();
}

void
cDMAudio::ReleaseDigitalHandle()
{
	AudioManager.ReleaseDigitalHandle();
}

void
cDMAudio::ReacquireDigitalHandle()
{
	AudioManager.ReacquireDigitalHandle();
}

void
cDMAudio::SetDynamicAcousticModelingStatus(const uint8 status)
{
	AudioManager.SetDynamicAcousticModelingStatus(status);
}

bool
cDMAudio::CheckForAnAudioFileOnCD()
{
	return AudioManager.CheckForAnAudioFileOnCD();
}

char
cDMAudio::GetCDAudioDriveLetter()
{
	return AudioManager.GetCDAudioDriveLetter();
}

bool
cDMAudio::IsAudioInitialised()
{
	return AudioManager.IsAudioInitialised();
}

void
cDMAudio::ReportCrime(const eCrimeType crime, const CVector &pos)
{
	AudioManager.ReportCrime(crime, pos);
}

int32
cDMAudio::CreateLoopingScriptObject(cAudioScriptObject *scriptObject)
{
	const int32 audioEntity = AudioManager.CreateEntity(AUDIOTYPE_SCRIPTOBJECT, scriptObject);

	if ( AEHANDLE_IS_OK(audioEntity) )
		AudioManager.SetEntityStatus(audioEntity, true);
	
	return audioEntity;
}

void
cDMAudio::DestroyLoopingScriptObject(const int32 audioEntity)
{
	AudioManager.DestroyEntity(audioEntity);
}

void
cDMAudio::CreateOneShotScriptObject(cAudioScriptObject *scriptObject)
{
	if (const int32 audioEntity = AudioManager.CreateEntity(AUDIOTYPE_SCRIPTOBJECT, scriptObject); AEHANDLE_IS_OK(audioEntity))
	{
		AudioManager.SetEntityStatus(audioEntity, true);
		AudioManager.PlayOneShot(audioEntity, scriptObject->AudioId, 0.0f);
	}
}

void
cDMAudio::PlaySuspectLastSeen(const float x, const float y, const float z)
{
	AudioManager.PlaySuspectLastSeen(x, y, z);
}

void
cDMAudio::ReportCollision(CEntity *entityA, CEntity *entityB, const uint8 surfaceTypeA, const uint8 surfaceTypeB, const float collisionPower, const float velocity)
{
	AudioManager.ReportCollision(entityA, entityB, surfaceTypeA, surfaceTypeB, collisionPower, velocity);
}

void
cDMAudio::PlayFrontEndSound(const uint16 frontend, const uint32 volume)
{
	AudioManager.PlayOneShot(AudioManager.m_nFrontEndEntity, frontend, static_cast<float>(volume));
}

void
cDMAudio::PlayRadioAnnouncement(const uint32 announcement)
{
	MusicManager.PlayAnnouncement(announcement);
}

void
cDMAudio::PlayFrontEndTrack(const uint32 track, const uint8 frontendFlag)
{
	MusicManager.PlayFrontEndTrack(track, frontendFlag);
}

void
cDMAudio::StopFrontEndTrack()
{
	MusicManager.StopFrontEndTrack();
}

void
cDMAudio::ResetTimers(const uint32 time)
{
	AudioManager.ResetTimers(time);
}

void
cDMAudio::ChangeMusicMode(const uint8 mode)
{
	MusicManager.ChangeMusicMode(mode);
}

void
cDMAudio::PreloadCutSceneMusic(const uint32 track)
{
	MusicManager.PreloadCutSceneMusic(track);
}

void
cDMAudio::PlayPreloadedCutSceneMusic()
{
	MusicManager.PlayPreloadedCutSceneMusic();
}

void
cDMAudio::StopCutSceneMusic()
{
	MusicManager.StopCutSceneMusic();
}

void
cDMAudio::PreloadMissionAudio(const uint8 slot, Const char *missionAudio)
{
	AudioManager.PreloadMissionAudio(slot, missionAudio);
}

uint8
cDMAudio::GetMissionAudioLoadingStatus(const uint8 slot)
{
	return AudioManager.GetMissionAudioLoadingStatus(slot);
}

void
cDMAudio::SetMissionAudioLocation(const uint8 slot, const float x, const float y, const float z)
{
	AudioManager.SetMissionAudioLocation(slot, x, y, z);
}

void
cDMAudio::PlayLoadedMissionAudio(const uint8 slot)
{
	AudioManager.PlayLoadedMissionAudio(slot);
}

bool
cDMAudio::IsMissionAudioSampleFinished(const uint8 slot)
{
	return AudioManager.IsMissionAudioSampleFinished(slot);
}

void
cDMAudio::ClearMissionAudio(const uint8 slot)
{
	AudioManager.ClearMissionAudio(slot);
}

uint8
cDMAudio::GetRadioInCar()
{
	return MusicManager.GetRadioInCar();
}

void
cDMAudio::SetRadioInCar(const uint32 radio)
{
	MusicManager.SetRadioInCar(radio);
}

void
cDMAudio::SetRadioChannel(const uint32 radio, const int32 pos)
{
	MusicManager.SetRadioChannelByScript(radio, pos);
}

void
cDMAudio::SetStartingTrackPositions(const uint8 isStartGame)
{
	MusicManager.SetStartingTrackPositions(isStartGame);
}

float *
cDMAudio::GetListenTimeArray()
{
	return MusicManager.GetListenTimeArray();
}

uint32
cDMAudio::GetFavouriteRadioStation()
{
	return MusicManager.GetFavouriteRadioStation();
}

int32
cDMAudio::GetRadioPosition(const uint32 station)
{
	return MusicManager.GetRadioPosition(station);
}

void
cDMAudio::SetPedTalkingStatus(CPed *ped, const uint8 status)
{
	return AudioManager.SetPedTalkingStatus(ped, status);
}

void
cDMAudio::SetPlayersMood(const uint8 mood, const uint32 time)
{
	return AudioManager.SetPlayersMood(mood, time);
}

void
cDMAudio::ShutUpPlayerTalking(const uint8 state)
{
	AudioManager.m_bIsPlayerShutUp = state;
}