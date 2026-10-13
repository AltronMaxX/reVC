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
    };

    extern WGPUGlobals wgpuGlobals;
}
