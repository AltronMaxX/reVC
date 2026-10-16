#ifndef __GTA_PARTICLE_H__
#define __GTA_PARTICLE_H__
#include "ParticleMgr.h"


class CEntity;

class CParticle
{
public:
	enum
	{
		RAND_TABLE_SIZE    = 20,
		SIN_COS_TABLE_SIZE = 1024
	};

	CVector   m_vecPosition;
	CVector   m_vecVelocity;
	uint32    m_nTimeWhenWillBeDestroyed;
	uint32    m_nTimeWhenColorWillBeChanged;
	float     m_fZGround;
	CVector   m_vecParticleMovementOffset;
	int16     m_nCurrentZRotation;
	uint16    m_nZRotationTimer;
	float     m_fCurrentZRadius;
	uint16    m_nZRadiusTimer;
	uint8     m_nColorIntensity;
	uint8     m_nAlpha;
	float     m_fSize;
	float     m_fExpansionRate;
	int16     m_nFadeToBlackTimer;
	int16     m_nFadeAlphaTimer;
	int16     m_nAnimationSpeedTimer;
	int16     m_nRotationStep;
	int16     m_nRotation;
	uint8     m_nCurrentFrame;
	RwRGBA    m_Color;
	CParticle *m_pNext;
	
	CParticle()
	{
		;
	}
	
	~CParticle()
	{
		;
	}

	static float      ms_afRandTable[RAND_TABLE_SIZE];
	static CParticle *m_pUnusedListHead;
	
	static float      m_SinTable[SIN_COS_TABLE_SIZE];
	static float      m_CosTable[SIN_COS_TABLE_SIZE];
	
	static float Sin(int32 value) { return m_SinTable[value]; }
	static float Cos(int32 value) { return m_CosTable[value]; }
	
	static void ReloadConfig();
	static void Initialise();
	static void Shutdown();
	
	static void AddParticlesAlongLine(tParticleType type, CVector const &vecStart, CVector const &vecEnd, CVector const &vecDir, float fPower, CEntity *pEntity = nil, float fSize = 0.0f,                     int32 nRotationSpeed = 0, int32 nRotation = 0, int32 nCurFrame = 0, int32 nLifeSpan = 0);	
	static void AddParticlesAlongLine(tParticleType type, CVector const &vecStart, CVector const &vecEnd, CVector const &vecDir, float fPower, CEntity *pEntity,       float fSize, RwRGBA const&color,        int32 nRotationSpeed = 0, int32 nRotation = 0, int32 nCurFrame = 0, int32 nLifeSpan = 0);

	static CParticle *AddParticle(tParticleType type, CVector const &vecPos, CVector const &vecDir, CEntity *pEntity = nil, float fSize = 0.0f,               int32 nRotationSpeed = 0, int32 nRotation = 0, int32 nCurFrame = 0, int32 nLifeSpan = 0);
	static CParticle *AddParticle(tParticleType type, CVector const &vecPos, CVector const &vecDir, CEntity *pEntity,       float fSize, RwRGBA const &color, int32 nRotationSpeed = 0, int32 nRotation = 0, int32 nCurFrame = 0, int32 nLifeSpan = 0);

	static void Update();
	static void Render();

	static void RemovePSystem(tParticleType type);
	static void RemoveParticle(CParticle *pParticle, CParticle *pPrevParticle, tParticleSystemData *pPSystemData);
	
	static void _Next(CParticle *&pParticle, CParticle *&pPrevParticle, tParticleSystemData *pPSystemData, bool bRemoveParticle)
	{
		if ( bRemoveParticle )
		{
			RemoveParticle(pParticle, pPrevParticle, pPSystemData);
					
			if ( pPrevParticle )
				pParticle = pPrevParticle->m_pNext;
			else
				pParticle = pPSystemData->m_pParticles;
		}
		else
		{
			pPrevParticle = pParticle;
			pParticle = pParticle->m_pNext;
		}
	}

	static void AddJetExplosion(CVector const &vecPos, float fPower, float fSize);
	static void AddYardieDoorSmoke(CVector const &vecPos, CMatrix const &matMatrix);
	static void CalWindDir(CVector *vecDirIn, CVector *vecDirOut);
	
	static void HandleShipsAtHorizonStuff();
	static void HandleShootableBirdsStuff(CEntity *entity, CVector const&camPos);
};

extern bool clearWaterDrop;
extern int32 numWaterDropOnScreen;
extern RwRaster *gpCarSplashRaster[];
extern RwRaster *gpHeatHazeRaster;
extern RwRaster *gpDotRaster;
extern RwRaster *gpRainDripRaster[];
extern RwRaster *gpRainDripDarkRaster[];

VALIDATE_SIZE(CParticle, 0x58);

#ifdef FIX_HIGH_FPS_BUGS
// Effects that add particles every frame were made for 30 fps and got denser (and used up the particle pool)
// at higher frame rates. Put this at the start of a scope that emits every frame: while it's alive particles
// are only added once per 30 fps frame of game time. Never put it around one-shot effects (hits, explosions).
class CContinuousParticleEmitter
{
public:
	static int32 ms_nActive;
	CContinuousParticleEmitter(void) { ms_nActive++; }
	~CContinuousParticleEmitter(void) { ms_nActive--; }
	static bool IsEmissionFrame(void);
};
// Put this at the start of one-shot effects that can be triggered from a continuous emitter (explosions)
class COneShotParticleEmitter
{
	int32 m_nSavedActive;
public:
	COneShotParticleEmitter(void) { m_nSavedActive = CContinuousParticleEmitter::ms_nActive; CContinuousParticleEmitter::ms_nActive = 0; }
	~COneShotParticleEmitter(void) { CContinuousParticleEmitter::ms_nActive = m_nSavedActive; }
};
#define CONTINUOUS_PARTICLE_EMITTER CContinuousParticleEmitter continuousParticleEmitter
#define ONE_SHOT_PARTICLE_EMITTER COneShotParticleEmitter oneShotParticleEmitter
#else
#define CONTINUOUS_PARTICLE_EMITTER
#define ONE_SHOT_PARTICLE_EMITTER
#endif

#endif // __GTA_PARTICLE_H__
