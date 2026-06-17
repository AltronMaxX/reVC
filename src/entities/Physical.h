#pragma once

#include "Lists.h"
#include "Entity.h"
#include "Timer.h"

enum {
	PHYSICAL_MAX_COLLISIONRECORDS = 6
};

#define GRAVITY (0.008f)

class CTreadable;

class CPhysical : public CEntity
{
public:
	// The not properly indented fields haven't been checked properly yet

	int32 m_audioEntityId;
	float m_phys_unused1;
	uint32 m_nLastTimeCollided;
	CVector m_vecMoveSpeed;		// velocity
	CVector m_vecTurnSpeed;		// angular velocity
	CVector m_vecMoveFriction;
	CVector m_vecTurnFriction;
	CVector m_vecMoveSpeedAvg;
	CVector m_vecTurnSpeedAvg;
	float m_fMass;
	float m_fTurnMass;	// moment of inertia
	float m_fForceMultiplier;
	float m_fAirResistance;
	float m_fElasticity;
	float m_fBuoyancy;
	CVector m_vecCentreOfMass;
	CEntryInfoList m_entryInfoList;
	CPtrNode *m_movingListNode;

	int8 m_phys_unused2;
	uint8 m_nStaticFrames;
	uint8 m_nCollisionRecords;
	CEntity *m_aCollisionRecords[PHYSICAL_MAX_COLLISIONRECORDS];

	float m_fDistanceTravelled;

	// damaged piece
	float m_fDamageImpulse;
	CEntity *m_pDamageEntity;
	CVector m_vecDamageNormal;
	int16 m_nDamagePieceType;

	uint8 bIsHeavy : 1;
	uint8 bAffectedByGravity : 1;
	uint8 bInfiniteMass : 1;
	uint8 m_phy_flagA08 : 1;
	uint8 bIsInWater : 1;
	uint8 m_phy_flagA20 : 1; // unused
	uint8 bHitByTrain : 1;
	uint8 bSkipLineCol : 1;

	uint8 bIsFrozen : 1;
	uint8 bDontLoadCollision : 1;
	uint8 m_bIsVehicleBeingShifted : 1;	// wrong name - also used on but never set for peds
	uint8 bJustCheckCollision : 1;	// just see if there is a collision

	uint8 m_nSurfaceTouched;
	int8 m_nZoneLevel;

	CPhysical();
	~CPhysical() override;

	// from CEntity
	void Add() override;
	void Remove() override;
	CRect GetBoundRect() override;
	void ProcessControl() override;
	void ProcessShift() override;
	void ProcessCollision() override;

	virtual int32 ProcessEntityCollision(CEntity *ent, CColPoint *colpoints);

	void RemoveAndAdd();
	void AddToMovingList();
	void RemoveFromMovingList();
	void SetDamagedPieceRecord(uint16 piece, float impulse, CEntity *entity, const CVector &dir);
	void AddCollisionRecord(CEntity *ent);
	void AddCollisionRecord_Treadable(CEntity *ent);
	bool GetHasCollidedWith(const CEntity *ent) const;
	void RemoveRefsToEntity(const CEntity *ent);
	static void PlacePhysicalRelativeToOtherPhysical(CPhysical *other, CPhysical *phys, const CVector &localPos);

	// get speed of point p relative to entity center
	[[nodiscard]] CVector GetSpeed(const CVector &r) const;
	[[nodiscard]] CVector GetSpeed() const { return GetSpeed(CVector(0.0f, 0.0f, 0.0f)); }
	[[nodiscard]] float GetMass(const CVector &pos, const CVector &dir) const {
		return 1.0f / (CrossProduct(pos, dir).MagnitudeSqr()/m_fTurnMass +
		               1.0f/m_fMass);
	}
	[[nodiscard]] float GetMassTweak(const CVector &pos, const CVector &dir, float t) const {
		return 1.0f / (CrossProduct(pos, dir).MagnitudeSqr()/(m_fTurnMass*t) +
		               1.0f/(m_fMass*t));
	}
	void UnsetIsInSafePosition() {
		m_vecMoveSpeed *= -1.0f;
		m_vecTurnSpeed *= -1.0f;
		ApplyTurnSpeed();
		ApplyMoveSpeed();
		m_vecMoveSpeed *= -1.0f;
		m_vecTurnSpeed *= -1.0f;
		bIsInSafePosition = false;	
	}

	[[nodiscard]] const CVector &GetMoveSpeed() const { return m_vecMoveSpeed; }
	void SetMoveSpeed(const float x, const float y, const float z) {
		m_vecMoveSpeed.x = x;
		m_vecMoveSpeed.y = y;
		m_vecMoveSpeed.z = z;
	}
	void SetMoveSpeed(const CVector& speed) {
		m_vecMoveSpeed = speed;
	}
	void AddToMoveSpeed(const float x, const float y, const float z) {
		m_vecMoveSpeed.x += x;
		m_vecMoveSpeed.y += y;
		m_vecMoveSpeed.z += z;
	}
	void AddToMoveSpeed(const CVector& addition) {
		m_vecMoveSpeed += addition;
	}
	void AddToMoveSpeed(const CVector2D& addition) {
		m_vecMoveSpeed += CVector(addition.x, addition.y, 0.0f);
	}
	[[nodiscard]] const CVector &GetTurnSpeed() const { return m_vecTurnSpeed; }
	void SetTurnSpeed(const float x, const float y, const float z) {
		m_vecTurnSpeed.x = x;
		m_vecTurnSpeed.y = y;
		m_vecTurnSpeed.z = z;
	}
	[[nodiscard]] const CVector &GetCenterOfMass() const { return m_vecCentreOfMass; }
	void SetCenterOfMass(const float x, const float y, const float z) {
		m_vecCentreOfMass.x = x;
		m_vecCentreOfMass.y = y;
		m_vecCentreOfMass.z = z;
	}

	void ApplyMoveSpeed();
	void ApplyTurnSpeed();
	// Force actually means Impulse here
	void ApplyMoveForce(float jx, float jy, float jz);
	void ApplyMoveForce(const CVector &j) { ApplyMoveForce(j.x, j.y, j.z); }
	// j(x,y,z) is direction of force, p(x,y,z) is point relative to model center where force is applied
	void ApplyTurnForce(float jx, float jy, float jz, float px, float py, float pz);
	// j is direction of force, p is point relative to model center where force is applied
	void ApplyTurnForce(const CVector &j, const CVector &p) { ApplyTurnForce(j.x, j.y, j.z, p.x, p.y, p.z); }
	void ApplyFrictionMoveForce(float jx, float jy, float jz);
	void ApplyFrictionMoveForce(const CVector &j) { ApplyFrictionMoveForce(j.x, j.y, j.z); }
	void ApplyFrictionTurnForce(float jx, float jy, float jz, float rx, float ry, float rz);
	void ApplyFrictionTurnForce(const CVector &j, const CVector &p) { ApplyFrictionTurnForce(j.x, j.y, j.z, p.x, p.y, p.z); }
	// springRatio: 1.0 fully extended, 0.0 fully compressed
	bool ApplySpringCollision(float springConst, const CVector &springDir, const CVector &point, float springRatio, float bias);
	bool ApplySpringCollisionAlt(float springConst, const CVector &springDir, const CVector &point, float springRatio, float bias, CVector &forceDir);
	bool ApplySpringDampening(float damping, const CVector &springDir, const CVector &point, const CVector &speed);
	void ApplyGravity();
	void ApplyFriction();
	void ApplyAirResistance();
	bool ApplyCollision(CPhysical *B, CColPoint &colpoint, float &impulseA, float &impulseB);
	bool ApplyCollision(const CColPoint &colpoint, float &impulse);
	bool ApplyCollisionAlt(CEntity *B, const CColPoint &colpoint, float &impulse, CVector &moveSpeed, CVector &turnSpeed);
	bool ApplyFriction(CPhysical *B, float adhesiveLimit, CColPoint &colpoint);
	bool ApplyFriction(float adhesiveLimit, const CColPoint &colpoint);

	bool ProcessShiftSectorList(CPtrList *ptrlists);
	bool ProcessCollisionSectorList_SimpleCar(CPtrList *lists);
	bool ProcessCollisionSectorList(CPtrList *lists);
	bool CheckCollision();
	bool CheckCollision_SimpleCar();
};
