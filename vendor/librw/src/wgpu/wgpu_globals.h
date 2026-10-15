#pragma once
#include "rwwgpu.h"

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__linux__)
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_WAYLAND
#endif
#ifdef LIBRW_GLFW
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#endif
#ifdef RW_WGPU
#include <webgpu/webgpu.h>
#include <webgpu/wgpu.h>   // wgpu-native extras: WGPUInstanceExtras, backend flags
#endif

inline WGPUStringView wgpuStr(const char* s)
{
    return WGPUStringView{s, WGPU_STRLEN};
}

#define WGPU_STR(str) wgpuStr(str)

namespace rw::wgpu
{
    struct WGPUGlobals
    {
        GLFWwindow* window;
        void** pWindow;
        int32 winWidth, winHeight;
        const char* winTitle;

        WGPUInstance instance;
        WGPUSurface surface;
        WGPUAdapter adapter;
        WGPUDevice device;
        WGPUQueue queue;

        WGPUSurfaceConfiguration surfaceConfig;
        WGPUTextureFormat surfaceFormat;

        // MSAA state. numSamples is the currently-applied sample count
        // (1 = no MSAA, 2/4/8/16 = MSAA). When DEVICESETMULTISAMPLINGLEVELS
        // is called, msaaDirty is set so the next beginUpdate() recreates
        // the backbuffer/depth/resolve-target textures and flushes all
        // render-pipeline caches (pipelines are sample-count-specific in
        // wgpu). The recreation happens at a frame boundary so no encoder
        // is in flight.
        uint32 numSamples = 1;
        bool    msaaDirty = false;
        // Requested-but-not-yet-applied sample count. DEVICESETMULTISAMPLINGLEVELS
        // writes here instead of numSamples so that currentSampleCount() keeps
        // returning the OLD value for the rest of the current frame. The actual
        // swap happens in applyPendingMsaaChange() at the next beginUpdate().
        // This prevents a pipeline/attachment sample-count mismatch crash when
        // the user changes MSAA mid-frame: without this, numSamples would change
        // immediately, new pipelines would be created with the new count, but
        // the render pass would still use the old backbuffer.
        uint32 pendingNumSamples = 1;
        // Maximum sample count supported by this device + format combo.
        // WebGPU spec guarantees [1, 4] for Rgba8Unorm / Depth32Float without
        // any extra features. With WGPUNativeFeature_TextureAdapterSpecificFormatFeatures
        // the device may expose [1, 2, 4, 8] (and occasionally 16). Set once
        // during device init; DEVICESETMULTISAMPLINGLEVELS clamps to it.
        uint32 maxSupportedSamples = 4;
    };

    extern WGPUGlobals wgpuGlobals;
}
