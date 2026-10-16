#ifndef RS_FRAMEPACING_H
#define RS_FRAMEPACING_H

#include <chrono>
#include <cmath>

// One policy and sub-millisecond clock for every desktop rendering backend.
// The platform performs the interruptible wait, then checks input/focus again.
class RsFramePacer
{
	bool m_started;
	int m_targetFPS;
	double m_lastFrame;

public:
	RsFramePacer() { Reset(); }

	static int SanitizeLimit(int fps)
	{
		return fps <= 0 ? 30 : (fps > 1000 ? 1000 : fps);
	}

	static int GetTargetFPS(bool limiterEnabled, int maxFPS, bool menuActive)
	{
		if (!limiterEnabled)
			return menuActive ? 60 : 0;
		const int limit = SanitizeLimit(maxFPS);
		return menuActive && limit > 60 ? 60 : limit;
	}

	static double CurrentTimeSeconds()
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	void Reset()
	{
		m_started = false;
		m_targetFPS = 0;
		m_lastFrame = 0.0;
	}

	// A zero result permits and records this frame. Never accumulate overdue
	// deadlines: slow rendering, loading or focus loss must not cause a burst.
	double RemainingSeconds(double now, int targetFPS)
	{
		if (!std::isfinite(now)) {
			Reset();
			return 0.0;
		}

		targetFPS = targetFPS > 0 ? SanitizeLimit(targetFPS) : 0;
		if (!m_started || targetFPS == 0 || targetFPS != m_targetFPS || now < m_lastFrame) {
			m_started = true;
			m_targetFPS = targetFPS;
			m_lastFrame = now;
			return 0.0;
		}

		const double remaining = 1.0 / (double)targetFPS - (now - m_lastFrame);
		// Arithmetic at an exact deadline can differ by a fraction of a
		// nanosecond. Do not dispatch another OS wait for that rounding noise.
		if (remaining <= 1.0e-9) {
			m_lastFrame = now;
			return 0.0;
		}
		return remaining;
	}
};

#endif
