# Ultimate ASI Loader for Windows amd64 artifacts

This directory contains the 64-bit build of Ultimate ASI Loader v9.7.4.
It is packaged as `dinput8.dll` only by the Windows amd64 workflow.

The updated loader ignores `ERROR_BAD_EXE_FORMAT` when it encounters a
32-bit ASI plugin in a 64-bit game process. This prevents the sequence of
non-fatal `Error: 193` message boxes produced by older loader versions.

Do not copy this DLL into x86 artifacts. Its PE machine type is AMD64
(`0x8664`), so a 32-bit executable cannot load it.

File: `dist/Win64/dinput8.dll`

Version: `9.7.4`

SHA-256:
`FA266E3513D02C08A1B808F28C10538A489EAFFAA4B0707F7CC1066E71B5AFD7`

Upstream release:
https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/tag/v9.7.4

Relevant upstream fix:
https://github.com/ThirteenAG/Ultimate-ASI-Loader/commit/2473921e5040735d6d6555788e9890345808ae7c

Ultimate ASI Loader is licensed under the MIT License. See `LICENSE`.
