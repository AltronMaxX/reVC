#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwengine.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"

#ifdef RW_WGPU

#include "wgpu_internal.h"

#define PLUGIN_ID 0

namespace rw
{
    namespace wgpu
    {
        // ========================================================================
        // Custom im2d shaders — game-supplied post-FX fragments (colour filter,
        // contrast, screen droplets). See rwwgpu.h for the binding contract.
        // ========================================================================

        // Prefix prepended to the game's fragment source: the full custom bind
        // group and the standard im2d vertex stage. VsOut must stay in sync with
        // the stock im2d shaders in wgpuimmed.cpp.
        static const char* k_im2dCustomPrefixWGSL = R"(
struct Xform {
    xScl : f32,
    yScl : f32,
    xOff : f32,
    yOff : f32,
};
@group(0) @binding(0) var<uniform> u_xform : Xform;
@group(0) @binding(1) var t0 : texture_2d<f32>;
@group(0) @binding(2) var s0 : sampler;
struct Params {
    p : array<vec4<f32>, 8>,
};
@group(0) @binding(3) var<uniform> u_params : Params;
@group(0) @binding(4) var t1 : texture_2d<f32>;
@group(0) @binding(5) var s1 : sampler;

struct VsIn {
    @location(0) pos   : vec4<f32>,
    @location(1) color : vec4<f32>,
    @location(2) uv    : vec2<f32>,
};
struct VsOut {
    @builtin(position) clip_pos : vec4<f32>,
    @location(0)       color    : vec4<f32>,
    @location(1)       uv       : vec2<f32>,
};

@vertex
fn vs_main(v : VsIn) -> VsOut {
    var out : VsOut;
    let ndcX = v.pos.x * u_xform.xScl + u_xform.xOff;
    let ndcY = v.pos.y * u_xform.yScl + u_xform.yOff;
    let w = v.pos.w;
    out.clip_pos = vec4<f32>(ndcX * w, ndcY * w, v.pos.z * w, w);
    out.color = v.color;
    out.uv    = v.uv;
    return out;
}
)";

#define IM2D_CUSTOM_PARAMS_SIZE 128
#define IM2D_CUSTOM_MAX_BGS     8

        struct Im2DShaderBG
        {
            WGPUTextureView v0, v1;
            WGPUBindGroup bg;
        };

        struct Im2DShader
        {
            WGPUShaderModule module;
            WGPUPipelineLayout playout;
            Im2DPipeCache pc; // pipelines keyed by blend/z state + topology
            // staged user params, uploaded from the ring at draw time
            uint8 params[IM2D_CUSTOM_PARAMS_SIZE];
            // bind-group cache keyed by (raster view, texture2 view); flushed when the
            // uniform ring is recreated (generation bump)
            Im2DShaderBG bgs[IM2D_CUSTOM_MAX_BGS];
            int numBGs;
            uint32 bgGen;
            // ring-overflow fallback
            WGPUBuffer fallbackParams;
            // custom vertex layout (createIm2DShaderEx); numAttribs==0 → stock Im2DVertex
            uint32 stride;
            uint32 numAttribs;
            WGPUVertexAttribute attribs[8];
        };

        Im2DShader* im2dOverrideShader = nil;

        // Second texture (t1/s1) for override shaders — droplets bind the grabbed
        // screen here while the drop mask rides the normal TEXTURERASTER state.
        static WGPUTextureView s_im2dTex2View = nullptr;

        // Layout shared by every custom im2d shader (created lazily, freed never —
        // device teardown reclaims it).
        static WGPUBindGroupLayout s_customBGL = nullptr;

        static WGPUBindGroupLayout
        customBGL(void)
        {
            if (s_customBGL)
                return s_customBGL;
            WGPUBindGroupLayoutEntry e[6] = {};
            e[0].binding = 0;
            e[0].visibility = WGPUShaderStage_Vertex;
            e[0].buffer.type = WGPUBufferBindingType_Uniform;
            e[0].buffer.minBindingSize = 16;
            e[1].binding = 1;
            e[1].visibility = WGPUShaderStage_Fragment;
            e[1].texture.sampleType = WGPUTextureSampleType_Float;
            e[1].texture.viewDimension = WGPUTextureViewDimension_2D;
            e[2].binding = 2;
            e[2].visibility = WGPUShaderStage_Fragment;
            e[2].sampler.type = WGPUSamplerBindingType_Filtering;
            e[3].binding = 3;
            e[3].visibility = WGPUShaderStage_Fragment;
            e[3].buffer.type = WGPUBufferBindingType_Uniform;
            e[3].buffer.minBindingSize = IM2D_CUSTOM_PARAMS_SIZE;
            e[3].buffer.hasDynamicOffset = true; // sub-allocated from the uniform ring
            e[4].binding = 4;
            e[4].visibility = WGPUShaderStage_Fragment;
            e[4].texture.sampleType = WGPUTextureSampleType_Float;
            e[4].texture.viewDimension = WGPUTextureViewDimension_2D;
            e[5].binding = 5;
            e[5].visibility = WGPUShaderStage_Fragment;
            e[5].sampler.type = WGPUSamplerBindingType_Filtering;

            WGPUBindGroupLayoutDescriptor d = {};
            d.label = WGPU_STR("im2d custom bgl");
            d.entryCount = 6;
            d.entries = e;
            s_customBGL = wgpuDeviceCreateBindGroupLayout(wgpuGlobals.device, &d);
            return s_customBGL;
        }

        Im2DShader*
        createIm2DShader(const char* fragmentWGSL)
        {
            WGPUDevice dev = wgpuGlobals.device;
            if (dev == nullptr)
                return nil;

            size_t plen = strlen(k_im2dCustomPrefixWGSL);
            size_t flen = strlen(fragmentWGSL);
            char* src = rwNewT(char, plen + flen + 1, MEMDUR_EVENT);
            memcpy(src, k_im2dCustomPrefixWGSL, plen);
            memcpy(src + plen, fragmentWGSL, flen + 1);

            WGPUShaderSourceWGSL wgslDesc = {};
            wgslDesc.chain.sType = WGPUSType_ShaderSourceWGSL;
            wgslDesc.code = WGPU_STR(src);
            WGPUShaderModuleDescriptor sd = {};
            sd.nextInChain = &wgslDesc.chain;
            sd.label = WGPU_STR("im2d custom shader");
            WGPUShaderModule mod = wgpuDeviceCreateShaderModule(dev, &sd);
            rwFree(src);
            if (mod == nullptr)
                return nil;

            Im2DShader* sh = rwNewT(Im2DShader, 1, MEMDUR_EVENT);
            memset(sh, 0, sizeof(*sh));
            sh->module = mod;
            sh->bgGen = 0xFFFFFFFFu;

            WGPUBindGroupLayout bgl = customBGL();
            WGPUPipelineLayoutDescriptor pld = {};
            pld.label = WGPU_STR("im2d custom pl");
            pld.bindGroupLayoutCount = 1;
            pld.bindGroupLayouts = &bgl;
            sh->playout = wgpuDeviceCreatePipelineLayout(dev, &pld);

            sh->pc.shader = mod;
            sh->pc.playout = sh->playout;
            sh->pc.count = 0;
            return sh;
        }

        // Full-source variant: the game supplies the whole WGSL module (own VsIn/
        // VsOut and vs_main/fs_main) plus the matching vertex layout — used by
        // screen droplets, whose vertices carry a second UV set. The binding
        // contract (group 0, bindings 0-5) must still be honoured by the module.
        Im2DShader*
        createIm2DShaderEx(const char* fullWGSL, const Im2DCustomAttrib* attribs,
                           uint32 numAttribs, uint32 vertexStride)
        {
            WGPUDevice dev = wgpuGlobals.device;
            if (dev == nullptr || numAttribs == 0 || numAttribs > 8)
                return nil;

            WGPUShaderSourceWGSL wgslDesc = {};
            wgslDesc.chain.sType = WGPUSType_ShaderSourceWGSL;
            wgslDesc.code = WGPU_STR(fullWGSL);
            WGPUShaderModuleDescriptor sd = {};
            sd.nextInChain = &wgslDesc.chain;
            sd.label = WGPU_STR("im2d custom shader (ex)");
            WGPUShaderModule mod = wgpuDeviceCreateShaderModule(dev, &sd);
            if (mod == nullptr)
                return nil;

            Im2DShader* sh = rwNewT(Im2DShader, 1, MEMDUR_EVENT);
            memset(sh, 0, sizeof(*sh));
            sh->module = mod;
            sh->bgGen = 0xFFFFFFFFu;

            sh->stride = vertexStride;
            sh->numAttribs = numAttribs;
            for (uint32 i = 0; i < numAttribs; i++)
            {
                sh->attribs[i].shaderLocation = attribs[i].location;
                sh->attribs[i].offset = attribs[i].offset;
                switch (attribs[i].format)
                {
                case IM2DATTR_FLOAT2: sh->attribs[i].format = WGPUVertexFormat_Float32x2;
                    break;
                case IM2DATTR_FLOAT4: sh->attribs[i].format = WGPUVertexFormat_Float32x4;
                    break;
                case IM2DATTR_UNORM8X4: sh->attribs[i].format = WGPUVertexFormat_Unorm8x4;
                    break;
                default: sh->attribs[i].format = WGPUVertexFormat_Float32x4;
                    break;
                }
            }

            WGPUBindGroupLayout bgl = customBGL();
            WGPUPipelineLayoutDescriptor pld = {};
            pld.label = WGPU_STR("im2d custom pl (ex)");
            pld.bindGroupLayoutCount = 1;
            pld.bindGroupLayouts = &bgl;
            sh->playout = wgpuDeviceCreatePipelineLayout(dev, &pld);

            sh->pc.shader = mod;
            sh->pc.playout = sh->playout;
            sh->pc.count = 0;
            return sh;
        }

        void
        destroyIm2DShader(Im2DShader* sh)
        {
            if (sh == nil)
                return;
            if (im2dOverrideShader == sh)
                im2dOverrideShader = nil;
            imPipeCacheRelease(&sh->pc);
            for (int i = 0; i < sh->numBGs; i++)
                wgpuBindGroupRelease(sh->bgs[i].bg);
            if (sh->fallbackParams)
                wgpuBufferRelease(sh->fallbackParams);
            if (sh->playout)
                wgpuPipelineLayoutRelease(sh->playout);
            if (sh->module)
                wgpuShaderModuleRelease(sh->module);
            rwFree(sh);
        }

        void
        setIm2DShaderParams(Im2DShader* sh, const void* data, uint32 size)
        {
            if (sh == nil)
                return;
            if (size > IM2D_CUSTOM_PARAMS_SIZE)
                size = IM2D_CUSTOM_PARAMS_SIZE;
            memcpy(sh->params, data, size);
        }

        void
        setIm2DTexture2(Texture* tex)
        {
            s_im2dTex2View = nullptr;
            if (tex && tex->raster)
            {
                WGPURaster* ext = GETWGPURASTEREXT(tex->raster);
                if (ext && ext->view)
                    s_im2dTex2View = ext->view;
            }
        }

        // Build/cache a pipeline for the current blend/z render state (mirrors
        // im2dPipelineForBlend, but with the custom shader module/layout).
        static WGPURenderPipeline
        customPipeline(Im2DShader* sh, WGPUPrimitiveTopology topology)
        {
            Im2DPipeCache* pc = &sh->pc;
            WGPUBlendFactor sf = toWGPUBlendFactor(s_renderState.srcBlend);
            WGPUBlendFactor df = toWGPUBlendFactor(s_renderState.destBlend);
            bool ztest = s_renderState.zTest != 0;
            bool zwrite = s_renderState.zWrite != 0;
            bool isLine = (topology == WGPUPrimitiveTopology_LineList);
            uint32 key = ((uint32)sf << 8) | (uint32)df | (ztest ? (1u << 20) : 0u)
                | (isLine ? (1u << 21) : 0u) | (zwrite ? (1u << 22) : 0u)
                | (currentSampleCount() << 28);
            WGPURenderPipeline cached = imPipeCacheFind(pc, key);
            if (cached) return cached;

            WGPUDevice dev = wgpuGlobals.device;
            // stock Im2DVertex layout unless the shader carries its own (createIm2DShaderEx)
            WGPUVertexAttribute attrs[3] = {};
            attrs[0].shaderLocation = 0;
            attrs[0].format = WGPUVertexFormat_Float32x4;
            attrs[0].offset = 0;
            attrs[1].shaderLocation = 1;
            attrs[1].format = WGPUVertexFormat_Unorm8x4;
            attrs[1].offset = offsetof(Im2DVertex, r);
            attrs[2].shaderLocation = 2;
            attrs[2].format = WGPUVertexFormat_Float32x2;
            attrs[2].offset = offsetof(Im2DVertex, u);
            WGPUVertexBufferLayout vbLayout = {};
            vbLayout.stepMode = WGPUVertexStepMode_Vertex;
            if (sh->numAttribs > 0)
            {
                vbLayout.arrayStride = sh->stride;
                vbLayout.attributeCount = sh->numAttribs;
                vbLayout.attributes = sh->attribs;
            }
            else
            {
                vbLayout.arrayStride = sizeof(Im2DVertex);
                vbLayout.attributeCount = 3;
                vbLayout.attributes = attrs;
            }

            WGPUBlendState blend = {};
            blend.color.srcFactor = sf;
            blend.color.dstFactor = df;
            blend.color.operation = WGPUBlendOperation_Add;
            blend.alpha.srcFactor = sf;
            blend.alpha.dstFactor = df;
            blend.alpha.operation = WGPUBlendOperation_Add;
            WGPUColorTargetState colorTarget = {};
            colorTarget.format = wgpuGlobals.surfaceFormat;
            colorTarget.blend = &blend;
            colorTarget.writeMask = WGPUColorWriteMask_All;
            WGPUFragmentState fragState = {};
            fragState.module = pc->shader;
            fragState.entryPoint = WGPU_STR("fs_main");
            fragState.targetCount = 1;
            fragState.targets = &colorTarget;

            WGPUDepthStencilState ds = makeDepthState(zwrite);
            ds.depthCompare = ztest ? WGPUCompareFunction_LessEqual : WGPUCompareFunction_Always;

            WGPURenderPipelineDescriptor rpDesc = {};
            rpDesc.label = WGPU_STR("im2d custom pipeline");
            rpDesc.layout = pc->playout;
            rpDesc.vertex.module = pc->shader;
            rpDesc.vertex.entryPoint = WGPU_STR("vs_main");
            rpDesc.vertex.bufferCount = 1;
            rpDesc.vertex.buffers = &vbLayout;
            rpDesc.primitive.topology = topology;
            rpDesc.primitive.stripIndexFormat = WGPUIndexFormat_Undefined;
            rpDesc.primitive.frontFace = WGPUFrontFace_CCW;
            rpDesc.primitive.cullMode = WGPUCullMode_None;
            rpDesc.depthStencil = &ds;
            rpDesc.multisample.count = currentSampleCount();
            rpDesc.multisample.mask = 0xFFFFFFFF;
            rpDesc.fragment = &fragState;
            WGPURenderPipeline pipe = wgpuDeviceCreateRenderPipeline(dev, &rpDesc);

            imPipeCacheInsert(pc, key, pipe);
            return pipe;
        }

        static WGPUBindGroup
        customBindGroup(Im2DShader* sh, WGPUTextureView v0, WGPUTextureView v1)
        {
            uint32 gen = wgpuUniformRingGeneration();
            if (gen != sh->bgGen)
            {
                for (int i = 0; i < sh->numBGs; i++)
                    wgpuBindGroupRelease(sh->bgs[i].bg);
                sh->numBGs = 0;
                sh->bgGen = gen;
            }
            for (int i = 0; i < sh->numBGs; i++)
                if (sh->bgs[i].v0 == v0 && sh->bgs[i].v1 == v1)
                    return sh->bgs[i].bg;

            WGPUBindGroupEntry e[6] = {};
            e[0].binding = 0;
            e[0].buffer = s_im2d.uniformBuf;
            e[0].offset = 0;
            e[0].size = 16;
            e[1].binding = 1;
            e[1].textureView = v0;
            e[2].binding = 2;
            e[2].sampler = s_im2dText.sampler;
            e[3].binding = 3;
            e[3].buffer = wgpuUniformRingBuffer();
            e[3].offset = 0; // per-draw dynamic offset
            e[3].size = IM2D_CUSTOM_PARAMS_SIZE;
            e[4].binding = 4;
            e[4].textureView = v1;
            e[5].binding = 5;
            e[5].sampler = s_im2dText.sampler;

            WGPUBindGroupDescriptor d = {};
            d.label = WGPU_STR("im2d custom bg");
            d.layout = customBGL();
            d.entryCount = 6;
            d.entries = e;
            WGPUBindGroup bg = wgpuDeviceCreateBindGroup(wgpuGlobals.device, &d);

            if (sh->numBGs >= IM2D_CUSTOM_MAX_BGS)
            {
                for (int i = 0; i < sh->numBGs; i++)
                    wgpuBindGroupRelease(sh->bgs[i].bg);
                sh->numBGs = 0;
            }
            sh->bgs[sh->numBGs] = {v0, v1, bg};
            sh->numBGs++;
            return bg;
        }

        // Bind a custom shader's pipeline + bind group on the current pass.
        static void
        customBind(Im2DShader* sh, WGPURenderPassEncoder rp, WGPUPrimitiveTopology topology)
        {
            // t0 = current TEXTURERASTER; t1 = setIm2DTexture2; white fallback
            WGPUTextureView v0 = s_im2dText.textureView;
            if (s_renderState.currentRaster)
            {
                WGPURaster* ext = GETWGPURASTEREXT(s_renderState.currentRaster);
                if (ext && ext->view)
                    v0 = ext->view;
            }
            WGPUTextureView v1 = s_im2dTex2View ? s_im2dTex2View : s_im2dText.textureView;

            wgpuRenderPassEncoderSetPipeline(rp, customPipeline(sh, topology));

            uint32 dynOffset = wgpuUniformAlloc(sh->params, IM2D_CUSTOM_PARAMS_SIZE);
            if (dynOffset != 0xFFFFFFFFu)
            {
                WGPUBindGroup bg = customBindGroup(sh, v0, v1);
                wgpuRenderPassEncoderSetBindGroup(rp, 0, bg, 1, &dynOffset);
            }
            else
            {
                // ring full this frame (only until it grows): single private buffer
                if (sh->fallbackParams == nullptr)
                {
                    WGPUBufferDescriptor bd = {};
                    bd.label = WGPU_STR("im2d custom fallback params");
                    bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
                    bd.size = IM2D_CUSTOM_PARAMS_SIZE;
                    sh->fallbackParams = wgpuDeviceCreateBuffer(wgpuGlobals.device, &bd);
                }
                wgpuQueueWriteBuffer(wgpuGlobals.queue, sh->fallbackParams, 0,
                                     sh->params, IM2D_CUSTOM_PARAMS_SIZE);
                WGPUBindGroupEntry e[6] = {};
                e[0].binding = 0;
                e[0].buffer = s_im2d.uniformBuf;
                e[0].size = 16;
                e[1].binding = 1;
                e[1].textureView = v0;
                e[2].binding = 2;
                e[2].sampler = s_im2dText.sampler;
                e[3].binding = 3;
                e[3].buffer = sh->fallbackParams;
                e[3].size = IM2D_CUSTOM_PARAMS_SIZE;
                e[4].binding = 4;
                e[4].textureView = v1;
                e[5].binding = 5;
                e[5].sampler = s_im2dText.sampler;
                WGPUBindGroupDescriptor d = {};
                d.layout = customBGL();
                d.entryCount = 6;
                d.entries = e;
                WGPUBindGroup bg = wgpuDeviceCreateBindGroup(wgpuGlobals.device, &d);
                uint32 zero = 0;
                wgpuRenderPassEncoderSetBindGroup(rp, 0, bg, 1, &zero);
                wgpuBindGroupRelease(bg);
            }
        }

        // Entry used by im2DFlushVerts when im2dOverrideShader is set.
        void
        im2dCustomBind(WGPURenderPassEncoder rp, WGPUPrimitiveTopology topology)
        {
            customBind(im2dOverrideShader, rp, topology);
        }

        // Direct draw with a custom-vertex shader (createIm2DShaderEx). The stock
        // im2d entry points can't carry the extra attributes, so the game hands us
        // raw vertices + triangle-list indices (screen droplets).
        void
        im2DRenderCustom(Im2DShader* sh, const void* verts, uint32 numVerts,
                         const uint16* indices, uint32 numIndices)
        {
            if (sh == nil || verts == nil || numVerts == 0)
                return;
            if (!s_frame.inRenderPass || !s_frame.renderPass)
                return;

            WGPUDevice dev = wgpuGlobals.device;
            WGPUQueue q = wgpuGlobals.queue;
            uint32 stride = sh->numAttribs > 0 ? sh->stride : (uint32)sizeof(Im2DVertex);

            // Same screen→NDC xform as im2DFlushVerts (written only when changed)
            Camera* cam = (Camera*)engine->currentCamera;
            float W = (cam && cam->frameBuffer) ? (float)cam->frameBuffer->width : (float)wgpuGlobals.winWidth;
            float H = (cam && cam->frameBuffer) ? (float)cam->frameBuffer->height : (float)wgpuGlobals.winHeight;
            im2dUpdateXform(W, H);

            // Vertex/index data via the per-frame geometry ring; private buffers only
            // on ring overflow (mirrors im2DFlushVerts).
            uint64_t vbDataBytes = (uint64_t)numVerts * stride;
            uint64_t vbBytes = WGPU_ALIGN4(vbDataBytes);
            WGPUBuffer vb;
            uint64_t vbOffset = 0;
            bool vbPrivate = false;
            uint32 vroff = wgpuGeomAlloc(verts, (uint32)vbDataBytes);
            if (vroff != 0xFFFFFFFFu)
            {
                vb = wgpuGeomRingBuffer();
                vbOffset = vroff;
            }
            else
            {
                WGPUBufferDescriptor vbDesc = {};
                vbDesc.label = WGPU_STR("im2d custom verts");
                vbDesc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_Vertex;
                vbDesc.size = vbBytes;
                vb = wgpuDeviceCreateBuffer(dev, &vbDesc);
                wgpuQueueWriteBuffer(q, vb, 0, verts, vbDataBytes);
                vbPrivate = true;
            }

            WGPUBuffer ib = nullptr;
            uint64_t ibOffset = 0, ibBoundBytes = 0;
            bool ibPrivate = false;
            if (indices && numIndices > 0)
            {
                uint64_t ibDataBytes = (uint64_t)numIndices * sizeof(uint16);
                ibBoundBytes = WGPU_ALIGN4(ibDataBytes);
                uint32 iroff = wgpuGeomAlloc(indices, (uint32)ibDataBytes);
                if (iroff != 0xFFFFFFFFu)
                {
                    ib = wgpuGeomRingBuffer();
                    ibOffset = iroff;
                }
                else
                {
                    uint8* tmpIdx = new uint8[ibBoundBytes];
                    memcpy(tmpIdx, indices, ibDataBytes);
                    if (ibBoundBytes > ibDataBytes)
                        memset(tmpIdx + ibDataBytes, 0, ibBoundBytes - ibDataBytes);
                    WGPUBufferDescriptor ibDesc = {};
                    ibDesc.label = WGPU_STR("im2d custom indices");
                    ibDesc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_Index;
                    ibDesc.size = ibBoundBytes;
                    ib = wgpuDeviceCreateBuffer(dev, &ibDesc);
                    wgpuQueueWriteBuffer(q, ib, 0, tmpIdx, ibBoundBytes);
                    delete[] tmpIdx;
                    ibPrivate = true;
                }
            }

            WGPURenderPassEncoder rp = s_frame.renderPass;
            customBind(sh, rp, WGPUPrimitiveTopology_TriangleList);
            wgpuRenderPassEncoderSetVertexBuffer(rp, 0, vb, vbOffset, vbBytes);
            if (ib)
            {
                wgpuRenderPassEncoderSetIndexBuffer(rp, ib, WGPUIndexFormat_Uint16, ibOffset, ibBoundBytes);
                wgpuRenderPassEncoderDrawIndexed(rp, numIndices, 1, 0, 0, 0);
                if (ibPrivate) wgpuBufferRelease(ib);
            }
            else
            {
                wgpuRenderPassEncoderDraw(rp, numVerts, 1, 0, 0);
            }
            if (vbPrivate) wgpuBufferRelease(vb);
        }

        // ========================================================================
        // Custom object-pipeline shaders — the game's "neo" pipes (vehicle specular
        // + env reflection, world lightmaps, gloss, rim light). See rwwgpu.h for
        // the binding contract. Mirrors the gl3 custom-pipe pattern where the game
        // supplies shaders and per-mesh logic while the backend provides uniforms,
        // instancing and the draw loop.
        // ========================================================================

#define OBJ_CUSTOM_PARAMS_SIZE 256   // 16 vec4 of user params
#define OBJ_CUSTOM_MAX_BGS     32

        // Prefix prepended to the game's WGSL body: the standard object uniforms
        // (layout must match ObjectUniforms in wgpu_internal.h), the user-params
        // block, both textures and the shared helpers. The body defines its own
        // VsIn/VsOut and vs_main/fs_main.
        static const char* k_objCustomPrefixWGSL = R"(
struct Uniforms {
    viewProj: mat4x4<f32>,
    world: mat4x4<f32>,
    ambLight: vec4<f32>,
    matColor: vec4<f32>,
    surfProps: vec4<f32>,  // ambient, specular, diffuse, matAlpha
    alphaTest: vec4<f32>,  // x=ref(0..1), y=enable, z=func(1=GEqual,2=Less)
    fogColor: vec4<f32>,
    fogData:  vec4<f32>,   // x=start, y=end, z=range, w=disable
    lightParams:    array<vec4<f32>, 8>,
    lightPosition:  array<vec4<f32>, 8>,
    lightDirection: array<vec4<f32>, 8>,
    lightColor:     array<vec4<f32>, 8>,
};
struct ObjParams {
    p : array<vec4<f32>, 16>,
};
@group(0) @binding(0) var<uniform> uniforms : Uniforms;
@group(0) @binding(1) var t0 : texture_2d<f32>;
@group(0) @binding(2) var s0 : sampler;
@group(0) @binding(3) var<uniform> u_params : ObjParams;
@group(0) @binding(4) var t1 : texture_2d<f32>;
@group(0) @binding(5) var s1 : sampler;

fn AlphaTestOK(a : f32) -> bool {
    if(uniforms.alphaTest.y > 0.5){
        if(uniforms.alphaTest.z < 1.5){
            if(a < uniforms.alphaTest.x) { return false; }
        } else {
            if(a >= uniforms.alphaTest.x) { return false; }
        }
    }
    return true;
}

fn DoFog(w : f32) -> f32 {
    return clamp((w - uniforms.fogData.y) * uniforms.fogData.z,
                 uniforms.fogData.w, 1.0);
}
)";

        // Extra binding for the skinned variant (68-byte skin vertex layout).
        static const char* k_objCustomSkinWGSL = R"(
@group(0) @binding(6) var<uniform> boneMatrices : array<mat4x4<f32>, 64>;
)";

        struct ObjShaderBG
        {
            WGPUTextureView v0, v1;
            WGPUBindGroup bg;
        };

        struct ObjShader
        {
            WGPUShaderModule module;
            WGPUPipelineLayout playout;
            bool skinned;
            // staged user params, uploaded from the ring per mesh
            uint8 params[OBJ_CUSTOM_PARAMS_SIZE];
            Im2DPipeCache pc;
            // bind-group cache keyed by (texture0 view, texture1 view)
            ObjShaderBG bgs[OBJ_CUSTOM_MAX_BGS];
            int numBGs;
            uint32 bgGen;
        };

        // Second texture (t1/s1) for subsequent objShaderRender draws; white when unset.
        static WGPUTextureView s_objTex2View = nullptr;
        // Per-mesh override for t0 (gloss pass); reset after each mesh.
        static WGPUTextureView s_objTex0View = nullptr;

        // Shared layouts (lazy; reclaimed at device teardown).
        static WGPUBindGroupLayout s_objCustomBGL = nullptr; // 6 bindings
        static WGPUBindGroupLayout s_objCustomSkinBGL = nullptr; // + bones at 6

        void
        wgpuCustomResetBGLs(void)
        {
            s_customBGL = nullptr;
            s_objCustomBGL = nullptr;
            s_objCustomSkinBGL = nullptr;
        }

        static WGPUBindGroupLayout
        objCustomBGL(bool skinned)
        {
            WGPUBindGroupLayout* slot = skinned ? &s_objCustomSkinBGL : &s_objCustomBGL;
            if (*slot)
                return *slot;
            WGPUBindGroupLayoutEntry e[7] = {};
            e[0].binding = 0;
            e[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
            e[0].buffer.type = WGPUBufferBindingType_Uniform;
            e[0].buffer.minBindingSize = sizeof(ObjectUniforms);
            e[0].buffer.hasDynamicOffset = true;
            e[1].binding = 1;
            e[1].visibility = WGPUShaderStage_Fragment;
            e[1].texture.sampleType = WGPUTextureSampleType_Float;
            e[1].texture.viewDimension = WGPUTextureViewDimension_2D;
            e[2].binding = 2;
            e[2].visibility = WGPUShaderStage_Fragment;
            e[2].sampler.type = WGPUSamplerBindingType_Filtering;
            e[3].binding = 3;
            e[3].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
            e[3].buffer.type = WGPUBufferBindingType_Uniform;
            e[3].buffer.minBindingSize = OBJ_CUSTOM_PARAMS_SIZE;
            e[3].buffer.hasDynamicOffset = true;
            e[4].binding = 4;
            e[4].visibility = WGPUShaderStage_Fragment;
            e[4].texture.sampleType = WGPUTextureSampleType_Float;
            e[4].texture.viewDimension = WGPUTextureViewDimension_2D;
            e[5].binding = 5;
            e[5].visibility = WGPUShaderStage_Fragment;
            e[5].sampler.type = WGPUSamplerBindingType_Filtering;
            e[6].binding = 6;
            e[6].visibility = WGPUShaderStage_Vertex;
            e[6].buffer.type = WGPUBufferBindingType_Uniform;
            e[6].buffer.minBindingSize = OBJ_SKIN_MAX_BONES * 64;
            e[6].buffer.hasDynamicOffset = true;

            WGPUBindGroupLayoutDescriptor d = {};
            d.label = WGPU_STR(skinned ? "obj custom skin bgl" : "obj custom bgl");
            d.entryCount = skinned ? 7 : 6;
            d.entries = e;
            *slot = wgpuDeviceCreateBindGroupLayout(wgpuGlobals.device, &d);
            return *slot;
        }

        static ObjShader*
        createObjShaderCommon(const char* bodyWGSL, bool skinned)
        {
            WGPUDevice dev = wgpuGlobals.device;
            if (dev == nullptr)
                return nil;

            // prefix (+ skin binding + shared lighting) + game body
            size_t plen = strlen(k_objCustomPrefixWGSL);
            size_t slen = skinned ? strlen(k_objCustomSkinWGSL) : 0;
            size_t llen = strlen(k_doDynamicLightWGSL);
            size_t blen = strlen(bodyWGSL);
            char* src = rwNewT(char, plen + slen + llen + blen + 1, MEMDUR_EVENT);
            char* p = src;
            memcpy(p, k_objCustomPrefixWGSL, plen);
            p += plen;
            if (skinned)
            {
                memcpy(p, k_objCustomSkinWGSL, slen);
                p += slen;
            }
            memcpy(p, k_doDynamicLightWGSL, llen);
            p += llen;
            memcpy(p, bodyWGSL, blen + 1);

            WGPUShaderSourceWGSL wgslDesc = {};
            wgslDesc.chain.sType = WGPUSType_ShaderSourceWGSL;
            wgslDesc.code = WGPU_STR(src);
            WGPUShaderModuleDescriptor sd = {};
            sd.nextInChain = &wgslDesc.chain;
            sd.label = WGPU_STR("obj custom shader");
            WGPUShaderModule mod = wgpuDeviceCreateShaderModule(dev, &sd);
            rwFree(src);
            if (mod == nullptr)
                return nil;

            ObjShader* sh = rwNewT(ObjShader, 1, MEMDUR_EVENT);
            memset(sh, 0, sizeof(*sh));
            sh->module = mod;
            sh->skinned = skinned;
            sh->bgGen = 0xFFFFFFFFu;

            WGPUBindGroupLayout bgl = objCustomBGL(skinned);
            WGPUPipelineLayoutDescriptor pld = {};
            pld.label = WGPU_STR("obj custom pl");
            pld.bindGroupLayoutCount = 1;
            pld.bindGroupLayouts = &bgl;
            sh->playout = wgpuDeviceCreatePipelineLayout(dev, &pld);
            sh->pc.shader = mod; // (P1-6) Im2DPipeCache setup — memset zeroed it
            sh->pc.playout = sh->playout;
            return sh;
        }

        ObjShader*
        createObjShader(const char* bodyWGSL)
        {
            return createObjShaderCommon(bodyWGSL, false);
        }

        ObjShader*
        createObjSkinShader(const char* bodyWGSL)
        {
            return createObjShaderCommon(bodyWGSL, true);
        }

        void
        destroyObjShader(ObjShader* sh)
        {
            if (sh == nil)
                return;
            imPipeCacheRelease(&sh->pc); // (P1-6)
            for (int i = 0; i < sh->numBGs; i++)
                wgpuBindGroupRelease(sh->bgs[i].bg);
            if (sh->playout)
                wgpuPipelineLayoutRelease(sh->playout);
            if (sh->module)
                wgpuShaderModuleRelease(sh->module);
            rwFree(sh);
        }

        void
        setObjShaderParams(ObjShader* sh, const void* data, uint32 size)
        {
            if (sh == nil)
                return;
            if (size > OBJ_CUSTOM_PARAMS_SIZE)
                size = OBJ_CUSTOM_PARAMS_SIZE;
            memcpy(sh->params, data, size);
        }

        void
        setObjTexture2(Texture* tex)
        {
            s_objTex2View = nullptr;
            if (tex && tex->raster)
            {
                WGPURaster* ext = GETWGPURASTEREXT(tex->raster);
                if (ext && ext->view)
                    s_objTex2View = ext->view;
            }
        }

        void
        setObjTexture0(Texture* tex)
        {
            s_objTex0View = nullptr;
            if (tex && tex->raster)
            {
                WGPURaster* ext = GETWGPURASTEREXT(tex->raster);
                if (ext && ext->view)
                    s_objTex0View = ext->view;
            }
        }

        static WGPUCullMode
        objToWGPUCullMode(uint32 rwCull)
        {
            switch (rwCull)
            {
            case CULLBACK: return WGPUCullMode_Back;
            case CULLFRONT: return WGPUCullMode_Front;
            default: return WGPUCullMode_None;
            }
        }

        // Build/cache a pipeline for the given blend/depth state (mirrors the object
        // pipeline cache, but with the custom module and its vertex layout).
        static WGPURenderPipeline
        objCustomPipeline(ObjShader* sh, bool blendEnable, bool zWrite)
        {
            uint32 sf = s_renderState.srcBlend, df = s_renderState.destBlend;
            bool ztest = s_renderState.zTest != 0;
            uint32 cull = s_renderState.cullMode;
            uint32 key = (uint32)blendEnable
                | ((uint32)zWrite << 1)
                | ((sf & 0xF) << 2)
                | ((df & 0xF) << 6)
                | ((cull & 0x3) << 10)
                | ((uint32)ztest << 12)
                | (currentSampleCount() << 28);
            WGPURenderPipeline cached = imPipeCacheFind(&sh->pc, key);
            if (cached) return cached;

            WGPUDevice dev = wgpuGlobals.device;

            // Vertex layout: default object layout (56B, 5 attrs incl. uv1) or the
            // skinned layout (68B, 6 attrs incl. weights/indices).
            WGPUVertexAttribute attrs[6] = {};
            WGPUVertexBufferLayout vbLayout = {};
            vbLayout.stepMode = WGPUVertexStepMode_Vertex;
            attrs[0].shaderLocation = 0;
            attrs[0].format = WGPUVertexFormat_Float32x3;
            attrs[0].offset = 0;
            attrs[1].shaderLocation = 1;
            attrs[1].format = WGPUVertexFormat_Float32x3;
            attrs[1].offset = 12;
            attrs[2].shaderLocation = 2;
            attrs[2].format = WGPUVertexFormat_Float32x4;
            attrs[2].offset = 24;
            attrs[3].shaderLocation = 3;
            attrs[3].format = WGPUVertexFormat_Float32x2;
            attrs[3].offset = 40;
            if (sh->skinned)
            {
                attrs[4].shaderLocation = 4;
                attrs[4].format = WGPUVertexFormat_Float32x4;
                attrs[4].offset = 48;
                attrs[5].shaderLocation = 5;
                attrs[5].format = WGPUVertexFormat_Uint8x4;
                attrs[5].offset = 64;
                vbLayout.arrayStride = OBJ_SKIN_VERTEX_STRIDE;
                vbLayout.attributeCount = 6;
            }
            else
            {
                attrs[4].shaderLocation = 4;
                attrs[4].format = WGPUVertexFormat_Float32x2;
                attrs[4].offset = 48;
                vbLayout.arrayStride = OBJ_VERTEX_STRIDE;
                vbLayout.attributeCount = 5;
            }
            vbLayout.attributes = attrs;

            WGPUBlendState blend = {};
            blend.color.srcFactor = toWGPUBlendFactor(sf);
            blend.color.dstFactor = toWGPUBlendFactor(df);
            blend.color.operation = WGPUBlendOperation_Add;
            blend.alpha.srcFactor = blend.color.srcFactor;
            blend.alpha.dstFactor = blend.color.dstFactor;
            blend.alpha.operation = WGPUBlendOperation_Add;
            WGPUColorTargetState colorTarget = {};
            colorTarget.format = wgpuGlobals.surfaceFormat;
            colorTarget.blend = blendEnable ? &blend : nullptr;
            colorTarget.writeMask = WGPUColorWriteMask_All;
            WGPUFragmentState fragState = {};
            fragState.module = sh->module;
            fragState.entryPoint = WGPU_STR("fs_main");
            fragState.targetCount = 1;
            fragState.targets = &colorTarget;

            WGPUDepthStencilState ds = makeDepthState(zWrite);
            ds.depthCompare = ztest ? WGPUCompareFunction_LessEqual : WGPUCompareFunction_Always;

            WGPURenderPipelineDescriptor rpDesc = {};
            rpDesc.label = WGPU_STR("obj custom pipeline");
            rpDesc.layout = sh->playout;
            rpDesc.vertex.module = sh->module;
            rpDesc.vertex.entryPoint = WGPU_STR("vs_main");
            rpDesc.vertex.bufferCount = 1;
            rpDesc.vertex.buffers = &vbLayout;
            rpDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
            rpDesc.primitive.stripIndexFormat = WGPUIndexFormat_Undefined;
            rpDesc.primitive.frontFace = WGPUFrontFace_CCW;
            rpDesc.primitive.cullMode = objToWGPUCullMode(cull);
            rpDesc.depthStencil = &ds;
            rpDesc.multisample.count = currentSampleCount();
            rpDesc.multisample.mask = 0xFFFFFFFF;
            rpDesc.fragment = &fragState;
            WGPURenderPipeline pipe = wgpuDeviceCreateRenderPipeline(dev, &rpDesc);

            imPipeCacheInsert(&sh->pc, key, pipe);
            return pipe;
        }

        // Ring-backed bind group cached per (t0, t1). All buffer bindings point at the
        // uniform ring and are addressed with per-draw dynamic offsets.
        static WGPUBindGroup
        objCustomBindGroup(ObjShader* sh, WGPUTextureView v0, WGPUTextureView v1)
        {
            uint32 gen = wgpuUniformRingGeneration();
            if (gen != sh->bgGen)
            {
                for (int i = 0; i < sh->numBGs; i++)
                    wgpuBindGroupRelease(sh->bgs[i].bg);
                sh->numBGs = 0;
                sh->bgGen = gen;
            }
            for (int i = 0; i < sh->numBGs; i++)
                if (sh->bgs[i].v0 == v0 && sh->bgs[i].v1 == v1)
                    return sh->bgs[i].bg;

            WGPUBuffer ring = wgpuUniformRingBuffer();
            WGPUSampler sampler = objDefaultSampler();
            WGPUBindGroupEntry e[7] = {};
            e[0].binding = 0;
            e[0].buffer = ring;
            e[0].size = sizeof(ObjectUniforms);
            e[1].binding = 1;
            e[1].textureView = v0;
            e[2].binding = 2;
            e[2].sampler = sampler;
            e[3].binding = 3;
            e[3].buffer = ring;
            e[3].size = OBJ_CUSTOM_PARAMS_SIZE;
            e[4].binding = 4;
            e[4].textureView = v1;
            e[5].binding = 5;
            e[5].sampler = sampler;
            e[6].binding = 6;
            e[6].buffer = ring;
            e[6].size = OBJ_SKIN_MAX_BONES * 64;

            WGPUBindGroupDescriptor d = {};
            d.label = WGPU_STR("obj custom bg");
            d.layout = objCustomBGL(sh->skinned);
            d.entryCount = sh->skinned ? 7 : 6;
            d.entries = e;
            WGPUBindGroup bg = wgpuDeviceCreateBindGroup(wgpuGlobals.device, &d);

            if (sh->numBGs >= OBJ_CUSTOM_MAX_BGS)
            {
                for (int i = 0; i < sh->numBGs; i++)
                    wgpuBindGroupRelease(sh->bgs[i].bg);
                sh->numBGs = 0;
            }
            sh->bgs[sh->numBGs] = {v0, v1, bg};
            sh->numBGs++;
            return bg;
        }

        void
        objDefaultRender(ObjPipeline* pipe, Atomic* atomic)
        {
            atomicRender(pipe, atomic);
        }

        void
        objDefaultInstance(ObjPipeline* pipe, Atomic* atomic)
        {
            instance(pipe, atomic);
        }

        void
        objSkinInstance(ObjPipeline* pipe, Atomic* atomic)
        {
            (void)pipe;
            skinInstanceAtomic(atomic);
        }

        static float s_objBoneMatrices[OBJ_SKIN_MAX_BONES * 16];

        void
        objShaderRender(ObjShader* sh, Atomic* atomic, ObjShaderMeshCB meshCB, void* user,
                        uint32 flags)
        {
            if (sh == nil || !s_frame.inRenderPass || !s_frame.renderPass)
                return;

            Geometry* geo = atomic->geometry;
            if (sh->skinned)
                skinInstanceAtomic(atomic);
            else
                instance(nil, atomic);
            if (geo->instData == nil || geo->instData->platform != PLATFORM_WGPU)
                return;
            WGPUInstanceDataHeader* header = (WGPUInstanceDataHeader*)geo->instData;

            ObjectUniforms uniforms;
            fillObjectUniforms(atomic, &uniforms);

            bool hasTexture = (geo->flags & Geometry::TEXTURED) != 0;

            // Bone matrices: one ring allocation shared by all meshes of the atomic.
            uint32 boneDynOffset = 0;
            if (sh->skinned)
            {
                memset(s_objBoneMatrices, 0, sizeof(s_objBoneMatrices));
                skinComputeBoneMatrices(atomic, s_objBoneMatrices);
                boneDynOffset = wgpuUniformAlloc(s_objBoneMatrices, sizeof(s_objBoneMatrices));
                if (boneDynOffset == 0xFFFFFFFFu)
                    return; // ring exhausted this frame; it grows for the next one
            }

            for (uint32 i = 0; i < header->numMeshes; i++)
            {
                WGPUInstanceData* inst = &header->inst[i];
                Material* mat = inst->material;

                if (mat)
                {
                    uniforms.ambient[0] = mat->color.red / 255.0f;
                    uniforms.ambient[1] = mat->color.green / 255.0f;
                    uniforms.ambient[2] = mat->color.blue / 255.0f;
                    uniforms.ambient[3] = mat->color.alpha / 255.0f;
                    uniforms.matProps[0] = mat->surfaceProps.ambient;
                    uniforms.matProps[1] = mat->surfaceProps.specular;
                    uniforms.matProps[2] = mat->surfaceProps.diffuse;
                    uniforms.matProps[3] = mat->color.alpha / 255.0f;
                }
                else
                {
                    uniforms.ambient[0] = uniforms.ambient[1] = uniforms.ambient[2] = uniforms.ambient[3] = 1.0f;
                    uniforms.matProps[0] = 1.0f;
                    uniforms.matProps[1] = 0.0f;
                    uniforms.matProps[2] = 1.0f;
                    uniforms.matProps[3] = 1.0f;
                }

                // Per-mesh game hook: may update params (setObjShaderParams), override
                // t0 (setObjTexture0) or skip the mesh entirely.
                s_objTex0View = nullptr;
                if (meshCB && !meshCB(sh, atomic, mat, user))
                    continue;

                WGPUTextureView v0 = objDefaultTextureView();
                if (s_objTex0View)
                    v0 = s_objTex0View;
                else if (hasTexture && mat && mat->texture && mat->texture->raster)
                {
                    WGPURaster* ext = GETWGPURASTEREXT(mat->texture->raster);
                    if (ext && ext->view)
                        v0 = ext->view;
                }
                WGPUTextureView v1 = s_objTex2View ? s_objTex2View : objDefaultTextureView();

                // Same blend rule + PS2 GS alpha-test emulation as the default object
                // pipeline (wgpupipe.cpp defaultRenderCB) so custom pipes don't regress
                // soft alpha (ped hair, glass). OBJRENDER_FORCEBLEND (gloss ONE/ONE
                // pass) forces blending regardless of mesh alpha.
                bool texHasAlpha = false;
                if (mat && mat->texture && mat->texture->raster)
                {
                    uint32 cf = mat->texture->raster->format & 0xF00;
                    texHasAlpha = (cf == Raster::C8888 || cf == Raster::C1555 || cf == Raster::C4444);
                }
                bool meshBlend = (flags & OBJRENDER_FORCEBLEND) != 0 ||
                    inst->vertexAlpha || (mat && mat->color.alpha != 255) || texHasAlpha;

                struct DrawPass
                {
                    bool blend;
                    bool zWrite;
                    float atRef;
                    float atEnable;
                    float atFunc;
                };
                DrawPass passes[2];
                int numPasses = 1;
                bool gsEmu = s_renderState.gsAlphaTest && meshBlend &&
                    (flags & OBJRENDER_FORCEBLEND) == 0;
                if (gsEmu && s_renderState.zWrite != 0)
                {
                    float gsref = s_renderState.gsAlphaTestRef / 255.0f;
                    passes[0] = {true, true, gsref, 1.0f, (float)ALPHAGREATEREQUAL};
                    passes[1] = {true, false, gsref, 1.0f, (float)ALPHALESS};
                    numPasses = 2;
                }
                else if (gsEmu)
                {
                    passes[0] = {true, false, 0.0f, 0.0f, (float)ALPHAALWAYS};
                }
                else
                {
                    passes[0] = {
                        meshBlend, s_renderState.zWrite != 0,
                        s_renderState.alphaTestRef / 255.0f,
                        (s_renderState.alphaTestFunc != ALPHAALWAYS) ? 1.0f : 0.0f,
                        (float)s_renderState.alphaTestFunc
                    };
                }

                for (int p = 0; p < numPasses; p++)
                {
                    uniforms.alphaTest[0] = passes[p].atRef;
                    uniforms.alphaTest[1] = passes[p].atEnable;
                    uniforms.alphaTest[2] = passes[p].atFunc;

                    WGPURenderPipeline pipeline = objCustomPipeline(sh, passes[p].blend, passes[p].zWrite);
                    wgpuRenderPassEncoderSetPipeline(s_frame.renderPass, pipeline);

                    uint32 uniOffset = wgpuUniformAlloc(&uniforms, sizeof(ObjectUniforms));
                    uint32 paramsOffset = wgpuUniformAlloc(sh->params, OBJ_CUSTOM_PARAMS_SIZE);
                    if (uniOffset == 0xFFFFFFFFu || paramsOffset == 0xFFFFFFFFu)
                        continue; // ring exhausted; it grows for the next frame

                    WGPUBindGroup bg = objCustomBindGroup(sh, v0, v1);
                    // dynamic offsets in ascending binding order: 0 (uniforms),
                    // 3 (params), 6 (bones, skinned only)
                    uint32 dynOffsets[3] = {uniOffset, paramsOffset, boneDynOffset};
                    wgpuRenderPassEncoderSetBindGroup(s_frame.renderPass, 0, bg,
                                                      sh->skinned ? 3 : 2, dynOffsets);
                    wgpuRenderPassEncoderSetVertexBuffer(s_frame.renderPass, 0, inst->vertexBuffer, 0, WGPU_WHOLE_SIZE);
                    wgpuRenderPassEncoderSetIndexBuffer(s_frame.renderPass, inst->indexBuffer, WGPUIndexFormat_Uint16,
                                                        0, WGPU_WHOLE_SIZE);
                    wgpuRenderPassEncoderDrawIndexed(s_frame.renderPass, inst->numIndices, 1, 0, 0, 0);
                }
            }
            s_objTex0View = nullptr;
        }
    } // namespace wgpu
} // namespace rw
#endif
