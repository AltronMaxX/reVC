//#define JUICY_OAL

#ifdef AUDIO_OAL
#include <ctime>

#include "eax.h"
#include "eax-util.h"

#ifdef _WIN32
#include <io.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>
#include <AL/efx-presets.h>

// for user MP3s
#include <direct.h>
#include <shlobj.h>
#include <shlguid.h>
#else
#define _getcwd getcwd
#endif

#if defined _MSC_VER && !defined CMAKE_NO_AUTOLINK
#pragma comment( lib, "OpenAL32.lib" )
#endif

#include "common.h"
#include "crossplatform.h"

#include "sampman.h"

#include "oal/oal_utils.h"
#include "oal/aldlist.h"
#include "oal/channel.h"
#include "oal/stream.h"

#include "AudioManager.h"
#include "MusicManager.h"
#include "Frontend.h"
#include "Timer.h"
#ifdef AUDIO_OAL_USE_OPUS
#include <opusfile.h>
#endif

//TODO: fix eax3 reverb
//TODO: max channels

cSampleManager SampleManager;
bool _bSampmanInitialised = false;

uint32 BankStartOffset[MAX_SFX_BANKS];

int           prevprovider=-1;
int           curprovider=-1;
int           usingEAX=0;
int           usingEAX3=0;
//int         speaker_type=0;
ALCdevice    *ALDevice = NULL;
ALCcontext   *ALContext = NULL;
unsigned int _maxSamples;
float        _fPrevEaxRatioDestination;
bool         _usingEFX;
float        _fEffectsLevel;
ALuint       ALEffect = AL_EFFECT_NULL;
ALuint       ALEffectSlot = AL_EFFECTSLOT_NULL;
struct
{
	char id[256];
	char name[256];
	int sources;
}providers[MAXPROVIDERS];

int defaultProvider;


char SampleBankDescFilename[] = "audio/sfx.SDT";
char SampleBankDataFilename[] = "audio/sfx.RAW";

FILE *fpSampleDescHandle;
#ifdef OPUS_SFX
OggOpusFile *fpSampleDataHandle;
#else
FILE *fpSampleDataHandle;
#endif
bool  bSampleBankLoaded            [MAX_SFX_BANKS];
int32 nSampleBankDiscStartOffset   [MAX_SFX_BANKS];
int32 nSampleBankSize              [MAX_SFX_BANKS];
uintptr nSampleBankMemoryStartAddress[MAX_SFX_BANKS];
int32 _nSampleDataEndOffset;

int32 nPedSlotSfx    [MAX_PEDSFX];
int32 nPedSlotSfxAddr[MAX_PEDSFX];
uint8 nCurrentPedSlot;

CChannel aChannel[MAXCHANNELS+MAX2DCHANNELS];
uint8 nChannelVolume[MAXCHANNELS+MAX2DCHANNELS];

uint32 nStreamLength[TOTAL_STREAMED_SOUNDS];
ALuint ALStreamSources[MAX_STREAMS][2];
ALuint ALStreamBuffers[MAX_STREAMS][NUM_STREAMBUFFERS];

struct tMP3Entry
{
	char aFilename[MAX_PATH];

	uint32 nTrackLength;
	uint32 nTrackStreamPos;

	tMP3Entry* pNext;
	char* pLinkPath;
};

uint32 nNumMP3s;
tMP3Entry* _pMP3List;
char _mp3DirectoryPath[MAX_PATH]; 
CStream    *aStream[MAX_STREAMS];
uint8      nStreamPan   [MAX_STREAMS];
uint8      nStreamVolume[MAX_STREAMS];
uint8      nStreamLoopedFlag[MAX_STREAMS];
uint32 _CurMP3Index;
int32 _CurMP3Pos;
bool _bIsMp3Active;
///////////////////////////////////////////////////////////////
//	Env		Size	Diffus	Room	RoomHF	RoomLF	DecTm	DcHF	DcLF	Refl	RefDel	Ref Pan				Revb	RevDel		Rev Pan				EchTm	EchDp	ModTm	ModDp	AirAbs	HFRef		LFRef	RRlOff	FLAGS
EAXLISTENERPROPERTIES StartEAX3 =
	{26,	1.7f,	0.8f,	-1000,	-1000,	-100,	4.42f,	0.14f,	1.00f,	429,	0.014f,	0.00f,0.00f,0.00f,	1023,	0.021f,		0.00f,0.00f,0.00f,	0.250f,	0.000f,	0.250f,	0.000f,	-5.0f,	2727.1f,	250.0f,	0.00f,	0x3f };

EAXLISTENERPROPERTIES FinishEAX3 =
	{26,	100.0f,	1.0f,	0,		-1000,	-2200,	20.0f,	1.39f,	1.00f,	1000,	0.069f,	0.00f,0.00f,0.00f,	400,	0.100f,		0.00f,0.00f,0.00f,	0.250f,	1.000f,	3.982f,	0.000f,	-18.0f,	3530.8f,	417.9f,	6.70f,	0x3f };

EAXLISTENERPROPERTIES EAX3Params;


bool IsFXSupported()
{
	return usingEAX || usingEAX3 || _usingEFX;
}

void EAX_SetAll(const EAXLISTENERPROPERTIES *allparameters)
{
	if ( usingEAX || usingEAX3 )
		EAX3_Set(ALEffect, allparameters);
	else
		EFX_Set(ALEffect, allparameters);
}

static void
add_providers()
{
	SampleManager.SetNum3DProvidersAvailable(0);

	const ALDeviceList *pDeviceList = nullptr;
	pDeviceList = new ALDeviceList();

	if (pDeviceList && pDeviceList->GetNumDevices())
	{
		const int devNumber = Min(pDeviceList->GetNumDevices(), MAXPROVIDERS);
		int n = 0;
		
		for (int i = 0; i < devNumber; i++) 
		{
			if ( n < MAXPROVIDERS )
			{
				strcpy(providers[n].id, pDeviceList->GetDeviceName(i));
				strncpy(providers[n].name, pDeviceList->GetDeviceName(i), sizeof(providers[n].name));
				providers[n].sources = pDeviceList->GetMaxNumSources(i);
				SampleManager.Set3DProviderName(n, providers[n].name);
				n++;
			}
			
			if ( alGetEnumValue("AL_EFFECT_EAXREVERB") != 0
				|| pDeviceList->IsExtensionSupported(i, ADEXT_EAX2)
				|| pDeviceList->IsExtensionSupported(i, ADEXT_EAX3) 
				|| pDeviceList->IsExtensionSupported(i, ADEXT_EAX4)
				|| pDeviceList->IsExtensionSupported(i, ADEXT_EAX5) )
			{
				if ( n < MAXPROVIDERS )
				{
					strcpy(providers[n].id, pDeviceList->GetDeviceName(i));
					strncpy(providers[n].name, pDeviceList->GetDeviceName(i), sizeof(providers[n].name));
					strcat(providers[n].name, " EAX");
					providers[n].sources = pDeviceList->GetMaxNumSources(i);
					SampleManager.Set3DProviderName(n, providers[n].name);
					n++;
				}
				
				if ( n < MAXPROVIDERS )
				{
					strcpy(providers[n].id, pDeviceList->GetDeviceName(i));
					strncpy(providers[n].name, pDeviceList->GetDeviceName(i), sizeof(providers[n].name));
					strcat(providers[n].name, " EAX3");
					providers[n].sources = pDeviceList->GetMaxNumSources(i);
					SampleManager.Set3DProviderName(n, providers[n].name);
					n++;
				}
			}
		}
		SampleManager.SetNum3DProvidersAvailable(n);
	
		for(int j=n;j<MAXPROVIDERS;j++)
			SampleManager.Set3DProviderName(j, NULL);
		
		defaultProvider = pDeviceList->GetDefaultDevice();
		if ( defaultProvider > MAXPROVIDERS )
			defaultProvider = 0;
	}
	
	delete pDeviceList;
}

static void
release_existing()
{
	for ( int32 i = 0; i < MAXCHANNELS; i++ )
		aChannel[i].Term();
	aChannel[CHANNEL2D].Term();
	
	if ( IsFXSupported() )
	{
		if ( alIsEffect(ALEffect) )
		{
			alEffecti(ALEffect, AL_EFFECT_TYPE, AL_EFFECT_NULL);
			alDeleteEffects(1, &ALEffect);
			ALEffect = AL_EFFECT_NULL;
		}
		
		if (alIsAuxiliaryEffectSlot(ALEffectSlot))
		{
			alAuxiliaryEffectSloti(ALEffectSlot, AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL);
			
			alDeleteAuxiliaryEffectSlots(1, &ALEffectSlot);
			ALEffectSlot = AL_EFFECTSLOT_NULL;
		}
	}
	
	for ( int32 i = 0; i < MAX_STREAMS; i++ )
	{
		if (CStream *stream = aStream[i])
			stream->ProviderTerm();
		
		alDeleteBuffers(NUM_STREAMBUFFERS, ALStreamBuffers[i]);
	}
	alDeleteSources(MAX_STREAMS*2, ALStreamSources[0]);
	
	CChannel::DestroyChannels();
	
	if ( ALContext )
	{
		alcMakeContextCurrent(nullptr);
		alcSuspendContext(ALContext);
		alcDestroyContext(ALContext);
	}
	if ( ALDevice )
		alcCloseDevice(ALDevice);
	
	ALDevice = nullptr;
	ALContext = nullptr;
	
	_fPrevEaxRatioDestination = 0.0f;
	_usingEFX                 = false;
	_fEffectsLevel            = 0.0f;
	
	DEV("release_existing()\n");
}

static bool
set_new_provider(const int index)
{
	if ( curprovider == index )
		return true;
	
	curprovider = index;
	
	release_existing();
	
	if ( curprovider != -1 )
	{
		DEV("set_new_provider()\n");
		
		//TODO:
		_maxSamples = MAXCHANNELS;

		constexpr ALCint attr[] = {ALC_FREQUENCY,MAX_FREQ,
						ALC_MONO_SOURCES, MAX_STREAMS * 2 + MAXCHANNELS,
						0,
						};
		
		ALDevice  = alcOpenDevice(providers[index].id);
		ASSERT(ALDevice != nullptr);
		
		ALContext = alcCreateContext(ALDevice, attr);
		ASSERT(ALContext != nullptr);
		
		alcMakeContextCurrent(ALContext);
	
		const char* ext=(const char*)alGetString(AL_EXTENSIONS);
		ASSERT(strstr(ext,"AL_SOFT_loop_points")!=nullptr);
		if ( strstr(ext,"AL_SOFT_loop_points")== nullptr)
		{
			curprovider=-1;
			release_existing();
			return false;
		}
		
		alListenerf (AL_GAIN,     1.0f);
		alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);
		alListener3f(AL_VELOCITY, 0.0f, 0.0f, 0.0f);
		const ALfloat orientation[6] = { 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f };
		alListenerfv(AL_ORIENTATION, orientation);
		
		alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
		
		if ( alcIsExtensionPresent(ALDevice, (ALCchar*)ALC_EXT_EFX_NAME) )
		{
			alGenAuxiliaryEffectSlots(1, &ALEffectSlot);
			alGenEffects(1, &ALEffect);
		}

		alGenSources(MAX_STREAMS*2, ALStreamSources[0]);
		for ( int32 i = 0; i < MAX_STREAMS; i++ )
		{
			alGenBuffers(NUM_STREAMBUFFERS, ALStreamBuffers[i]);
			alSourcei(ALStreamSources[i][0], AL_SOURCE_RELATIVE, AL_TRUE);
			alSource3f(ALStreamSources[i][0], AL_POSITION, 0.0f, 0.0f, 0.0f);
			alSourcef(ALStreamSources[i][0], AL_GAIN, 1.0f);
			alSourcei(ALStreamSources[i][1], AL_SOURCE_RELATIVE, AL_TRUE);
			alSource3f(ALStreamSources[i][1], AL_POSITION, 0.0f, 0.0f, 0.0f);
			alSourcef(ALStreamSources[i][1], AL_GAIN, 1.0f);

			if (CStream *stream = aStream[i])
				stream->ProviderInit();
		}
		
		usingEAX = 0;
		usingEAX3 = 0;
		_usingEFX = false;
		
		if ( !strcmp(&providers[index].name[strlen(providers[index].name) - strlen(" EAX3")], " EAX3") 
				&& alcIsExtensionPresent(ALDevice, (ALCchar*)ALC_EXT_EFX_NAME) )
		{
			EAX_SetAll(&FinishEAX3);
			
			usingEAX = 1;
			usingEAX3 = 1;

			DEV("EAX3\n");
		}
		else if ( alcIsExtensionPresent(ALDevice, (ALCchar*)ALC_EXT_EFX_NAME) )
		{
			EAX_SetAll(&EAX30_ORIGINAL_PRESETS[EAX_ENVIRONMENT_CAVE]);
			
			if ( !strcmp(&providers[index].name[strlen(providers[index].name) - strlen(" EAX")], " EAX"))
			{
				usingEAX = 1;
				DEV("EAX1\n");
			}
			else
			{
				_usingEFX = true;
				DEV("EFX\n");
			}
		}
		
		//SampleManager.SetSpeakerConfig(speaker_type);
		
		CChannel::InitChannels();

		for ( int32 i = 0; i < MAXCHANNELS; i++ )
			aChannel[i].Init(i);
		aChannel[CHANNEL2D].Init(CHANNEL2D, true);
		
		if ( IsFXSupported() )
		{
			/**/
			alAuxiliaryEffectSloti(ALEffectSlot, AL_EFFECTSLOT_EFFECT, ALEffect);
			/**/
			
			for ( int32 i = 0; i < MAXCHANNELS; i++ )
				aChannel[i].SetReverbMix(ALEffectSlot, 0.0f);
		}
		
		return true;
	}
	
	return false;
}

static bool
IsThisTrackAt16KHz(const uint32 track)
{
	return track == STREAMED_SOUND_RADIO_KCHAT || track == STREAMED_SOUND_RADIO_VCPR || track == STREAMED_SOUND_RADIO_POLICE;
}

cSampleManager::cSampleManager()
{
	;
}

cSampleManager::~cSampleManager()
{
	
}

void cSampleManager::SetSpeakerConfig()
{

}

uint32 cSampleManager::GetMaximumSupportedChannels()
{
	if ( _maxSamples > MAXCHANNELS )
		return MAXCHANNELS;
	
	return _maxSamples;
}

uint32 cSampleManager::GetNum3DProvidersAvailable() const {
	return m_nNumberOfProviders;
}

void cSampleManager::SetNum3DProvidersAvailable(const uint32 num)
{
	m_nNumberOfProviders = num;
}

char *cSampleManager::Get3DProviderName(const uint8 id) const {
	return m_aAudioProviders[id];
}

void cSampleManager::Set3DProviderName(const uint8 id, char *name)
{
	m_aAudioProviders[id] = name;
}

int8 cSampleManager::GetCurrent3DProviderIndex()
{
	return curprovider;
}

int8 cSampleManager::SetCurrent3DProvider(uint8 nProvider) const {
	const int savedprovider = curprovider;

	nProvider = CLAMP(nProvider, 0, m_nNumberOfProviders - 1);

	if ( set_new_provider(nProvider) )
		return curprovider;
	if ( savedprovider != -1 && savedprovider < m_nNumberOfProviders && set_new_provider(savedprovider) )
		return curprovider;
	return curprovider;
}

int8
cSampleManager::AutoDetect3DProviders()
{
	if (!AudioManager.IsAudioInitialised())
		return -1;

	if (defaultProvider >= 0 && defaultProvider < m_nNumberOfProviders) {
		if (set_new_provider(defaultProvider))
			return defaultProvider;
	}

	for (uint32 i = 0; i < GetNum3DProvidersAvailable(); i++)
	{
		if (const char* providername = Get3DProviderName(i); !strcasecmp(providername, "OPENAL SOFT")) {
			SetCurrent3DProvider(i);
			if (GetCurrent3DProviderIndex() == i)
				return i;
		}
	}

	return -1;
}

static bool
_ResolveLink(char const *path, char *out)
{
#ifdef _WIN32
	size_t len = strlen(path);
	if (len < 4 || strcmp(&path[len - 4], ".lnk") != 0)
		return false;
		
	IShellLink* psl;
	WIN32_FIND_DATA fd;
	char filepath[MAX_PATH];
	
	CoInitialize(NULL);
									   
	if (SUCCEEDED( CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLink, (LPVOID*)&psl ) ))
	{
		IPersistFile *ppf;

		if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile, (LPVOID*)&ppf)))
		{
			WCHAR wpath[MAX_PATH];
			
			MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);
			
			if (SUCCEEDED(ppf->Load(wpath, STGM_READ)))
			{
				/* Resolve the link */
				if (SUCCEEDED(psl->Resolve(NULL, SLR_ANY_MATCH|SLR_NO_UI|SLR_NOSEARCH)))
				{
					strcpy(filepath, path);
					
					if (SUCCEEDED(psl->GetPath(filepath, MAX_PATH, &fd, SLGP_UNCPRIORITY)))
					{
						OutputDebugString(fd.cFileName);
						
						strcpy(out, filepath);
						// FIX: Release the objects. Taken from SA.
#ifdef FIX_BUGS
						ppf->Release();
						psl->Release();
#endif
						return true;
					}
				}
			}
			
			ppf->Release();
		}
		psl->Release();
	}
	
	return false;
#else
	struct stat sb;

	if (lstat(path, &sb) == -1) {
		perror("lstat: ");
		return false;
	}

	if (S_ISLNK(sb.st_mode)) {
		const auto linkname = static_cast<char *>(alloca(sb.st_size + 1));
		if (linkname == nullptr) {
			fprintf(stderr, "insufficient memory\n");
			return false;
		}

		if (readlink(path, linkname, sb.st_size + 1) < 0) {
			perror("readlink: ");
			return false;
		}
		linkname[sb.st_size] = '\0';
		strcpy(out, linkname);
		return true;
	}
	return false;
#endif
}

static void
_FindMP3s()
{
	tMP3Entry *pList;
	bool bShortcut;	
	bool bInitFirstEntry;
	char path[MAX_PATH];
	char filepath[MAX_PATH*2];
	int total_ms;
	WIN32_FIND_DATA fd;
	
	if (getcwd(_mp3DirectoryPath, MAX_PATH) == nullptr) {
		perror("getcwd: ");
		return;
	}
	
	OutputDebugString("Finding MP3s...");
	strcpy(path, _mp3DirectoryPath);
	strcat(path, "\\MP3\\");
	
	strcpy(_mp3DirectoryPath, path);
	OutputDebugString(_mp3DirectoryPath);
	
	strcat(path, "*");

	const HANDLE hFind = FindFirstFile(path, &fd);
	
	if ( hFind == nullptr)
	{
		return;
	}
	
	strcpy(filepath, _mp3DirectoryPath);
	strcat(filepath, fd.cFileName);

	if (const size_t filepathlen = strlen(filepath); filepathlen <= 0)
	{
		FindClose(hFind);
		return;
	}

	if ( _ResolveLink(filepath, filepath) )
	{
		OutputDebugString("Resolving Link");
		OutputDebugString(filepath);
		bShortcut = true;
	} else
		bShortcut = false;
	
	aStream[0] = new CStream(filepath, ALStreamSources[0], ALStreamBuffers[0]);

	if (aStream[0] && aStream[0]->IsOpened())
	{
		total_ms = aStream[0]->GetLengthMS();
		delete aStream[0];
		aStream[0] = nullptr;

		OutputDebugString(fd.cFileName);
		
		_pMP3List = new tMP3Entry;
		
		if ( _pMP3List == nullptr)
		{
			FindClose(hFind);
			return;
		}
		
		nNumMP3s = 1;
		
		strcpy(_pMP3List->aFilename, fd.cFileName);
		
		_pMP3List->nTrackLength = total_ms;
		
		_pMP3List->pNext = nullptr;
		
		pList = _pMP3List;
		
		if ( bShortcut )
		{
			_pMP3List->pLinkPath = new char[MAX_PATH*2];
			strcpy(_pMP3List->pLinkPath, filepath);
		}
		else
		{
			_pMP3List->pLinkPath = nullptr;
		}

		bInitFirstEntry = false;
	}
	else
	{
		strcat(filepath, " - NOT A VALID MP3");
		
		OutputDebugString(filepath);

		bInitFirstEntry = true;
	}
	
	while ( true )
	{
		if ( !FindNextFile(hFind, &fd) )
			break;
		
		if ( bInitFirstEntry )
		{
			strcpy(filepath, _mp3DirectoryPath);
			strcat(filepath, fd.cFileName);

			if (const size_t filepathlen = strlen(filepath); filepathlen > 0 )
			{
				if ( _ResolveLink(filepath, filepath) )
				{
					OutputDebugString("Resolving Link");
					OutputDebugString(filepath);
					bShortcut = true;
				} else {
					bShortcut = false;
					if (filepathlen > MAX_PATH) {
						continue;
					}
				}
				aStream[0] = new CStream(filepath, ALStreamSources[0], ALStreamBuffers[0]);

				if (aStream[0] && aStream[0]->IsOpened())
				{
					total_ms = aStream[0]->GetLengthMS();
					delete aStream[0];
					aStream[0] = nullptr;
					
					OutputDebugString(fd.cFileName);
					
					_pMP3List = new tMP3Entry;
					
					if ( _pMP3List  == nullptr)
						break;
					
					nNumMP3s = 1;
					
					strcpy(_pMP3List->aFilename, fd.cFileName);
					
					_pMP3List->nTrackLength = total_ms;
					_pMP3List->pNext = nullptr;
					
					if ( bShortcut )
					{
						_pMP3List->pLinkPath = new char [MAX_PATH*2];
						strcpy(_pMP3List->pLinkPath, filepath);
					}
					else
					{
						_pMP3List->pLinkPath = nullptr;
					}
					
					pList = _pMP3List;

					bInitFirstEntry = false;
				}
				else
				{
					strcat(filepath, " - NOT A VALID MP3");
					OutputDebugString(filepath);
				}
			}
		}
		else
		{
			strcpy(filepath, _mp3DirectoryPath);
			strcat(filepath, fd.cFileName);

			if (const size_t filepathlen = strlen(filepath); filepathlen > 0 )
			{
				if ( _ResolveLink(filepath, filepath) )
				{
					OutputDebugString("Resolving Link");
					OutputDebugString(filepath);
					bShortcut = true;
				} else
					bShortcut = false;
				
				aStream[0] = new CStream(filepath, ALStreamSources[0], ALStreamBuffers[0]);

				if (aStream[0] && aStream[0]->IsOpened())
				{
					total_ms = aStream[0]->GetLengthMS();
					delete aStream[0];
					aStream[0] = nullptr;

					OutputDebugString(fd.cFileName);
					
					pList->pNext = new tMP3Entry;
					
					tMP3Entry *e = pList->pNext;
					
					if ( e == nullptr)
						break;
					
					pList = pList->pNext;
					
					strcpy(e->aFilename, fd.cFileName);
					e->nTrackLength = total_ms;
					e->pNext = nullptr;
					
					if ( bShortcut )
					{
						e->pLinkPath = new char [MAX_PATH*2];
						strcpy(e->pLinkPath, filepath);
					}
					else
					{
						e->pLinkPath = nullptr;
					}
					
					nNumMP3s++;
					
					OutputDebugString(fd.cFileName);
				}
				else
				{
					strcat(filepath, " - NOT A VALID MP3");
					OutputDebugString(filepath);
				}
			}
		}
	}

	FindClose(hFind);
}

static void
_DeleteMP3Entries()
{
	tMP3Entry *e = _pMP3List;

	while ( e != nullptr)
	{
		tMP3Entry *next = e->pNext;
		
		if ( next == nullptr)
			next = nullptr;
		
		if ( e->pLinkPath != nullptr)
		{
#ifndef FIX_BUGS
			delete   e->pLinkPath; // BUG: should be delete []
#else
			delete[] e->pLinkPath;
#endif
			e->pLinkPath = nullptr;
		}
		
		delete e;
		
		if ( next )
			e = next;
		else
			e = nullptr;
		
		nNumMP3s--;
	}
	
	
	if ( nNumMP3s != 0 )
	{
		OutputDebugString("Not all MP3 entries were deleted");
		nNumMP3s = 0;
	}
	
	_pMP3List = nullptr;
}

static tMP3Entry *
_GetMP3EntryByIndex(const uint32 idx)
{
	const uint32 n = ( idx < nNumMP3s ) ? idx : 0;
	
	if ( _pMP3List != nullptr)
	{
		tMP3Entry *e = _pMP3List;
		
		for ( uint32 i = 0; i < n; i++ )
			e = e->pNext;
		
		return e;
			
	}
	
	return nullptr;
}

static bool
_GetMP3PosFromStreamPos(uint32 *pPosition, tMP3Entry **pEntry)
{
	_CurMP3Index = 0;
	
	for ( *pEntry = _pMP3List; *pEntry != nullptr; *pEntry = (*pEntry)->pNext )
	{
		if (   *pPosition >= (*pEntry)->nTrackStreamPos
			&& *pPosition <  (*pEntry)->nTrackLength + (*pEntry)->nTrackStreamPos )
		{
			*pPosition -= (*pEntry)->nTrackStreamPos;
			_CurMP3Pos = *pPosition;
			
			return true;
		}
		
		_CurMP3Index++;
	}
				
	*pPosition = 0;
	*pEntry = _pMP3List;
	_CurMP3Pos = 0;
	_CurMP3Index = 0;
	
	return false;
}

bool
cSampleManager::IsMP3RadioChannelAvailable()
{
	return nNumMP3s != 0;
}


void cSampleManager::ReleaseDigitalHandle()
{
	if ( ALDevice )
	{
		prevprovider = curprovider;
		release_existing();
		curprovider = -1;
	}
}

void cSampleManager::ReacquireDigitalHandle()
{
	if ( ALDevice )
	{
		if ( prevprovider != -1 )
			set_new_provider(prevprovider);
	}
}

bool
cSampleManager::Initialise()
{
	if ( _bSampmanInitialised )
		return true;

	EFXInit();
	CStream::Initialise();

	{
		for (auto & m_aSample : m_aSamples)
		{
			m_aSample.nOffset    = 0;
			m_aSample.nSize      = 0;
			m_aSample.nFrequency = MAX_FREQ;
			m_aSample.nLoopStart = 0;
			m_aSample.nLoopEnd   = -1;
		}
		
		m_nEffectsVolume     = MAX_VOLUME;
		m_nMusicVolume       = MAX_VOLUME;
		m_nEffectsFadeVolume = MAX_VOLUME;
		m_nMusicFadeVolume   = MAX_VOLUME;
	
		m_nMonoMode = 0;
	}
	
	{
		curprovider = -1;
		prevprovider = -1;
			
		_usingEFX = false;
		usingEAX =0;
		usingEAX3=0;
			
		_fEffectsLevel = 0.0f;
			
		_maxSamples = 0;
		
		ALDevice = nullptr;
		ALContext = nullptr;
	}
	
	{
		fpSampleDescHandle = nullptr;
		fpSampleDataHandle = nullptr;
		
		for ( int32 i = 0; i < MAX_SFX_BANKS; i++ )
		{
			bSampleBankLoaded[i]             = false;
			nSampleBankDiscStartOffset[i]    = 0;
			nSampleBankSize[i]               = 0;
			nSampleBankMemoryStartAddress[i] = 0;
		}
	}
	
	{
		for ( int32 i = 0; i < MAX_PEDSFX; i++ )
		{
			nPedSlotSfx[i]     = NO_SAMPLE;
			nPedSlotSfxAddr[i] = 0;
		}
		
		nCurrentPedSlot = 0;
	}
	
	{
		for (unsigned char & i : nChannelVolume)
			i = 0;
	}
	
	{	
		for (unsigned int & i : nStreamLength)
			i = 0;
	}
	
		add_providers();

#ifdef AUDIO_CACHE
	if (FILE *cacheFile = fcaseopen("audio\\sound.cache", "rb")) {
		debug("Loadind audio cache (If game crashes around here, then your cache is corrupted, remove audio/sound.cache)\n");
		fread(nStreamLength, sizeof(uint32), TOTAL_STREAMED_SOUNDS, cacheFile);
		fclose(cacheFile);
	} else
	{
		debug("Cannot load audio cache\n");
#endif

		for ( int32 i = 0; i < TOTAL_STREAMED_SOUNDS; i++ )
		{	
			aStream[0] = new CStream(StreamedNameTable[i], ALStreamSources[0], ALStreamBuffers[0], IsThisTrackAt16KHz(i) ? 16000 : 32000);
			
			if ( aStream[0] && aStream[0]->IsOpened() )
			{
				const uint32 tatalms = aStream[0]->GetLengthMS();
				delete aStream[0];
				aStream[0] = nullptr;
				
				nStreamLength[i] = tatalms;
			}
			else
				USERERROR("Can't open '%s'\n", StreamedNameTable[i]);
		}
#ifdef AUDIO_CACHE
		cacheFile = fcaseopen("audio\\sound.cache", "wb");
		if(cacheFile) {
			debug("Saving audio cache\n");
			fwrite(nStreamLength, sizeof(uint32), TOTAL_STREAMED_SOUNDS, cacheFile);
			fclose(cacheFile);
		} else {
			debug("Cannot save audio cache\n");
		}
	}
#endif

	{
		if ( !InitialiseSampleBanks() )
		{
			Terminate();
			return false;
		}
		
		nSampleBankMemoryStartAddress[SFX_BANK_0] = reinterpret_cast<uintptr>(malloc(nSampleBankSize[SFX_BANK_0]));
		ASSERT(nSampleBankMemoryStartAddress[SFX_BANK_0] != 0);
		
		if ( nSampleBankMemoryStartAddress[SFX_BANK_0] == 0 )
		{
			Terminate();
			return false;
		}
		
		nSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS] = reinterpret_cast<uintptr>(malloc(PED_BLOCKSIZE * MAX_PEDSFX));
		ASSERT(nSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS] != 0);
	
		LoadSampleBank(SFX_BANK_0);
	}
	
	{
		for ( int32 i = 0; i < MAX_STREAMS; i++ )
		{
			aStream[i]       = nullptr;
			nStreamVolume[i] = 100;
			nStreamPan[i]    = 63;
		}
	}
	
	{
		_bSampmanInitialised = true;
		
		if ( defaultProvider >= 0 && defaultProvider < m_nNumberOfProviders )
		{
			set_new_provider(defaultProvider);
		}
		else
		{
			Terminate();
			return false;
		}
	}

	{
		nNumMP3s = 0;
		
		_pMP3List = nullptr;
		
		_FindMP3s();
		
		if ( nNumMP3s != 0 )
		{
			nStreamLength[STREAMED_SOUND_RADIO_MP3_PLAYER] = 0;
			
			for ( tMP3Entry *e = _pMP3List; e != nullptr; e = e->pNext )
			{
				e->nTrackStreamPos = nStreamLength[STREAMED_SOUND_RADIO_MP3_PLAYER];
				nStreamLength[STREAMED_SOUND_RADIO_MP3_PLAYER] += e->nTrackLength;
			}

			const time_t t = time(nullptr);
			tm *localtm;
			bool bUseRandomTable;
			
			if ( t == -1 )
				bUseRandomTable = true;
			else
			{
				bUseRandomTable = false;
				localtm = localtime(&t);
			}
			
			int32 randval;
			if ( bUseRandomTable )
				randval = AudioManager.GetRandomNumber(1);
			else
				randval = localtm->tm_sec * localtm->tm_min;
			
			_CurMP3Index = randval % nNumMP3s;

			const tMP3Entry *randmp3 = _pMP3List;
			for ( int32 i = randval % nNumMP3s; i > 0; --i)
				randmp3 = randmp3->pNext;
			
			if ( bUseRandomTable )
				_CurMP3Pos = AudioManager.GetRandomNumber(0)     % randmp3->nTrackLength;
			else
			{
				if ( localtm->tm_sec > 0 )
				{
					const int32 s = localtm->tm_sec;
					_CurMP3Pos = s*s*s*s*s*s*s*s                 % randmp3->nTrackLength;
				}
				else
					_CurMP3Pos = AudioManager.GetRandomNumber(0) % randmp3->nTrackLength;
			}
		}
		else
			_CurMP3Pos = 0;
		
		_bIsMp3Active = false;
	}
	
	return true;
}

void
cSampleManager::Terminate()
{
	for (auto & i : aStream)
	{
		if (const CStream *stream = i)
		{
			delete stream;
			i = nullptr;
		}
	}

	release_existing();

	_DeleteMP3Entries();

	CStream::Terminate();

	if ( nSampleBankMemoryStartAddress[SFX_BANK_0] != 0 )
	{
		free(reinterpret_cast<void *>(nSampleBankMemoryStartAddress[SFX_BANK_0]));
		nSampleBankMemoryStartAddress[SFX_BANK_0] = 0;
	}

	if ( nSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS] != 0 )
	{
		free(reinterpret_cast<void *>(nSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS]));
		nSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS] = 0;
	}
	
	_bSampmanInitialised = false;
}

bool cSampleManager::CheckForAnAudioFileOnCD()
{
	return true;
}

char cSampleManager::GetCDAudioDriveLetter()
{
	return '\0';
}

void
cSampleManager::UpdateEffectsVolume()
{
	if ( _bSampmanInitialised )
	{
		for ( int32 i = 0; i < MAXCHANNELS+MAX2DCHANNELS; i++ )
		{
			if ( GetChannelUsedFlag(i) )
			{
				if ( nChannelVolume[i] != 0 )
					aChannel[i].SetVolume(m_nEffectsFadeVolume*nChannelVolume[i]*m_nEffectsVolume >> 14);
			}
		}
	}
}

void
cSampleManager::SetEffectsMasterVolume(const uint8 nVolume)
{
	m_nEffectsVolume = nVolume;
	UpdateEffectsVolume();
}

void
cSampleManager::SetMusicMasterVolume(const uint8 nVolume)
{
	m_nMusicVolume = nVolume;
}

void
cSampleManager::SetMP3BoostVolume(const uint8 nVolume)
{
	m_nMP3BoostVolume = nVolume;
}

void
cSampleManager::SetEffectsFadeVolume(const uint8 nVolume)
{
	m_nEffectsFadeVolume = nVolume;
	UpdateEffectsVolume();
}

void
cSampleManager::SetMusicFadeVolume(const uint8 nVolume)
{
	m_nMusicFadeVolume = nVolume;
}

void
cSampleManager::SetMonoMode(const uint8 nMode)
{
	m_nMonoMode = nMode;
}

bool
cSampleManager::LoadSampleBank(const uint8 nBank)
{
	ASSERT( nBank < MAX_SFX_BANKS);
	
	if ( CTimer::GetIsCodePaused() )
		return false;
	
	if ( MusicManager.IsInitialised()
		&& MusicManager.GetMusicMode() == MUSICMODE_CUTSCENE
		&& nBank != SFX_BANK_0 )
	{
		return false;
	}
	
#ifdef OPUS_SFX
	int samplesRead = 0;
	int samplesSize = nSampleBankSize[nBank] / 2;
	op_pcm_seek(fpSampleDataHandle, 0);
	while (samplesSize > 0) {
		int size = op_read(fpSampleDataHandle, (opus_int16 *)(nSampleBankMemoryStartAddress[nBank] + samplesRead), samplesSize, NULL);
		if (size <= 0) {
			// huh?
			//assert(0);
			break;
		}
		samplesRead += size*2;
		samplesSize -= size;
	}
#else
	if ( fseek(fpSampleDataHandle, nSampleBankDiscStartOffset[nBank], SEEK_SET) != 0 )
		return false;
	
	if ( fread(reinterpret_cast<void *>(nSampleBankMemoryStartAddress[nBank]), 1, nSampleBankSize[nBank], fpSampleDataHandle) != nSampleBankSize[nBank] )
		return false;
#endif
	bSampleBankLoaded[nBank] = true;
	
	return true;
}

void
cSampleManager::UnloadSampleBank(const uint8 nBank)
{
	ASSERT( nBank < MAX_SFX_BANKS);
	
	bSampleBankLoaded[nBank] = false;
}

bool
cSampleManager::IsSampleBankLoaded(const uint8 nBank)
{
	ASSERT( nBank < MAX_SFX_BANKS);
	
	return bSampleBankLoaded[nBank];
}

bool
cSampleManager::IsPedCommentLoaded(const uint32 nComment)
{
	ASSERT( nComment < TOTAL_AUDIO_SAMPLES );

	for ( int32 i = 0; i < _TODOCONST(3); i++ )
	{
		int8 slot = nCurrentPedSlot - i - 1;
#ifdef FIX_BUGS
		if (slot < 0)
			slot += std::size(nPedSlotSfx);
#endif
		if ( nComment == nPedSlotSfx[slot] )
			return true;
	}
	
	return false;
}


int32
cSampleManager::_GetPedCommentSlot(const uint32 nComment)
{
	for (int32 i = 0; i < _TODOCONST(3); i++)
	{
		int8 slot = nCurrentPedSlot - i - 1;
#ifdef FIX_BUGS
		if (slot < 0)
			slot += std::size(nPedSlotSfx);
#endif
		if (nComment == nPedSlotSfx[slot])
			return slot;
	}

	return -1;
}

bool
cSampleManager::LoadPedComment(const uint32 nComment)
{
	ASSERT( nComment < TOTAL_AUDIO_SAMPLES );
	
	if ( CTimer::GetIsCodePaused() )
		return false;
	
	// no talking peds during cutsenes or the game end
	if ( MusicManager.IsInitialised() )
	{
		switch ( MusicManager.GetMusicMode() )
		{
			case MUSICMODE_CUTSCENE:
			{
				return false;

				break;
			}
		}
	}

#ifdef OPUS_SFX
	int samplesRead = 0;
	int samplesSize = m_aSamples[nComment].nSize / 2;
	op_pcm_seek(fpSampleDataHandle, m_aSamples[nComment].nOffset / 2);
	while (samplesSize > 0) {
		int size = op_read(fpSampleDataHandle, (opus_int16 *)(nSampleBankMemoryStartAddress[SAMPLEBANK_PED] + PED_BLOCKSIZE * nCurrentPedSlot + samplesRead),
		                   samplesSize, NULL);
		if (size <= 0) {
			return false;
		}
		samplesRead += size * 2;
		samplesSize -= size;
	}
#else
	if ( fseek(fpSampleDataHandle, m_aSamples[nComment].nOffset, SEEK_SET) != 0 )
		return false;
	
	if ( fread(reinterpret_cast<void *>(nSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS] + PED_BLOCKSIZE * nCurrentPedSlot), 1, m_aSamples[nComment].nSize, fpSampleDataHandle) != m_aSamples[nComment].nSize )
		return false;

#endif
	nPedSlotSfx[nCurrentPedSlot] = nComment;
		
	if ( ++nCurrentPedSlot >= MAX_PEDSFX )
		nCurrentPedSlot = 0;
	
	return true;
}

int32
cSampleManager::GetBankContainingSound(const uint32 offset)
{
	if ( offset >= BankStartOffset[SFX_BANK_PED_COMMENTS] )
		return SFX_BANK_PED_COMMENTS;
	
	if ( offset >= BankStartOffset[SFX_BANK_0] )
		return SFX_BANK_0;
	
	return INVALID_SFX_BANK;
}

int32
cSampleManager::GetSampleBaseFrequency(const uint32 nSample) const {
	ASSERT( nSample < TOTAL_AUDIO_SAMPLES );
	return m_aSamples[nSample].nFrequency;
}

int32
cSampleManager::GetSampleLoopStartOffset(const uint32 nSample) const {
	ASSERT( nSample < TOTAL_AUDIO_SAMPLES );
	return m_aSamples[nSample].nLoopStart;
}

int32
cSampleManager::GetSampleLoopEndOffset(const uint32 nSample) const {
	ASSERT( nSample < TOTAL_AUDIO_SAMPLES );
	return m_aSamples[nSample].nLoopEnd;
}

uint32
cSampleManager::GetSampleLength(const uint32 nSample) const {
	ASSERT( nSample < TOTAL_AUDIO_SAMPLES );
	return m_aSamples[nSample].nSize / sizeof(uint16);
}

bool cSampleManager::UpdateReverb()
{
	if ( !usingEAX && !_usingEFX )
		return false;

	if ( AudioManager.GetFrameCounter() & 15 )
		return false;

	float fRatio = 0.0f;

#define MIN_DIST 0.5f
#define CALCULATE_RATIO(value, maxDist, maxRatio) (value > MIN_DIST && value < maxDist ? value / maxDist * maxRatio : 0)

	fRatio += CALCULATE_RATIO(AudioManager.GetReflectionsDistance(REFLECTION_CEIL_NORTH), 10.0f, 1/2.f);
	fRatio += CALCULATE_RATIO(AudioManager.GetReflectionsDistance(REFLECTION_CEIL_SOUTH), 10.0f, 1/2.f);
	fRatio += CALCULATE_RATIO(AudioManager.GetReflectionsDistance(REFLECTION_CEIL_WEST), 10.0f, 1/2.f);
	fRatio += CALCULATE_RATIO(AudioManager.GetReflectionsDistance(REFLECTION_CEIL_EAST), 10.0f, 1/2.f);

	fRatio += CALCULATE_RATIO((AudioManager.GetReflectionsDistance(REFLECTION_NORTH) + AudioManager.GetReflectionsDistance(REFLECTION_SOUTH)) / 2.f, 4.0f, 1/3.f);
	fRatio += CALCULATE_RATIO((AudioManager.GetReflectionsDistance(REFLECTION_WEST) + AudioManager.GetReflectionsDistance(REFLECTION_EAST)) / 2.f, 4.0f, 1/3.f);

#undef CALCULATE_RATIO
#undef MIN_DIST
	
	fRatio = CLAMP(fRatio, 0.0f, 0.6f);
	
	if ( fRatio == _fPrevEaxRatioDestination )
		return false;
	
#ifdef JUICY_OAL
	if ( usingEAX3 || _usingEFX )
#else
	if ( usingEAX3 )
#endif
	{
		fRatio = Min(fRatio * 1.67f, 1.0f);
		if ( EAX3ListenerInterpolate(&StartEAX3, &FinishEAX3, fRatio, &EAX3Params, false) )
		{
			EAX_SetAll(&EAX3Params);
			
			/*
			if ( IsFXSupported() )
			{
				alAuxiliaryEffectSloti(ALEffectSlot, AL_EFFECTSLOT_EFFECT, ALEffect);
			
				for ( int32 i = 0; i < MAXCHANNELS; i++ )
					aChannel[i].UpdateReverb(ALEffectSlot);
			}
			*/
			
			_fEffectsLevel = fRatio * 0.75f;
		}
	}
	else
	{
		if ( _usingEFX )
			_fEffectsLevel = fRatio * 0.8f;
		else
			_fEffectsLevel = fRatio * 0.22f;
	}
	_fEffectsLevel = Min(_fEffectsLevel, 1.0f);

	_fPrevEaxRatioDestination = fRatio;
	
	return true;
}

void
cSampleManager::SetChannelReverbFlag(const uint32 nChannel, const uint8 nReverbFlag)
{
	ASSERT( nChannel < MAXCHANNELS+MAX2DCHANNELS );
	
	if ( usingEAX || _usingEFX )
	{
		if ( IsFXSupported() )
		{
			alAuxiliaryEffectSloti(ALEffectSlot, AL_EFFECTSLOT_EFFECT, ALEffect);
			
			if ( nReverbFlag != 0 )
				aChannel[nChannel].SetReverbMix(ALEffectSlot, _fEffectsLevel);
			else
				aChannel[nChannel].SetReverbMix(ALEffectSlot, 0.0f);
		}
	}
}

bool
cSampleManager::InitialiseChannel(const uint32 nChannel, const uint32 nSfx, const uint8 nBank)
{
	ASSERT( nChannel < MAXCHANNELS+MAX2DCHANNELS );
	
	uintptr addr;
	
	if ( nSfx < SAMPLEBANK_MAX )
	{
		if ( !IsSampleBankLoaded(nBank) )
			return false;
		
		addr = nSampleBankMemoryStartAddress[nBank] + m_aSamples[nSfx].nOffset - m_aSamples[BankStartOffset[nBank]].nOffset;
	}
	else
	{
		if ( !IsPedCommentLoaded(nSfx) )
			return false;

		const int32 slot = _GetPedCommentSlot(nSfx);
		addr = nSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS] + PED_BLOCKSIZE * slot;
	}
	
	if ( GetChannelUsedFlag(nChannel) )
	{
		TRACE("Stopping channel %d - really!!!", nChannel);
		StopChannel(nChannel);
	}
	
	aChannel[nChannel].Reset();
	if ( aChannel[nChannel].HasSource() )
	{	
		aChannel[nChannel].SetSampleData   (reinterpret_cast<void *>(addr), m_aSamples[nSfx].nSize, m_aSamples[nSfx].nFrequency);
		aChannel[nChannel].SetLoopPoints   (0, -1);
		aChannel[nChannel].SetPitch        (1.0f);
		return true;
	}
	
	return false;
}

void
cSampleManager::SetChannelEmittingVolume(const uint32 nChannel, const uint32 nVolume) const {
	ASSERT( nChannel != CHANNEL2D );
	ASSERT( nChannel < MAXCHANNELS+MAX2DCHANNELS );
	
	uint32 vol = nVolume;
	if ( vol > MAX_VOLUME ) vol = MAX_VOLUME;
	
	nChannelVolume[nChannel] = vol;
	
	if (MusicManager.GetMusicMode() == MUSICMODE_CUTSCENE ) {
		if (MusicManager.GetCurrentTrack() == STREAMED_SOUND_CUTSCENE_FINALE)
			nChannelVolume[nChannel] = 0;
		else
			nChannelVolume[nChannel] >>= 2;
	}

	// no idea, does this one looks like a bug, or it's SetChannelVolume ?
	aChannel[nChannel].SetVolume(m_nEffectsFadeVolume*nChannelVolume[nChannel]*m_nEffectsVolume >> 14);
}

void
cSampleManager::SetChannel3DPosition(const uint32 nChannel, const float fX, const float fY, const float fZ)
{
	ASSERT( nChannel != CHANNEL2D );
	ASSERT( nChannel < MAXCHANNELS+MAX2DCHANNELS );
	
	aChannel[nChannel].SetPosition(-fX, fY, fZ);
}

void
cSampleManager::SetChannel3DDistances(const uint32 nChannel, const float fMax, const float fMin)
{
	ASSERT( nChannel != CHANNEL2D );
	ASSERT( nChannel < MAXCHANNELS+MAX2DCHANNELS );
	aChannel[nChannel].SetDistances(fMax, fMin);
}

void
cSampleManager::SetChannelVolume(const uint32 nChannel, const uint32 nVolume) const {
	ASSERT( nChannel == CHANNEL2D );
	ASSERT( nChannel < MAXCHANNELS+MAX2DCHANNELS );
	
	if ( nChannel == CHANNEL2D )
	{
		uint32 vol = nVolume;
		if ( vol > MAX_VOLUME ) vol = MAX_VOLUME;
		
		nChannelVolume[nChannel] = vol;
		
		// increase the volume for JB.MP3 and S4_BDBD.MP3
		if (MusicManager.GetMusicMode() == MUSICMODE_CUTSCENE ) {
			if (MusicManager.GetCurrentTrack() == STREAMED_SOUND_CUTSCENE_FINALE)
				nChannelVolume[nChannel] = 0;
			else
				nChannelVolume[nChannel] >>= 2;
		}

		aChannel[nChannel].SetVolume(m_nEffectsFadeVolume*vol*m_nEffectsVolume >> 14);
	}
}

void
cSampleManager::SetChannelPan(const uint32 nChannel, const uint32 nPan)
{
	ASSERT(nChannel == CHANNEL2D);
	ASSERT( nChannel < MAXCHANNELS+MAX2DCHANNELS );
	
	if ( nChannel == CHANNEL2D )
	{
		aChannel[nChannel].SetPan(nPan);
	}
}

void
cSampleManager::SetChannelFrequency(const uint32 nChannel, const uint32 nFreq)
{
	ASSERT( nChannel < MAXCHANNELS+MAX2DCHANNELS );
	
	aChannel[nChannel].SetCurrentFreq(nFreq);
}

void
cSampleManager::SetChannelLoopPoints(const uint32 nChannel, const uint32 nLoopStart, const int32 nLoopEnd)
{
	ASSERT( nChannel < MAXCHANNELS+MAX2DCHANNELS );
	
	aChannel[nChannel].SetLoopPoints(nLoopStart / (DIGITALBITS / 8), nLoopEnd / (DIGITALBITS / 8));
}

void
cSampleManager::SetChannelLoopCount(const uint32 nChannel, const uint32 nLoopCount)
{
	ASSERT( nChannel < MAXCHANNELS+MAX2DCHANNELS );
	
	aChannel[nChannel].SetLoopCount(nLoopCount);
}

bool
cSampleManager::GetChannelUsedFlag(const uint32 nChannel)
{
	ASSERT( nChannel < MAXCHANNELS+MAX2DCHANNELS );
	
	return aChannel[nChannel].IsUsed();
}

void
cSampleManager::StartChannel(const uint32 nChannel)
{
	ASSERT( nChannel < MAXCHANNELS+MAX2DCHANNELS );
	
	aChannel[nChannel].Start();
}

void
cSampleManager::StopChannel(const uint32 nChannel)
{
	ASSERT( nChannel < MAXCHANNELS+MAX2DCHANNELS );
	
	aChannel[nChannel].Stop();
}

void
cSampleManager::PreloadStreamedFile(const uint32 nFile, const uint8 nStream)
{
	char filename[MAX_PATH];
	
	ASSERT( nStream < MAX_STREAMS );

	if ( nFile < TOTAL_STREAMED_SOUNDS )
	{
		if ( aStream[nStream] )
		{
			delete aStream[nStream];
			aStream[nStream] = nullptr;
		}
		
		strcpy(filename, StreamedNameTable[nFile]);
		
		auto *stream = new CStream(filename, ALStreamSources[nStream], ALStreamBuffers[nStream], IsThisTrackAt16KHz(nFile) ? 16000 : 32000);
		ASSERT(stream != nullptr);
		
		aStream[nStream] = stream;
		if ( !stream->Setup() )
		{
			delete stream;
			aStream[nStream] = nullptr;
		}
	}
}

void
cSampleManager::PauseStream(const uint8 nPauseFlag, const uint8 nStream)
{
	ASSERT( nStream < MAX_STREAMS );

	if ( CStream *stream = aStream[nStream] )
	{
		stream->SetPause(nPauseFlag != 0);
	}
}

void
cSampleManager::StartPreloadedStreamedFile(const uint8 nStream)
{
	ASSERT( nStream < MAX_STREAMS );

	if ( CStream *stream = aStream[nStream] )
	{
		if ( stream->IsOpened() )
		{
			stream->Start();
		}
	}
}

bool
cSampleManager::StartStreamedFile(uint32 nFile, const uint32 nPos, const uint8 nStream)
{
	uint32 position = nPos;
	char filename[MAX_PATH];
	
	if ( nFile >= TOTAL_STREAMED_SOUNDS )
		return false;

	if ( aStream[nStream] )
	{
		delete aStream[nStream];
		aStream[nStream] = nullptr;
	}
	if ( nFile == STREAMED_SOUND_RADIO_MP3_PLAYER )
	{
		int i = 0;
		do
		{
			// Just switched to MP3 player
			if ( !_bIsMp3Active && i == 0 )
			{
				if ( nPos > nStreamLength[STREAMED_SOUND_RADIO_MP3_PLAYER] )
					position = 0;
				tMP3Entry *e = _pMP3List;

				// Try to continue from previous song, if already started
				if(!_GetMP3PosFromStreamPos(&position, &e) && !e) {
					nFile = 0;

					strcpy(filename, StreamedNameTable[nFile]);
					const auto stream = new CStream(filename, ALStreamSources[nStream], ALStreamBuffers[nStream], IsThisTrackAt16KHz(nFile) ? 16000 : 32000);

					aStream[nStream] = stream;

					if (stream->Setup()) {
						stream->SetLoopCount(nStreamLoopedFlag[nStream] ? 0 : 1);
						nStreamLoopedFlag[nStream] = true;
						if (position != 0)
							stream->SetPosMS(position);

						stream->Start();

						return true;
					}
					delete stream;
					aStream[nStream] = nullptr;
					return false;

				} else {

					if (e->pLinkPath != nullptr)
						aStream[nStream] = new CStream(e->pLinkPath, ALStreamSources[nStream], ALStreamBuffers[nStream], IsThisTrackAt16KHz(nFile) ? 16000 : 32000);
					else {
						strcpy(filename, _mp3DirectoryPath);
						strcat(filename, e->aFilename);

						aStream[nStream] = new CStream(filename, ALStreamSources[nStream], ALStreamBuffers[nStream]);
					}

					if (aStream[nStream]->Setup()) {
						if (position != 0)
							aStream[nStream]->SetPosMS(position);

						aStream[nStream]->Start();

						_bIsMp3Active = true;
						return true;
					} else {
						delete aStream[nStream];
						aStream[nStream] = nullptr;
					}
					// fall through, start playing from another song
				}
			} else {
				if(++_CurMP3Index >= nNumMP3s) _CurMP3Index = 0;

				_CurMP3Pos = 0;

				const tMP3Entry *mp3 = _GetMP3EntryByIndex(_CurMP3Index);
				if ( !mp3 )
				{
					mp3 = _pMP3List;
					if ( !_pMP3List )
					{
						nFile = 0;
						_bIsMp3Active = false;
						strcpy(filename, StreamedNameTable[nFile]);

						auto* stream = new CStream(filename, ALStreamSources[nStream], ALStreamBuffers[nStream], IsThisTrackAt16KHz(nFile) ? 16000 : 32000);

						aStream[nStream] = stream;

						if (stream->Setup()) {
							stream->SetLoopCount(nStreamLoopedFlag[nStream] ? 0 : 1);
							nStreamLoopedFlag[nStream] = true;
							if (position != 0)
								stream->SetPosMS(position);

							stream->Start();

							return true;
						}
						delete stream;
						aStream[nStream] = nullptr;
						return false;
					}
				}
				if (mp3->pLinkPath != nullptr)
					aStream[nStream] = new CStream(mp3->pLinkPath, ALStreamSources[nStream], ALStreamBuffers[nStream], IsThisTrackAt16KHz(nFile) ? 16000 : 32000);
				else {
					strcpy(filename, _mp3DirectoryPath);
					strcat(filename, mp3->aFilename);

					aStream[nStream] = new CStream(filename, ALStreamSources[nStream], ALStreamBuffers[nStream], IsThisTrackAt16KHz(nFile) ? 16000 : 32000);
				}

				if (aStream[nStream]->Setup()) {
					aStream[nStream]->Start();
#ifdef FIX_BUGS
					_bIsMp3Active = true;
#endif
					return true;
				}
				delete aStream[nStream];
				aStream[nStream] = nullptr;
			}
			_bIsMp3Active = false;
		}
		while ( ++i < nNumMP3s );
		position = 0;
		nFile = 0;
	}
	strcpy(filename, StreamedNameTable[nFile]);
	
	auto *stream = new CStream(filename, ALStreamSources[nStream], ALStreamBuffers[nStream], IsThisTrackAt16KHz(nFile) ? 16000 : 32000);

	aStream[nStream] = stream;
	
	if ( stream->Setup() ) {
		stream->SetLoopCount(nStreamLoopedFlag[nStream] ? 0 : 1);
		nStreamLoopedFlag[nStream] = true;
		if (position != 0)
			stream->SetPosMS(position);	

		stream->Start();
		
		return true;
	}
	delete stream;
	aStream[nStream] = nullptr;
	return false;
}

void
cSampleManager::StopStreamedFile(const uint8 nStream)
{
	ASSERT( nStream < MAX_STREAMS );

	if (const CStream *stream = aStream[nStream] )
	{
		delete stream;
		aStream[nStream] = nullptr;

		if ( nStream == 0 )
			_bIsMp3Active = false;
	}
}

int32
cSampleManager::GetStreamedFilePosition(const uint8 nStream)
{
	ASSERT( nStream < MAX_STREAMS );

	if (const CStream *stream = aStream[nStream] )
	{
		if ( _bIsMp3Active )
		{
			if (const tMP3Entry *mp3 = _GetMP3EntryByIndex(_CurMP3Index); mp3 != nullptr)
			{
				return stream->GetPosMS() + mp3->nTrackStreamPos;
			}
			return 0;
		}
		return stream->GetPosMS();
	}
	
	return 0;
}

void
cSampleManager::SetStreamedVolumeAndPan(uint8 nVolume, uint8 nPan, const uint8 nEffectFlag, const uint8 nStream) const {
	ASSERT( nStream < MAX_STREAMS );
	
	float boostMult = 0.0f;

	if ( nVolume > MAX_VOLUME )
		nVolume = MAX_VOLUME;
	
	if ( nPan > MAX_VOLUME )
		nPan = MAX_VOLUME;

	if ( MusicManager.GetRadioInCar() == USERTRACK && !MusicManager.CheckForMusicInterruptions() )
			boostMult = m_nMP3BoostVolume / 64.f;
		
	nStreamVolume[nStream] = nVolume;
	nStreamPan   [nStream] = nPan;

	if ( CStream *stream = aStream[nStream] )
	{
		if ( nEffectFlag ) {
			if ( nStream == 1 || nStream == 2 )
				stream->SetVolume(128*nVolume*m_nEffectsVolume >> 14);
			else
				stream->SetVolume(m_nEffectsFadeVolume*nVolume*m_nEffectsVolume >> 14);
		}
		else
			stream->SetVolume((m_nMusicFadeVolume*nVolume*static_cast<uint32>(m_nMusicVolume * boostMult + m_nMusicVolume)) >> 14);
		
		stream->SetPan(nPan);
	}
}

int32
cSampleManager::GetStreamedFileLength(const uint8 nStream)
{
	ASSERT( nStream < TOTAL_STREAMED_SOUNDS );

	return nStreamLength[nStream];
}

bool
cSampleManager::IsStreamPlaying(const uint8 nStream)
{
	ASSERT( nStream < MAX_STREAMS );

	if ( CStream *stream = aStream[nStream] )
	{
		if ( stream->IsPlaying() )
			return true;
	}
	
	return false;
}

void
cSampleManager::Service()
{
	for (const auto stream : aStream)
	{
			if ( stream )
			stream->Update();
	}
	int refCount = CChannel::channelsThatNeedService;
	for ( int32 i = 0; refCount && i < MAXCHANNELS+MAX2DCHANNELS; i++ )
	{
		if ( aChannel[i].Update() )
			refCount--;
	}
}

bool
cSampleManager::InitialiseSampleBanks()
{
	int32 nBank = SFX_BANK_0;
	
	fpSampleDescHandle = fcaseopen(SampleBankDescFilename, "rb");
	if ( fpSampleDescHandle == nullptr)
		return false;
#ifndef OPUS_SFX
	fpSampleDataHandle = fcaseopen(SampleBankDataFilename, "rb");
	if ( fpSampleDataHandle == nullptr)
	{
		fclose(fpSampleDescHandle);
		fpSampleDescHandle = nullptr;
		
		return false;
	}
	
	fseek(fpSampleDataHandle, 0, SEEK_END);
	int32 _nSampleDataEndOffset = ftell(fpSampleDataHandle);
	rewind(fpSampleDataHandle);
#else
	int e;
	fpSampleDataHandle = op_open_file(SampleBankDataFilename, &e);
#endif
	fread(m_aSamples, sizeof(tSample), TOTAL_AUDIO_SAMPLES, fpSampleDescHandle);
#ifdef OPUS_SFX
	int32 _nSampleDataEndOffset = m_aSamples[TOTAL_AUDIO_SAMPLES - 1].nOffset + m_aSamples[TOTAL_AUDIO_SAMPLES - 1].nSize;
#endif
	fclose(fpSampleDescHandle);
	fpSampleDescHandle = nullptr;
	
	for ( int32 i = 0; i < TOTAL_AUDIO_SAMPLES; i++ )
	{
#ifdef FIX_BUGS
		if (nBank >= MAX_SFX_BANKS) break;
#endif
		if ( BankStartOffset[nBank] == BankStartOffset[SFX_BANK_0] + i )
		{
			nSampleBankDiscStartOffset[nBank] = m_aSamples[i].nOffset;
			nBank++;
		}
	}

	nSampleBankSize[SFX_BANK_0] = nSampleBankDiscStartOffset[SFX_BANK_PED_COMMENTS] - nSampleBankDiscStartOffset[SFX_BANK_0];
	nSampleBankSize[SFX_BANK_PED_COMMENTS]  = _nSampleDataEndOffset                      - nSampleBankDiscStartOffset[SFX_BANK_PED_COMMENTS];

	return true;
}

void
cSampleManager::SetStreamedFileLoopFlag(const uint8 nLoopFlag, const uint8 nChannel)
{
	nStreamLoopedFlag[nChannel] = nLoopFlag;
}

#endif
