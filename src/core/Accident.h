#pragma once
#include "config.h"

class CPed;

class CAccident
{
public:
	CPed *m_pVictim;
	uint32 m_nMedicsAttending;
	uint32 m_nMedicsPerformingCPR;
	CAccident() : m_pVictim(nullptr), m_nMedicsAttending(0), m_nMedicsPerformingCPR(0) {}
};

class CAccidentManager
{
	CAccident m_aAccidents[NUM_ACCIDENTS];
	enum {
		MAX_MEDICS_TO_ATTEND_ACCIDENT = 2
	};
public:
	CAccident *GetNextFreeAccident();
	void ReportAccident(CPed *ped);
	void Update();
	CAccident *FindNearestAccident(const CVector &vecPos, float *pDistance);
	[[nodiscard]] uint16 CountActiveAccidents() const;
	[[nodiscard]] bool UnattendedAccidents() const;
	[[nodiscard]] bool WorkToDoForMedics() const;
};

extern CAccidentManager gAccidentManager;