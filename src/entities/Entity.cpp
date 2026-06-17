#include "common.h"

#include "General.h"
#include "RwHelper.h"
#include "ModelIndices.h"
#include "Timer.h"
#include "Entity.h"
#include "Object.h"
#include "World.h"
#include "Camera.h"
#include "Glass.h"
#include "Weather.h"
#include "Timecycle.h"
#include "TrafficLights.h"
#include "Coronas.h"
#include "PointLights.h"
#include "Shadows.h"
#include "Pickups.h"
#include "SpecialFX.h"
#include "Zones.h"
#include "MemoryHeap.h"
#include "Bones.h"
#include "Debug.h"
#include "Ped.h"
#include "Dummy.h"
#include "WindModifiers.h"

int gBuildings;

CEntity::CEntity()
{
	m_type = ENTITY_TYPE_NOTHING;
	m_status = STATUS_ABANDONED;

	bUsesCollision = false;
	bCollisionProcessed = false;
	bIsStatic = false;
	bHasContacted = false;
	bPedPhysics = false;
	bIsStuck = false;
	bIsInSafePosition = false;
	bUseCollisionRecords = false;

	bWasPostponed = false;
	bExplosionProof = false;
	bIsVisible = true;
	bHasCollided = false;
	bRenderScorched = false;
	bHasBlip = false;
	bIsBIGBuilding = false;
	bStreamBIGBuilding = false;

	bRenderDamaged = false;
	bBulletProof = false;
	bFireProof = false;
	bCollisionProof = false;
	bMeleeProof = false;
	bOnlyDamagedByPlayer = false;
	bStreamingDontDelete = false;
	bRemoveFromWorld = false;

	bHasHitWall = false;
	bImBeingRendered = false;
	bTouchingWater = false;
	bIsSubway = false;
	bDrawLast = false;
	bNoBrightHeadLights = false;
	bDoNotRender = false;
	bDistanceFade = false;

	m_flagE1 = false;
	m_flagE2 = false;
	bOffscreen = false;
	bIsStaticWaitingForCollision = false;
	bDontStream = false;
	bUnderwater = false;
	bHasPreRenderEffects = false;

	m_scanCode = 0;
	m_modelIndex = -1;
	m_rwObject = nullptr;
	m_area = AREA_MAIN_MAP;
	m_randomSeed = CGeneral::GetRandomNumber();
	m_pFirstReference = nullptr;
}

CEntity::~CEntity()
{
	CEntity::DeleteRwObject();
	ResolveReferences();
}

void
CEntity::SetModelIndex(const uint32 id)
{
	m_modelIndex = id;
	bHasPreRenderEffects = HasPreRenderEffects();
	CreateRwObject();
}

void
CEntity::SetModelIndexNoCreate(const uint32 id)
{
	m_modelIndex = id;
	bHasPreRenderEffects = HasPreRenderEffects();
}

void
CEntity::CreateRwObject()
{
	CBaseModelInfo *mi = CModelInfo::GetModelInfo(m_modelIndex);

	PUSH_MEMID(MEMID_WORLD);
	m_rwObject = mi->CreateInstance();
	POP_MEMID();

	if(m_rwObject){
		if(IsBuilding())
			gBuildings++;
		if(RwObjectGetType(m_rwObject) == rpATOMIC)
			m_matrix.AttachRW(RwFrameGetMatrix(RpAtomicGetFrame(reinterpret_cast<RpAtomic *>(m_rwObject))), false);
		else if(RwObjectGetType(m_rwObject) == rpCLUMP)
			m_matrix.AttachRW(RwFrameGetMatrix(RpClumpGetFrame(reinterpret_cast<RpClump *>(m_rwObject))), false);

		mi->AddRef();
	}
}

void
CEntity::AttachToRwObject(RwObject *obj)
{
	m_rwObject = obj;
	if(m_rwObject){
		if(RwObjectGetType(m_rwObject) == rpATOMIC)
			m_matrix.Attach(RwFrameGetMatrix(RpAtomicGetFrame(reinterpret_cast<RpAtomic *>(m_rwObject))), false);
		else if(RwObjectGetType(m_rwObject) == rpCLUMP)
			m_matrix.Attach(RwFrameGetMatrix(RpClumpGetFrame(reinterpret_cast<RpClump *>(m_rwObject))), false);

		CModelInfo::GetModelInfo(m_modelIndex)->AddRef();
	}
}

void
CEntity::DetachFromRwObject()
{
	if(m_rwObject)
		CModelInfo::GetModelInfo(m_modelIndex)->RemoveRef();
	m_rwObject = nullptr;
	m_matrix.Detach();
}

RpAtomic*
AtomicRemoveAnimFromSkinCB(RpAtomic *atomic, void *data)
{
	if(RpSkinGeometryGetSkin(RpAtomicGetGeometry(atomic))){
		RpHAnimHierarchy *hier = RpSkinAtomicGetHAnimHierarchy(atomic);
#ifdef LIBRW
		if(hier && hier->interpolator->currentAnim){
			RpHAnimAnimationDestroy(hier->interpolator->currentAnim);
			hier->interpolator->currentAnim = nullptr;
		}
#else
		if(hier && hier->currentAnim){
			RpHAnimAnimationDestroy(hier->currentAnim->pCurrentAnim);
			hier->currentAnim = nil;
		}
#endif
	}
	return atomic;
}

void
CEntity::DeleteRwObject()
{
	m_matrix.Detach();
	if(m_rwObject){
		if(RwObjectGetType(m_rwObject) == rpATOMIC){
			RwFrame *f = RpAtomicGetFrame(reinterpret_cast<RpAtomic *>(m_rwObject));
			RpAtomicDestroy(reinterpret_cast<RpAtomic *>(m_rwObject));
			RwFrameDestroy(f);
		}else if(RwObjectGetType(m_rwObject) == rpCLUMP){
			if(IsClumpSkinned(reinterpret_cast<RpClump *>(m_rwObject)))
				RpClumpForAllAtomics(reinterpret_cast<RpClump *>(m_rwObject), AtomicRemoveAnimFromSkinCB, nullptr);
			RpClumpDestroy(reinterpret_cast<RpClump *>(m_rwObject));
		}
		m_rwObject = nullptr;
		CModelInfo::GetModelInfo(m_modelIndex)->RemoveRef();
		if(IsBuilding())
			gBuildings--;
	}
}

CRect
CEntity::GetBoundRect()
{
	CRect rect;
	const CColModel *col = CModelInfo::GetModelInfo(m_modelIndex)->GetColModel();

	rect.ContainPoint(m_matrix * col->boundingBox.min);
	rect.ContainPoint(m_matrix * col->boundingBox.max);

	CVector v = col->boundingBox.min;
	v.x = col->boundingBox.max.x;
	rect.ContainPoint(m_matrix * v);

	v = col->boundingBox.max;
	v.x = col->boundingBox.min.x;
	rect.ContainPoint(m_matrix * v);

	return rect;
}

CVector
CEntity::GetBoundCentre()
{
	CVector v;
	GetBoundCentre(v);
	return v;
}

void
CEntity::GetBoundCentre(CVector &out) const {
	out = m_matrix * CModelInfo::GetModelInfo(m_modelIndex)->GetColModel()->boundingSphere.center;
}

float
CEntity::GetBoundRadius() const {
	return CModelInfo::GetModelInfo(m_modelIndex)->GetColModel()->boundingSphere.radius;
}

void
CEntity::UpdateRwFrame() const {
	if(m_rwObject)
		RwFrameUpdateObjects(rwObjectGetParent(m_rwObject));
}

void
CEntity::UpdateRpHAnim()
{
	if(IsClumpSkinned(GetClump())){
		RpHAnimHierarchy *hier = GetAnimHierarchyFromSkinClump(GetClump());
		RpHAnimHierarchyUpdateMatrices(hier);
	}
}

bool
CEntity::HasPreRenderEffects()
{
	return IsTreeModel(GetModelIndex()) ||
	   GetModelIndex() == MI_COLLECTABLE1 ||
	   GetModelIndex() == MI_MONEY ||
	   GetModelIndex() == MI_CARMINE ||
	   GetModelIndex() == MI_NAUTICALMINE ||
	   GetModelIndex() == MI_BRIEFCASE ||
	   GetModelIndex() == MI_GRENADE ||
	   GetModelIndex() == MI_MOLOTOV ||
	   GetModelIndex() == MI_MISSILE ||
	   GetModelIndex() == MI_BEACHBALL ||
	   IsGlass(GetModelIndex()) ||
	   IsObject() && dynamic_cast<CObject *>(this)->bIsPickup ||
	   IsLightWithPreRenderEffects(GetModelIndex());
}

void
CEntity::PreRender()
{
	if (CModelInfo::GetModelInfo(GetModelIndex())->GetNum2dEffects() != 0)
		ProcessLightsForEntity();

	if(!bHasPreRenderEffects)
		return;

	switch(m_type){
	case ENTITY_TYPE_BUILDING:
		if(IsTreeModel(GetModelIndex())){
			const float dist = (TheCamera.GetPosition() - GetPosition()).Magnitude2D();
			CObject::fDistToNearestTree = Min(CObject::fDistToNearestTree, dist);
			ModifyMatrixForTreeInWind();
		}
		break;
	case ENTITY_TYPE_OBJECT:
		if(GetModelIndex() == MI_COLLECTABLE1){
			CPickups::DoCollectableEffects(this);
			GetMatrix().UpdateRW();
			UpdateRwFrame();
		}else if(GetModelIndex() == MI_MONEY){
			CPickups::DoMoneyEffects(this);
			GetMatrix().UpdateRW();
			UpdateRwFrame();
		}else if(GetModelIndex() == MI_NAUTICALMINE ||
		         GetModelIndex() == MI_CARMINE ||
		         GetModelIndex() == MI_BRIEFCASE){
			if(dynamic_cast<CObject *>(this)->bIsPickup){
				CPickups::DoMineEffects(this);
				GetMatrix().UpdateRW();
				UpdateRwFrame();
			}
		}else if(GetModelIndex() == MI_MISSILE){
			CVector pos = GetPosition();
			const float flicker = (CGeneral::GetRandomNumber() & 0xF)/static_cast<float>(0x10);
			CShadows::StoreShadowToBeRendered(SHADOWTYPE_ADDITIVE,
				gpShadowExplosionTex, &pos,
				8.0f, 0.0f, 0.0f, -8.0f,
				255, 200.0f*flicker, 160.0f*flicker, 120.0f*flicker,
				20.0f, false, 1.0f, nullptr, false);
			CPointLights::AddLight(CPointLights::LIGHT_POINT,
				pos, CVector(0.0f, 0.0f, 0.0f),
				8.0f,
				1.0f*flicker,
				0.8f*flicker,
				0.6f*flicker,
				CPointLights::FOG_NONE, true);
			CCoronas::RegisterCorona(reinterpret_cast<uintptr>(this),
				255.0f*flicker, 220.0f*flicker, 190.0f*flicker, 255,
				pos, 6.0f*flicker, 80.0f, gpCoronaTexture[CCoronas::TYPE_STAR],
				CCoronas::FLARE_NONE, CCoronas::REFLECTION_ON,
				CCoronas::LOSCHECK_OFF, CCoronas::STREAK_OFF, 0.0f);
		}else if(IsGlass(GetModelIndex())){
			PreRenderForGlassWindow();
		}else if (dynamic_cast<CObject *>(this)->bIsPickup) {
			CPickups::DoPickUpEffects(this);
			GetMatrix().UpdateRW();
			UpdateRwFrame();
		} else if (GetModelIndex() == MI_GRENADE) {
			CMotionBlurStreaks::RegisterStreak(reinterpret_cast<uintptr>(this),
				100, 100, 100,
				GetPosition() - 0.07f * TheCamera.GetRight(),
				GetPosition() + 0.07f * TheCamera.GetRight());
		} else if (GetModelIndex() == MI_MOLOTOV) {
			CMotionBlurStreaks::RegisterStreak(reinterpret_cast<uintptr>(this),
				0, 100, 0,
				GetPosition() - 0.07f * TheCamera.GetRight(),
				GetPosition() + 0.07f * TheCamera.GetRight());
		}else if(GetModelIndex() == MI_BEACHBALL){
			CVector pos = GetPosition();
			CShadows::StoreShadowToBeRendered(SHADOWTYPE_DARK,
				gpShadowPedTex, &pos,
				0.4f, 0.0f, 0.0f, -0.4f,
				CTimeCycle::GetShadowStrength(),
				CTimeCycle::GetShadowStrength(),
				CTimeCycle::GetShadowStrength(),
				CTimeCycle::GetShadowStrength(),
				20.0f, false, 1.0f, nullptr, false);
		}
		// fall through
	case ENTITY_TYPE_DUMMY:
		if(GetModelIndex() == MI_TRAFFICLIGHTS){
			CTrafficLights::DisplayActualLight(this);
			CShadows::StoreShadowForPole(this, 2.957f, 0.147f, 0.0f, 16.0f, 0.4f, 0);
		}else if(GetModelIndex() == MI_TRAFFICLIGHTS_VERTICAL){
			CTrafficLights::DisplayActualLight(this);
		}else if(GetModelIndex() == MI_TRAFFICLIGHTS_MIAMI){
			CTrafficLights::DisplayActualLight(this);
			CShadows::StoreShadowForPole(this, 4.819f, 1.315f, 0.0f, 16.0f, 0.4f, 0);
		}else if(GetModelIndex() == MI_TRAFFICLIGHTS_TWOVERTICAL){
			CTrafficLights::DisplayActualLight(this);
			CShadows::StoreShadowForPole(this, 7.503f, 0.0f, 0.0f, 16.0f, 0.4f, 0);
		}else if(GetModelIndex() == MI_SINGLESTREETLIGHTS1)
			CShadows::StoreShadowForPole(this, 0.744f, 0.0f, 0.0f, 16.0f, 0.4f, 0);
		else if(GetModelIndex() == MI_SINGLESTREETLIGHTS2)
			CShadows::StoreShadowForPole(this, 0.043f, 0.0f, 0.0f, 16.0f, 0.4f, 0);
		else if(GetModelIndex() == MI_SINGLESTREETLIGHTS3)
			CShadows::StoreShadowForPole(this, 1.143f, 0.145f, 0.0f, 16.0f, 0.4f, 0);
		else if(GetModelIndex() == MI_DOUBLESTREETLIGHTS)
			CShadows::StoreShadowForPole(this, 0.0f, -0.048f, 0.0f, 16.0f, 0.4f, 0);
		break;
	}
}

void
CEntity::Render()
{
	if(m_rwObject){
		bImBeingRendered = true;
		if(RwObjectGetType(m_rwObject) == rpATOMIC)
			RpAtomicRender(reinterpret_cast<RpAtomic *>(m_rwObject));
		else
			RpClumpRender(reinterpret_cast<RpClump *>(m_rwObject));
		bImBeingRendered = false;
	}
}

bool
CEntity::GetIsTouching(CVector const &center, const float radius)
{
	return sq(GetBoundRadius()+radius) > (GetBoundCentre()-center).MagnitudeSqr();
}

bool
CEntity::IsVisible()
{
	return m_rwObject && bIsVisible && GetIsOnScreen();
}

bool
CEntity::IsVisibleComplex()
{
	return m_rwObject && bIsVisible && GetIsOnScreenComplex();
}

bool
CEntity::GetIsOnScreen()
{
	return TheCamera.IsSphereVisible(GetBoundCentre(), GetBoundRadius(),
		&TheCamera.GetCameraMatrix());
}

bool
CEntity::GetIsOnScreenComplex()
{
	CVector boundBox[8];

	if(TheCamera.IsPointVisible(GetBoundCentre(), &TheCamera.GetCameraMatrix()))
		return true;

	const CRect rect = GetBoundRect();
	const CColModel *colmodel = CModelInfo::GetModelInfo(m_modelIndex)->GetColModel();
	const float z = GetPosition().z;
	const float minz = z + colmodel->boundingBox.min.z;
	const float maxz = z + colmodel->boundingBox.max.z;
	boundBox[0].x = rect.left;
	boundBox[0].y = rect.bottom;
	boundBox[0].z = minz;
	boundBox[1].x = rect.left;
	boundBox[1].y = rect.top;
	boundBox[1].z = minz;
	boundBox[2].x = rect.right;
	boundBox[2].y = rect.bottom;
	boundBox[2].z = minz;
	boundBox[3].x = rect.right;
	boundBox[3].y = rect.top;
	boundBox[3].z = minz;
	boundBox[4].x = rect.left;
	boundBox[4].y = rect.bottom;
	boundBox[4].z = maxz;
	boundBox[5].x = rect.left;
	boundBox[5].y = rect.top;
	boundBox[5].z = maxz;
	boundBox[6].x = rect.right;
	boundBox[6].y = rect.bottom;
	boundBox[6].z = maxz;
	boundBox[7].x = rect.right;
	boundBox[7].y = rect.top;
	boundBox[7].z = maxz;

	return TheCamera.IsBoxVisible(boundBox, &TheCamera.GetCameraMatrix());
}

void
CEntity::Add()
{
	CPtrList *list;

	const CRect bounds = GetBoundRect();
	const int xstart = CWorld::GetSectorIndexX(bounds.left);
	const int xend = CWorld::GetSectorIndexX(bounds.right);
	const int xmid = CWorld::GetSectorIndexX((bounds.left + bounds.right) / 2.0f);
	const int ystart = CWorld::GetSectorIndexY(bounds.top);
	const int yend = CWorld::GetSectorIndexY(bounds.bottom);
	const int ymid = CWorld::GetSectorIndexY((bounds.top + bounds.bottom) / 2.0f);
	assert(xstart >= 0);
	assert(xend < NUMSECTORS_X);
	assert(ystart >= 0);
	assert(yend < NUMSECTORS_Y);

	for(int y = ystart; y <= yend; y++)
		for(int x = xstart; x <= xend; x++){
			CSector *s = CWorld::GetSector(x, y);
			if(x == xmid && y == ymid) switch(m_type){
			case ENTITY_TYPE_BUILDING:
				list = &s->m_lists[ENTITYLIST_BUILDINGS];
				break;
			case ENTITY_TYPE_VEHICLE:
				list = &s->m_lists[ENTITYLIST_VEHICLES];
				break;
			case ENTITY_TYPE_PED:
				list = &s->m_lists[ENTITYLIST_PEDS];
				break;
			case ENTITY_TYPE_OBJECT:
				list = &s->m_lists[ENTITYLIST_OBJECTS];
				break;
			case ENTITY_TYPE_DUMMY:
				list = &s->m_lists[ENTITYLIST_DUMMIES];
				break;
			}else switch(m_type){
			case ENTITY_TYPE_BUILDING:
				list = &s->m_lists[ENTITYLIST_BUILDINGS_OVERLAP];
				break;
			case ENTITY_TYPE_VEHICLE:
				list = &s->m_lists[ENTITYLIST_VEHICLES_OVERLAP];
				break;
			case ENTITY_TYPE_PED:
				list = &s->m_lists[ENTITYLIST_PEDS_OVERLAP];
				break;
			case ENTITY_TYPE_OBJECT:
				list = &s->m_lists[ENTITYLIST_OBJECTS_OVERLAP];
				break;
			case ENTITY_TYPE_DUMMY:
				list = &s->m_lists[ENTITYLIST_DUMMIES_OVERLAP];
				break;
			}
			list->InsertItem(this);
		}
}

void
CEntity::Remove()
{
	CPtrList *list;

	const CRect bounds = GetBoundRect();
	const int xstart = CWorld::GetSectorIndexX(bounds.left);
	const int xend = CWorld::GetSectorIndexX(bounds.right);
	const int xmid = CWorld::GetSectorIndexX((bounds.left + bounds.right) / 2.0f);
	const int ystart = CWorld::GetSectorIndexY(bounds.top);
	const int yend = CWorld::GetSectorIndexY(bounds.bottom);
	const int ymid = CWorld::GetSectorIndexY((bounds.top + bounds.bottom) / 2.0f);
	assert(xstart >= 0);
	assert(xend < NUMSECTORS_X);
	assert(ystart >= 0);
	assert(yend < NUMSECTORS_Y);

	for(int y = ystart; y <= yend; y++)
		for(int x = xstart; x <= xend; x++){
			CSector *s = CWorld::GetSector(x, y);
			if(x == xmid && y == ymid) switch(m_type){
			case ENTITY_TYPE_BUILDING:
				list = &s->m_lists[ENTITYLIST_BUILDINGS];
				break;
			case ENTITY_TYPE_VEHICLE:
				list = &s->m_lists[ENTITYLIST_VEHICLES];
				break;
			case ENTITY_TYPE_PED:
				list = &s->m_lists[ENTITYLIST_PEDS];
				break;
			case ENTITY_TYPE_OBJECT:
				list = &s->m_lists[ENTITYLIST_OBJECTS];
				break;
			case ENTITY_TYPE_DUMMY:
				list = &s->m_lists[ENTITYLIST_DUMMIES];
				break;
			}else switch(m_type){
			case ENTITY_TYPE_BUILDING:
				list = &s->m_lists[ENTITYLIST_BUILDINGS_OVERLAP];
				break;
			case ENTITY_TYPE_VEHICLE:
				list = &s->m_lists[ENTITYLIST_VEHICLES_OVERLAP];
				break;
			case ENTITY_TYPE_PED:
				list = &s->m_lists[ENTITYLIST_PEDS_OVERLAP];
				break;
			case ENTITY_TYPE_OBJECT:
				list = &s->m_lists[ENTITYLIST_OBJECTS_OVERLAP];
				break;
			case ENTITY_TYPE_DUMMY:
				list = &s->m_lists[ENTITYLIST_DUMMIES_OVERLAP];
				break;
			}
			list->RemoveItem(this);
		}
}

float
CEntity::GetDistanceFromCentreOfMassToBaseOfModel() const {
	return -CModelInfo::GetModelInfo(m_modelIndex)->GetColModel()->boundingBox.min.z;
}

void
CEntity::SetupBigBuilding()
{
	auto *mi = dynamic_cast<CSimpleModelInfo *>(CModelInfo::GetModelInfo(m_modelIndex));
	bIsBIGBuilding = true;
	bStreamingDontDelete = true;
	bUsesCollision = false;
	m_level = CTheZones::GetLevelFromPosition(&GetPosition());
	if(mi->m_lodDistances[0] <= 2000.0f)
		bStreamBIGBuilding = true;
	if(mi->m_lodDistances[0] > 2500.0f || mi->m_ignoreDrawDist)
		m_level = LEVEL_GENERIC;
	else if(m_level == LEVEL_GENERIC)
		printf("%s isn't in a level\n", mi->GetModelName());
}

float WindTabel[] = {
	1.0f, 0.5f, 0.2f, 0.7f, 0.4f, 1.0f, 0.5f, 0.3f,
	0.2f, 0.1f, 0.7f, 0.6f, 0.3f, 1.0f, 0.5f, 0.2f,
};

void
CEntity::ModifyMatrixForTreeInWind()
{
	uint16 t;
	float f;
	float strength, flutter;

	if(CTimer::GetIsPaused())
		return;

	CMatrix mat(GetMatrix().m_attachment);

	if(CWeather::Wind >= 0.5){
		t = m_randomSeed + 8*CTimer::GetTimeInMilliseconds();
		f = (t & 0xFFF)/static_cast<float>(0x1000);
		flutter = f * WindTabel[(t>>12)+1 & 0xF] +
			(1.0f - f) * WindTabel[(t>>12) & 0xF] +
			1.0f;
		strength = -0.015f*CWeather::Wind;
	}else if(CWeather::Wind >= 0.2){
		t = reinterpret_cast<uintptr>(this) + CTimer::GetTimeInMilliseconds();
		f = (t & 0xFFF)/static_cast<float>(0x1000);
		flutter = Sin(f * 6.28f);
		strength = -0.008f;
	}else{
		t = reinterpret_cast<uintptr>(this) + CTimer::GetTimeInMilliseconds();
		f = (t & 0xFFF)/static_cast<float>(0x1000);
		flutter = Sin(f * 6.28f);
		strength = -0.005f;
	}

	mat.GetUp().x = strength * flutter;
	if(IsPalmTreeModel(GetModelIndex()))
		mat.GetUp().x += -0.07f*CWeather::Wind;
	mat.GetUp().y = mat.GetUp().x;

	CWindModifiers::FindWindModifier(GetPosition(), &mat.GetUp().x, &mat.GetUp().y);

	mat.UpdateRW();
	UpdateRwFrame();
}

float BannerWindTabel[] = {
	0.0f, 0.3f, 0.6f, 0.85f, 0.99f, 0.97f, 0.65f, 0.15f,
	-0.1f, 0.0f, 0.35f, 0.57f, 0.55f, 0.35f, 0.45f, 0.67f,
	0.73f, 0.45f, 0.25f, 0.35f, 0.35f, 0.11f, 0.13f, 0.21f,
	0.28f, 0.28f, 0.22f, 0.1f, 0.0f, -0.1f, -0.17f, -0.12f
};

// unused
void
CEntity::ModifyMatrixForBannerInWind()
{
	float strength;

	if(CTimer::GetIsPaused())
		return;

	if(CWeather::Wind < 0.1f)
		strength = 0.2f;
	else if(CWeather::Wind < 0.8f)
		strength = 0.43f;
	else
		strength = 0.66f;

	const uint16 t = (static_cast<int>(GetMatrix().GetPosition().x + GetMatrix().GetPosition().y) << 10) + 16 *
	           CTimer::GetTimeInMilliseconds();
	const float f = (t & 0x7FF) / static_cast<float>(0x800);
	float flutter = f * BannerWindTabel[(t >> 11) + 1 & 0x1F] +
	                (1.0f - f) * BannerWindTabel[(t >> 11) & 0x1F];
	flutter *= strength;

	CVector right = CrossProduct(GetForward(), GetUp());
	right.z = 0.0f;
	right.Normalise();
	CVector up = right * flutter;
	up.z = Sqrt(sq(1.0f) - sq(flutter));
	GetRight() = CrossProduct(GetForward(), up);
	GetUp() = up;

	GetMatrix().UpdateRW();
	UpdateRwFrame();
}

void
CEntity::PreRenderForGlassWindow()
{
	if(static_cast<CSimpleModelInfo *>(CModelInfo::GetModelInfo(m_modelIndex))->m_isArtistGlass)
		return;
	CGlass::AskForObjectToBeRenderedInGlass(this);
	bIsVisible = false;
}

/*
0x487A10 - SetAtomicAlphaCB
0x4879E0 - SetClumpAlphaCB
*/

RpMaterial*
SetAtomicAlphaCB(RpMaterial *material, void *data)
{
	const_cast<RwRGBA *>(RpMaterialGetColor(material))->alpha = static_cast<uint8>(reinterpret_cast<uintptr>(data));
	return material;
}

RpAtomic*
SetClumpAlphaCB(RpAtomic *atomic, void *data)
{
	RpGeometry *geometry = RpAtomicGetGeometry(atomic);
	RpGeometrySetFlags(geometry, RpGeometryGetFlags(geometry) | rpGEOMETRYMODULATEMATERIALCOLOR);
	RpGeometryForAllMaterials(geometry, SetAtomicAlphaCB, (void*)data);
	return atomic;
}

void
CEntity::SetRwObjectAlpha(const int32 alpha) const {
	if (m_rwObject != nullptr) {
		switch (RwObjectGetType(m_rwObject)) {
		case rpATOMIC: {
			RpGeometry *geometry = RpAtomicGetGeometry(reinterpret_cast<RpAtomic *>(m_rwObject));
			RpGeometrySetFlags(geometry, RpGeometryGetFlags(geometry) | rpGEOMETRYMODULATEMATERIALCOLOR);
			RpGeometryForAllMaterials(geometry, SetAtomicAlphaCB, reinterpret_cast<void *>(alpha));
			break;
		}
		case rpCLUMP:
			RpClumpForAllAtomics(reinterpret_cast<RpClump *>(m_rwObject), SetClumpAlphaCB, reinterpret_cast<void *>(alpha));
			break;
		}
	}
}

bool IsEntityPointerValid(CEntity* pEntity)
{
	if (!pEntity)
		return false;
	switch (pEntity->GetType()) {
	case ENTITY_TYPE_NOTHING: return false;
	case ENTITY_TYPE_BUILDING: return IsBuildingPointerValid(dynamic_cast<CBuilding *>(pEntity));
	case ENTITY_TYPE_VEHICLE: return IsVehiclePointerValid(dynamic_cast<CVehicle *>(pEntity));
	case ENTITY_TYPE_PED: return IsPedPointerValid(dynamic_cast<CPed *>(pEntity));
	case ENTITY_TYPE_OBJECT: return IsObjectPointerValid(dynamic_cast<CObject *>(pEntity));
	case ENTITY_TYPE_DUMMY: return IsDummyPointerValid(dynamic_cast<CDummy *>(pEntity));
	}
	return false;
}

#ifdef COMPATIBLE_SAVES
void
CEntity::SaveEntityFlags(uint8*& buf) const {
	uint32 tmp = 0;
	tmp |= m_type & BIT(3) - 1;
	tmp |= (m_status & BIT(5) - 1) << 3;

	if (bUsesCollision) tmp |= BIT(8);
	if (bCollisionProcessed) tmp |= BIT(9);
	if (bIsStatic)  tmp |= BIT(10);
	if (bHasContacted) tmp |= BIT(11);
	if (bPedPhysics) tmp |= BIT(12);
	if (bIsStuck) tmp |= BIT(13);
	if (bIsInSafePosition) tmp |= BIT(14);
	if (bUseCollisionRecords) tmp |= BIT(15);

	if (bWasPostponed) tmp |= BIT(16);
	if (bExplosionProof) tmp |= BIT(17);
	if (bIsVisible)  tmp |= BIT(18);
	if (bHasCollided) tmp |= BIT(19);
	if (bRenderScorched) tmp |= BIT(20);
	if (bHasBlip) tmp |= BIT(21);
	if (bIsBIGBuilding) tmp |= BIT(22);
	if (bStreamBIGBuilding) tmp |= BIT(23);

	if (bRenderDamaged) tmp |= BIT(24);
	if (bBulletProof) tmp |= BIT(25);
	if (bFireProof) tmp |= BIT(26);
	if (bCollisionProof)  tmp |= BIT(27);
	if (bMeleeProof) tmp |= BIT(28);
	if (bOnlyDamagedByPlayer) tmp |= BIT(29);
	if (bStreamingDontDelete) tmp |= BIT(30);
	if (bRemoveFromWorld) tmp |= BIT(31);

	WriteSaveBuf(buf, tmp);

	tmp = 0;

	if (bHasHitWall) tmp |= BIT(0);
	if (bImBeingRendered)  tmp |= BIT(1);
	if (bTouchingWater) tmp |= BIT(2);
	if (bIsSubway) tmp |= BIT(3);
	if (bDrawLast) tmp |= BIT(4);
	if (bNoBrightHeadLights) tmp |= BIT(5);
	if (bDoNotRender) tmp |= BIT(6);
	if (bDistanceFade) tmp |= BIT(7);

	if (m_flagE1) tmp |= BIT(8);
	if (m_flagE2) tmp |= BIT(9);
	if (bOffscreen) tmp |= BIT(10);
	if (bIsStaticWaitingForCollision) tmp |= BIT(11);
	if (bDontStream) tmp |= BIT(12);
	if (bUnderwater) tmp |= BIT(13);
	if (bHasPreRenderEffects) tmp |= BIT(14);

	WriteSaveBuf(buf, tmp);
}

void
CEntity::LoadEntityFlags(uint8*& buf)
{
	uint32 tmp;
	ReadSaveBuf(&tmp, buf);
	m_type = tmp & BIT(3) - 1;
	m_status = tmp >> 3 & BIT(5) - 1;

	bUsesCollision = !!(tmp & BIT(8));
	bCollisionProcessed = !!(tmp & BIT(9));
	bIsStatic = !!(tmp & BIT(10));
	bHasContacted = !!(tmp & BIT(11));
	bPedPhysics = !!(tmp & BIT(12));
	bIsStuck = !!(tmp & BIT(13));
	bIsInSafePosition = !!(tmp & BIT(14));
	bUseCollisionRecords = !!(tmp & BIT(15));

	bWasPostponed = !!(tmp & BIT(16));
	bExplosionProof = !!(tmp & BIT(17));
	bIsVisible = !!(tmp & BIT(18));
	bHasCollided = !!(tmp & BIT(19));
	bRenderScorched = !!(tmp & BIT(20));
	bHasBlip = !!(tmp & BIT(21));
	bIsBIGBuilding = !!(tmp & BIT(22));
	bStreamBIGBuilding = !!(tmp & BIT(23));

	bRenderDamaged = !!(tmp & BIT(24));
	bBulletProof = !!(tmp & BIT(25));
	bFireProof = !!(tmp & BIT(26));
	bCollisionProof = !!(tmp & BIT(27));
	bMeleeProof = !!(tmp & BIT(28));
	bOnlyDamagedByPlayer = !!(tmp & BIT(29));
	bStreamingDontDelete = !!(tmp & BIT(30));
	bRemoveFromWorld = !!(tmp & BIT(31));

	ReadSaveBuf(&tmp, buf);

	bHasHitWall = !!(tmp & BIT(0));
	bImBeingRendered = !!(tmp & BIT(1));
	bTouchingWater = !!(tmp & BIT(2));
	bIsSubway = !!(tmp & BIT(3));
	bDrawLast = !!(tmp & BIT(4));
	bNoBrightHeadLights = !!(tmp & BIT(5));
	bDoNotRender = !!(tmp & BIT(6));
	bDistanceFade = !!(tmp & BIT(7));

	m_flagE1 = !!(tmp & BIT(8));
	m_flagE2 = !!(tmp & BIT(9));
	bOffscreen = !!(tmp & BIT(10));
	bIsStaticWaitingForCollision = !!(tmp & BIT(11));
	bDontStream = !!(tmp & BIT(12));
	bUnderwater = !!(tmp & BIT(13));
	bHasPreRenderEffects = !!(tmp & BIT(14));
}

#endif

