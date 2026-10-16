#ifndef __GTA_TIMER_H__
#define __GTA_TIMER_H__

class CTimer
{

	static uint32 m_snTimeInMilliseconds;
	static uint32 m_snTimeInMillisecondsPauseMode;
	static uint32 m_snTimeInMillisecondsNonClipped;
	static uint32 m_snPreviousTimeInMilliseconds;
	static uint32 m_FrameCounter;
	static float ms_fTimeScale;
	static float ms_fTimeStep;
	static float ms_fTimeStepNonClipped;
#ifdef FIX_BUGS
	static uint32 m_LogicalFrameCounter;
	static uint32 m_LogicalFramesPassed;
#endif
#ifdef FIX_HIGH_FPS_BUGS
	static uint32 m_SimFrameCounter;
	static uint32 m_SimFramesPassed;
#endif
public:
	static bool  m_UserPause;
	static bool  m_CodePause;
	static bool  m_WindowMinimizedPause;

	static const float &GetTimeStep(void) { return ms_fTimeStep; }
	static void SetTimeStep(float ts) { ms_fTimeStep = ts; }
	static float GetTimeStepInSeconds() { return ms_fTimeStep / 50.0f; }
#ifdef FIX_HIGH_FPS_BUGS
	// Truncating the time step to whole milliseconds loses up to 1ms every frame, which made every timer
	// that accumulates this value run slow at high frame rates (~14% at 144 fps).
	// The millisecond clock already carries the fraction over to the next frame, so its delta sums up exactly.
	static uint32 GetTimeStepInMilliseconds() { return m_snTimeInMilliseconds - m_snPreviousTimeInMilliseconds; }
#else
	static uint32 GetTimeStepInMilliseconds() { return ms_fTimeStep / 50.0f * 1000.0f; }
#endif
	static const float &GetTimeStepNonClipped(void) { return ms_fTimeStepNonClipped; }
	static float GetTimeStepNonClippedInSeconds(void) { return ms_fTimeStepNonClipped / 50.0f; }
	static float GetTimeStepNonClippedInMilliseconds(void) { return ms_fTimeStepNonClipped / 50.0f * 1000.0f; }
	static void SetTimeStepNonClipped(float ts) { ms_fTimeStepNonClipped = ts; }
	static const uint32 &GetFrameCounter(void) { return m_FrameCounter; }
	static void SetFrameCounter(uint32 fc) { m_FrameCounter = fc; }
	static const uint32 &GetTimeInMilliseconds(void) { return m_snTimeInMilliseconds; }
	static void SetTimeInMilliseconds(uint32 t) { m_snTimeInMilliseconds = t; }
	static uint32 GetTimeInMillisecondsNonClipped(void) { return m_snTimeInMillisecondsNonClipped; }
	static void SetTimeInMillisecondsNonClipped(uint32 t) { m_snTimeInMillisecondsNonClipped = t; }
	static uint32 GetTimeInMillisecondsPauseMode(void) { return m_snTimeInMillisecondsPauseMode; }
	static void SetTimeInMillisecondsPauseMode(uint32 t) { m_snTimeInMillisecondsPauseMode = t; }
	static uint32 GetPreviousTimeInMilliseconds(void) { return m_snPreviousTimeInMilliseconds; }
	static void SetPreviousTimeInMilliseconds(uint32 t) { m_snPreviousTimeInMilliseconds = t; }
	static const float &GetTimeScale(void) { return ms_fTimeScale; }
	static void SetTimeScale(float ts) { ms_fTimeScale = ts; }
	static uint32 GetCyclesPerFrame();

	static bool GetIsPaused() { return m_UserPause || m_CodePause || m_WindowMinimizedPause; }
	static bool GetIsUserPaused() { return m_UserPause; }
	static bool GetIsCodePaused() { return m_CodePause; }
	static void SetCodePause(bool pause) { m_CodePause = pause; }
	static bool GetWindowMinimizedPause() { return m_WindowMinimizedPause; }
	static void SetWindowMinimizedPause(bool pause);
	
	static void Initialise(void);
	static void Shutdown(void);
	static void Update(void);
	static void Suspend(void);
	static void Resume(void);
	static uint32 GetCyclesPerMillisecond(void);
	static uint32 GetCurrentTimeInCycles(void);
	static bool GetIsSlowMotionActive(void);
	static void Stop(void);
	static void StartUserPause(void);
	static void EndUserPause(void);

	friend bool GenericLoad(void);
	friend bool GenericSave(int file);
	friend class CMemoryCard;

#ifdef FIX_BUGS
	static float GetDefaultTimeStep(void) { return 50.0f / 30.0f; }
	static float GetTimeStepFix(void) { return GetTimeStep() / GetDefaultTimeStep(); }
	static uint32 GetLogicalFrameCounter(void) { return m_LogicalFrameCounter; }
	static uint32 GetLogicalFramesPassed(void) { return m_LogicalFramesPassed; }
#endif
#ifdef FIX_HIGH_FPS_BUGS
	// The game logic was tuned for GetDefaultTimeStep() (30 fps).
	// These convert per-frame constants to the current time step so the result per second stays the same.

	// x *= k every frame  ->  x *= ScaleFrameMultiplier(k)
	static float ScaleFrameMultiplier(float k) { return Pow(k, GetTimeStepFix()); }
	// x += (target - x) * t every frame  ->  x += (target - x) * ScaleFrameLerp(t)
	static float ScaleFrameLerp(float t) { return t >= 1.0f ? 1.0f : 1.0f - Pow(1.0f - t, GetTimeStepFix()); }
	// Time step of the whole frame. Unlike GetTimeStep() it isn't changed while CPhysical::ProcessCollision
	// subdivides the step, so use it for per-frame quantities computed during collision processing.
	static float GetFrameTimeStepFix(void) { return ms_fTimeStepNonClipped / GetDefaultTimeStep(); }

	// Frame counter that advances once per 30 fps frame of game time (stops while paused, follows the time scale).
	// Use it for logic that counts frames, so it doesn't run faster at higher frame rates.
	static uint32 GetSimFrameCounter(void) { return m_SimFrameCounter; }
	static uint32 GetSimFramesPassed(void) { return m_SimFramesPassed; }
	// Replacement for ((GetFrameCounter() + offset) % n) == 0: true once every n 30 fps frames of game time
	static bool IsSimFrameDue(uint32 n, uint32 offset = 0) {
		for(uint32 i = 0; i < m_SimFramesPassed; i++)
			if((m_SimFrameCounter - i + offset) % n == 0)
				return true;
		return false;
	}
#endif
};

#endif // __GTA_TIMER_H__
