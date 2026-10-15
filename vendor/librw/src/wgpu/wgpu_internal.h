#pragma once

#include <cstdlib>
#include "wgpu_globals.h"

namespace rw
{
    namespace wgpu
    {
        const char* const k_doDynamicLightWGSL = R"(
fn DoDynamicLight(V: vec3<f32>, N: vec3<f32>) -> vec3<f32> {
    var color = vec3<f32>(0.0);
    for(var i = 0; i < 8; i = i + 1){
        let t = uniforms.lightParams[i].x;
        if(t == 0.0) { break; }
        if(t == 1.0){
            // directional
            let l = max(0.0, dot(N, -uniforms.lightDirection[i].xyz));
            color = color + l * uniforms.lightColor[i].rgb;
        } else if(t == 2.0){
            // point
            let dir = V - uniforms.lightPosition[i].xyz;
            let dist = length(dir);
            let atten = max(0.0, 1.0 - dist / uniforms.lightParams[i].y);
            let l = max(0.0, dot(N, -normalize(dir)));
            color = color + l * uniforms.lightColor[i].rgb * atten;
        } else if(t == 3.0){
            // spot
            var dir = V - uniforms.lightPosition[i].xyz;
            let dist = length(dir);
            let atten = max(0.0, 1.0 - dist / uniforms.lightParams[i].y);
            dir = dir / dist;
            var l = max(0.0, dot(N, -dir));
            let pcos = dot(dir, uniforms.lightDirection[i].xyz);
            let ccos = -uniforms.lightParams[i].z;
            let falloff = (pcos - ccos) / (1.0 - ccos);
            if(falloff < 0.0) { l = 0.0; }
            l = l * max(falloff, uniforms.lightParams[i].w);
            color = color + l * uniforms.lightColor[i].rgb * atten;
        }
    }
    return color;
}
)";

        inline uint16* buildTriListIndices(uint16* src, uint32 srcCount, bool tristrip, uint32* outCount)
        {
            if (!tristrip)
            {
                *outCount = srcCount;
                return src;
            }
            uint16* out = new uint16[srcCount >= 3 ? (srcCount - 2) * 3 : 1];
            uint32 k = 0;
            for (uint32 j = 0; j + 2 < srcCount; j++)
            {
                uint16 a = src[j], b = src[j + 1], c = src[j + 2];
                if (a == b || b == c || a == c) continue; // degenerate stitch triangle
                if (j & 1)
                {
                    out[k++] = b;
                    out[k++] = a;
                    out[k++] = c;
                }
                else
                {
                    out[k++] = a;
                    out[k++] = b;
                    out[k++] = c;
                }
            }
            *outCount = k;
            return out;
        }

        struct FrameState
        {
            WGPUTexture texture;
            WGPUTextureView view;
            WGPUCommandEncoder encoder;
            WGPURenderPassEncoder renderPass;
            WGPUTexture depthTexture;
            WGPUTextureView depthView;
            bool inFrame;
            bool inRenderPass;
            bool isOffscreen;
            uint32 fbWidth;
            uint32 fbHeight;
            Raster* currentFrameBuffer;
        };

        struct RenderState
        {
            Raster* currentRaster;
            uint32 textureAddress;
            uint32 textureAddressU;
            uint32 textureAddressV;
            uint32 textureFilter;
            uint32 cullMode;
            uint32 zTest;
            uint32 zWrite;
            uint32 vertexAlpha;
            uint32 srcBlend;
            uint32 destBlend;
            uint32 fogEnable;
            RGBA fogColor;
            uint32 alphaTestFunc;
            uint32 alphaTestRef;
            uint32 gsAlphaTest;
            uint32 gsAlphaTestRef;
            uint32 stencilEnable;
            uint32 stencilFail;
            uint32 stencilZFail;
            uint32 stencilPass;
            uint32 stencilFunc;
            uint32 stencilFuncRef;
            uint32 stencilFuncMask;
            uint32 stencilFuncWriteMask;
        };

        struct WGPURaster
        {
            WGPUTexture texture;
            WGPUTextureView view;
            WGPUBindGroup bindGroup;
            uint8* lockBuffer;
            int32 lockLevel;
            int32 lockMode;
            WGPUBuffer stagingBuffer;
            uint32 stagingSize;
            WGPUTexture lockSavedTexture;
            int32 numLevels;
        };

        struct Im2DPipeCache
        {
            WGPUShaderModule shader;
            WGPUPipelineLayout playout;
            uint32* key;
            WGPURenderPipeline* pipe;
            int32 count;
            int32 capacity;
        };

        // Lookup a cached pipeline; nullptr on miss. Linear scan — the game uses a
        // handful of distinct state combos per subsystem (<10), so this stays cheap.
        static inline WGPURenderPipeline
        imPipeCacheFind(const Im2DPipeCache* pc, uint32 key)
        {
            for (int32 i = 0; i < pc->count; i++)
                if (pc->key[i] == key)
                    return pc->pipe[i];
            return nullptr;
        }

        // Store a pipeline (takes over one reference). Grows the arrays on demand
        // and never evicts: distinct state combos live forever, so a combo can never
        // fall back into a create-per-frame loop.
        static inline void
        imPipeCacheInsert(Im2DPipeCache* pc, uint32 key, WGPURenderPipeline pipe)
        {
            if (pc->count >= pc->capacity)
            {
                int32 newCap = pc->capacity ? pc->capacity * 2 : 32;
                pc->key = (uint32*)realloc(pc->key, (size_t)newCap * sizeof(uint32));
                pc->pipe = (WGPURenderPipeline*)realloc(pc->pipe, (size_t)newCap * sizeof(WGPURenderPipeline));
                pc->capacity = newCap;
            }
            pc->key[pc->count] = key;
            pc->pipe[pc->count] = pipe;
            pc->count++;
        }

        // Release every cached pipeline and free the arrays. Only call while the
        // device that created the pipelines is still alive (device teardown paths —
        // closeIm2D/closeIm3D/destroyIm2DShader/flushPipelineCache).
        static inline void
        imPipeCacheRelease(Im2DPipeCache* pc)
        {
            for (int32 i = 0; i < pc->count; i++)
                if (pc->pipe[i])
                    wgpuRenderPipelineRelease(pc->pipe[i]);
            free(pc->key);
            free(pc->pipe);
            pc->key = nullptr;
            pc->pipe = nullptr;
            pc->count = 0;
            pc->capacity = 0;
        }

        struct Im2DState
        {
            WGPURenderPipeline pipeline;
            WGPUBuffer uniformBuf;
            WGPUBindGroupLayout bindGroupLayout;
            WGPUBindGroup bindGroup;
            Im2DPipeCache pc;
        };

        struct Im2DTextState
        {
            WGPURenderPipeline pipeline;
            WGPUSampler sampler;
            WGPUSampler samplerNearest;
            WGPUTexture texture;
            WGPUTextureView textureView;
            WGPUBindGroup bindGroup;
            WGPUBindGroupLayout bindGroupLayout;
            Im2DPipeCache pc;
        };

        struct Im3DState
        {
            WGPURenderPipeline pipeline;
            WGPUBuffer sceneBuffer;
            WGPUBindGroup bindGroup;
            WGPUBindGroupLayout bindGroupLayout;
            WGPUTexture defaultTexture;
            WGPUTextureView defaultView;
            WGPUSampler defaultSampler; // ClampToEdge (shared w/ im2dText)
            WGPUSampler wrapSampler; // Repeat — tiled water etc.
            WGPUSampler nearestSampler;
            uint32 sceneOffset;
            bool sceneOnRing;

            WGPUBuffer vertexBuffer; // geometry ring (not owned) or private
            uint64_t vertexOffset; // byte offset into vertexBuffer
            bool vertexPrivate; // true = own buffer (ring overflow), release it
            int32 numVertices;
            bool hasTransform;

            Im2DPipeCache pc; // pipelines keyed by blend/ztest/zwrite state
        };

        struct AtomicState;

        // Per-mesh GPU buffers for an instanced geometry. Shared by the default object
        // pipeline (wgpupipe.cpp) and the skin pipeline (wgpuskin.cpp); both tag the
        // header with platform==PLATFORM_WGPU so freeInstanceData handles either.
        struct WGPUInstanceData
        {
            WGPUBuffer vertexBuffer;
            WGPUBuffer indexBuffer;
            uint32 numIndices;
            Material* material;
            bool vertexAlpha; // geometry has per-vertex alpha < 255 (needs blend)
        };

        struct WGPUInstanceDataHeader
        {
            uint32 platform;
            uint32 numMeshes;
            uint32 totalNumVertex;
            uint32 totalNumIndex;
            WGPUInstanceData* inst;
            WGPUBuffer sharedVbo;
        };

        // ========================================================================
        // External globals (defined in wgpudevice.cpp)
        // ========================================================================

        extern FrameState s_frame;
        extern RenderState s_renderState;
        extern Im2DState s_im2d;
        extern Im2DTextState s_im2dText;
        extern Im3DState s_im3d;
        extern AtomicState s_atomic;
        extern int32 wgpuRasterOffset;
        extern WGPUSampler s_sharedSampler;

        // ========================================================================
        // Macros
        // ========================================================================

#define GETWGPURASTEREXT(raster) PLUGINOFFSET(WGPURaster, raster, wgpuRasterOffset)
#define WGPU_ALIGN4(x) (((uint64_t)(x) + 3u) & ~3u)

        // ========================================================================
        // Shared helper functions (defined in wgpudevice.cpp)
        // ========================================================================

        bool acquireFrame(void);
        bool acquireOffscreenFrame(Camera* cam);
        void endRenderPassIfOpen(void);
        void mat4Mult(float* dst, const float* a, const float* b);
        WGPUDepthStencilState makeDepthState(bool write);
        WGPUBlendState makeAlphaBlend(void);
        void resetRenderState(void);
        WGPUBlendFactor toWGPUBlendFactor(uint32 rwBlend);

        // Uniform ring buffer (wgpudevice.cpp). Sub-allocate per-draw uniform data at a
        // 256-aligned dynamic offset instead of creating a buffer per draw. Returns the
        // offset, or 0xFFFFFFFF when the ring is full this frame (caller falls back to a
        // private buffer). Bind groups referencing the ring must rebuild when the
        // generation changes (the ring grew and was recreated).
        uint32 wgpuUniformAlloc(const void* data, uint32 size);
        WGPUBuffer wgpuUniformRingBuffer(void);
        uint32 wgpuUniformRingGeneration(void);

        // Geometry ring (wgpudevice.cpp): per-frame vertex/index sub-allocations for
        // the immediate-mode draws (im2d/im3d), bound at an offset — replaces a buffer
        // create + write per draw. 16-aligned; 0xFFFFFFFF = full this frame (caller
        // falls back to a private buffer).
        uint32 wgpuGeomAlloc(const void* data, uint32 size);
        WGPUBuffer wgpuGeomRingBuffer(void);

        // im2d screen→NDC xform uniform, written only on size change (wgpuimmed.cpp).
        void im2dUpdateXform(float W, float H);

        // ========================================================================
        // Lighting (shared by the object/skin/matfx pipelines)
        // ========================================================================
#define WGPU_MAX_LIGHTS 8

        // Default object vertex layout: position(12) + normal(12) + color(16) +
        // uv0(8) + uv1(8) = 56 bytes. uv1 (offset 48) is only consumed by custom
        // object shaders (dual-texture / lightmap pipes); the stock pipelines skip it.
#define OBJ_VERTEX_STRIDE 56

        // CPU-side light block, laid out to match the WGSL uniform fields:
        //   ambient, then 4 arrays of WGPU_MAX_LIGHTS vec4.
        // params[i] = (type[1=dir,2=point,3=spot], radius, minusCosAngle, hardSpot).
        struct LightDataGPU
        {
            float ambient[4];
            float params[WGPU_MAX_LIGHTS * 4];
            float position[WGPU_MAX_LIGHTS * 4];
            float direction[WGPU_MAX_LIGHTS * 4];
            float color[WGPU_MAX_LIGHTS * 4];
        };

        void gatherLights(Atomic* atomic, LightDataGPU* out);

        // ========================================================================
        // Raster functions (defined in wgpuraster.cpp)
        // ========================================================================

        Raster* rasterCreate(Raster* raster);
        void rasterDestroy(Raster* raster);
        void* destroyNativeRaster(void *object, int32 offset, int32 size);
        uint8* rasterLock(Raster* raster, int32 level, int32 lockMode);
        void rasterUnlock(Raster* raster, int32 level);
        int32 rasterNumLevels(Raster* raster);
        bool32 imageFindRasterFormat(Image* img, int32 type, int32* pWidth, int32* pHeight, int32* pDepth,
                                     int32* pFormat);
        bool32 rasterFromImage(Raster* raster, Image* image);
        Image* rasterToImage(Raster* raster);

        // ========================================================================
        // Pipeline functions (defined in wgpupipe.cpp)
        // ========================================================================

        // Per-draw uniform block for the object/skin pipelines (736 bytes). CPU field
        // names lightDir/ambient/matProps map by offset to the WGSL ambLight/matColor/
        // surfProps. This is what the object shaders declare, so ring-buffer bindings
        // must cover the whole struct.
        struct ObjectUniforms
        {
            float viewProj[16];
            float world[16];
            float lightDir[4]; // -> ambLight
            float ambient[4]; // -> matColor
            float matProps[4]; // -> surfProps (ambient, specular, diffuse, matAlpha)
            float alphaTest[4]; // x=ref(0..1), y=enable, z=func
            float fogColor[4]; // rgb fog color
            float fogData[4]; // start, end, range, disable
            float lightParams[WGPU_MAX_LIGHTS * 4];
            float lightPosition[WGPU_MAX_LIGHTS * 4];
            float lightDirection[WGPU_MAX_LIGHTS * 4];
            float lightColor[WGPU_MAX_LIGHTS * 4];
        };

        void openAtomic(void);
        void closeAtomic(void);
        void atomicRender(ObjPipeline* pipe, Atomic* atomic);
        void instance(ObjPipeline* pipe, Atomic* atomic);
        void freeInstanceData(Geometry* geometry);
        void* destroyNativeData(void* object, int32, int32);
        // Fill the per-atomic part (viewProj/world/lights/alphaTest/fog); per-mesh
        // ambient/matProps are the caller's job.
        void fillObjectUniforms(Atomic* atomic, ObjectUniforms* u);
        // White 1x1 fallback texture view + shared object sampler.
        WGPUTextureView objDefaultTextureView(void);
        WGPUSampler objDefaultSampler(void);

        // ========================================================================
        // Skin pipeline (defined in wgpuskin.cpp)
        // ========================================================================

        // Skinned vertex layout: pos(12)+normal(12)+color(16)+uv(8)+weights(16)+
        // indices(4) = 68 bytes (no uv1).
#define OBJ_SKIN_VERTEX_STRIDE 68
#define OBJ_SKIN_MAX_BONES     64

        void initSkin(void);
        void closeSkin(void);
        // For custom (game) skin shaders: instance an atomic with the skinned vertex
        // layout, and compute its bone matrices (dst = 64 mat4 = 4096 bytes; caller
        // zeroes the buffer first).
        void skinInstanceAtomic(Atomic* atomic);
        void skinComputeBoneMatrices(Atomic* atomic, float* dst);

        // ========================================================================
        // MatFX pipeline (defined in wgpumatfx.cpp)
        // ========================================================================

        void initMatFX(void);
        void closeMatFX(void);

        // ========================================================================
        // Im2D / Im3D functions (defined in wgpuimmed.cpp)
        // ========================================================================

        void im2DRenderLine(void* verts, int32 numVerts, int32 v1, int32 v2);
        void im2DRenderTriangle(void* verts, int32 numVerts, int32 v1, int32 v2, int32 v3);
        void im2DRenderPrimitive(PrimitiveType primType, void* verts, int32 numVerts);
        void im2DRenderIndexedPrimitive(PrimitiveType primType, void* verts, int32 numVerts,
                                        void* indices, int32 numIndices);
        void im3DTransform(void* verts, int32 numVerts, Matrix* world, uint32 flags);
        void im3DRenderPrimitive(PrimitiveType primType);
        void im3DRenderIndexedPrimitive(PrimitiveType primType, void* indices, int32 numIndices);
        void im3DEnd(void);

        void openIm2D(void);
        void closeIm2D(void);
        void openIm2DText(void);
        void closeIm2DText(void);
        void openIm3D(void);
        void closeIm3D(void);

        // Pipeline-only flush functions for real-time MSAA change.
        // Cheaper than close* — releases the cached render pipelines but
        // leaves shaders, BGLs, samplers, uniform buffers intact. Pipelines
        // are sample-count-specific in wgpu, so they must be released when
        // wgpuGlobals.numSamples changes; the next draw will lazily recreate
        // them via the existing getOrCreate* path with the new sample count.
        void flushIm2DPipelines(void);
        void flushIm2DTextPipelines(void);
        void flushIm3DPipelines(void);
        void flushMatfxPipelines(void);
        void flushSkinCache(void);   // defined in wgpuskin.cpp
        void flushPipelineCache(void); // defined in wgpupipe.cpp

        // Current render-target sample count: returns wgpuGlobals.numSamples
        // when rendering to the main backbuffer (MSAA), 1 for offscreen rasters.
        // Pipeline cache keys include this so MSAA and non-MSAA variants coexist.
        uint32 currentSampleCount(void);

        // ========================================================================
        // Custom im2d shaders (defined in wgpucustom.cpp; public API in rwwgpu.h)
        // ========================================================================

        // Bind im2dOverrideShader's pipeline + bind group on the current pass
        // (im2dOverrideShader must be non-nil).
        void im2dCustomBind(WGPURenderPassEncoder rp, WGPUPrimitiveTopology topology);
        void wgpuCustomResetBGLs(void);
    } // namespace wgpu
} // namespace rw
