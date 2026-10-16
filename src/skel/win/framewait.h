#ifndef RS_FRAMEWAIT_WIN32_H
#define RS_FRAMEWAIT_WIN32_H

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#include <cmath>

// Wait without dispatching callbacks. Both desktop skeletons recheck their
// native message/focus state in the main loop before beginning another frame.
inline void
RsWaitForFrameWin32(double remainingSeconds)
{
	if (!std::isfinite(remainingSeconds) || remainingSeconds <= 0.0)
		return;
	// The supported minimum limit is one FPS. Bound conversions defensively
	// and allow the caller to recheck its deadline after at most one second.
	const double remaining = 1000.0 * (remainingSeconds > 1.0 ? 1.0 : remainingSeconds);
	struct FrameWaitTimer {
		HANDLE handle;
		FrameWaitTimer() : handle(nullptr)
		{
			typedef HANDLE (WINAPI *CreateTimerFn)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
			CreateTimerFn createTimer = (CreateTimerFn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateWaitableTimerExW");
			if (createTimer)
				// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+).
				handle = createTimer(nullptr, nullptr, 0x00000002, SYNCHRONIZE | TIMER_MODIFY_STATE);
		}
		~FrameWaitTimer()
		{
			if (handle) {
				CancelWaitableTimer(handle);
				CloseHandle(handle);
			}
		}
	};
	static FrameWaitTimer timer;
	if (timer.handle) {
		LARGE_INTEGER dueTime;
		dueTime.QuadPart = -(LONGLONG)std::ceil(remaining * 10000.0);
		if (SetWaitableTimer(timer.handle, &dueTime, 0, nullptr, nullptr, FALSE)) {
			DWORD result = MsgWaitForMultipleObjectsEx(1, &timer.handle, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
			CancelWaitableTimer(timer.handle);
			if (result != WAIT_FAILED)
				return;
		}
	}

	// Older Windows versions lack high-resolution waitable timers. Request a
	// finer timeout only for this wait, and release it before returning.
	const DWORD waitMs = (DWORD)std::ceil(remaining);
	const bool raisedTimerResolution = timeBeginPeriod(1) == TIMERR_NOERROR;
	if (MsgWaitForMultipleObjectsEx(0, nullptr, waitMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_FAILED)
		Sleep(waitMs);
	if (raisedTimerResolution)
		timeEndPeriod(1);
}
#endif

#endif
