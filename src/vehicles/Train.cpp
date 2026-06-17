#include "common.h"
#include "main.h"

#include "Timer.h"
#include "ModelIndices.h"
#include "FileMgr.h"
#include "Streaming.h"
#include "Pad.h"
#include "Camera.h"
#include "Coronas.h"
#include "World.h"
#include "Ped.h"
#include "DMAudio.h"
#include "HandlingMgr.h"
#include "Train.h"
#include "AudioScriptObject.h"

static CTrainNode* pTrackNodes;
static int16 NumTrackNodes;
static float StationDist[3] = { 873.0f, 1522.0f, 2481.0f };
static float TotalLengthOfTrack;
static float TotalDurationOfTrack;
static CTrainInterpolationLine aLineBits[17];
static float EngineTrackPosition[2];
static float EngineTrackSpeed[2];

static CTrainNode* pTrackNodes_S;
static int16 NumTrackNodes_S;
static float StationDist_S[4] = { 55.0f, 1388.0f, 2337.0f, 3989.0f };
static float TotalLengthOfTrack_S;
static float TotalDurationOfTrack_S;
static CTrainInterpolationLine aLineBits_S[18];
static float EngineTrackPosition_S[4];
static float EngineTrackSpeed_S[4];

CVector CTrain::aStationCoors[3];
CVector CTrain::aStationCoors_S[4];

static bool bTrainArrivalAnnounced[3] = {false, false, false};

CTrain::CTrain(int32 id, uint8 CreatedBy)
 : CVehicle(CreatedBy)
{
	assert(0 && "No trains in this game");
}

void
CTrain::SetModelIndex(uint32 id)
{
}

void
CTrain::ProcessControl(void)
{
}

void
CTrain::PreRender(void)
{
}

void
CTrain::Render(void)
{
}

void
CTrain::TrainHitStuff(CPtrList &list)
{
}

void
CTrain::AddPassenger(CPed *ped)
{
}

void
CTrain::OpenTrainDoor(float ratio)
{
}



void
CTrain::InitTrains(void)
{
}

void
CTrain::Shutdown(void)
{
}

void
CTrain::ReadAndInterpretTrackFile(Const char *filename, CTrainNode **nodes, int16 *numNodes, int32 numStations, float *stationDists,
		float *totalLength, float *totalDuration, CTrainInterpolationLine *interpLines, bool rightRail)
{
}

void
PlayAnnouncement(uint8 sound, uint8 station)
{
	// this was gone in a PC version but inlined on PS2
	cAudioScriptObject *obj = new cAudioScriptObject;
	obj->AudioId = sound;
	obj->Posn = CTrain::aStationCoors[station];
	obj->AudioEntity = AEHANDLE_NONE;
	DMAudio.CreateOneShotScriptObject(obj);
}

void
ProcessTrainAnnouncements(void)
{
}

void
CTrain::UpdateTrains(void)
{
}
