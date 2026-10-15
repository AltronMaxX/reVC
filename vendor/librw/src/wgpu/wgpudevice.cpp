#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>
#include <cassert>
#include <atomic>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwengine.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"

#ifdef RW_WGPU

#include "wgpu_internal.h"

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__linux__)
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_WAYLAND
#endif
#include <GLFW/glfw3native.h>

#define PLUGIN_ID 0

namespace rw
{
    namespace wgpu
    {
        WGPUGlobals wgpuGlobals;

        FrameState s_frame = {};
        RenderState s_renderState = {};

        // The acquired swapchain texture, held across mid-frame submits. A frame can be
        // interrupted by offscreen render-to-texture passes (e.g. the neo env map):
        // the main frame's commands are submitted so the offscreen target can take
        // over, but the surface texture must stay acquired — presenting happens only
        // in showRaster(), and rendering later resumes onto the same texture.
        static struct
        {
            WGPUTexture texture;
            WGPUTextureView view;
            bool acquired;
        } s_swap = {};

        // Depth attachment for the main framebuffer. Persistent (recreated only on
        // window resize OR MSAA change) so that depth contents survive a mid-frame
        // offscreen interruption — draws after the resume still test against the
        // scene's depth. sampleCount mirrors s_backbuffer.sampleCount: the depth
        // attachment must match the colour attachment's sample count, and we have
        // no public wgpu-native API to query a texture's sample count at runtime.
        static struct
        {
            WGPUTexture texture;
            WGPUTextureView view;
            uint32 width, height;
            uint32 sampleCount = 1;
        } s_mainDepth = {};

        int32 wgpuRasterOffset = 0;

        // ========================================================================
        // Uniform ring buffer — per-frame dynamic-offset uniform allocations
        // ========================================================================
        // Previously every mesh, every pass, every frame created and destroyed its own
        // uniform buffer + bind group (thousands of GPU allocations per frame, doubled
        // by the GSemu two-pass). Instead draws now sub-allocate from one big buffer at
        // 256-aligned offsets and bind it with a dynamic offset, while bind groups are
        // cached per texture. The head rewinds each submit; the queue serialises
        // writeBuffer after the previous submit, so reusing the same storage next frame
        // is hazard-free (no explicit double buffering needed). The buffer grows only at
        // a frame boundary; the generation counter tells bind-group caches the
        // underlying buffer was recreated so they can rebuild.
        static WGPUBuffer s_uniformRing = nullptr;
        static uint8* s_uniformShadow = nullptr; // CPU mirror; uploaded once per submit
        static uint32 s_uniformRingCapacity = 0;
        static uint32 s_uniformRingHead = 0; // bytes consumed this frame (may exceed capacity)
        static uint32 s_uniformRingGeneration = 0;
        static const uint32 WGPU_UNIFORM_ALIGN = 256; // minUniformBufferOffsetAlignment (WebGPU guaranteed)

        static void uniformRingCreate(uint32 capacity)
        {
            if (s_uniformRing) wgpuBufferRelease(s_uniformRing);
            WGPUBufferDescriptor d = {};
            d.label = WGPU_STR("uniform ring");
            d.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
            d.size = capacity;
            s_uniformRing = wgpuDeviceCreateBuffer(wgpuGlobals.device, &d);
            s_uniformRingCapacity = capacity;
            s_uniformRingGeneration++;
            free(s_uniformShadow);
            s_uniformShadow = (uint8*)malloc(capacity);
        }

        // Sub-allocate `size` bytes at a 256-aligned offset, stage `data`, return the
        // offset — or 0xFFFFFFFF if the ring is (this frame) too small, in which case
        // the caller falls back to a private per-draw buffer. The head keeps advancing
        // past capacity so uniformRingEndFrame() can grow to the true peak.
        // The data lands in a CPU shadow; one writeBuffer per SUBMIT uploads the whole
        // used range (per-allocation writeBuffer calls were a dominant CPU cost —
        // thousands per frame at a few µs each inside wgpu-native).
        uint32 wgpuUniformAlloc(const void* data, uint32 size)
        {
            if (!s_uniformRing) uniformRingCreate(256u * 1024u);
            uint32 off = (s_uniformRingHead + (WGPU_UNIFORM_ALIGN - 1)) & ~(WGPU_UNIFORM_ALIGN - 1);
            uint32 slot = (size + (WGPU_UNIFORM_ALIGN - 1)) & ~(WGPU_UNIFORM_ALIGN - 1);
            s_uniformRingHead = off + slot;
            if (off + slot > s_uniformRingCapacity)
                return 0xFFFFFFFFu;
            memcpy(s_uniformShadow + off, data, size);
            return off;
        }

        WGPUBuffer wgpuUniformRingBuffer(void) { return s_uniformRing; }
        uint32 wgpuUniformRingGeneration(void) { return s_uniformRingGeneration; }

        // ------------------------------------------------------------------------
        // Geometry ring — per-frame vertex/index data for the immediate-mode paths
        // (im2d/im3d: HUD, fonts, coronas, particles, water, shadows). Replaces a
        // buffer create + writeBuffer per draw with a sub-allocation; draws bind the
        // ring at an offset. Same shadow + flush-per-submit scheme as the uniform
        // ring. No bind groups reference it, so growing needs no generation counter.
        // ------------------------------------------------------------------------
        static WGPUBuffer s_geomRing = nullptr;
        static uint8* s_geomShadow = nullptr;
        static uint32 s_geomRingCapacity = 0;
        static uint32 s_geomRingHead = 0;
        static const uint32 WGPU_GEOM_ALIGN = 16; // covers vertex (4) and index (4) offset rules

        static void geomRingCreate(uint32 capacity)
        {
            if (s_geomRing) wgpuBufferRelease(s_geomRing);
            WGPUBufferDescriptor d = {};
            d.label = WGPU_STR("geometry ring");
            d.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
            d.size = capacity;
            s_geomRing = wgpuDeviceCreateBuffer(wgpuGlobals.device, &d);
            s_geomRingCapacity = capacity;
            free(s_geomShadow);
            s_geomShadow = (uint8*)malloc(capacity);
        }

        uint32 wgpuGeomAlloc(const void* data, uint32 size)
        {
            if (!s_geomRing) geomRingCreate(1024u * 1024u);
            uint32 off = (s_geomRingHead + (WGPU_GEOM_ALIGN - 1)) & ~(WGPU_GEOM_ALIGN - 1);
            uint32 slot = (size + (WGPU_GEOM_ALIGN - 1)) & ~(WGPU_GEOM_ALIGN - 1);
            s_geomRingHead = off + slot;
            if (off + slot > s_geomRingCapacity)
                return 0xFFFFFFFFu;
            memcpy(s_geomShadow + off, data, size);
            return off;
        }

        WGPUBuffer wgpuGeomRingBuffer(void) { return s_geomRing; }

        // Upload both rings' staged data in one writeBuffer each. Must run before the
        // queue submit that consumes them; correct across mid-frame submits too (the
        // queue serialises: write A, submit A, write B, submit B).
        static void ringsFlush(void)
        {
            uint32 used = s_uniformRingHead < s_uniformRingCapacity ? s_uniformRingHead : s_uniformRingCapacity;
            if (used && s_uniformRing)
                wgpuQueueWriteBuffer(wgpuGlobals.queue, s_uniformRing, 0, s_uniformShadow, used);
            used = s_geomRingHead < s_geomRingCapacity ? s_geomRingHead : s_geomRingCapacity;
            if (used && s_geomRing)
                wgpuQueueWriteBuffer(wgpuGlobals.queue, s_geomRing, 0, s_geomShadow, used);
        }

        // Called once per submit (frame boundary): grow to fit the peak the frame
        // actually needed (doubling), then rewind for the next frame.
        static void uniformRingEndFrame(void)
        {
            if (s_uniformRingHead > s_uniformRingCapacity)
            {
                uint32 want = s_uniformRingCapacity ? s_uniformRingCapacity : (256u * 1024u);
                while (want < s_uniformRingHead) want *= 2;
                uniformRingCreate(want);
            }
            s_uniformRingHead = 0;
            if (s_geomRingHead > s_geomRingCapacity)
            {
                uint32 want = s_geomRingCapacity ? s_geomRingCapacity : (1024u * 1024u);
                while (want < s_geomRingHead) want *= 2;
                geomRingCreate(want);
            }
            s_geomRingHead = 0;
        }

        void wgpuUniformRingDestroy(void)
        {
            if (s_uniformRing)
            {
                wgpuBufferRelease(s_uniformRing);
                s_uniformRing = nullptr;
            }
            free(s_uniformShadow);
            s_uniformShadow = nullptr;
            s_uniformRingCapacity = 0;
            s_uniformRingHead = 0;
            if (s_geomRing)
            {
                wgpuBufferRelease(s_geomRing);
                s_geomRing = nullptr;
            }
            free(s_geomShadow);
            s_geomShadow = nullptr;
            s_geomRingCapacity = 0;
            s_geomRingHead = 0;
        }

        // ========================================================================
        // Backend selection (Vulkan / OpenGL / D3D12 / Metal)
        // ========================================================================
        static int s_backendPref = BACKEND_AUTO; // requested
        static WGPUBackendType s_backendActual = WGPUBackendType_Undefined; // what we got

        void setBackend(int backend) { s_backendPref = backend; }
        int getBackend(void) { return s_backendPref; }

        static const char* backendTypeName(WGPUBackendType b)
        {
            switch (b)
            {
            case WGPUBackendType_Vulkan: return "Vulkan";
            case WGPUBackendType_OpenGL: return "OpenGL";
            case WGPUBackendType_OpenGLES: return "OpenGL ES";
            case WGPUBackendType_D3D12: return "D3D12";
            case WGPUBackendType_D3D11: return "D3D11";
            case WGPUBackendType_Metal: return "Metal";
            default: return "Auto";
            }
        }

        const char* getBackendName(void) { return backendTypeName(s_backendActual); }

        // Case-insensitive equality (portable — MSVC has no strcasecmp).
        static bool ieq(const char* a, const char* b)
        {
            for (; *a && *b; a++, b++)
            {
                char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
                char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
                if (ca != cb) return false;
            }
            return *a == *b;
        }

        // Effective preference: the LIBRW_WGPU_BACKEND env var overrides the API value.
        static int resolveBackendPref(void)
        {
            const char* e = getenv("LIBRW_WGPU_BACKEND");
            if (e && e[0])
            {
                if (ieq(e, "vulkan") || ieq(e, "vk")) return BACKEND_VULKAN;
                if (ieq(e, "gl") || ieq(e, "opengl")) return BACKEND_GL;
                if (ieq(e, "d3d12") || ieq(e, "dx12")) return BACKEND_D3D12;
                if (ieq(e, "metal")) return BACKEND_METAL;
                if (ieq(e, "auto")) return BACKEND_AUTO;
            }
            return s_backendPref;
        }

        static WGPUInstanceBackend backendPrefToFlag(int pref)
        {
            switch (pref)
            {
            case BACKEND_VULKAN: return WGPUInstanceBackend_Vulkan;
            case BACKEND_GL: return WGPUInstanceBackend_GL;
            case BACKEND_D3D12: return WGPUInstanceBackend_DX12;
            case BACKEND_METAL: return WGPUInstanceBackend_Metal;
            default: return WGPUInstanceBackend_All; // 0 = all
            }
        }

        static WGPUBackendType backendPrefToType(int pref)
        {
            switch (pref)
            {
            case BACKEND_VULKAN: return WGPUBackendType_Vulkan;
            case BACKEND_GL: return WGPUBackendType_OpenGL;
            case BACKEND_D3D12: return WGPUBackendType_D3D12;
            case BACKEND_METAL: return WGPUBackendType_Metal;
            default: return WGPUBackendType_Undefined;
            }
        }

        // ------------------------------------------------------------------------
        // Offscreen backbuffer + present blit
        // ------------------------------------------------------------------------
        // The main frame renders into a private offscreen texture ("backbuffer") that
        // matches the surface's format and size; presentFrame() blits it into the
        // acquired swapchain texture with one fullscreen pass. Two reasons:
        //  * GTA snapshots the presented frame every so often (motion-blur trails,
        //    sniper blur, postfx GetBackBuffer, rain screen droplets) via
        //    rasterRenderFast(), which copies FROM the current render target — that
        //    needs COPY_SRC, and GL swapchain textures only support RENDER_ATTACHMENT.
        //    Our own texture can carry COPY_SRC, so the snapshots work on GL too.
        //  * The blit fragment shader writes alpha = 1, which subsumes the old GL-only
        //    fillSwapchainAlpha() pass: the X11/EGL compositor honours the window
        //    alpha, and the game leaves sub-1 alpha in the frame (fades, alpha UI).
        // The blit is an identity copy — a fragment at NDC (x,y) samples the texel the
        // scene wrote at the same NDC position — so orientation survives on every
        // backend; depth/MSAA are untouched (1 sample, no depth attachment).
        static struct
        {
            WGPUTexture texture;       // MSAA color attachment (or 1-sample if MSAA off)
            WGPUTextureView view;
            WGPUTexture resolveTexture; // non-MSAA resolve target (only when numSamples > 1)
            WGPUTextureView resolveView;
            uint32 width, height;
            // Sample count the current textures were created with. Tracked here
            // because wgpu-native has no public API to query a texture's sample
            // count at runtime, and ensureBackbuffer() must recreate the backbuffer
            // if numSamples changed even when the size didn't (e.g. user toggled
            // 2x↔4x MSAA at the same window size). applyPendingMsaaChange() is the
            // primary trigger for recreation, but this is the safety net for any
            // path that bumps wgpuGlobals.numSamples without going through
            // applyPendingMsaaChange (or in case the call ordering regresses).
            uint32 sampleCount = 1;
        } s_backbuffer = {};

        static WGPURenderPipeline s_presentBlitPipe = nullptr;
        static WGPUPipelineLayout s_presentBlitPL = nullptr;
        static WGPUBindGroupLayout s_presentBlitBGL = nullptr;
        static WGPUSampler s_presentSampler = nullptr;
        // (P2-15) Sub-rect camera Z-clear pipeline. Declared here (near the other
        // static pipelines) so applyPendingMsaaChange can release it on MSAA change;
        // defined lazily in ensureDepthClearPipe (see ~line 972). The pipeline's
        // multisample.count must match the depth attachment's sample count, so it
        // must be released + recreated when wgpuGlobals.numSamples changes.
        static WGPURenderPipeline s_depthClearPipe = nullptr;
        static const char* k_presentBlitWGSL = R"(
@group(0) @binding(0) var samp : sampler;
@group(0) @binding(1) var tex  : texture_2d<f32>;

struct VSOut {
    @builtin(position) pos : vec4<f32>,
    @location(0)       uv  : vec2<f32>,
};
@vertex fn vs(@builtin(vertex_index) i : u32) -> VSOut {
    var p = array<vec2<f32>,3>(vec2<f32>(-1.0,-1.0), vec2<f32>(3.0,-1.0), vec2<f32>(-1.0,3.0));
    var o : VSOut;
    o.pos = vec4<f32>(p[i], 0.0, 1.0);
    // Texture row 0 is NDC y=+1 (top of the target), hence the flipped v.
    o.uv  = vec2<f32>(p[i].x*0.5 + 0.5, 0.5 - p[i].y*0.5);
    return o;
}
@fragment fn fs(vin : VSOut) -> @location(0) vec4<f32> {
    return vec4<f32>(textureSample(tex, samp, vin.uv).rgb, 1.0);
}
)";

        static void ensurePresentBlitPipe(void)
        {
            if (s_presentBlitPipe) return;
            WGPUDevice dev = wgpuGlobals.device;

            WGPUShaderSourceWGSL src = {};
            src.chain.sType = WGPUSType_ShaderSourceWGSL;
            src.code = WGPU_STR(k_presentBlitWGSL);
            WGPUShaderModuleDescriptor smd = {};
            smd.nextInChain = &src.chain;
            WGPUShaderModule sm = wgpuDeviceCreateShaderModule(dev, &smd);

            // Nearest filtering: this is a 1:1 copy, no smearing; ClampToEdge guards
            // the half-texel edge on odd sizes.
            WGPUSamplerDescriptor sd = {};
            sd.label = WGPU_STR("present blit sampler");
            sd.addressModeU = WGPUAddressMode_ClampToEdge;
            sd.addressModeV = WGPUAddressMode_ClampToEdge;
            sd.addressModeW = WGPUAddressMode_ClampToEdge;
            sd.magFilter = WGPUFilterMode_Nearest;
            sd.minFilter = WGPUFilterMode_Nearest;
            sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
            sd.maxAnisotropy = 1;
            s_presentSampler = wgpuDeviceCreateSampler(dev, &sd);

            // binding0 = sampler, binding1 = backbuffer texture
            WGPUBindGroupLayoutEntry bglEntries[2] = {};
            bglEntries[0].binding = 0;
            bglEntries[0].visibility = WGPUShaderStage_Fragment;
            bglEntries[0].sampler.type = WGPUSamplerBindingType_Filtering;
            bglEntries[1].binding = 1;
            bglEntries[1].visibility = WGPUShaderStage_Fragment;
            bglEntries[1].texture.sampleType = WGPUTextureSampleType_Float;
            bglEntries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
            WGPUBindGroupLayoutDescriptor bglDesc = {};
            bglDesc.label = WGPU_STR("present blit bgl");
            bglDesc.entryCount = 2;
            bglDesc.entries = bglEntries;
            s_presentBlitBGL = wgpuDeviceCreateBindGroupLayout(dev, &bglDesc);

            WGPUPipelineLayoutDescriptor plDesc = {};
            plDesc.label = WGPU_STR("present blit pl");
            plDesc.bindGroupLayoutCount = 1;
            plDesc.bindGroupLayouts = &s_presentBlitBGL;
            s_presentBlitPL = wgpuDeviceCreatePipelineLayout(dev, &plDesc);

            WGPUColorTargetState ct = {};
            ct.format = wgpuGlobals.surfaceFormat;
            ct.writeMask = WGPUColorWriteMask_All;
            WGPUFragmentState fs = {};
            fs.module = sm;
            fs.entryPoint = WGPU_STR("fs");
            fs.targetCount = 1;
            fs.targets = &ct;

            WGPURenderPipelineDescriptor pd = {};
            pd.label = WGPU_STR("present blit pipe");
            pd.layout = s_presentBlitPL;
            pd.vertex.module = sm;
            pd.vertex.entryPoint = WGPU_STR("vs");
            pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
            pd.multisample.count = 1;
            pd.multisample.mask = 0xFFFFFFFFu;
            pd.fragment = &fs;
            s_presentBlitPipe = wgpuDeviceCreateRenderPipeline(dev, &pd);
            wgpuShaderModuleRelease(sm);
        }

        // (Re)create the offscreen backbuffer at the current window size.
        static void ensureBackbuffer(void)
        {
            uint32 w = (uint32)wgpuGlobals.winWidth;
            uint32 h = (uint32)wgpuGlobals.winHeight;
            if (w == 0 || h == 0) return;
            // Recreate when size OR sample count changed. The sample-count check is
            // the safety net for paths that bump wgpuGlobals.numSamples without
            // going through applyPendingMsaaChange (which already releases the
            // backbuffer). Without it, toggling 2x↔4x MSAA at the same window
            // size would silently reuse the old-sample-count backbuffer and crash
            // on the next set_pipeline ("Incompatible sample count").
            if (s_backbuffer.texture && s_backbuffer.width == w && s_backbuffer.height == h &&
                s_backbuffer.sampleCount == wgpuGlobals.numSamples)
                return;
            if (s_backbuffer.view) wgpuTextureViewRelease(s_backbuffer.view);
            if (s_backbuffer.texture) wgpuTextureRelease(s_backbuffer.texture);
            if (s_backbuffer.resolveView) wgpuTextureViewRelease(s_backbuffer.resolveView);
            if (s_backbuffer.resolveTexture) wgpuTextureRelease(s_backbuffer.resolveTexture);
            s_backbuffer.resolveView = nullptr;
            s_backbuffer.resolveTexture = nullptr;

            WGPUTextureDescriptor d = {};
            d.label = WGPU_STR("wgpu backbuffer");
            d.size = {w, h, 1};
            d.format = wgpuGlobals.surfaceFormat;
            d.usage = WGPUTextureUsage_RenderAttachment
                | WGPUTextureUsage_CopySrc
                | WGPUTextureUsage_TextureBinding;
            d.dimension = WGPUTextureDimension_2D;
            d.mipLevelCount = 1;
            d.sampleCount = wgpuGlobals.numSamples;
            s_backbuffer.texture = wgpuDeviceCreateTexture(wgpuGlobals.device, &d);

            WGPUTextureViewDescriptor v = {};
            v.format = wgpuGlobals.surfaceFormat;
            v.dimension = WGPUTextureViewDimension_2D;
            v.aspect = WGPUTextureAspect_All;
            v.baseMipLevel = 0;
            v.mipLevelCount = 1;
            v.baseArrayLayer = 0;
            v.arrayLayerCount = 1;
            s_backbuffer.view = wgpuTextureCreateView(s_backbuffer.texture, &v);

            // When MSAA is on, also create a non-MSAA resolve target.
            // The render pass writes MSAA color into `texture` and the
            // resolved average into `resolveTexture` (storeOp=Resolve).
            // The present blit then samples from resolveTexture.
            //
            // Usage flags:
            //   * RenderAttachment — the render pass resolves into this texture.
            //   * TextureBinding   — the present blit samples it via @binding(1).
            //   * CopySrc           — RwRasterRenderFast (motion-blur / front-buffer
            //     snapshot) copies from this texture into a CAMERATEXTURE
            //     destination; wgpuCommandEncoderCopyTextureToTexture requires the
            //     source to carry COPY_SRC. Without it, loading a save with MSAA on
            //     crashes: "Usage flags ... do not contain required usage flags
            //     TextureUsages(COPY_SRC)" at wgpuQueueSubmit.
            if (wgpuGlobals.numSamples > 1) {
                WGPUTextureDescriptor rd = {};
                rd.label = WGPU_STR("wgpu backbuffer resolve");
                rd.size = {w, h, 1};
                rd.format = wgpuGlobals.surfaceFormat;
                rd.usage = WGPUTextureUsage_RenderAttachment
                    | WGPUTextureUsage_TextureBinding
                    | WGPUTextureUsage_CopySrc;
                rd.dimension = WGPUTextureDimension_2D;
                rd.mipLevelCount = 1;
                rd.sampleCount = 1;
                s_backbuffer.resolveTexture = wgpuDeviceCreateTexture(wgpuGlobals.device, &rd);
                WGPUTextureViewDescriptor rv = {};
                rv.format = wgpuGlobals.surfaceFormat;
                rv.dimension = WGPUTextureViewDimension_2D;
                rv.aspect = WGPUTextureAspect_All;
                rv.baseMipLevel = 0;
                rv.mipLevelCount = 1;
                rv.baseArrayLayer = 0;
                rv.arrayLayerCount = 1;
                s_backbuffer.resolveView = wgpuTextureCreateView(s_backbuffer.resolveTexture, &rv);
            }
            s_backbuffer.width = w;
            s_backbuffer.height = h;
            s_backbuffer.sampleCount = wgpuGlobals.numSamples;
        }

        // Fullscreen blit backbuffer -> swapchain texture (alpha forced to 1), encoded
        // on the main frame's encoder right before the final submit.
        static void blitBackbufferToSwapchain(void)
        {
            if (!s_frame.encoder || !s_swap.view || !s_backbuffer.view) return;
            ensurePresentBlitPipe();

            // Bind group rebuilt per frame: the backbuffer view changes on resize.
            // When MSAA is on, the present blit samples from the resolved
            // non-MSAA texture (sampling from a multisample texture isn't
            // allowed in wgpu — you can only resolve via render pass).
            WGPUTextureView srcView = wgpuGlobals.numSamples > 1
                ? s_backbuffer.resolveView
                : s_backbuffer.view;
            WGPUBindGroupEntry bgEntries[2] = {};
            bgEntries[0].binding = 0;
            bgEntries[0].sampler = s_presentSampler;
            bgEntries[1].binding = 1;
            bgEntries[1].textureView = srcView;
            WGPUBindGroupDescriptor bgDesc = {};
            bgDesc.label = WGPU_STR("present blit bg");
            bgDesc.layout = s_presentBlitBGL;
            bgDesc.entryCount = 2;
            bgDesc.entries = bgEntries;
            WGPUBindGroup bg = wgpuDeviceCreateBindGroup(wgpuGlobals.device, &bgDesc);
            WGPURenderPassColorAttachment ca = {};
            ca.view = s_swap.view;
            ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
            ca.loadOp = WGPULoadOp_Clear;
            ca.storeOp = WGPUStoreOp_Store;
            ca.clearValue = {0.0, 0.0, 0.0, 1.0};
            WGPURenderPassDescriptor rp = {};
            rp.label = WGPU_STR("present blit");
            rp.colorAttachmentCount = 1;
            rp.colorAttachments = &ca;
            WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(s_frame.encoder, &rp);
            wgpuRenderPassEncoderSetPipeline(pass, s_presentBlitPipe);
            wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
            wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
            wgpuRenderPassEncoderEnd(pass);
            wgpuRenderPassEncoderRelease(pass);
            wgpuBindGroupRelease(bg);
        }

        // ========================================================================
        // Вспомогательные функции кадра
        // ========================================================================

        // The swapchain surface is configured once at open with the requested window
        // size. If the window manager resizes the window (e.g. a tiling WM), the
        // surface — and the depth attachment derived from winWidth/winHeight — go
        // stale, and the compositor scales our old-sized frame to fit (soft/blurry
        // text). Detect a size change and reconfigure. Only called when acquiring a
        // new swapchain texture (never while one is held mid-frame), so this happens
        // on a safe frame boundary. The main depth texture follows via ensureMainDepth.
        static void reconfigureSurfaceIfResized()
        {
            if (!wgpuGlobals.window) return;
            int fw = 0, fh = 0;
            glfwGetFramebufferSize(wgpuGlobals.window, &fw, &fh);
            if (fw <= 0 || fh <= 0) return; // minimized / zero-area: keep last config
            if (fw == wgpuGlobals.winWidth && fh == wgpuGlobals.winHeight) return;

            wgpuGlobals.winWidth = fw;
            wgpuGlobals.winHeight = fh;
            wgpuGlobals.surfaceConfig.width = (uint32_t)fw;
            wgpuGlobals.surfaceConfig.height = (uint32_t)fh;
            wgpuSurfaceConfigure(wgpuGlobals.surface, &wgpuGlobals.surfaceConfig);
        }

        // (Re)create the persistent main-framebuffer depth attachment on size OR
        // sample-count change. The depth attachment's sample count must match the
        // colour attachment's, so any MSAA toggle forces a recreate even at the
        // same window size.
        static void ensureMainDepth(uint32 fbw, uint32 fbh)
        {
            if (s_mainDepth.texture && s_mainDepth.width == fbw && s_mainDepth.height == fbh &&
                s_mainDepth.sampleCount == wgpuGlobals.numSamples)
                return;
            if (s_mainDepth.view) wgpuTextureViewRelease(s_mainDepth.view);
            if (s_mainDepth.texture) wgpuTextureRelease(s_mainDepth.texture);

            WGPUTextureDescriptor depthDesc = {};
            depthDesc.label = WGPU_STR("depth texture");
            depthDesc.size = {fbw, fbh, 1};
            depthDesc.format = WGPUTextureFormat_Depth32Float;
            depthDesc.usage = WGPUTextureUsage_RenderAttachment;
            depthDesc.dimension = WGPUTextureDimension_2D;
            depthDesc.mipLevelCount = 1;
            depthDesc.sampleCount = wgpuGlobals.numSamples;
            s_mainDepth.texture = wgpuDeviceCreateTexture(wgpuGlobals.device, &depthDesc);

            WGPUTextureViewDescriptor dvd = {};
            dvd.format = WGPUTextureFormat_Depth32Float;
            dvd.dimension = WGPUTextureViewDimension_2D;
            dvd.aspect = WGPUTextureAspect_All;
            dvd.baseMipLevel = 0;
            dvd.mipLevelCount = 1;
            dvd.baseArrayLayer = 0;
            dvd.arrayLayerCount = 1;
            s_mainDepth.view = wgpuTextureCreateView(s_mainDepth.texture, &dvd);
            s_mainDepth.width = fbw;
            s_mainDepth.height = fbh;
            s_mainDepth.sampleCount = wgpuGlobals.numSamples;
        }

        bool acquireFrame()
        {
            if (s_frame.inFrame) return true;

            // Acquire the swapchain texture once per displayed frame; a resumed frame
            // (after an offscreen interruption) keeps rendering onto the same texture.
            if (!s_swap.acquired)
            {
                reconfigureSurfaceIfResized();

                WGPUSurfaceTexture surfTex = {};
                wgpuSurfaceGetCurrentTexture(wgpuGlobals.surface, &surfTex);
                if (surfTex.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
                    surfTex.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)
                {
                    // Self-heal ONCE before giving up. A failed acquire is otherwise
                    // silently catastrophic: Camera::beginUpdate (librw) cannot report
                    // failure — the void Device callback just returns — so the game
                    // keeps "rendering": every clear/draw no-ops (no render pass) and
                    // showRaster presents nothing. The window then keeps showing
                    // stale garbage while the game believes it drew a frame (the
                    // movie player / loading screen black screen). Two standard
                    // remedies, both cheap: pump wgpu's internal event processing
                    // (lets the compositor/Wayland deliver buffer releases and the
                    // backend retire pending presents) and force a full surface
                    // unconfigure + reconfigure (the documented fix for an Outdated
                    // surface after a window mode/size change).
                    wgpuInstanceProcessEvents(wgpuGlobals.instance);
                    wgpuSurfaceUnconfigure(wgpuGlobals.surface);
                    wgpuSurfaceConfigure(wgpuGlobals.surface, &wgpuGlobals.surfaceConfig);
                    wgpuSurfaceGetCurrentTexture(wgpuGlobals.surface, &surfTex);
                }
                if (surfTex.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
                    surfTex.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)
                {
                    // Still failing — say so (throttled). A dropped frame should be
                    // visible in the log, not just on the screen.
                    static uint32_t s_acquireErrCount = 0;
                    if ((s_acquireErrCount++ % 60) == 0)
                        fprintf(stderr, "[wgpu] acquireFrame: surface acquire failed (status %d) — frame dropped\n",
                                (int)surfTex.status);
                    return false;
                }

                s_swap.texture = surfTex.texture;
                WGPUTextureViewDescriptor viewDesc = {};
                viewDesc.format = wgpuGlobals.surfaceFormat;
                viewDesc.dimension = WGPUTextureViewDimension_2D;
                viewDesc.baseMipLevel = 0;
                viewDesc.mipLevelCount = 1;
                viewDesc.baseArrayLayer = 0;
                viewDesc.arrayLayerCount = 1;
                viewDesc.aspect = WGPUTextureAspect_All;
                s_swap.view = wgpuTextureCreateView(s_swap.texture, &viewDesc);
                s_swap.acquired = true;
            }

            // Render into our offscreen backbuffer, not the swapchain texture: the GL
            // backend's surface supports only RENDER_ATTACHMENT, but rasterRenderFast()
            // (motion blur / postfx / droplets snapshots) copies the presented image
            // FROM the current target — impossible on the surface, fine on our own
            // texture (COPY_SRC). The acquired surface texture is only the present
            // target for the blit in presentFrame().
            ensureBackbuffer();
            s_frame.texture = s_backbuffer.texture;
            s_frame.view = s_backbuffer.view;

            uint32 fbw = (uint32_t)wgpuGlobals.winWidth;
            uint32 fbh = (uint32_t)wgpuGlobals.winHeight;
            ensureMainDepth(fbw, fbh);
            s_frame.depthTexture = s_mainDepth.texture;
            s_frame.depthView = s_mainDepth.view;

            s_frame.fbWidth = fbw;
            s_frame.fbHeight = fbh;
            s_frame.isOffscreen = false;
            s_frame.currentFrameBuffer = nullptr;

            WGPUCommandEncoderDescriptor encDesc = {};
            encDesc.label = WGPU_STR("frame encoder");
            s_frame.encoder = wgpuDeviceCreateCommandEncoder(wgpuGlobals.device, &encDesc);

            s_frame.inFrame = true;
            s_frame.inRenderPass = false;
            s_frame.renderPass = nullptr;
            return true;
        }

        bool acquireOffscreenFrame(Camera* cam)
        {
            if (s_frame.inFrame) return true;

            Raster* fb = cam->frameBuffer;
            if (!fb) return false;
            Raster* parent = fb->parent ? fb->parent : fb;

            WGPURaster* ext = GETWGPURASTEREXT(parent);
            if (!ext || !ext->texture) return false;

            s_frame.texture = ext->texture;
            s_frame.view = ext->view;

            // Per-frame depth texture for this offscreen size (released at submit;
            // s_frame.depth* is always null here — submitFrame clears it).
            uint32 fbw = (uint32_t)fb->width;
            uint32 fbh = (uint32_t)fb->height;

            WGPUTextureDescriptor depthDesc = {};
            depthDesc.label = WGPU_STR("offscreen depth");
            depthDesc.size = {fbw, fbh, 1};
            depthDesc.format = WGPUTextureFormat_Depth32Float;
            depthDesc.usage = WGPUTextureUsage_RenderAttachment;
            depthDesc.dimension = WGPUTextureDimension_2D;
            depthDesc.mipLevelCount = 1;
            depthDesc.sampleCount = 1;
            s_frame.depthTexture = wgpuDeviceCreateTexture(wgpuGlobals.device, &depthDesc);

            WGPUTextureViewDescriptor dvd = {};
            dvd.format = WGPUTextureFormat_Depth32Float;
            dvd.dimension = WGPUTextureViewDimension_2D;
            dvd.aspect = WGPUTextureAspect_All;
            dvd.baseMipLevel = 0;
            dvd.mipLevelCount = 1;
            dvd.baseArrayLayer = 0;
            dvd.arrayLayerCount = 1;
            s_frame.depthView = wgpuTextureCreateView(s_frame.depthTexture, &dvd);

            s_frame.fbWidth = fbw;
            s_frame.fbHeight = fbh;
            s_frame.isOffscreen = true;
            s_frame.currentFrameBuffer = parent;

            WGPUCommandEncoderDescriptor encDesc = {};
            encDesc.label = WGPU_STR("offscreen encoder");
            s_frame.encoder = wgpuDeviceCreateCommandEncoder(wgpuGlobals.device, &encDesc);

            s_frame.inFrame = true;
            s_frame.inRenderPass = false;
            s_frame.renderPass = nullptr;
            return true;
        }

        void endRenderPassIfOpen()
        {
            if (s_frame.inRenderPass && s_frame.renderPass)
            {
                wgpuRenderPassEncoderEnd(s_frame.renderPass);
                wgpuRenderPassEncoderRelease(s_frame.renderPass);
                s_frame.renderPass = nullptr;
                s_frame.inRenderPass = false;
            }
        }

        static void beginLoadRenderPass();

        // Confine a sub-rect camera (a sub-raster framebuffer, e.g. a mini-view inset)
        // to its rectangle of the parent framebuffer via viewport+scissor. Clamped to
        // the attachment so we never trip WGPU's scissor/viewport bounds.
        static void applySubRectViewport(Raster* fb)
        {
            if (!s_frame.inRenderPass || !fb || fb->width <= 0 || fb->height <= 0) return;
            uint32 x = (uint32)(fb->offsetX < 0 ? 0 : fb->offsetX);
            uint32 y = (uint32)(fb->offsetY < 0 ? 0 : fb->offsetY);
            uint32 w = (uint32)fb->width;
            uint32 h = (uint32)fb->height;
            if (x < s_frame.fbWidth && y < s_frame.fbHeight)
            {
                if (x + w > s_frame.fbWidth) w = s_frame.fbWidth - x;
                if (y + h > s_frame.fbHeight) h = s_frame.fbHeight - y;
                wgpuRenderPassEncoderSetViewport(s_frame.renderPass,
                                                 (float)x, (float)y, (float)w, (float)h, 0.0f, 1.0f);
                wgpuRenderPassEncoderSetScissorRect(s_frame.renderPass, x, y, w, h);
            }
        }

        // Finish and submit the current frame's commands. Does NOT present — a frame
        // may be submitted mid-display-frame to hand the GPU over to an offscreen
        // render-to-texture target (and later resumed onto the same swapchain
        // texture); presenting happens only in showRaster().
        static void submitFrame()
        {
            if (!s_frame.inFrame) return;
            endRenderPassIfOpen();
            ringsFlush(); // upload this segment's staged uniform + geometry data
            WGPUCommandBufferDescriptor cmdDesc = {};
            cmdDesc.label = WGPU_STR("frame commands");
            WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(s_frame.encoder, &cmdDesc);
            wgpuQueueSubmit(wgpuGlobals.queue, 1, &cmd);
            wgpuCommandBufferRelease(cmd);
            // Rewind the uniform ring (safe mid-frame too: the queue serialises
            // subsequent writeBuffer after the commands just submitted).
            uniformRingEndFrame();
            wgpuCommandEncoderRelease(s_frame.encoder);
            // Offscreen frames own their depth texture; the main frame's belongs to
            // the persistent s_mainDepth (contents must survive a mid-frame resume).
            if (s_frame.isOffscreen)
            {
                if (s_frame.depthView) wgpuTextureViewRelease(s_frame.depthView);
                if (s_frame.depthTexture) wgpuTextureRelease(s_frame.depthTexture);
            }
            s_frame.depthView = nullptr;
            s_frame.depthTexture = nullptr;
            s_frame.encoder = nullptr;
            s_frame.view = nullptr;
            s_frame.texture = nullptr;
            s_frame.inFrame = false;
        }

        // Present the acquired swapchain texture and release it. The backbuffer→surface
        // blit must be encoded before the final submit, so run it (on the main frame's
        // still-open encoder) before submitFrame().
        static void presentFrame()
        {
            if (s_frame.inFrame && !s_frame.isOffscreen)
            {
                endRenderPassIfOpen();
                blitBackbufferToSwapchain(); // backbuffer → surface, alpha forced to 1
            }
            submitFrame();
            if (s_swap.acquired)
            {
                wgpuSurfacePresent(wgpuGlobals.surface);
                if (s_swap.view) wgpuTextureViewRelease(s_swap.view);
                if (s_swap.texture) wgpuTextureRelease(s_swap.texture);
                s_swap.view = nullptr;
                s_swap.texture = nullptr;
                s_swap.acquired = false;
            }
        }

        // ========================================================================
        // Функции рендеринга Camera
        // ========================================================================

        // Current render-target sample count. Returns wgpuGlobals.numSamples when
        // rendering to the main backbuffer (MSAA), 1 for offscreen rasters.
        // Called by every pipeline cache lookup so MSAA and non-MSAA variants
        // get separate cache entries (key bit 28+).
        uint32 currentSampleCount(void)
        {
            return s_frame.isOffscreen ? 1u : wgpuGlobals.numSamples;
        }

        // Apply a pending MSAA change (set via DEVICESETMULTISAMPLINGLEVELS).
        // Called at the start of beginUpdate — at this point the previous
        // frame's encoder has been submitted (in endUpdate) and no encoder
        // is in flight for this frame yet. We:
        //   1. Process pending instance events to drain any in-flight work.
        //   2. Release the backbuffer / depth / resolve-target textures.
        //      They'll be recreated by ensureBackbuffer / ensureMainDepth on
        //      the next render pass with the new sampleCount.
        //   3. Flush every subsystem's render-pipeline cache. Pipelines are
        //      sample-count-specific in wgpu (the WGPURenderPipelineDescriptor
        //      has a multisample.count that must match the attachment). The
        //      next draw will lazily recreate them via the existing
        //      getOrCreate* path.
        // Cost: ~10-50ms freeze (pipeline recompilation). Acceptable in the
        // pause menu where the user is changing settings; would be jarring
        // mid-game.
        static void applyPendingMsaaChange(void)
        {
            if (!wgpuGlobals.msaaDirty) return;
            wgpuGlobals.msaaDirty = false;

            // Apply the pending sample count. This is where numSamples actually
            // changes — NOT in DEVICESETMULTISAMPLINGLEVELS. By deferring the
            // swap to here (start of beginUpdate, before any draw calls), we
            // ensure that during the frame where the user changed MSAA:
            //   * currentSampleCount() still returns the OLD numSamples
            //   * pipelines still match the OLD backbuffer
            //   * no sample-count mismatch crash
            // The new numSamples takes effect for THIS frame's backbuffer
            // recreation + pipeline cache flush.
            wgpuGlobals.numSamples = wgpuGlobals.pendingNumSamples;
            // 1. Drain pending GPU work. wgpuInstanceProcessEvents pumps the
            //    event loop so any pending callbacks / surface releases land
            //    before we tear down textures. (wgpu-native has no per-device
            //    "tick" — instance-level processing is the documented way.)
            for (int i = 0; i < 4; i++) wgpuInstanceProcessEvents(wgpuGlobals.instance);

            // 2. Release backbuffer + resolve target + main depth. Setting
            //    width/height to 0 forces ensureBackbuffer / ensureMainDepth
            //    to recreate on next use with the new sample count.
            if (s_backbuffer.view)            { wgpuTextureViewRelease(s_backbuffer.view);            s_backbuffer.view = nullptr; }
            if (s_backbuffer.texture)         { wgpuTextureRelease(s_backbuffer.texture);            s_backbuffer.texture = nullptr; }
            if (s_backbuffer.resolveView)     { wgpuTextureViewRelease(s_backbuffer.resolveView);    s_backbuffer.resolveView = nullptr; }
            if (s_backbuffer.resolveTexture) { wgpuTextureRelease(s_backbuffer.resolveTexture);    s_backbuffer.resolveTexture = nullptr; }
            s_backbuffer.width = 0;
            s_backbuffer.height = 0;
            s_backbuffer.sampleCount = 0; // forces ensureBackbuffer to recreate

            if (s_mainDepth.view)    { wgpuTextureViewRelease(s_mainDepth.view);    s_mainDepth.view = nullptr; }
            if (s_mainDepth.texture) { wgpuTextureRelease(s_mainDepth.texture);    s_mainDepth.texture = nullptr; }
            s_mainDepth.width = 0;
            s_mainDepth.height = 0;
            s_mainDepth.sampleCount = 0; // forces ensureMainDepth to recreate

            // 3. Flush all render-pipeline caches.
            flushIm2DPipelines();
            flushIm2DTextPipelines();
            flushIm3DPipelines();
            flushMatfxPipelines();
            flushSkinCache();
            flushPipelineCache();

            // 4. Release the depth-clear pipe (uses numSamples). The present
            //    blit pipe stays — it renders into the swapchain (1-sample).
            if (s_depthClearPipe) { wgpuRenderPipelineRelease(s_depthClearPipe); s_depthClearPipe = nullptr; }
        }

        static void beginUpdate(Camera* cam)
        {
            // Apply any pending MSAA change BEFORE this frame's rendering starts.
            // The recreation is safe here: the previous frame's encoder was
            // submitted in endUpdate and no encoder is in flight yet.
            applyPendingMsaaChange();

            // Строим матрицы из камеры — как делает GL3 backend.
            // Матрицы сохраняем в cam->devView и cam->devProj
            // (они используются im3DTransform при каждом вызове).
            float view[16];
            Matrix inv;
            Matrix::invert(&inv, cam->getFrame()->getLTM());
            // Flip X для left-handed view space (librw convention, как в GL3)
            view[0] = -inv.right.x;
            view[1] = inv.right.y;
            view[2] = inv.right.z;
            view[3] = 0.0f;
            view[4] = -inv.up.x;
            view[5] = inv.up.y;
            view[6] = inv.up.z;
            view[7] = 0.0f;
            view[8] = -inv.at.x;
            view[9] = inv.at.y;
            view[10] = inv.at.z;
            view[11] = 0.0f;
            view[12] = -inv.pos.x;
            view[13] = inv.pos.y;
            view[14] = inv.pos.z;
            view[15] = 1.0f;
            memcpy(&cam->devView, view, sizeof(RawMatrix));

            float proj[16] = {};
            float32 invwx = 1.0f / cam->viewWindow.x;
            float32 invwy = 1.0f / cam->viewWindow.y;
            float32 invz = 1.0f / (cam->farPlane - cam->nearPlane);
            proj[0] = invwx;
            proj[5] = invwy;
            proj[8] = cam->viewOffset.x * invwx;
            proj[9] = cam->viewOffset.y * invwy;
            proj[12] = -proj[8];
            proj[13] = -proj[9];
            // WebGPU/D3D clip-space depth range is [0,1] (NOT OpenGL's [-1,1]). Build the
            // projection so z_view in [near,far] maps to z_ndc in [0,1]:
            //   z_ndc = f/(f-n) - f*n/((f-n)*z)
            // This matches RW's im2d screen-Z convention (CSprite computes screenz with
            // device zNear=0/zFar=1 the same way), so depth-tested im2d — e.g. coronas —
            // occlude correctly against the scene depth buffer. Using the GL [-1,1] form
            // here both broke that comparison (coronas vanished) and clipped the near half
            // of the scene (see-through geometry close to the camera).
            if (cam->projection == Camera::PERSPECTIVE)
            {
                proj[10] = cam->farPlane * invz;
                proj[11] = 1.0f;
                proj[14] = -cam->nearPlane * cam->farPlane * invz;
                proj[15] = 0.0f;
            }
            else
            {
                proj[10] = invz;
                proj[11] = 0.0f;
                proj[14] = -cam->nearPlane * invz;
                proj[15] = 1.0f;
            }
            memcpy(&cam->devProj, proj, sizeof(RawMatrix));

            // Make this camera's framebuffer the active render target. Normally the
            // preceding clear() already did this, but a camera may begin an update
            // without a clear — e.g. the neo env map does EndUpdate(main) → render to
            // texture → BeginUpdate(main), and rendering must resume on the main
            // framebuffer (contents preserved), not stay on the last target.
            Raster* fb = cam->frameBuffer;
            Raster* parent = (fb && fb->parent) ? fb->parent : fb;
            bool offscreen = parent && parent->type == Raster::CAMERATEXTURE;
            Raster* target = offscreen ? parent : nullptr;

            if (s_frame.inFrame && s_frame.currentFrameBuffer != target)
                submitFrame();
            if (!s_frame.inFrame)
            {
                if (target) acquireOffscreenFrame(cam);
                else acquireFrame();
            }
            // Resumed without a clear: reopen a Load pass so draws have somewhere to go.
            if (s_frame.inFrame && !s_frame.inRenderPass)
            {
                beginLoadRenderPass();
                if (!offscreen && fb && fb->parent && fb->parent != fb)
                    applySubRectViewport(fb);
            }
        }

        static void endUpdate(Camera* cam)
        {
            // endUpdate just ends the current render pass.
            // Command submission and present happen in showRaster().
            endRenderPassIfOpen();
        }

        // (P2-15) Sub-rect camera Z-clear. WebGPU's loadOp=Clear wipes the WHOLE
        // depth attachment (scissor does not clip clears), so a mini-view inset
        // clearing Z would erase the parent scene's depth. Instead the parent
        // depth is LOADED and a depth-only quad (no fragment stage) writes z=1
        // inside the sub-rect's viewport+scissor — the WebGPU equivalent of
        // glClear(GL_DEPTH_BUFFER_BIT) under a scissor test.
        // s_depthClearPipe is declared above (near s_presentBlitPipe) so that
        // applyPendingMsaaChange can release it on MSAA change.
        static const char* k_depthClearWGSL = R"(
@vertex fn vs(@builtin(vertex_index) i : u32) -> @builtin(position) vec4<f32> {
    var p = array<vec2<f32>,3>(vec2<f32>(-1.0,-1.0), vec2<f32>(3.0,-1.0), vec2<f32>(-1.0,3.0));
    return vec4<f32>(p[i], 1.0, 1.0); // z = 1.0 = depthClearValue
}
)";

        static void ensureDepthClearPipe(void)
        {
            if (s_depthClearPipe) return;
            WGPUDevice dev = wgpuGlobals.device;

            WGPUShaderSourceWGSL src = {};
            src.chain.sType = WGPUSType_ShaderSourceWGSL;
            src.code = WGPU_STR(k_depthClearWGSL);
            WGPUShaderModuleDescriptor smd = {};
            smd.nextInChain = &src.chain;
            WGPUShaderModule sm = wgpuDeviceCreateShaderModule(dev, &smd);

            WGPUPipelineLayoutDescriptor plDesc = {};
            plDesc.label = WGPU_STR("depth clear pl");
            plDesc.bindGroupLayoutCount = 0;
            plDesc.bindGroupLayouts = nullptr;
            WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(dev, &plDesc);

            WGPUDepthStencilState ds = {};
            ds.format = WGPUTextureFormat_Depth32Float;
            ds.depthWriteEnabled = WGPUOptionalBool_True;
            ds.depthCompare = WGPUCompareFunction_Always;
            ds.stencilFront = WGPU_STENCIL_FACE_STATE_INIT;
            ds.stencilBack = WGPU_STENCIL_FACE_STATE_INIT;
            ds.stencilReadMask = 0;
            ds.stencilWriteMask = 0;

            WGPURenderPipelineDescriptor pd = {};
            pd.label = WGPU_STR("depth clear quad");
            pd.layout = pl;
            pd.vertex.module = sm;
            pd.vertex.entryPoint = WGPU_STR("vs");
            pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
            pd.primitive.stripIndexFormat = WGPUIndexFormat_Undefined;
            pd.primitive.frontFace = WGPUFrontFace_CCW;
            pd.primitive.cullMode = WGPUCullMode_None;
            pd.depthStencil = &ds;
            pd.multisample.count = currentSampleCount();
            pd.multisample.mask = 0xFFFFFFFFu;
            // no fragment state — depth-only pipeline
            s_depthClearPipe = wgpuDeviceCreateRenderPipeline(dev, &pd);

            wgpuPipelineLayoutRelease(pl);
            wgpuShaderModuleRelease(sm);
        }

        static void clearCamera(Camera* cam, RGBA* col, uint32 mode)
        {
            // Apply any pending MSAA change BEFORE acquiring the frame and opening
            // the render pass. RwCameraClear is called BEFORE RsCameraBeginUpdate in
            // reVC's main loop (FrontendIdle / Idle both do RwCameraClear then
            // RsCameraBeginUpdate), so the render pass is opened HERE, not in
            // beginUpdate. If we don't apply the pending sample count first, the
            // backbuffer / depth / resolve-target textures are still at the OLD
            // sample count when the render pass is created; then beginUpdate's
            // applyPendingMsaaChange swaps numSamples, flushes the pipeline caches,
            // and subsequent im2d draws use pipelines with the NEW sample count on
            // a render pass with the OLD sample count — instant "Incompatible
            // sample count" validation error at wgpuQueueSubmit. Calling it here
            // (and again in beginUpdate, which is a no-op once msaaDirty=false)
            // keeps everything in lockstep.
            applyPendingMsaaChange();

            // Determine the render target for this camera
            Raster* fb = cam->frameBuffer;
            Raster* parent = (fb && fb->parent) ? fb->parent : fb;
            bool offscreen = parent && parent->type == Raster::CAMERATEXTURE;
            Raster* target = offscreen ? parent : nullptr;

            // A non-offscreen camera whose framebuffer is a sub-raster (e.g. the
            // mini-view inset) renders into a sub-rectangle of its parent's frame. We
            // must NOT clear the whole colour attachment (that wipes the parent + any
            // UI already drawn there) and must confine drawing via viewport+scissor.
            // NB: Raster::create sets parent = self by default, so a non-null parent is
            // NOT enough — a genuine sub-raster has parent != self (see gl3
            // getFramebufferRect). Using fb->parent != nullptr here wrongly treats the
            // MAIN camera as a sub-rect, which suppresses its colour clear (loadOp=Load)
            // and installs a redundant viewport.
            bool subRect = !offscreen && fb && fb->parent && fb->parent != fb;

            // If already in a frame but rendering to a different target, finalize and restart
            if (s_frame.inFrame && s_frame.currentFrameBuffer != target)
            {
                submitFrame();
            }

            if (!s_frame.inFrame)
            {
                if (target)
                {
                    acquireOffscreenFrame(cam);
                }
                else
                {
                    acquireFrame();
                }
            }
            if (!s_frame.inFrame) return;

            endRenderPassIfOpen();

            bool clearColor = (mode & Camera::CLEARIMAGE) != 0;
            bool clearDepth = (mode & Camera::CLEARZ) != 0;

            WGPURenderPassColorAttachment colorAtt = {};
            colorAtt.view = s_frame.view;
            colorAtt.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
            // MSAA: write the resolved color into the non-MSAA resolve target.
            // In WebGPU, the resolve happens automatically when resolveTarget is
            // set — storeOp only controls what happens to the MSAA texture itself
            // (Store = keep the multisample content, Discard = throw it away).
            // We use Store here because it's universally available; the multisample
            // texture is never read directly (the present blit samples resolveView).
            // MSAA resolve only for the main backbuffer — offscreen rasters
            // are non-MSAA (sampleCount=1) and must not set resolveTarget.
            bool isMsaaPass = !s_frame.isOffscreen && wgpuGlobals.numSamples > 1;
            if (isMsaaPass) {
                colorAtt.resolveTarget = s_backbuffer.resolveView;
                colorAtt.storeOp = WGPUStoreOp_Store;
            } else {
                colorAtt.resolveTarget = nullptr;
                colorAtt.storeOp = WGPUStoreOp_Store;
            }
            // Sub-rect cameras preserve the parent's colour (WGPU clears whole
            // attachments, so a Clear here would wipe everything outside the sub-rect).
            colorAtt.loadOp = (clearColor && !subRect) ? WGPULoadOp_Clear : WGPULoadOp_Load;
            if (col)
            {
                colorAtt.clearValue = {
                    col->red / 255.0, col->green / 255.0,
                    col->blue / 255.0, col->alpha / 255.0
                };
            }
            else
            {
                colorAtt.clearValue = {0.0, 0.0, 0.0, 1.0};
            }

            WGPURenderPassDepthStencilAttachment depthAtt = {};
            depthAtt.view = s_frame.depthView;
            depthAtt.depthLoadOp = (clearDepth && !subRect) ? WGPULoadOp_Clear : WGPULoadOp_Load;
            depthAtt.depthStoreOp = WGPUStoreOp_Store;
            depthAtt.depthClearValue = 1.0f;
            depthAtt.depthReadOnly = false;
            depthAtt.stencilLoadOp = WGPULoadOp_Undefined;
            depthAtt.stencilStoreOp = WGPUStoreOp_Undefined;
            depthAtt.stencilClearValue = 0;
            depthAtt.stencilReadOnly = false;

            WGPURenderPassDescriptor rpDesc = {};
            rpDesc.label = WGPU_STR("main pass");
            rpDesc.colorAttachmentCount = 1;
            rpDesc.colorAttachments = &colorAtt;
            rpDesc.depthStencilAttachment = &depthAtt;

            s_frame.renderPass = wgpuCommandEncoderBeginRenderPass(s_frame.encoder, &rpDesc);
            s_frame.inRenderPass = true;

            if (subRect)
            {
                applySubRectViewport(fb);
                // (P2-15) Clear Z inside the sub-rect only, via the depth-only quad.
                if (clearDepth)
                {
                    ensureDepthClearPipe();
                    wgpuRenderPassEncoderSetPipeline(s_frame.renderPass, s_depthClearPipe);
                    wgpuRenderPassEncoderDraw(s_frame.renderPass, 3, 1, 0, 0);
                }
            }
        }

        static void showRaster(Raster* raster, uint32 flags)
        {
            presentFrame();
        }

        // Reopen a Load render pass on the current frame's colour+depth attachments,
        // preserving their contents. Used to resume rendering after an encoder-level
        // operation (a texture copy) that must run outside any render pass.
        static void beginLoadRenderPass()
        {
            WGPURenderPassColorAttachment colorAtt = {};
            colorAtt.view = s_frame.view;
            colorAtt.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
            colorAtt.loadOp = WGPULoadOp_Load;
            colorAtt.storeOp = WGPUStoreOp_Store;
            // Mirror clearCamera's MSAA-resolve wiring: when the main backbuffer
            // is MSAA, the load pass also needs a resolveTarget so the resolve
            // texture stays current with the MSAA colour attachment. Without
            // this, a mid-frame endRenderPassIfOpen() (e.g. rasterRenderFast's
            // texture copy) would NOT trigger a resolve, and the next
            // rasterRenderFast would copy STALE resolve data into the snapshot
            // destination — visible as a one-frame-behind motion-blur trail.
            // Offscreen frames stay sample count 1 (no resolveTarget).
            bool isMsaaPass = !s_frame.isOffscreen && wgpuGlobals.numSamples > 1;
            if (isMsaaPass) {
                colorAtt.resolveTarget = s_backbuffer.resolveView;
                // storeOp=Store keeps the MSAA content (we don't read it directly
                // — the present blit samples resolveView — but Store is universal).
            }

            WGPURenderPassDepthStencilAttachment depthAtt = {};
            depthAtt.view = s_frame.depthView;
            depthAtt.depthLoadOp = WGPULoadOp_Load;
            depthAtt.depthStoreOp = WGPUStoreOp_Store;
            depthAtt.depthReadOnly = false;
            depthAtt.stencilLoadOp = WGPULoadOp_Undefined;
            depthAtt.stencilStoreOp = WGPUStoreOp_Undefined;
            depthAtt.stencilReadOnly = false;

            WGPURenderPassDescriptor rpDesc = {};
            rpDesc.label = WGPU_STR("resume load pass");
            rpDesc.colorAttachmentCount = 1;
            rpDesc.colorAttachments = &colorAtt;
            rpDesc.depthStencilAttachment = &depthAtt;
            s_frame.renderPass = wgpuCommandEncoderBeginRenderPass(s_frame.encoder, &rpDesc);
            s_frame.inRenderPass = true;
        }

        // Copy the presented (or offscreen) frame into the current-context raster.
        // GTA's motion-blur trails do RwRasterPushContext(pFrontBuffer) +
        // RwRasterRenderFast(cameraRaster, 0, 0) to snapshot the frame for next-frame
        // blending; postfx GetBackBuffer works the same way. src == a CAMERA raster
        // (main camera → the offscreen backbuffer held in s_frame; offscreen → its own
        // texture); dst == a CAMERATEXTURE. The copy is an encoder-level op so it must
        // sit outside a render pass — we close the open pass and reopen a Load pass so
        // subsequent im2d (HUD/2D) keeps drawing over the same frame.
        static bool32 rasterRenderFast(Raster* raster, int32 x, int32 y)
        {
            if (!s_frame.inFrame || !s_frame.encoder) return 0;
            if (x < 0 || y < 0) return 0;

            Raster* dst = Raster::getCurrentContext();
            if (!dst) return 0;
            WGPURaster* natdst = GETWGPURASTEREXT(dst);
            if (!natdst || !natdst->texture) return 0;

            // Source texture: an offscreen raster owns its texture; the main camera's
            // framebuffer has none (the offscreen backbuffer lives in s_frame and,
            // unlike the swapchain texture, carries COPY_SRC on every backend).
            WGPURaster* natsrc = GETWGPURASTEREXT(raster);
            WGPUTexture srcTex;
            int srcBranch = 0; // 1=natsrc->texture, 2=resolveTexture, 3=s_frame.texture
            if (natsrc && natsrc->texture) {
                // Offscreen source raster — always 1-sample, matches dst.
                srcTex = natsrc->texture;
                srcBranch = 1;
            } else if (!s_frame.isOffscreen && wgpuGlobals.numSamples > 1 &&
                       s_backbuffer.resolveTexture) {
                // Main camera framebuffer + MSAA on: s_frame.texture is the MSAA
                // colour attachment (numSamples samples). Copying it into a 1-sample
                // CAMERATEXTURE destination is a wgpu validation error
                // ("Source sample count (N) and destination sample count (1) are not
                // equal"). Use the resolve target instead — endRenderPassIfOpen()
                // below triggers the MSAA resolve, so the resolve texture holds the
                // averaged 1-sample image by the time the copy runs.
                srcTex = s_backbuffer.resolveTexture;
                srcBranch = 2;
            } else if (!s_frame.isOffscreen && wgpuGlobals.numSamples > 1 &&
                       !s_backbuffer.resolveTexture) {
                // MSAA is on but the resolve texture is null — this should never
                // happen (ensureBackbuffer creates it whenever numSamples > 1), but
                // if it does (e.g. mid-frame MSAA change race with no backbuffer
                // recreation yet), bail out instead of falling through to the
                // 2-sample s_frame.texture and crashing the queue submit.
                return 0;
            } else {
                // Non-MSAA main backbuffer (sample count 1) — direct copy is safe.
                srcTex = s_frame.texture;
                srcBranch = 3;
            }
            if (!srcTex) return 0;

            uint32 w = raster->width > 0 ? (uint32)raster->width : s_frame.fbWidth;
            uint32 h = raster->height > 0 ? (uint32)raster->height : s_frame.fbHeight;
            // Clamp the copy to both textures' actual sizes (src and dst may differ,
            // and the raster's nominal size can disagree with the GPU texture).
            uint32 srcW = wgpuTextureGetWidth(srcTex), srcH = wgpuTextureGetHeight(srcTex);
            if (w > srcW) w = srcW;
            if (h > srcH) h = srcH;
            if ((uint32)x >= (uint32)dst->width || (uint32)y >= (uint32)dst->height) return 0;
            if ((uint32)x + w > (uint32)dst->width) w = (uint32)dst->width - (uint32)x;
            if ((uint32)y + h > (uint32)dst->height) h = (uint32)dst->height - (uint32)y;
            if (w == 0 || h == 0) return 0;

            bool wasInPass = s_frame.inRenderPass;
            endRenderPassIfOpen();

            WGPUTexelCopyTextureInfo srcInfo = {};
            srcInfo.texture = srcTex;
            srcInfo.mipLevel = 0;
            srcInfo.origin = {0, 0, 0};
            srcInfo.aspect = WGPUTextureAspect_All;
            WGPUTexelCopyTextureInfo dstInfo = {};
            dstInfo.texture = natdst->texture;
            dstInfo.mipLevel = 0;
            dstInfo.origin = {(uint32)x, (uint32)y, 0};
            dstInfo.aspect = WGPUTextureAspect_All;
            WGPUExtent3D copySize = {w, h, 1};
            wgpuCommandEncoderCopyTextureToTexture(s_frame.encoder, &srcInfo, &dstInfo, &copySize);

            if (wasInPass) beginLoadRenderPass();
            return 1;
        }

        // ========================================================================
        // Lighting — gather the world lights affecting an atomic (mirrors gl3
        // enumerateLights + setLights). Output matches the WGSL light uniform block.
        // ========================================================================
        void gatherLights(Atomic* atomic, LightDataGPU* out)
        {
            memset(out, 0, sizeof(*out));
            out->ambient[3] = 1.0f;
            out->params[0] = 0.0f; // type 0 = list terminator (no lights)

            World* world = (World*)engine->currentWorld;
            if (world == nullptr || (atomic->geometry->flags & Geometry::LIGHT) == 0)
                return;

            WorldLights lightData;
            Light* directionals[WGPU_MAX_LIGHTS];
            Light* locals[WGPU_MAX_LIGHTS];
            lightData.directionals = directionals;
            lightData.numDirectionals = WGPU_MAX_LIGHTS;
            lightData.locals = locals;
            lightData.numLocals = WGPU_MAX_LIGHTS;
            world->enumerateLights(atomic, &lightData);

            out->ambient[0] = lightData.ambient.red;
            out->ambient[1] = lightData.ambient.green;
            out->ambient[2] = lightData.ambient.blue;
            out->ambient[3] = 1.0f;

            int n = 0;
            for (int32 i = 0; i < lightData.numDirectionals && n < WGPU_MAX_LIGHTS; i++)
            {
                Light* l = lightData.directionals[i];
                Matrix* m = l->getFrame()->getLTM();
                out->params[n * 4 + 0] = 1.0f;
                out->color[n * 4 + 0] = l->color.red;
                out->color[n * 4 + 1] = l->color.green;
                out->color[n * 4 + 2] = l->color.blue;
                out->color[n * 4 + 3] = 1.0f;
                out->direction[n * 4 + 0] = m->at.x;
                out->direction[n * 4 + 1] = m->at.y;
                out->direction[n * 4 + 2] = m->at.z;
                n++;
            }
            for (int32 i = 0; i < lightData.numLocals && n < WGPU_MAX_LIGHTS; i++)
            {
                Light* l = lightData.locals[i];
                Matrix* m = l->getFrame()->getLTM();
                int32 t = l->getType();
                if (t == Light::POINT)
                {
                    out->params[n * 4 + 0] = 2.0f;
                    out->params[n * 4 + 1] = l->radius;
                    out->position[n * 4 + 0] = m->pos.x;
                    out->position[n * 4 + 1] = m->pos.y;
                    out->position[n * 4 + 2] = m->pos.z;
                }
                else if (t == Light::SPOT || t == Light::SOFTSPOT)
                {
                    out->params[n * 4 + 0] = 3.0f;
                    out->params[n * 4 + 1] = l->radius;
                    out->params[n * 4 + 2] = l->minusCosAngle;
                    out->params[n * 4 + 3] = (t == Light::SOFTSPOT) ? 0.0f : 1.0f;
                    out->position[n * 4 + 0] = m->pos.x;
                    out->position[n * 4 + 1] = m->pos.y;
                    out->position[n * 4 + 2] = m->pos.z;
                    out->direction[n * 4 + 0] = m->at.x;
                    out->direction[n * 4 + 1] = m->at.y;
                    out->direction[n * 4 + 2] = m->at.z;
                }
                else
                    continue;
                out->color[n * 4 + 0] = l->color.red;
                out->color[n * 4 + 1] = l->color.green;
                out->color[n * 4 + 2] = l->color.blue;
                out->color[n * 4 + 3] = 1.0f;
                n++;
            }
            if (n < WGPU_MAX_LIGHTS)
                out->params[n * 4 + 0] = 0.0f; // terminator
        }

        // ========================================================================
        // Render State
        // ========================================================================
#define RS_U(v)   ((uint32)(uintptr_t)(v))
#define RS_PTR(v) ((void*)(uintptr_t)(v))

        static void setRenderState(int32 state, void* pvalue)
        {
            uint32 v = RS_U(pvalue);
            switch (state)
            {
            case TEXTURERASTER: s_renderState.currentRaster = (Raster*)pvalue;
                break;
            case TEXTUREADDRESS: s_renderState.textureAddress = v;
                s_renderState.textureAddressU = v;
                s_renderState.textureAddressV = v;
                break;
            case TEXTUREADDRESSU: s_renderState.textureAddressU = v;
                break;
            case TEXTUREADDRESSV: s_renderState.textureAddressV = v;
                break;
            case TEXTUREFILTER: s_renderState.textureFilter = v;
                break;
            case VERTEXALPHA: s_renderState.vertexAlpha = v;
                break;
            case SRCBLEND: s_renderState.srcBlend = v;
                break;
            case DESTBLEND: s_renderState.destBlend = v;
                break;
            case ZTESTENABLE: s_renderState.zTest = v;
                break;
            case ZWRITEENABLE: s_renderState.zWrite = v;
                break;
            case FOGENABLE: s_renderState.fogEnable = v;
                break;
            case FOGCOLOR: s_renderState.fogColor = *(RGBA*)&v;
                break;
            case CULLMODE: s_renderState.cullMode = v;
                break;
            case STENCILENABLE: s_renderState.stencilEnable = v;
                break;
            case STENCILFAIL: s_renderState.stencilFail = v;
                break;
            case STENCILZFAIL: s_renderState.stencilZFail = v;
                break;
            case STENCILPASS: s_renderState.stencilPass = v;
                break;
            case STENCILFUNCTION: s_renderState.stencilFunc = v;
                break;
            case STENCILFUNCTIONREF: s_renderState.stencilFuncRef = v;
                break;
            case STENCILFUNCTIONMASK: s_renderState.stencilFuncMask = v;
                break;
            case STENCILFUNCTIONWRITEMASK: s_renderState.stencilFuncWriteMask = v;
                break;
            case ALPHATESTFUNC: s_renderState.alphaTestFunc = v;
                break;
            case ALPHATESTREF: s_renderState.alphaTestRef = v;
                break;
            case GSALPHATEST: s_renderState.gsAlphaTest = v;
                break;
            case GSALPHATESTREF: s_renderState.gsAlphaTestRef = v;
                break;
            }
        }

        static void* getRenderState(int32 state)
        {
            switch (state)
            {
            case TEXTURERASTER: return s_renderState.currentRaster;
            case TEXTUREADDRESS: return RS_PTR(s_renderState.textureAddress);
            case TEXTUREADDRESSU: return RS_PTR(s_renderState.textureAddressU);
            case TEXTUREADDRESSV: return RS_PTR(s_renderState.textureAddressV);
            case TEXTUREFILTER: return RS_PTR(s_renderState.textureFilter);
            case VERTEXALPHA: return RS_PTR(s_renderState.vertexAlpha);
            case SRCBLEND: return RS_PTR(s_renderState.srcBlend);
            case DESTBLEND: return RS_PTR(s_renderState.destBlend);
            case ZTESTENABLE: return RS_PTR(s_renderState.zTest);
            case ZWRITEENABLE: return RS_PTR(s_renderState.zWrite);
            case FOGENABLE: return RS_PTR(s_renderState.fogEnable);
            case FOGCOLOR:
                {
                    uint32 c;
                    *(RGBA*)&c = s_renderState.fogColor;
                    return RS_PTR(c);
                }
            case CULLMODE: return RS_PTR(s_renderState.cullMode);
            case STENCILENABLE: return RS_PTR(s_renderState.stencilEnable);
            case STENCILFAIL: return RS_PTR(s_renderState.stencilFail);
            case STENCILZFAIL: return RS_PTR(s_renderState.stencilZFail);
            case STENCILPASS: return RS_PTR(s_renderState.stencilPass);
            case STENCILFUNCTION: return RS_PTR(s_renderState.stencilFunc);
            case STENCILFUNCTIONREF: return RS_PTR(s_renderState.stencilFuncRef);
            case STENCILFUNCTIONMASK: return RS_PTR(s_renderState.stencilFuncMask);
            case STENCILFUNCTIONWRITEMASK: return RS_PTR(s_renderState.stencilFuncWriteMask);
            case ALPHATESTFUNC: return RS_PTR(s_renderState.alphaTestFunc);
            case ALPHATESTREF: return RS_PTR(s_renderState.alphaTestRef);
            case GSALPHATEST: return RS_PTR(s_renderState.gsAlphaTest);
            case GSALPHATESTREF: return RS_PTR(s_renderState.gsAlphaTestRef);
            default: return nullptr;
            }
        }

        // Map rw BlendFunction -> WGPUBlendFactor
        WGPUBlendFactor toWGPUBlendFactor(uint32 rwBlend)
        {
            switch (rwBlend)
            {
            case BLENDZERO: return WGPUBlendFactor_Zero;
            case BLENDONE: return WGPUBlendFactor_One;
            case BLENDSRCCOLOR: return WGPUBlendFactor_Src;
            case BLENDINVSRCCOLOR: return WGPUBlendFactor_OneMinusSrc;
            case BLENDSRCALPHA: return WGPUBlendFactor_SrcAlpha;
            case BLENDINVSRCALPHA: return WGPUBlendFactor_OneMinusSrcAlpha;
            case BLENDDESTALPHA: return WGPUBlendFactor_DstAlpha;
            case BLENDINVDESTALPHA: return WGPUBlendFactor_OneMinusDstAlpha;
            case BLENDDESTCOLOR: return WGPUBlendFactor_Dst;
            case BLENDINVDESTCOLOR: return WGPUBlendFactor_OneMinusDst;
            case BLENDSRCALPHASAT: return WGPUBlendFactor_SrcAlphaSaturated;
            default: return WGPUBlendFactor_One;
            }
        }

        // Set RW default render states (parity with gl3 resetRenderState)
        void resetRenderState(void)
        {
            s_renderState.textureAddress = Texture::WRAP;
            s_renderState.textureAddressU = Texture::WRAP;
            s_renderState.textureAddressV = Texture::WRAP;
            s_renderState.textureFilter = Texture::LINEAR;
            s_renderState.cullMode = CULLNONE;
            s_renderState.zTest = 1;
            s_renderState.zWrite = 1;
            s_renderState.vertexAlpha = 0;
            s_renderState.srcBlend = BLENDSRCALPHA;
            s_renderState.destBlend = BLENDINVSRCALPHA;
            s_renderState.fogEnable = 0;
            s_renderState.fogColor = {255, 255, 255, 255};
            s_renderState.alphaTestFunc = ALPHAGREATEREQUAL;
            s_renderState.alphaTestRef = 10;
            s_renderState.gsAlphaTest = 0;
            s_renderState.gsAlphaTestRef = 128; // librw default
            s_renderState.stencilEnable = 0;
        }

        // ========================================================================
        // Adapter / Device request (synchronous via ProcessEvents loop)
        // ========================================================================
        // wgpu-native v29 не поддерживает TimedWaitAny — используем
        // WGPUCallbackMode_AllowSpontaneous + wgpuInstanceProcessEvents в цикле.
        // Колбэк вызывается на первом же ProcessEvents после готовности GPU.

        // Probe whether the device actually supports a given sample count for a
        // given texture format. The WebGPU spec only GUARANTEES sample counts 1
        // and 4 for color attachments on byte unorm formats; 2, 8 and 16 are
        // optional and depend on the GPU/driver (e.g. AMD Vega iGPU supports up
        // to 8 on Vulkan, but some Intel iGPUs only support 1 and 4). The
        // previous code hardcoded 8 (if TextureAdapterSpecificFormatFeatures
        // feature was enabled) or 4 — both wrong on real hardware: a device
        // that "has the feature" doesn't necessarily support 8x MSAA on the
        // surface format we actually picked, and a device without the feature
        // might still support 2x.
        //
        // This probe actually creates a tiny 1x1 texture with the requested
        // sample count, captures any validation error via an error scope, and
        // returns true if creation succeeded without a validation error.
        //
        // Cheap (creates + releases a 1x1 texture per call) — only called at
        // device init, so the cost is negligible (~1 ms total).
        static bool probeSampleCountSupported(WGPUDevice dev, WGPUTextureFormat fmt, uint32 sampleCount)
        {
            if (!dev) return false;
            struct UD {
                std::atomic<bool> hadError{false};
                std::atomic<bool> done{false};
            };
            UD ud;
            auto cb = [](WGPUPopErrorScopeStatus status, WGPUErrorType type,
                         WGPUStringView message, void* u1, void* u2)
            {
                (void)message; (void)u2;
                UD& d = *(UD*)u1;
                // status=Success means the scope popped cleanly (no error captured).
                // Any non-NoError type means there WAS a validation error during the
                // operations inside the scope.
                if (status != WGPUPopErrorScopeStatus_Success || type != WGPUErrorType_NoError) {
                    d.hadError.store(true, std::memory_order_release);
                }
                d.done.store(true, std::memory_order_release);
            };
            // Push a validation error scope so any error from wgpuDeviceCreateTexture
            // is captured here instead of going to the global uncaptured-error handler.
            wgpuDevicePushErrorScope(dev, WGPUErrorFilter_Validation);
            WGPUTextureDescriptor td = {};
            td.label = WGPU_STR("MSAA probe");
            td.size = {1, 1, 1};
            td.format = fmt;
            td.usage = WGPUTextureUsage_RenderAttachment;
            td.dimension = WGPUTextureDimension_2D;
            td.mipLevelCount = 1;
            td.sampleCount = sampleCount;
            WGPUTexture testTex = wgpuDeviceCreateTexture(dev, &td);
            WGPUPopErrorScopeCallbackInfo ci = {};
            ci.mode = WGPUCallbackMode_AllowSpontaneous;
            ci.callback = cb;
            ci.userdata1 = &ud;
            wgpuDevicePopErrorScope(dev, ci);
            // Pump events so the pop callback fires (usually 1 iteration).
            auto t0 = std::chrono::steady_clock::now();
            while (!ud.done.load(std::memory_order_acquire)) {
                wgpuInstanceProcessEvents(wgpuGlobals.instance);
                if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(5)) {
                    fprintf(stderr, "[wgpu] MSAA probe (format=%d, sc=%u): timeout — assuming not supported\n",
                            (int)fmt, sampleCount);
                    if (testTex) wgpuTextureRelease(testTex);
                    return false;
                }
            }
            // Release the probe texture (safe even if creation triggered a validation error —
            // the handle is still valid to release, the underlying resource is just not allocated).
            if (testTex) wgpuTextureRelease(testTex);
            return !ud.hadError.load(std::memory_order_acquire);
        }

        // Determine the device's actual max supported sample count for MSAA on the
        // surface color format + the depth format we use (Depth32Float). Returns
        // the highest sample count in {1, 2, 4, 8, 16} supported by BOTH formats,
        // since the render pass needs MSAA on the colour attachment AND the depth
        // attachment to use the same sample count.
        //
        // WebGPU spec: 1 is always supported. Returns at least 1 even on devices
        // that for some reason don't expose 4 (rare/buggy driver — falls back to
        // no MSAA).
        static uint32 probeMaxSupportedSamples(WGPUDevice dev, WGPUTextureFormat colorFmt)
        {
            if (!dev) return 1;
            // Highest first — once we find one that's supported on BOTH formats,
            // that's our answer.
            static const uint32 candidates[] = { 16, 8, 4, 2 };
            for (uint32 sc : candidates) {
                bool colorOk = probeSampleCountSupported(dev, colorFmt, sc);
                bool depthOk = probeSampleCountSupported(dev, WGPUTextureFormat_Depth32Float, sc);
                if (colorOk && depthOk) {
                    fprintf(stderr, "[wgpu] MSAA probe: sample count %u supported on color (fmt=%d) + depth\n",
                            sc, (int)colorFmt);
                    return sc;
                }
                fprintf(stderr, "[wgpu] MSAA probe: sample count %u NOT supported (color=%d depth=%d) — trying lower\n",
                        sc, (int)colorOk, (int)depthOk);
            }
            // Per WebGPU spec, sample count 1 is always supported (no MSAA).
            fprintf(stderr, "[wgpu] MSAA probe: no MSAA level above 1 supported — using no MSAA\n");
            return 1;
        }

        static WGPUAdapter requestAdapterSync(WGPUInstance instance, WGPURequestAdapterOptions const* options)
        {
            struct UD
            {
                std::atomic<WGPUAdapter> adapter{nullptr};
                std::atomic<bool> done{false};
            };
            UD ud;
            auto cb = [](WGPURequestAdapterStatus s, WGPUAdapter a, WGPUStringView m, void* u1, void* u2)
            {
                UD& d = *(UD*)u1;
                if (s == WGPURequestAdapterStatus_Success)
                    d.adapter.store(a, std::memory_order_release);
                else
                    fprintf(stderr, "[wgpu] requestAdapter failed: %.*s\n", (int)m.length, m.data);
                d.done.store(true, std::memory_order_release);
            };
            WGPURequestAdapterCallbackInfo ci = {};
            ci.mode = WGPUCallbackMode_AllowSpontaneous;
            ci.callback = cb;
            ci.userdata1 = &ud;
            wgpuInstanceRequestAdapter(instance, options, ci);
            // ProcessEvents дёргает колбэк как только драйвер ответил (обычно 1 итерация)
            auto t0 = std::chrono::steady_clock::now();
            while (!ud.done.load(std::memory_order_acquire))
            {
                wgpuInstanceProcessEvents(instance);
                if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(15))
                {
                    fprintf(stderr, "[wgpu] requestAdapter: no answer after 15s (driver hang?) — giving up\n");
                    return nullptr;
                }
            }
            return ud.adapter.load(std::memory_order_acquire);
        }

        static WGPUDevice requestDeviceSync(WGPUInstance instance, WGPUAdapter adapter,
                                            WGPUDeviceDescriptor const* desc)
        {
            if (!adapter)
            {
                fprintf(stderr, "[wgpu] requestDeviceSync: adapter is null — no GPU found\n");
                return nullptr;
            }
            struct UD
            {
                std::atomic<WGPUDevice> device{nullptr};
                std::atomic<bool> done{false};
            };
            UD ud;
            auto cb = [](WGPURequestDeviceStatus s, WGPUDevice d, WGPUStringView m, void* u1, void* u2)
            {
                UD& data = *(UD*)u1;
                if (s == WGPURequestDeviceStatus_Success)
                    data.device.store(d, std::memory_order_release);
                else
                    fprintf(stderr, "[wgpu] requestDevice failed: %.*s\n", (int)m.length, m.data);
                data.done.store(true, std::memory_order_release);
            };
            WGPURequestDeviceCallbackInfo ci = {};
            ci.mode = WGPUCallbackMode_AllowSpontaneous;
            ci.callback = cb;
            ci.userdata1 = &ud;
            wgpuAdapterRequestDevice(adapter, desc, ci);
            auto t0 = std::chrono::steady_clock::now();
            while (!ud.done.load(std::memory_order_acquire))
            {
                wgpuInstanceProcessEvents(instance);
                if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(15))
                {
                    fprintf(stderr, "[wgpu] requestDevice: no answer after 15s (driver hang?) — giving up\n");
                    return nullptr;
                }
            }
            return ud.device.load(std::memory_order_acquire);
        }

        // ========================================================================
        // (P3-16) Device diagnostics — surface errors instead of failing silently
        // ========================================================================
        static void onDeviceLost(WGPUDevice const* device, WGPUDeviceLostReason reason,
                                 WGPUStringView message, void* u1, void* u2)
        {
            (void)device;
            (void)u1;
            (void)u2;
            fprintf(stderr, "[wgpu] DEVICE LOST (reason %d): %.*s\n",
                    (int)reason, (int)message.length, message.data ? message.data : "");
            fprintf(stderr, "[wgpu] Rendering cannot continue — restart the game.\n");
        }

        static void onUncapturedError(WGPUDevice const* device, WGPUErrorType type,
                                      WGPUStringView message, void* u1, void* u2)
        {
            (void)device;
            (void)u1;
            (void)u2;
            fprintf(stderr, "[wgpu] uncaptured error (type %d): %.*s\n",
                    (int)type, (int)message.length, message.data ? message.data : "");
        }

        // ========================================================================
        // WGPU Device open/close/start/stop
        // ========================================================================
        static int openWGPU(EngineOpenParams* openparams)
        {
            wgpuGlobals.winWidth = openparams->width;
            wgpuGlobals.winHeight = openparams->height;
            wgpuGlobals.winTitle = openparams->windowtitle;
            wgpuGlobals.pWindow = (void**)openparams->window;
#if defined(__linux__)
            const char* waylandDisplay = getenv("WAYLAND_DISPLAY");
#if defined(GLFW_EXPOSE_NATIVE_WAYLAND)
            if (waylandDisplay && waylandDisplay[0] != '\0')
                glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
            else
#endif
                glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#endif
            if (!glfwInit())
            {
                RWERROR((ERR_GENERAL, "glfwInit() failed"));
                return 0;
            }
            return 1;
        }

        static int closeWGPU(void)
        {
            glfwTerminate();
            return 1;
        }

        // Create the WebGPU surface for the GLFW window (platform-specific). Returns
        // nullptr on failure. Split out so backend fallback can recreate it against a
        // fresh instance.
        static WGPUSurface createWindowSurface(WGPUInstance instance, GLFWwindow* win)
        {
            WGPUSurfaceDescriptor surfDesc = {};
#if defined(_WIN32)
            WGPUSurfaceSourceWindowsHWND winDesc = {};
            winDesc.chain.sType = WGPUSType_SurfaceSourceWindowsHWND;
            winDesc.hinstance = GetModuleHandle(nullptr);
            winDesc.hwnd = glfwGetWin32Window(win);
            surfDesc.nextInChain = &winDesc.chain;
#elif defined(__linux__)
#if defined(GLFW_EXPOSE_NATIVE_WAYLAND)
            const char* waylandDisplay = getenv("WAYLAND_DISPLAY");
            if (waylandDisplay && waylandDisplay[0] != '\0')
            {
                WGPUSurfaceSourceWaylandSurface wlDesc = {};
                wlDesc.chain.sType = WGPUSType_SurfaceSourceWaylandSurface;
                wlDesc.display = glfwGetWaylandDisplay();
                wlDesc.surface = glfwGetWaylandWindow(win);
                surfDesc.nextInChain = &wlDesc.chain;
                return wgpuInstanceCreateSurface(instance, &surfDesc);
            }
#endif
            WGPUSurfaceSourceXlibWindow x11Desc = {};
            x11Desc.chain.sType = WGPUSType_SurfaceSourceXlibWindow;
            x11Desc.display = glfwGetX11Display();
            x11Desc.window = glfwGetX11Window(win);
            surfDesc.nextInChain = &x11Desc.chain;
#endif
            return wgpuInstanceCreateSurface(instance, &surfDesc);
        }

        static int startWGPU(void)
        {
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            GLFWwindow* win = glfwCreateWindow(wgpuGlobals.winWidth, wgpuGlobals.winHeight,
                                               wgpuGlobals.winTitle, nullptr, nullptr);
            if (!win)
            {
                RWERROR((ERR_GENERAL, "glfwCreateWindow() failed"));
                return 0;
            }
            wgpuGlobals.window = win;
            *wgpuGlobals.pWindow = win;

            // wgpu-native v29 не поддерживает TimedWaitAny — синхронизация через
            // ProcessEvents (см. ниже). Try the requested backend first, then fall
            // back to auto: some backends can't pair with our surface — e.g.
            // wgpu-native's GL backend rejects the X11/Wayland surface of a NO_API
            // GLFW window ("gl not compatible with provided surface") — so a bad
            // preference must not brick startup.
            //
            // ВАЖНО: инстанс больше НИКОГДА не ограничивается одним бэкендом
            // (instExtras.backends не заполняется — 0 = все бэкенды). Если инстанс
            // создан "только Vulkan", а Vulkan-драйвера на машине нет, процесс
            // умирает прямо внутри wgpuInstanceCreateSurface ("Failed to create
            // surface for any enabled backend" → паника wgpu-native → SIGABRT):
            // до отката на Auto дело не доходит, игра падает на старте. Поэтому:
            //   1) инстанс всегда со всеми бэкендами — поверхность создаётся через
            //      любой живой бэкенд (GL/EGL есть почти всегда);
            //   2) принуждение к бэкенду перенесено на запрос адаптера:
            //      WGPURequestAdapterOptions::backendType в wgpu-native официально
            //      возвращает null при недоступности ("If this is not possible, the
            //      request returns null") — мягкий отказ вместо паники;
            //   3) один инстанс + одна поверхность на все попытки: поверхность
            //      принадлежит инстансу, пересоздавать их между попытками не нужно.
            int backendPref = resolveBackendPref();
            if (backendPref != BACKEND_AUTO)
                fprintf(stderr, "[wgpu] Forcing backend %s (instance backend mask 0x%x)\n",
                        backendTypeName(backendPrefToType(backendPref)),
                        (unsigned)backendPrefToFlag(backendPref));
            WGPUInstanceDescriptor instDesc = {};
            WGPUInstanceExtras instExtras = {};
            instExtras.chain.sType = (WGPUSType)WGPUSType_InstanceExtras;
            // instExtras.backends сознательно НЕ заполняем (0 = все бэкенды),
            // см. большой комментарий выше: ограничение бэкенда делается только
            // на уровне запроса адаптера (adapterOpts.backendType ниже).
            // displayHandle передаётся ВСЕГДА, а не только при явном выборе
            // бэкенда: GL-бэкенд без платформенного дисплея инициализирует EGL
            // на платформе по умолчанию (X11/surfaceless) и отклоняет поверхность
            // окна ("gl not compatible with provided surface"). На Wayland это
            // убивает Auto-режим на машинах без Vulkan полностью; на X11 работало
            // только потому, что Mesa выбирает X11-платформу по DISPLAY.
            // Другие бэкенды displayHandle игнорируют.
#if defined(__linux__)
#if defined(GLFW_EXPOSE_NATIVE_WAYLAND)
            const char* wlDisp = getenv("WAYLAND_DISPLAY");
            if (wlDisp && wlDisp[0])
            {
                instExtras.displayHandle.type = WGPUNativeDisplayHandleType_Wayland;
                instExtras.displayHandle.data.wayland.display = glfwGetWaylandDisplay();
            }
            else
#endif
            {
                Display* dpy = glfwGetX11Display();
                instExtras.displayHandle.type = WGPUNativeDisplayHandleType_Xlib;
                instExtras.displayHandle.data.xlib.display = dpy;
                instExtras.displayHandle.data.xlib.screen = DefaultScreen(dpy);
            }
#endif
            instDesc.nextInChain = &instExtras.chain;
            WGPUInstance inst = wgpuCreateInstance(&instDesc);
            if (!inst)
            {
                RWERROR((ERR_GENERAL, "WebGPU instance creation failed"));
                return 0;
            }

            WGPUSurface surf = createWindowSurface(inst, win);
            if (!surf)
            {
                wgpuInstanceRelease(inst);
                RWERROR((ERR_GENERAL, "WebGPU surface creation failed"));
                return 0;
            }

            // Адаптер: сперва запрошенный бэкенд, при отказе — auto (любой,
            // совместимый с поверхностью). requestAdapterSync при неудаче честно
            // возвращает nullptr (без паники), поэтому откат гарантированно работает.
            int tries[2] = {backendPref, BACKEND_AUTO};
            int nTries = (backendPref == BACKEND_AUTO) ? 1 : 2;
            WGPUAdapter ad = nullptr;
            for (int t = 0; t < nTries; t++)
            {
                int pref = tries[t];

                WGPURequestAdapterOptions adapterOpts = {};
                adapterOpts.compatibleSurface = surf;
                adapterOpts.backendType = backendPrefToType(pref);
                ad = requestAdapterSync(inst, &adapterOpts);
                if (ad) break;
                if (t + 1 < nTries)
                    fprintf(stderr, "[wgpu] Backend %s unavailable for this surface — falling back to auto\n",
                            backendTypeName(backendPrefToType(pref)));
            }
            if (ad)
            {
                wgpuGlobals.instance = inst;
                wgpuGlobals.surface = surf;
                wgpuGlobals.adapter = ad;
            }
            else
            {
                wgpuSurfaceRelease(surf);
                wgpuInstanceRelease(inst);
            }

            if (!wgpuGlobals.adapter)
            {
                RWERROR((ERR_GENERAL, "No WebGPU adapter found — check drivers / backend selection"));
                return 0;
            }

            // Record which backend we actually got (for the menu / logging).
            WGPUAdapterInfo adapterInfo = {};
            if (wgpuAdapterGetInfo(wgpuGlobals.adapter, &adapterInfo) == WGPUStatus_Success)
            {
                s_backendActual = adapterInfo.backendType;
                // Печатаем и сам адаптер: на машине может быть несколько ICD
                // (дискретный/встроенный GPU + программный llvmpipe) — "Backend:
                // Vulkan" без имени не отвечает на вопрос, НА ЧЁМ реально играем.
                auto sv = [](WGPUStringView v, int* len) -> const char*
                {
                    *len = (v.data && v.length && v.length < 256) ? (int)v.length : 0;
                    return v.data ? v.data : "";
                };
                int devLen, descLen;
                const char* dev = sv(adapterInfo.device, &devLen);
                const char* desc = sv(adapterInfo.description, &descLen);
                fprintf(stdout, "[wgpu] Backend: %s (requested %s) — adapter: %.*s (%.*s)\n",
                        backendTypeName(adapterInfo.backendType),
                        backendTypeName(backendPrefToType(backendPref)),
                        devLen, dev, descLen, desc);
                wgpuAdapterInfoFreeMembers(adapterInfo);
            }

            // Device
            // Check if the adapter exposes the native
            // TextureAdapterSpecificFormatFeatures feature. With it, the
            // device supports MSAA sample counts [1, 2, 4, 8] for Rgba8Unorm
            // / Depth32Float; without it the WebGPU spec only guarantees
            // [1, 4]. We request the feature when available so users can
            // pick 2x / 8x MSAA in addition to 1x / 4x.
            bool hasMsaaFeature = false;
            if (wgpuGlobals.adapter) {
                hasMsaaFeature = wgpuAdapterHasFeature(
                    wgpuGlobals.adapter,
                    (WGPUFeatureName)WGPUNativeFeature_TextureAdapterSpecificFormatFeatures);
            }
            WGPUFeatureName requiredFeature = (WGPUFeatureName)WGPUNativeFeature_TextureAdapterSpecificFormatFeatures;
            WGPUDeviceDescriptor devDesc = {};
            devDesc.label = WGPU_STR("librw Device");
            devDesc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
            devDesc.deviceLostCallbackInfo.callback = onDeviceLost;
            devDesc.uncapturedErrorCallbackInfo.callback = onUncapturedError;
            if (hasMsaaFeature) {
                devDesc.requiredFeatureCount = 1;
                devDesc.requiredFeatures = &requiredFeature;
            }
            // NOTE: We no longer hardcode maxSupportedSamples here — it's set
            // AFTER device creation via probeMaxSupportedSamples(), which actually
            // attempts to create 1x1 textures with each candidate sample count
            // (16/8/4/2) and captures validation errors via error scopes. This
            // returns the device's REAL maximum supported MSAA level, which can
            // be 4 (spec-guaranteed minimum), 8 (common on AMD iGPUs), or in
            // rare cases only 1 or 2 — and the menu will dynamically filter
            // options accordingly. Initial conservative default of 4 covers
            // the brief window between device creation and the probe call.
            wgpuGlobals.maxSupportedSamples = 4;
            wgpuGlobals.device = requestDeviceSync(wgpuGlobals.instance, wgpuGlobals.adapter, &devDesc);
            if (!wgpuGlobals.device)
            {
                // If requesting the feature failed (e.g. driver glitch), retry
                // without it. The probe below will figure out the actual max.
                if (hasMsaaFeature) {
                    fprintf(stderr, "[wgpu] device request with TextureAdapterSpecificFormatFeatures failed, retrying without\n");
                    devDesc.requiredFeatureCount = 0;
                    devDesc.requiredFeatures = nullptr;
                    wgpuGlobals.device = requestDeviceSync(wgpuGlobals.instance, wgpuGlobals.adapter, &devDesc);
                }
                if (!wgpuGlobals.device) {
                    RWERROR((ERR_GENERAL, "WebGPU device creation failed"));
                    wgpuSurfaceRelease(wgpuGlobals.surface);
                    wgpuGlobals.surface = nullptr;
                    wgpuAdapterRelease(wgpuGlobals.adapter);
                    wgpuGlobals.adapter = nullptr;
                    wgpuInstanceRelease(wgpuGlobals.instance);
                    wgpuGlobals.instance = nullptr;
                    return 0;
                }
            }
            wgpuGlobals.queue = wgpuDeviceGetQueue(wgpuGlobals.device);

            // Surface caps & format — moved up from below so we know the surface
            // format BEFORE probing for MSAA support (the probe creates 1x1
            // textures with the surface's actual color format).
            WGPUSurfaceCapabilities caps = {};
            wgpuSurfaceGetCapabilities(wgpuGlobals.surface, wgpuGlobals.adapter, &caps);
            if (!caps.formats || caps.formatCount == 0)
            {
                RWERROR((ERR_GENERAL, "No surface formats available"));
                wgpuSurfaceCapabilitiesFreeMembers(caps);
                return 0;
            }
            // Prefer a NON-sRGB (linear byte) format. RenderWare/GTA does no gamma
            // management: textures are stored as sRGB bytes and we load them as
            // *_Unorm (pass-through), so the surface must also be *_Unorm — writing to
            // an *_UnormSrgb surface would sRGB-encode already-sRGB values a second
            // time, brightening midtones (a washed-out / grey look). caps.formats[0] is
            // often the Srgb variant on wgpu-native, so scan for the linear one.
            wgpuGlobals.surfaceFormat = caps.formats[0];
            for (size_t i = 0; i < caps.formatCount; i++)
            {
                WGPUTextureFormat f = caps.formats[i];
                if (f == WGPUTextureFormat_BGRA8Unorm || f == WGPUTextureFormat_RGBA8Unorm)
                {
                    wgpuGlobals.surfaceFormat = f;
                    break;
                }
            }

            // Probe the device's actual max supported MSAA sample count for the
            // surface color format + Depth32Float (our depth attachment format).
            // This replaces the old "8 if hasMsaaFeature else 4" hardcoded guess,
            // which was wrong on real hardware (e.g. AMD Vega iGPU supports 8x on
            // Vulkan but the feature flag alone doesn't prove 8x works on our
            // format). Probe is cheap (~1 ms total) and runs once at device init.
            wgpuGlobals.maxSupportedSamples = probeMaxSupportedSamples(wgpuGlobals.device, wgpuGlobals.surfaceFormat);
            fprintf(stderr, "[wgpu] maxSupportedSamples = %u (probed for format %d)\n",
                    wgpuGlobals.maxSupportedSamples, (int)wgpuGlobals.surfaceFormat);

            // Clamp any pendingNumSamples set earlier (by reVC's psSelectDevice
            // calling RwD3D8EngineSetMultiSamplingLevels before DEVICEINIT) to
            // what the device actually supports. We clamp pendingNumSamples
            // (not numSamples) because DEVICESETMULTISAMPLINGLEVELS writes to
            // pendingNumSamples — the actual swap happens in applyPendingMsaaChange.
            if (wgpuGlobals.pendingNumSamples > wgpuGlobals.maxSupportedSamples) {
                fprintf(stderr, "[wgpu] clamping MSAA %ux -> %ux (device max)\n",
                        wgpuGlobals.pendingNumSamples, wgpuGlobals.maxSupportedSamples);
                wgpuGlobals.pendingNumSamples = wgpuGlobals.maxSupportedSamples;
            }
            // Also apply it immediately for the first frame so that openIm2D etc.
            // create pipelines with the correct sample count.
            wgpuGlobals.numSamples = wgpuGlobals.pendingNumSamples;

            // Present mode. Fifo (strict vsync) quantizes to divisors of the refresh
            // rate: the moment a frame takes over 1/60s the rate hard-drops to 30.
            // Prefer Mailbox when available — still tear-free, but the newest frame
            // wins instead of blocking, so a 17ms frame shows ~58fps, not 30.
            // Override with LIBRW_WGPU_PRESENT=fifo|mailbox|immediate.
            WGPUPresentMode present = WGPUPresentMode_Fifo;
            for (size_t i = 0; i < caps.presentModeCount; i++)
                if (caps.presentModes[i] == WGPUPresentMode_Mailbox)
                    present = WGPUPresentMode_Mailbox;
            const char* pmEnv = getenv("LIBRW_WGPU_PRESENT");
            if (pmEnv)
            {
                if (strcmp(pmEnv, "fifo") == 0) present = WGPUPresentMode_Fifo;
                else if (strcmp(pmEnv, "mailbox") == 0) present = WGPUPresentMode_Mailbox;
                else if (strcmp(pmEnv, "immediate") == 0) present = WGPUPresentMode_Immediate;
            }
            wgpuSurfaceCapabilitiesFreeMembers(caps);

            // Configure surface
            wgpuGlobals.surfaceConfig.device = wgpuGlobals.device;
            // The surface is a pure present target now: the game renders into the
            // offscreen backbuffer (see acquireFrame), which is also what
            // rasterRenderFast() snapshots — the surface itself is never read, so plain
            // RENDER_ATTACHMENT suffices on every backend (GL swapchains support only
            // that anyway).
            wgpuGlobals.surfaceConfig.usage = WGPUTextureUsage_RenderAttachment;
            wgpuGlobals.surfaceConfig.format = wgpuGlobals.surfaceFormat;
            wgpuGlobals.surfaceConfig.width = (uint32_t)wgpuGlobals.winWidth;
            wgpuGlobals.surfaceConfig.height = (uint32_t)wgpuGlobals.winHeight;
            wgpuGlobals.surfaceConfig.presentMode = present;
            // Opaque, not Auto: the game backbuffer carries a non-1 alpha (fades,
            // alpha-textured UI), and Auto can resolve to Inherit on some backends (the
            // GL backend), letting that alpha punch through to the desktop compositor.
            wgpuGlobals.surfaceConfig.alphaMode = WGPUCompositeAlphaMode_Opaque;
            wgpuSurfaceConfigure(wgpuGlobals.surface, &wgpuGlobals.surfaceConfig);

            return 1;
        }

        static int stopWGPU(void)
        {
            if (wgpuGlobals.surface)
            {
                wgpuSurfaceUnconfigure(wgpuGlobals.surface);
                wgpuSurfaceRelease(wgpuGlobals.surface);
                wgpuGlobals.surface = nullptr;
            }
            if (wgpuGlobals.queue)
            {
                wgpuQueueRelease(wgpuGlobals.queue);
                wgpuGlobals.queue = nullptr;
            }
            if (wgpuGlobals.device)
            {
                wgpuDeviceRelease(wgpuGlobals.device);
                wgpuGlobals.device = nullptr;
            }
            if (wgpuGlobals.adapter)
            {
                wgpuAdapterRelease(wgpuGlobals.adapter);
                wgpuGlobals.adapter = nullptr;
            }
            if (wgpuGlobals.instance)
            {
                wgpuInstanceRelease(wgpuGlobals.instance);
                wgpuGlobals.instance = nullptr;
            }
            if (wgpuGlobals.window)
            {
                glfwDestroyWindow(wgpuGlobals.window);
                wgpuGlobals.window = nullptr;
            }
            return 1;
        }

        static int initWGPU(void)
        {
            resetRenderState();
            openIm2D();
            openIm2DText();
            openIm3D();
            openAtomic();
            return 1;
        }

        static int termWGPU(void)
        {
            closeAtomic();
            closeIm3D();
            closeIm2DText();
            closeIm2D();
            wgpuUniformRingDestroy();
            wgpuCustomResetBGLs();
            if (s_depthClearPipe)
            {
                wgpuRenderPipelineRelease(s_depthClearPipe);
                s_depthClearPipe = nullptr;
            }
            if (s_presentBlitPipe)
            {
                wgpuRenderPipelineRelease(s_presentBlitPipe);
                s_presentBlitPipe = nullptr;
            }
            if (s_presentBlitPL)
            {
                wgpuPipelineLayoutRelease(s_presentBlitPL);
                s_presentBlitPL = nullptr;
            }
            if (s_presentBlitBGL)
            {
                wgpuBindGroupLayoutRelease(s_presentBlitBGL);
                s_presentBlitBGL = nullptr;
            }
            if (s_presentSampler)
            {
                wgpuSamplerRelease(s_presentSampler);
                s_presentSampler = nullptr;
            }
            if (s_backbuffer.view)
            {
                wgpuTextureViewRelease(s_backbuffer.view);
                s_backbuffer.view = nullptr;
            }
            if (s_backbuffer.texture)
            {
                wgpuTextureRelease(s_backbuffer.texture);
                s_backbuffer.texture = nullptr;
            }
            if (s_backbuffer.resolveView)
            {
                wgpuTextureViewRelease(s_backbuffer.resolveView);
                s_backbuffer.resolveView = nullptr;
            }
            if (s_backbuffer.resolveTexture)
            {
                wgpuTextureRelease(s_backbuffer.resolveTexture);
                s_backbuffer.resolveTexture = nullptr;
            }
            s_backbuffer = {};
            if (s_swap.texture) { wgpuTextureRelease(s_swap.texture); }
            if (s_swap.view) { wgpuTextureViewRelease(s_swap.view); }
            s_swap.view = nullptr;
            s_swap.texture = nullptr;
            s_swap.acquired = false;
            if (s_mainDepth.view) wgpuTextureViewRelease(s_mainDepth.view);
            if (s_mainDepth.texture) wgpuTextureRelease(s_mainDepth.texture);
            s_mainDepth = {};
            return 1;
        }

        static int finalizeWGPU(void) { return 1; }

        // ========================================================================
        // Driver registration
        // ========================================================================
        static void* driverOpen(void* o, int32, int32)
        {
            if (wgpuRasterOffset == 0)
                wgpuRasterOffset = Raster::registerPlugin(sizeof(WGPURaster), 0, nil, destroyNativeRaster, nil);

            engine->driver[PLATFORM_WGPU]->rasterNativeOffset = wgpuRasterOffset;
            engine->driver[PLATFORM_WGPU]->rasterCreate = rasterCreate;
            engine->driver[PLATFORM_WGPU]->rasterLock = rasterLock;
            engine->driver[PLATFORM_WGPU]->rasterUnlock = rasterUnlock;
            engine->driver[PLATFORM_WGPU]->rasterNumLevels = rasterNumLevels;
            engine->driver[PLATFORM_WGPU]->imageFindRasterFormat = imageFindRasterFormat;
            engine->driver[PLATFORM_WGPU]->rasterFromImage = rasterFromImage;
            engine->driver[PLATFORM_WGPU]->rasterToImage = rasterToImage;

            static ObjPipeline* defaultPipeline = nullptr;
            if (!defaultPipeline)
            {
                defaultPipeline = ObjPipeline::create();
                defaultPipeline->impl.instance = instance;
                defaultPipeline->impl.uninstance = nullptr;
                defaultPipeline->impl.render = atomicRender;
            }

            engine->driver[PLATFORM_WGPU]->defaultPipeline = defaultPipeline;
            return o;
        }

        static void* driverClose(void* o, int32, int32) { return o; }

        void registerPlatformPlugins(void)
        {
            Driver::registerPlugin(PLATFORM_WGPU, 0, PLATFORM_WGPU, driverOpen, driverClose);
            initSkin();
        }

        // ========================================================================
        // Device system
        // ========================================================================
        // Video mode enumeration (mirrors gl3): mode 0 is the desktop / windowed mode
        // (flags = 0); the remaining entries are the monitor's fullscreen modes
        // (VIDEOMODEEXCLUSIVE). re3/reVC's device selection REQUIRES at least one
        // EXCLUSIVE mode that fits the requested resolution, otherwise it aborts with
        // "Cannot find desired video mode, selecting device cancelled".
        static VideoMode s_videoModes[64];
        static int s_numVideoModes = 0;
        static int s_currentVideoMode = 0;

        static void buildVideoModeList()
        {
            if (s_numVideoModes > 0) return;
            const int MAXMODES = (int)(sizeof(s_videoModes) / sizeof(s_videoModes[0]));
            GLFWmonitor* mon = glfwGetPrimaryMonitor();

            // Mode 0: windowed (desktop resolution), flags = 0.
            const GLFWvidmode* dm = mon ? glfwGetVideoMode(mon) : nullptr;
            if (dm)
            {
                s_videoModes[0].width = dm->width;
                s_videoModes[0].height = dm->height;
            }
            else
            {
                s_videoModes[0].width = wgpuGlobals.winWidth > 0 ? wgpuGlobals.winWidth : 640;
                s_videoModes[0].height = wgpuGlobals.winHeight > 0 ? wgpuGlobals.winHeight : 480;
            }
            s_videoModes[0].depth = 32;
            s_videoModes[0].flags = 0;
            s_numVideoModes = 1;

            // Modes 1..: the monitor's fullscreen modes, flags = VIDEOMODEEXCLUSIVE.
            int count = 0;
            const GLFWvidmode* modes = mon ? glfwGetVideoModes(mon, &count) : nullptr;
            for (int i = 0; i < count && s_numVideoModes < MAXMODES; i++)
            {
                bool dup = false;
                for (int j = 1; j < s_numVideoModes; j++)
                    if (s_videoModes[j].width == modes[i].width && s_videoModes[j].height == modes[i].height)
                    {
                        dup = true;
                        break;
                    }
                if (dup) continue;
                s_videoModes[s_numVideoModes].width = modes[i].width;
                s_videoModes[s_numVideoModes].height = modes[i].height;
                s_videoModes[s_numVideoModes].depth = 32;
                s_videoModes[s_numVideoModes].flags = VIDEOMODEEXCLUSIVE;
                s_numVideoModes++;
            }
            // Guarantee at least one EXCLUSIVE mode (matching the desktop) so selection
            // never fails even without a monitor mode list.
            if (s_numVideoModes == 1)
            {
                s_videoModes[1] = s_videoModes[0];
                s_videoModes[1].flags = VIDEOMODEEXCLUSIVE;
                s_numVideoModes = 2;
            }
        }

        static int deviceSystemWGPU(DeviceReq req, void* arg, int32 n)
        {
            switch (req)
            {
            case DEVICEOPEN: return openWGPU((EngineOpenParams*)arg);
            case DEVICECLOSE: return closeWGPU();
            case DEVICEINIT: return startWGPU() && initWGPU();
            case DEVICETERM: return termWGPU() && stopWGPU();
            case DEVICEFINALIZE: return finalizeWGPU();

            case DEVICEGETNUMVIDEOMODES: buildVideoModeList();
                return s_numVideoModes;
            case DEVICEGETCURRENTVIDEOMODE: return s_currentVideoMode;
            case DEVICESETVIDEOMODE:
                buildVideoModeList();
                if (n < 0 || n >= s_numVideoModes) return 0;
                s_currentVideoMode = n;
                return 1;
            case DEVICEGETVIDEOMODEINFO:
                {
                    buildVideoModeList();
                    VideoMode* rwmode = (VideoMode*)arg;
                    int idx = (n < 0 || n >= s_numVideoModes) ? 0 : n;
                    *rwmode = s_videoModes[idx];
                    return 1;
                }
            // MSAA support — real-time (Option B). The caller sets the
            // desired sample count via RwD3D8EngineSetMultiSamplingLevels;
            // we just record it and mark dirty. The actual recreation
            // (textures + all pipelines) happens at the next frame boundary
            // in applyPendingMsaaChange() called from beginUpdate.
            case DEVICEGETMAXMULTISAMPLINGLEVELS:
                // Returns the maximum sample count this device supports for the
                // render-target / depth formats we use. Set during device init
                // based on whether WGPUNativeFeature_TextureAdapterSpecificFormatFeatures
                // was requested. Reported to reVC via RwD3D8EngineGetMaxMultiSamplingLevels
                // so the menu's L/R adjust knows how many levels to cycle through.
                return wgpuGlobals.maxSupportedSamples;
            case DEVICEGETMULTISAMPLINGLEVELS:
                return wgpuGlobals.numSamples ? wgpuGlobals.numSamples : 1;
            case DEVICESETMULTISAMPLINGLEVELS:
                {
                    uint32 newSamples = (uint32)n;
                    // Must be a power of two in 1..16.
                    if (newSamples < 1 || newSamples > 16 ||
                        (newSamples & (newSamples - 1)) != 0)
                        newSamples = 1;
                    // Clamp to device-supported max — but ONLY after the device has
                    // been created (i.e. after probeMaxSupportedSamples has actually
                    // probed the device in startWGPU). Before DEVICEINIT, the device
                    // is null and maxSupportedSamples is the conservative spec-
                    // guaranteed default of 4; clamping here would silently drop
                    // a user-saved 8X setting to 4X even on hardware that supports
                    // 8X, and the later clamp in startWGPU (after probe) wouldn't
                    // un-clamp it back up. So we let pre-DEVICEINIT calls store the
                    // full requested sample count and let startWGPU's probe + clamp
                    // reduce it if (and only if) the device really doesn't support it.
                    if (wgpuGlobals.device &&
                        newSamples > wgpuGlobals.maxSupportedSamples)
                        newSamples = wgpuGlobals.maxSupportedSamples;
                    // For pre-DEVICEINIT calls (device null), the comparison against
                    // numSamples is meaningless (numSamples is also uninitialised) —
                    // just record pendingNumSamples and let the next beginUpdate/
                    // clearCamera apply it.
                    if (wgpuGlobals.device && newSamples == wgpuGlobals.numSamples)
                        return 1;
                    // Don't change numSamples here — write to pendingNumSamples
                    // instead. applyPendingMsaaChange() (at the next beginUpdate)
                    // will swap numSamples = pendingNumSamples and recreate
                    // textures + flush pipeline caches. This prevents a mid-frame
                    // mismatch where currentSampleCount() would return the new
                    // value but the backbuffer still has the old sample count.
                    wgpuGlobals.pendingNumSamples = newSamples;
                    wgpuGlobals.msaaDirty = true;
                    return 1;
                }
            default: return 1;
            }
        }

        Device renderdevice = {
            0.0f, 1.0f,
            wgpu::beginUpdate,
            wgpu::endUpdate,
            wgpu::clearCamera,
            wgpu::showRaster,
            wgpu::rasterRenderFast,
            wgpu::setRenderState,
            wgpu::getRenderState,
            wgpu::im2DRenderLine,
            wgpu::im2DRenderTriangle,
            wgpu::im2DRenderPrimitive,
            wgpu::im2DRenderIndexedPrimitive,
            wgpu::im3DTransform,
            wgpu::im3DRenderPrimitive,
            wgpu::im3DRenderIndexedPrimitive,
            wgpu::im3DEnd,
            wgpu::deviceSystemWGPU
        };
    }
}
#endif
