#ifndef RS_WINDOWSSESSION_H
#define RS_WINDOWSSESSION_H

#ifdef _WIN32
#include <windows.h>

// Keep game-only requests scoped to this main thread. Never alter the user's
// foreground-lock timeout, screensaver or power policy, or broadcast settings
// changes to every other application during startup/shutdown.
class RsWindowsSession
{
	EXECUTION_STATE m_previousExecutionState;
	DWORD m_savedStickyHotkeys;
	bool m_stickyKeysChanged;

public:
	RsWindowsSession()
		: m_previousExecutionState(SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED)),
		  m_savedStickyHotkeys(0), m_stickyKeysChanged(false)
	{
		STICKYKEYS sticky = {};
		sticky.cbSize = sizeof(sticky);
		if(!SystemParametersInfo(SPI_GETSTICKYKEYS, sizeof(sticky), &sticky, 0) ||
		   (sticky.dwFlags & SKF_STICKYKEYSON))
			return;

		const DWORD hotkeyFlags = SKF_HOTKEYACTIVE | SKF_CONFIRMHOTKEY;
		m_savedStickyHotkeys = sticky.dwFlags & hotkeyFlags;
		if(m_savedStickyHotkeys == 0) return;

		// Suppress only the five-Shift activation prompt, never an enabled
		// accessibility feature or any of its other user preferences.
		sticky.dwFlags &= ~hotkeyFlags;
		m_stickyKeysChanged = SystemParametersInfo(SPI_SETSTICKYKEYS, sizeof(sticky), &sticky, 0) != FALSE;
	}

	~RsWindowsSession()
	{
		if(m_previousExecutionState)
			SetThreadExecutionState(m_previousExecutionState | ES_CONTINUOUS);
		if(!m_stickyKeysChanged) return;

		STICKYKEYS sticky = {};
		sticky.cbSize = sizeof(sticky);
		if(!SystemParametersInfo(SPI_GETSTICKYKEYS, sizeof(sticky), &sticky, 0)) return;

		const DWORD hotkeyFlags = SKF_HOTKEYACTIVE | SKF_CONFIRMHOTKEY;
		// If the user enabled StickyKeys or changed either managed hotkey bit
		// while playing, their current choice takes precedence over our snapshot.
		if(sticky.dwFlags & (SKF_STICKYKEYSON | hotkeyFlags)) return;
		sticky.dwFlags |= m_savedStickyHotkeys;
		SystemParametersInfo(SPI_SETSTICKYKEYS, sizeof(sticky), &sticky, 0);
	}

	RsWindowsSession(const RsWindowsSession&) = delete;
	RsWindowsSession& operator=(const RsWindowsSession&) = delete;
};
#endif

#endif
