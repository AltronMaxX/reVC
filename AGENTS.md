# reVC — Project Notes for Agents

Reverse-engineered source code of GTA Vice City (fork of re3, main branch: `miami`).
Requires original game assets. Targets Windows and Linux (amd64). No license — educational/modding use only.

## Tech stack
- C++23, built with CMake (primary) and premake5 (VS2026); CodeWarrior 7 project kept as reference.
- Rendering: original RenderWare (D3D8) or librw reimplementation (D3D9, OpenGL 2.1+/ES 2.0+); experimental WebGPU backend via local librw-wgpu fork (wgpu-native, Vulkan/GL/D3D12 selectable in Display menu).
- Audio: OpenAL (default) or Miles Sound System (`REVC_AUDIO=OAL|MSS`); mpg123 required; optional opusfile, libsndfile.
- Optional Discord Rich Presence (`REVC_WITH_DISCORD_RPC`).
- Vendored deps in `vendor/`: librw, glfw, openal-soft, mpg123, ogg/opus, discord-rpc, milessdk.

## Source layout (`src/`)
Subsystems mirror the original game: `core/` (main loop, `config.h` with all toggles, input, menu),
`renderer/`, `control/`, `peds/`, `vehicles/`, `collision/`, `animation/`, `audio/`, `modelinfo/`,
`weapons/`, `objects/`, `buildings/`, `entities/`, `save/`, `text/`, `math/`, `fakerw/`, `skel/`, `extras/`, `rw/`.

- `gamefiles/` — required game assets to copy into a GTA VC install.
- `utils/gxt/` — GXT text-table tool; localisations incl. russian/ukrainian sources.
- User settings live in `reVC.ini` (not `gta_vc.set`); debug menu Ctrl-M, debug cam Ctrl-B.

## Notable improvements over the original
SA-style mechanics (free aim, crouch, swimming, climbing, first person), high-FPS physics fixes,
widescreen support, PS2/Xbox effects (MatFX, rain droplets, lightmaps, rim light), autosaves after missions,
no loading screens between islands, Xbox/PS2 asset loading, Discord RPC, Ukrainian localisation.

## Contribution rules (important)
Code outside preprocessor conditions must be **pure reversed code** from the original binaries.
Custom changes are only accepted behind defines (e.g. `FIX_BUGS`) or as platform/skeleton layers.
PRs accepted: new features matching a VC release, bug fixes (original-code fixes behind `FIX_BUGS`),
unreversed platform code, accuracy improvements, compat layers, translation fixes, maintainability.
Coding style: see `CODING_STYLE.md` (loosely enforced).

## WGPU optimization session (2026-11-26/27) — results & gotchas

Measured on the maintainer's machine (Ryzen-class, Wayland, Vulkan via wgpu-native).
Baseline facts: in-game ~90 FPS capped by present mode; main thread is the bottleneck
(RenderScene encode ~2-4 ms of an 8-11 ms frame); ~1100-1500 draws/frame, 84% via the
custom object pipes (`wgpucustom.cpp objShaderRender`), ~60% of draws redraw geometry
already drawn this frame (instancing potential, not yet exploited).

Changes made (all in `vendor/librw/src/wgpu/` unless noted):
1. `wgpu_internal.h/wgpudevice.cpp`: per-frame draw stats (`FrameStats`, `getFrameStats`,
   `statNoteGeometry`), printed by `[PERF]` log in `src/core/timebars.cpp tbPerfLog`.
2. Pass-state bind cache: `passSetPipeline/passSetVertexBuffer0/passSetIndexBufferU16` skip
   redundant FFI calls; reset on pass begin/end and after direct-binding paths
   (im2d/im3d/im2DRenderCustom already call `passBindCacheReset()`).
3. `wgpucustom.cpp`: custom-pipe bind-group cache was a fixed 32-slot array with
   wipe-on-full — the world pipe uses hundreds of texture pairs, so it thrashed and
   created a bind group per draw. Now `std::unordered_map` keyed by (t0,t1) per shader.
   NOTE: `ObjShader` is allocated with raw `rwNewT` — the map needs placement new +
   explicit destructor call (was a SIGFPE crash when it was only memset).
4. `src/core/timebars.cpp`: real-millisecond timers (RsTimer; the old
   cycles/cyclesPerFrame units were meaningless) + always-on stderr `[PERF]` log
   (works without the debug-menu timebars toggle).

**GOTCHA: `clock_gettime` costs ~1.2 µs on this machine (slow vDSO/syscall path).**
Never use chrono/steady_clock for per-draw micro-profiling — use rdtsc. A chrono-based
per-mesh instrumentation pass was eating ~5-10 µs/draw and masqueraded as wgpu cost.

Env knobs: `LIBRW_WGPU_PRESENT=fifo|mailbox|immediate` (default prefers mailbox, which
caps FPS ~90 on this setup; immediate gave 165-190 FPS). Game launch under Wayland needs
`XDG_SESSION_TYPE=wayland` (glfw 3.4 picks X11 otherwise).

Known leftover: heap corruption SIGABRT seen once on game exit (`free(): invalid next
size`) and a SIGFPE during save-load was traced to the bg-map lifetime bug (fixed) —
watch for regressions. Next big lever: instancing repeated geometries (deferral +
per-instance world matrices in WGSL, touches `custompipes_wgpu.cpp` shader bodies).

## Rendering architecture (studied 2026-11-26)Frame flow in `src/core/main.cpp` `Idle()` (~line 1640):
1. `CRenderer::ConstructRenderList()` — sector scan + frustum culling, fills visible entity/building/vehicle arrays and alpha lists (`src/renderer/Renderer.cpp`).
2. `CRenderer::PreRender()` — per-entity updates, alpha-atomic distance sorting (linked lists, `src/rw/VisibilityPlugins.cpp`).
3. `RenderScene()` → with `gbNewRenderer` (default true, needs `NEW_RENDERER`+librw) → `RenderScene_new()`/`MattRenderScene()` — LCS-style cWorldStream emulation: water, `RenderWorld(0)` roads, `RenderWorld(1)` opaque + `PASS_NOZ` blend, peds, boats, transparent water (stencil keep-out), `RenderWorld(2)` transparent (`PASS_ADD`+`PASS_BLEND` via `WorldRender::RenderBlendPass`).
4. `CustomPipes::EnvMapRender()` — vehicle reflection env map re-render every `EnvMapUpdatePeriod=2` frames (second scene render!).
5. `RenderEffects_new()` — shadows, skidmarks, glass, coronas, particles, weather, ropes.
6. Screen droplets + `CPostFX::GetBackBuffer` (fullscreen grab) + motion blur, then `Render2dStuff()` (HUD/radar).

Draw path: `CEntity::Render()` → `RpClumpRender`/`RpAtomicRender` → per-atomic callbacks (`CVisibilityPlugins::Render*CB`, LOD/fade logic) → obj pipeline → librw GL3 backend `drawInst` = one `glDrawElements` per atomic mesh, no batching/instancing; alpha-tested geometry drawn twice under GS emulation (`drawInst_GSemu`). VAOs behind `RW_GL_USE_VAOS`.
Custom shader pipes (`EXTENDED_PIPELINES`, `src/extras/custompipes*.cpp`, GLSL/HLSL in `src/extras/shaders/`): neo vehicle (envmap+fresnel), world lightmap, gloss, rimlight. Backends: gl3 + experimental wgpu (`vendor/librw/src/wgpu/`).
Profiling aids: `tbStartTimer`/`tbEndTimer` timebars (`src/core/timebars.cpp`), debug menu Ctrl-M, `PUSH_RENDERGROUP` markers (RenderDoc-friendly).

Optimization candidates: draw-call batching/instancing in gl3 backend; sort-by-texture/material; alpha list sorting (linked-list insertion sort); envmap update cost; fullscreen backbuffer grabs; stencil water; `ms_lodDistScale=1.8` (stock 1.2) inflates draw distance; `SQUEEZE_PERFORMANCE` define exists but disabled.

## Build quick reference
- Clone with `--recursive -b miami`.
- CMake build dirs used locally: `cmake-build-debug/`, `cmake-build-release/` (Ninja).
- Premake: `premake-vs2026.cmd` variants; env `GTA_VC_RE_DIR` = GTA VC root for post-build copy; `--lto` option available.
- Key config knobs in `src/core/config.h`.
