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
        // Im2D — WGSL шейдер (нетекстурированный)
        // ========================================================================
        static const char* k_im2dWGSL = R"(
struct Xform {
    xScl : f32,
    yScl : f32,
    xOff : f32,
    yOff : f32,
};
@group(0) @binding(0) var<uniform> u_xform : Xform;

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
    // Unorm8x4 reads bytes in memory order (r,g,b,a) -> vec4(R,G,B,A); no swap.
    out.color = v.color;
    out.uv    = v.uv;
    return out;
}

@fragment
fn fs_main(f : VsOut) -> @location(0) vec4<f32> {
    return f.color;
}
)";

        // ========================================================================
        // Im2D — WGSL шейдер (текстурированный)
        // ========================================================================
        static const char* k_im2dTextWGSL = R"(
struct Xform {
    xScl : f32,
    yScl : f32,
    xOff : f32,
    yOff : f32,
};
@group(0) @binding(0) var<uniform> u_xform : Xform;
@group(0) @binding(1) var t_diffuse : texture_2d<f32>;
@group(0) @binding(2) var s_diffuse : sampler;

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

@fragment
fn fs_main(f : VsOut) -> @location(0) vec4<f32> {
    return textureSample(t_diffuse, s_diffuse, f.uv) * f.color;
}
)";

        // ========================================================================
        // Im3D — WGSL шейдер
        // ========================================================================
        static const char* k_im3dWGSL = R"(
struct SceneUniforms {
    viewProj : mat4x4<f32>,  // combined View * Proj  (64 bytes each)
    world    : mat4x4<f32>,
    fogColor : vec4<f32>,
    fogData : vec4<f32>,
}

@group(0) @binding(0) var<uniform> scene    : SceneUniforms;
@group(0) @binding(1) var t_diffuse : texture_2d<f32>;
@group(0) @binding(2) var s_diffuse : sampler;

struct VsIn {
    @location(0) position : vec3<f32>,
    @location(1) normal   : vec3<f32>,
    @location(2) color    : vec4<f32>,
    @location(3) uv       : vec2<f32>,
}
struct VsOut {
    @builtin(position) clip_pos : vec4<f32>,
    @location(0)       color    : vec4<f32>,
    @location(1)       uv       : vec2<f32>,
    @location(2) fog : f32,
}

@vertex
fn vs_main(v : VsIn) -> VsOut {
    var out : VsOut;
    let world_pos = scene.world * vec4<f32>(v.position, 1.0);
    out.clip_pos = scene.viewProj * world_pos;
    // Unorm8x4 reads bytes in memory order (r,g,b,a) -> vec4(R,G,B,A); no swap.
    out.color = v.color;
    out.uv    = v.uv;
    // Fog: same model as gl3 im3d.vert (v_fog = DoFog(gl_Position.w)) and
    // the object shader. GTA's far/LOD water and other im3d geometry fade
    // into the horizon fog; without this the far water is an unfogged sheet
    // stretching to the horizon.
    out.fog = clamp((out.clip_pos.w - scene.fogData.y) * scene.fogData.z,
                    scene.fogData.w, 1.0);
    return out;
}

@fragment
fn fs_main(f : VsOut) -> @location(0) vec4<f32> {
    let texColor = textureSample(t_diffuse, s_diffuse, f.uv);
    var rgb = texColor.rgb * f.color.rgb;
    rgb = mix(scene.fogColor.rgb, rgb, f.fog);
    return vec4<f32>(rgb, texColor.a * f.color.a);
}
)";

        // ========================================================================
        // Im2D / Im3D global state
        // ========================================================================
        Im2DState s_im2d = {};

        // Temporary vertex buffer for single-call primitives
        Im2DVertex s_im2dTmpBuf[3];

        // Reusable scratch for the fan/strip → list index conversion (was a new[] per
        // draw). One entry of slack so the private-buffer fallback can pad the upload
        // to a 4-byte multiple without reading past the end.
        static uint16* s_imScratchIdx = nullptr;
        static int32 s_imScratchCap = 0;

        static uint16* imScratchIndices(int32 n)
        {
            if (n + 1 > s_imScratchCap)
            {
                int32 want = s_imScratchCap ? s_imScratchCap : 4096;
                while (want < n + 1) want *= 2;
                free(s_imScratchIdx);
                s_imScratchIdx = (uint16*)malloc((size_t)want * sizeof(uint16));
                s_imScratchCap = want;
            }
            return s_imScratchIdx;
        }

        // Write the im2d screen→NDC xform uniform only when the target size changed —
        // this used to run per im2d draw (fonts/HUD/coronas), and queue writes have
        // real per-call cost. Shared by im2DFlushVerts and im2DRenderCustom (both feed
        // the same buffer); reset in openIm2D so a device restart rewrites it.
        static float s_im2dXformW = 0.0f, s_im2dXformH = 0.0f;

        void im2dUpdateXform(float W, float H)
        {
            if (W <= 0.0f || H <= 0.0f) return;
            if (W == s_im2dXformW && H == s_im2dXformH) return;
            struct
            {
                float xScl, yScl, xOff, yOff;
            } xform = {2.0f / W, -2.0f / H, -1.0f, 1.0f};
            wgpuQueueWriteBuffer(wgpuGlobals.queue, s_im2d.uniformBuf, 0, &xform, sizeof(xform));
            s_im2dXformW = W;
            s_im2dXformH = H;
        }

        Im2DTextState s_im2dText = {};

        // (P2-13) 1-slot cache for the NEAREST bind group of the raster currently
        // drawn via im2d (rare state — most HUD elements are Linear).
        static WGPUTextureView s_im2dNearestView = nullptr;
        static WGPUBindGroup s_im2dNearestBg = nullptr;

        Im3DState s_im3d = {};

        // ========================================================================
        // Utility
        // ========================================================================
        WGPUDepthStencilState makeDepthState(bool write)
        {
            WGPUDepthStencilState ds = {};
            ds.format = WGPUTextureFormat_Depth32Float;
            ds.depthWriteEnabled = write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
            ds.depthCompare = write ? WGPUCompareFunction_LessEqual : WGPUCompareFunction_Always;
            ds.stencilFront = WGPU_STENCIL_FACE_STATE_INIT;
            ds.stencilBack = WGPU_STENCIL_FACE_STATE_INIT;
            ds.stencilReadMask = 0;
            ds.stencilWriteMask = 0;
            return ds;
        }

        WGPUBlendState makeAlphaBlend()
        {
            WGPUBlendState b = {};
            b.color.srcFactor = WGPUBlendFactor_SrcAlpha;
            b.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
            b.color.operation = WGPUBlendOperation_Add;
            b.alpha.srcFactor = WGPUBlendFactor_One;
            b.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
            b.alpha.operation = WGPUBlendOperation_Add;
            return b;
        }

        // ========================================================================
        // Matrix multiply helper (row-major 4x4)
        // ========================================================================
        void mat4Mult(float* dst, const float* a, const float* b)
        {
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                {
                    float s = 0;
                    for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
                    dst[r * 4 + c] = s;
                }
        }

        // Convert an im2d/im3d primitive (tri list/fan/strip) into triangle-list
        // indices. `srcIdx` may be null → implicit sequence 0..srcCount-1 (non-indexed).
        // `out` must hold up to 3*max(srcCount,1). Returns the produced index count.
        // Our im2d/im3d pipelines bake TriangleList topology (line/point prims skipped).
        static int32 imBuildTriListIndices(PrimitiveType primType, const uint16* srcIdx, int32 srcCount, uint16* out)
        {
#define IM_SRC(i) (srcIdx ? srcIdx[i] : (uint16_t)(i))
            int32 n = 0;
            switch (primType)
            {
            case PRIMTYPETRILIST:
                for (int32 i = 0; i + 2 < srcCount; i += 3)
                {
                    out[n++] = IM_SRC(i);
                    out[n++] = IM_SRC(i+1);
                    out[n++] = IM_SRC(i+2);
                }
                break;
            case PRIMTYPETRIFAN:
                for (int32 i = 1; i + 1 < srcCount; i++)
                {
                    out[n++] = IM_SRC(0);
                    out[n++] = IM_SRC(i);
                    out[n++] = IM_SRC(i+1);
                }
                break;
            case PRIMTYPETRISTRIP:
                for (int32 i = 0; i + 2 < srcCount; i++)
                {
                    if (i & 1)
                    {
                        out[n++] = IM_SRC(i+1);
                        out[n++] = IM_SRC(i);
                        out[n++] = IM_SRC(i+2);
                    }
                    else
                    {
                        out[n++] = IM_SRC(i);
                        out[n++] = IM_SRC(i+1);
                        out[n++] = IM_SRC(i+2);
                    }
                }
                break;
            default: break; // lines/points not renderable by the triangle pipeline
            }
#undef IM_SRC
            return n;
        }

        // Build line-list indices for a line primitive. LINELIST → pairs (0,1)(2,3)…,
        // POLYLINE → consecutive (0,1)(1,2)(2,3)… `srcIdx` null → implicit 0..srcCount-1.
        // `out` must hold up to 2*srcCount. Returns the produced index count.
        static int32 imBuildLineIndices(PrimitiveType primType, const uint16* srcIdx, int32 srcCount, uint16* out)
        {
#define IM_SRC(i) (srcIdx ? srcIdx[i] : (uint16_t)(i))
            int32 n = 0;
            switch (primType)
            {
            case PRIMTYPELINELIST:
                for (int32 i = 0; i + 1 < srcCount; i += 2)
                {
                    out[n++] = IM_SRC(i);
                    out[n++] = IM_SRC(i+1);
                }
                break;
            case PRIMTYPEPOLYLINE:
                for (int32 i = 0; i + 1 < srcCount; i++)
                {
                    out[n++] = IM_SRC(i);
                    out[n++] = IM_SRC(i+1);
                }
                break;
            default: break;
            }
#undef IM_SRC
            return n;
        }

        // Select (build + cache) an im2d pipeline for the caller's current blend state
        // and the requested topology (triangle-list for quads/sprites, line-list for
        // corona streaks / debug lines). GTA draws additive coronas, alpha UI, etc. via
        // im2d and sets SRCBLEND/DESTBLEND per draw; WGPU bakes blend + topology into the
        // pipeline, so we key a small cache on both.
        static WGPURenderPipeline im2dPipelineForBlend(Im2DPipeCache* pc, WGPUPrimitiveTopology topology)
        {
            WGPUBlendFactor sf = toWGPUBlendFactor(s_renderState.srcBlend);
            WGPUBlendFactor df = toWGPUBlendFactor(s_renderState.destBlend);
            // Respect ZTEST (HUD/text draw on top with ztest off; coronas/pickups set
            // ZTEST on so scene geometry occludes them) AND ZWRITE. The radar circular
            // mask (CRadar::DrawRadarMask) relies on im2d honouring ZWRITE: it stamps
            // depth into the four corner segments outside the circle (blend ZERO/ONE, so
            // colour is untouched), then the map tiles draw with ZTEST on / ZWRITE off
            // and are clipped to the circle by that depth. Without ZWRITE the map is a
            // square.
            bool ztest = s_renderState.zTest != 0;
            bool zwrite = s_renderState.zWrite != 0;
            bool isLine = (topology == WGPUPrimitiveTopology_LineList);
            // (P2-14) gl3 parity: blending is enabled only when the caller set
            // VERTEXALPHA (gl3's setVertexAlpha -> setAlphaBlend). With alpha = 255
            // the result is identical, but vertex alpha < 255 with VERTEXALPHA off
            // must write the source colour directly, not blend it.
            bool vertexAlpha = s_renderState.vertexAlpha != 0;
            uint32 key = ((uint32)sf << 8) | (uint32)df | (ztest ? (1u << 20) : 0u)
                | (isLine ? (1u << 21) : 0u) | (zwrite ? (1u << 22) : 0u)
                | (vertexAlpha ? (1u << 23) : 0u);
            WGPURenderPipeline cached = imPipeCacheFind(pc, key);
            if (cached) return cached;

            WGPUDevice dev = wgpuGlobals.device;
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
            vbLayout.arrayStride = sizeof(Im2DVertex);
            vbLayout.stepMode = WGPUVertexStepMode_Vertex;
            vbLayout.attributeCount = 3;
            vbLayout.attributes = attrs;

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

            // Depth: write when ZWRITE is on (radar mask), test when ZTEST is on.
            WGPUDepthStencilState ds = makeDepthState(zwrite);
            ds.depthCompare = ztest ? WGPUCompareFunction_LessEqual : WGPUCompareFunction_Always;

            WGPURenderPipelineDescriptor rpDesc = {};
            rpDesc.label = WGPU_STR("im2d pipeline (blend)");
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
            rpDesc.multisample.count = 1;
            rpDesc.multisample.mask = 0xFFFFFFFF;
            rpDesc.fragment = &fragState;
            WGPURenderPipeline pipe = wgpuDeviceCreateRenderPipeline(dev, &rpDesc);

            imPipeCacheInsert(pc, key, pipe);
            return pipe;
        }

        // ========================================================================
        // Im2D flush
        // ========================================================================
        static void im2DFlushVerts(PrimitiveType primType, const void* verts, int32 numVerts,
                                   const void* indices, int32 numIndices)
        {
            if (!s_frame.inRenderPass || !s_frame.renderPass) return;
            if (numVerts <= 0) return;

            // (P3-20) Non-indexed draws build implicit uint16 indices; more than
            // 65536 vertices would silently wrap (i % 65536) and re-draw the first
            // vertices. Clamp once and warn instead of drawing wrapped geometry.
            if (!indices && numVerts > 65536)
            {
                static bool warned = false;
                if (!warned)
                {
                    fprintf(stderr, "[wgpu] im2d: non-indexed draw with %d verts — clamping to 65536\n",
                            numVerts);
                    warned = true;
                }
                numVerts = 65536;
            }

            WGPUDevice dev = wgpuGlobals.device;
            WGPUQueue q = wgpuGlobals.queue;

            // Update xform uniform (writes only when the target size changed)
            Camera* cam = (Camera*)engine->currentCamera;
            float W = (cam && cam->frameBuffer && cam->frameBuffer->width > 0)
                          ? (float)cam->frameBuffer->width
                          : (float)s_frame.fbWidth;
            float H = (cam && cam->frameBuffer && cam->frameBuffer->height > 0)
                          ? (float)cam->frameBuffer->height
                          : (float)s_frame.fbHeight;
            im2dUpdateXform(W, H);

            // Vertex data — sub-allocate from the geometry ring; private buffer only on
            // ring overflow (a buffer create per draw was a dominant per-draw cost).
            uint64_t vbDataBytes = (uint64_t)numVerts * sizeof(Im2DVertex);
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
                vbDesc.label = WGPU_STR("im2d verts");
                vbDesc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_Vertex;
                vbDesc.size = vbBytes;
                vb = wgpuDeviceCreateBuffer(dev, &vbDesc);
                wgpuQueueWriteBuffer(q, vb, 0, verts, vbDataBytes);
                vbPrivate = true;
            }

            // Our im2d pipelines bake topology, but callers submit tri FANS/STRIPS/lists
            // and line lists/polylines (coronas streaks, debug). Build an index array in
            // the matching primitive class for a baked LineList or TriangleList pipeline.
            // Source indices are the caller's (indexed) or implicit 0..numVerts-1.
            bool isLine = (primType == PRIMTYPELINELIST || primType == PRIMTYPEPOLYLINE);
            WGPUPrimitiveTopology topology = isLine
                                                 ? WGPUPrimitiveTopology_LineList
                                                 : WGPUPrimitiveTopology_TriangleList;
            int32 srcCount = (indices && numIndices > 0) ? numIndices : numVerts;
            const uint16* srcIdx = (const uint16*)indices;
            uint16* listIdx = imScratchIndices((srcCount > 2 ? srcCount : 3) * 3);
            int32 listCount = isLine
                                  ? imBuildLineIndices(primType, srcIdx, srcCount, listIdx)
                                  : imBuildTriListIndices(primType, srcIdx, srcCount, listIdx);

            if (listCount == 0)
            {
                if (vbPrivate) wgpuBufferRelease(vb);
                return;
            }

            uint64_t ibDataBytes = (uint64_t)listCount * sizeof(uint16_t);
            uint64_t ibBoundBytes = WGPU_ALIGN4(ibDataBytes);
            WGPUBuffer ib;
            uint64_t ibOffset = 0;
            bool ibPrivate = false;
            uint32 iroff = wgpuGeomAlloc(listIdx, (uint32)ibDataBytes);
            if (iroff != 0xFFFFFFFFu)
            {
                ib = wgpuGeomRingBuffer();
                ibOffset = iroff;
            }
            else
            {
                listIdx[listCount] = 0; // pad the upload to a 4-byte multiple (scratch has slack)
                WGPUBufferDescriptor ibDesc = {};
                ibDesc.label = WGPU_STR("im2d indices");
                ibDesc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_Index;
                ibDesc.size = ibBoundBytes;
                ib = wgpuDeviceCreateBuffer(dev, &ibDesc);
                wgpuQueueWriteBuffer(q, ib, 0, listIdx, ibBoundBytes);
                ibPrivate = true;
            }

            WGPURenderPassEncoder rp = s_frame.renderPass;

            // Choose pipeline (per current blend state) and bind group. A set
            // im2dOverrideShader (postfx colour filter / screen droplets) replaces the
            // stock shaders for every im2d draw until cleared.
            if (im2dOverrideShader)
            {
                im2dCustomBind(rp, topology);
            }
            else if (s_renderState.currentRaster)
            {
                WGPURaster* ext = GETWGPURASTEREXT(s_renderState.currentRaster);
                if (ext && ext->bindGroup)
                {
                    WGPUBindGroup bg = ext->bindGroup;
                    // (P2-13) rwFILTERNEAREST: the raster's cached bind group uses
                    // the shared Linear sampler; swap in a Nearest one for this draw.
                    if (s_renderState.textureFilter == Texture::NEAREST &&
                        ext->view && s_im2dText.samplerNearest)
                    {
                        if (s_im2dNearestView != ext->view || !s_im2dNearestBg)
                        {
                            if (s_im2dNearestBg) wgpuBindGroupRelease(s_im2dNearestBg);
                            WGPUBindGroupEntry nEntries[3] = {};
                            nEntries[0].binding = 0;
                            nEntries[0].buffer = s_im2d.uniformBuf;
                            nEntries[0].offset = 0;
                            nEntries[0].size = 16;
                            nEntries[1].binding = 1;
                            nEntries[1].textureView = ext->view;
                            nEntries[2].binding = 2;
                            nEntries[2].sampler = s_im2dText.samplerNearest;
                            WGPUBindGroupDescriptor nDesc = {};
                            nDesc.label = WGPU_STR("im2d nearest bg");
                            nDesc.layout = s_im2dText.bindGroupLayout;
                            nDesc.entryCount = 3;
                            nDesc.entries = nEntries;
                            s_im2dNearestBg = wgpuDeviceCreateBindGroup(dev, &nDesc);
                            s_im2dNearestView = ext->view;
                        }
                        bg = s_im2dNearestBg;
                    }
                    wgpuRenderPassEncoderSetPipeline(rp, im2dPipelineForBlend(&s_im2dText.pc, topology));
                    wgpuRenderPassEncoderSetBindGroup(rp, 0, bg, 0, nullptr);
                }
                else
                {
                    wgpuRenderPassEncoderSetPipeline(rp, im2dPipelineForBlend(&s_im2d.pc, topology));
                    wgpuRenderPassEncoderSetBindGroup(rp, 0, s_im2d.bindGroup, 0, nullptr);
                }
            }
            else
            {
                wgpuRenderPassEncoderSetPipeline(rp, im2dPipelineForBlend(&s_im2d.pc, topology));
                wgpuRenderPassEncoderSetBindGroup(rp, 0, s_im2d.bindGroup, 0, nullptr);
            }

            wgpuRenderPassEncoderSetVertexBuffer(rp, 0, vb, vbOffset, vbBytes);
            wgpuRenderPassEncoderSetIndexBuffer(rp, ib, WGPUIndexFormat_Uint16, ibOffset, ibBoundBytes);
            wgpuRenderPassEncoderDrawIndexed(rp, (uint32_t)listCount, 1, 0, 0, 0);

            if (vbPrivate) wgpuBufferRelease(vb);
            if (ibPrivate) wgpuBufferRelease(ib);
        }

        void im2DRenderLine(void* verts, int32 numVerts, int32 v1, int32 v2)
        {
            Im2DVertex* vs = (Im2DVertex*)verts;
            s_im2dTmpBuf[0] = vs[v1];
            s_im2dTmpBuf[1] = vs[v2];
            im2DFlushVerts(PRIMTYPELINELIST, s_im2dTmpBuf, 2, nullptr, 0);
        }

        void im2DRenderTriangle(void* verts, int32 numVerts, int32 v1, int32 v2, int32 v3)
        {
            Im2DVertex* vs = (Im2DVertex*)verts;
            s_im2dTmpBuf[0] = vs[v1];
            s_im2dTmpBuf[1] = vs[v2];
            s_im2dTmpBuf[2] = vs[v3];
            im2DFlushVerts(PRIMTYPETRILIST, s_im2dTmpBuf, 3, nullptr, 0);
        }

        void im2DRenderPrimitive(PrimitiveType primType, void* verts, int32 numVerts)
        {
            im2DFlushVerts(primType, verts, numVerts, nullptr, 0);
        }

        void im2DRenderIndexedPrimitive(PrimitiveType primType, void* verts, int32 numVerts,
                                        void* indices, int32 numIndices)
        {
            im2DFlushVerts(primType, verts, numVerts, indices, numIndices);
        }

        // ========================================================================
        // Im3D — BindGroup helper
        // ========================================================================
        // Cache for textured im3d bind groups (shadows, skidmarks, water — many draws
        // per frame; creating one per draw was measurable). Safe to key by the view
        // pointer: the cached bind group holds a reference, so the view can't be
        // destroyed and its pointer reused while the entry lives. Freed in closeIm3D.
#define IM3D_BG_CACHE 64

        static struct
        {
            WGPUTextureView view;
            bool wrap;
            bool nearest;
            WGPUBindGroup bg;
        } s_im3dBGs[IM3D_BG_CACHE];

        static int s_im3dBGCount = 0;
        static uint32 s_im3dBGGen = 0xFFFFFFFFu;

        void im3DFreeBindGroupCache(void)
        {
            for (int i = 0; i < s_im3dBGCount; i++)
                wgpuBindGroupRelease(s_im3dBGs[i].bg);
            s_im3dBGCount = 0;
        }

        // Build a bind group for the given texture view. Binds the uniform ring at
        // binding 0 (dynamic offset supplied at draw time), or `sceneOverride` when
        // the ring was exhausted this frame (P0-2 fallback).
        static WGPUBindGroup im3DMakeBindGroup(WGPUTextureView texView, bool nearest,
                                               WGPUBuffer sceneOverride)
        {
            WGPUBindGroupEntry bgEntries[3] = {};
            bgEntries[0].binding = 0;
            bgEntries[0].buffer = sceneOverride ? sceneOverride : wgpuUniformRingBuffer();
            bgEntries[0].offset = 0;
            bgEntries[0].size = 160;

            bgEntries[1].binding = 1;
            bgEntries[1].textureView = texView;

            // Pick WRAP vs CLAMP sampler from the current texture-address state (RW
            // Addressing: WRAP=1, MIRROR=2, CLAMP=3). Tiled water needs WRAP; shadows and
            // most decals set CLAMP.
            bool wrap = (s_renderState.textureAddress == rw::Texture::WRAP ||
                s_renderState.textureAddress == rw::Texture::MIRROR);
            bgEntries[2].binding = 2;
            bgEntries[2].sampler = (nearest && s_im3d.nearestSampler)
                                       ? s_im3d.nearestSampler
                                       : ((wrap && s_im3d.wrapSampler)
                                              ? s_im3d.wrapSampler
                                              : s_im3d.defaultSampler);

            WGPUBindGroupDescriptor bgDesc = {};
            bgDesc.layout = s_im3d.bindGroupLayout;
            bgDesc.entryCount = 3;
            bgDesc.entries = bgEntries;
            return wgpuDeviceCreateBindGroup(wgpuGlobals.device, &bgDesc);
        }

        // ========================================================================
        // Im3D — Transform
        // ========================================================================
        void im3DTransform(void* verts, int32 numVerts, Matrix* world, uint32 flags)
        {
            if (!s_frame.inRenderPass || !s_frame.renderPass) return;
            if (numVerts <= 0 || !verts) return;

            WGPUDevice dev = wgpuGlobals.device;
            WGPUQueue q = wgpuGlobals.queue;

            // --- Upload vertices (geometry ring; private buffer only on overflow) ---
            uint64_t vbDataBytes = (uint64_t)numVerts * sizeof(Im3DVertex);
            if (s_im3d.vertexBuffer && s_im3d.vertexPrivate)
                wgpuBufferRelease(s_im3d.vertexBuffer);
            s_im3d.vertexBuffer = nullptr;
            s_im3d.vertexPrivate = false;
            uint32 vroff = wgpuGeomAlloc(verts, (uint32)vbDataBytes);
            if (vroff != 0xFFFFFFFFu)
            {
                s_im3d.vertexBuffer = wgpuGeomRingBuffer();
                s_im3d.vertexOffset = vroff;
            }
            else
            {
                WGPUBufferDescriptor vbDesc = {};
                vbDesc.label = WGPU_STR("im3d verts");
                vbDesc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_Vertex;
                vbDesc.size = WGPU_ALIGN4(vbDataBytes);
                s_im3d.vertexBuffer = wgpuDeviceCreateBuffer(dev, &vbDesc);
                wgpuQueueWriteBuffer(q, s_im3d.vertexBuffer, 0, verts, vbDataBytes);
                s_im3d.vertexOffset = 0;
                s_im3d.vertexPrivate = true;
            }
            s_im3d.numVertices = numVerts;

            // --- Build scene uniform (viewProj + world) ---
            Camera* cam = (Camera*)engine->currentCamera;
            if (!cam) return;

            float* V = (float*)&cam->devView; // view matrix
            float* P = (float*)&cam->devProj; // proj matrix

            // viewProj = View * Proj  (same order as GL3: vertex shader does VP * world * pos)
            float viewProj[16];
            mat4Mult(viewProj, V, P);

            // World matrix
            static Matrix identMatrix;
            static bool identInit = false;
            if (!identInit)
            {
                identMatrix.setIdentity();
                identInit = true;
            }
            Matrix* w = world ? world : &identMatrix;

            float worldArr[16];
            worldArr[0] = w->right.x;
            worldArr[1] = w->right.y;
            worldArr[2] = w->right.z;
            worldArr[3] = 0.0f;
            worldArr[4] = w->up.x;
            worldArr[5] = w->up.y;
            worldArr[6] = w->up.z;
            worldArr[7] = 0.0f;
            worldArr[8] = w->at.x;
            worldArr[9] = w->at.y;
            worldArr[10] = w->at.z;
            worldArr[11] = 0.0f;
            worldArr[12] = w->pos.x;
            worldArr[13] = w->pos.y;
            worldArr[14] = w->pos.z;
            worldArr[15] = 1.0f;

            // Upload: viewProj (64) + world (64) + fogColor (16) + fogData (16) = 160 bytes
            struct SceneData
            {
                float viewProj[16];
                float world[16];
                float fogColor[4];
                float fogData[4]; // x=start, y=end, z=range, w=disable
            } sceneData;
            memcpy(sceneData.viewProj, viewProj, 64);
            memcpy(sceneData.world, worldArr, 64);
            // Fog snapshot for this transform block — same model as the object
            // shader (fillObjectUniforms) and gl3's im3d (u_fogColor/u_fogData).
            // The game sets FOGENABLE around its im3d draws (far water etc.); with
            // fog off the factor clamps to 1 and the shader is a plain passthrough.
            sceneData.fogColor[0] = s_renderState.fogColor.red / 255.0f;
            sceneData.fogColor[1] = s_renderState.fogColor.green / 255.0f;
            sceneData.fogColor[2] = s_renderState.fogColor.blue / 255.0f;
            sceneData.fogColor[3] = s_renderState.fogColor.alpha / 255.0f;
            if (s_renderState.fogEnable)
            {
                float fogStart = cam->fogPlane;
                float fogEnd = cam->farPlane;
                sceneData.fogData[0] = fogStart;
                sceneData.fogData[1] = fogEnd;
                sceneData.fogData[2] = 1.0f / (fogStart - fogEnd);
                sceneData.fogData[3] = 0.0f; // enabled
            }
            else
            {
                sceneData.fogData[0] = 0.0f;
                sceneData.fogData[1] = 0.0f;
                sceneData.fogData[2] = 0.0f;
                sceneData.fogData[3] = 1.0f; // disabled -> factor clamps to 1 (no fog)
            }

            // (P0-2) Stage the scene uniforms in the shared ring and bind them at a
            // dynamic offset. The previous queue-level writeBuffer into ONE shared
            // sceneBuffer suffered last-write-wins across the whole submit: with
            // several RwIm3DTransform / RenderIndexedPrimitive / End batches per
            // submit (water + particles + skidmarks in one pass) every batch drew
            // with the LAST batch's matrices and fog. The ring is flushed once per
            // submit, so each transform's 160-byte block is stable.
            s_im3d.sceneOffset = wgpuUniformAlloc(&sceneData, sizeof(sceneData));
            if (s_im3d.sceneOffset != 0xFFFFFFFFu)
                s_im3d.sceneOnRing = true;
            else
            {
                // Ring exhausted this frame (it grows at the frame boundary):
                // fall back to the private 160-byte buffer + a throwaway bind
                // group in im3DFlush.
                wgpuQueueWriteBuffer(q, s_im3d.sceneBuffer, 0, &sceneData, sizeof(sceneData));
                s_im3d.sceneOnRing = false;
            }

            s_im3d.hasTransform = true;
        }

        // Build/cache an im3d pipeline for the current blend + depth render state.
        // GTA's water etc. draw via im3d with alpha blending and (importantly) depth
        // WRITE off — writing depth made overlapping transparent water tiles z-fight
        // into visible squares. Mirrors the object pipeline / im2d state handling.
        static WGPURenderPipeline im3dPipelineForState(WGPUPrimitiveTopology topology)
        {
            Im2DPipeCache* pc = &s_im3d.pc;
            WGPUBlendFactor sf = toWGPUBlendFactor(s_renderState.srcBlend);
            WGPUBlendFactor df = toWGPUBlendFactor(s_renderState.destBlend);
            bool ztest = s_renderState.zTest != 0;
            bool zwrite = s_renderState.zWrite != 0;
            bool isLine = (topology == WGPUPrimitiveTopology_LineList);
            bool vertexAlpha = s_renderState.vertexAlpha != 0;
            uint32 key = ((uint32)sf << 8) | (uint32)df | (ztest ? (1u << 20) : 0u) | (zwrite ? (1u << 21) : 0u) | (
                isLine ? (1u << 22) : 0u) | (vertexAlpha ? (1u << 23) : 0u);
            WGPURenderPipeline cached = imPipeCacheFind(pc, key);
            if (cached) return cached;

            WGPUDevice dev = wgpuGlobals.device;
            WGPUVertexAttribute attrs[4] = {};
            attrs[0].shaderLocation = 0;
            attrs[0].format = WGPUVertexFormat_Float32x3;
            attrs[0].offset = 0;
            attrs[1].shaderLocation = 1;
            attrs[1].format = WGPUVertexFormat_Float32x3;
            attrs[1].offset = offsetof(Im3DVertex, normal);
            attrs[2].shaderLocation = 2;
            attrs[2].format = WGPUVertexFormat_Unorm8x4;
            attrs[2].offset = offsetof(Im3DVertex, r);
            attrs[3].shaderLocation = 3;
            attrs[3].format = WGPUVertexFormat_Float32x2;
            attrs[3].offset = offsetof(Im3DVertex, u);
            WGPUVertexBufferLayout vbLayout = {};
            vbLayout.arrayStride = sizeof(Im3DVertex);
            vbLayout.stepMode = WGPUVertexStepMode_Vertex;
            vbLayout.attributeCount = 4;
            vbLayout.attributes = attrs;

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
            rpDesc.label = WGPU_STR("im3d pipeline (state)");
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
            rpDesc.multisample.count = 1;
            rpDesc.multisample.mask = 0xFFFFFFFF;
            rpDesc.fragment = &fragState;
            WGPURenderPipeline pipe = wgpuDeviceCreateRenderPipeline(dev, &rpDesc);
            imPipeCacheInsert(pc, key, pipe);
            return pipe;
        }

        // ========================================================================
        // Im3D — Render
        // ========================================================================
        static void im3DFlush(PrimitiveType primType, WGPUBuffer indexBuf, uint64_t ibOffset,
                              int32 numIndices, uint64_t ibBoundBytes)
        {
            WGPURenderPassEncoder rp = s_frame.renderPass;

            // (P0-2) Bind groups reference the uniform ring; drop the caches when
            // the ring was recreated (generation bump).
            uint32 ringGen = wgpuUniformRingGeneration();
            if (ringGen != s_im3dBGGen)
            {
                im3DFreeBindGroupCache();
                if (s_im3d.bindGroup)
                {
                    wgpuBindGroupRelease(s_im3d.bindGroup);
                    s_im3d.bindGroup = nullptr;
                }
                s_im3dBGGen = ringGen;
            }

            // Resolve the texture view + sampler state (P2-13: NEAREST filter).
            WGPUTextureView texView = s_im3d.defaultView;
            bool nearest = s_renderState.textureFilter == Texture::NEAREST &&
                s_im3d.nearestSampler != nullptr;
            if (s_renderState.currentRaster)
            {
                WGPURaster* ext = GETWGPURASTEREXT(s_renderState.currentRaster);
                if (ext && ext->view)
                    texView = ext->view;
            }

            bool ownedBg = false;
            WGPUBindGroup bg;
            if (s_im3d.sceneOnRing)
            {
                // Ring-backed bind group, cached per texture view + address mode +
                // filter (the scene data itself is addressed by the dynamic offset).
                bool wrap = (s_renderState.textureAddress == rw::Texture::WRAP ||
                    s_renderState.textureAddress == rw::Texture::MIRROR);
                bg = nullptr;
                for (int i = 0; i < s_im3dBGCount; i++)
                {
                    if (s_im3dBGs[i].view == texView && s_im3dBGs[i].wrap == wrap &&
                        s_im3dBGs[i].nearest == nearest)
                    {
                        bg = s_im3dBGs[i].bg;
                        break;
                    }
                }
                if (!bg)
                {
                    bg = im3DMakeBindGroup(texView, nearest, nullptr);
                    if (s_im3dBGCount < IM3D_BG_CACHE)
                    {
                        s_im3dBGs[s_im3dBGCount].view = texView;
                        s_im3dBGs[s_im3dBGCount].wrap = wrap;
                        s_im3dBGs[s_im3dBGCount].nearest = nearest;
                        s_im3dBGs[s_im3dBGCount].bg = bg;
                        s_im3dBGCount++;
                    }
                    else
                        ownedBg = true; // cache full — release after the draw
                }
            }
            else
            {
                // Ring exhausted this frame (P0-2 fallback): build a throwaway bind
                // group pointing at the private sceneBuffer at offset 0. Without this
                // bg stays uninitialized and wgpuSetBindGroup panics with
                // "BindGroup does not exist".
                bg = im3DMakeBindGroup(texView, nearest, s_im3d.sceneBuffer);
                ownedBg = true;
            }

            bool isLine = (primType == PRIMTYPELINELIST || primType == PRIMTYPEPOLYLINE);
            WGPUPrimitiveTopology topology = isLine
                                                 ? WGPUPrimitiveTopology_LineList
                                                 : WGPUPrimitiveTopology_TriangleList;

            wgpuRenderPassEncoderSetPipeline(rp, im3dPipelineForState(topology));
            if (s_im3d.sceneOnRing)
                wgpuRenderPassEncoderSetBindGroup(rp, 0, bg, 1, &s_im3d.sceneOffset);
            else
            {
                uint32 zero = 0;
                wgpuRenderPassEncoderSetBindGroup(rp, 0, bg, 1, &zero);
            }
            wgpuRenderPassEncoderSetVertexBuffer(rp, 0, s_im3d.vertexBuffer, s_im3d.vertexOffset,
                                                 (uint64_t)s_im3d.numVertices * sizeof(Im3DVertex));

            if (indexBuf && numIndices > 0)
            {
                wgpuRenderPassEncoderSetIndexBuffer(rp, indexBuf, WGPUIndexFormat_Uint16, ibOffset, ibBoundBytes);
                wgpuRenderPassEncoderDrawIndexed(rp, (uint32_t)numIndices, 1, 0, 0, 0);
            }
            else
            {
                wgpuRenderPassEncoderDraw(rp, (uint32_t)s_im3d.numVertices, 1, 0, 0);
            }

            if (ownedBg) wgpuBindGroupRelease(bg);
        }

        // Draw the current im3d vertex buffer as `primType`, converting tri fans/strips
        // to a triangle list (our im3d pipeline bakes TriangleList). `srcIdx` is the
        // caller's index array or null for the implicit 0..numVertices-1 sequence.
        // Used by both indexed and non-indexed im3d render entry points. GTA's water
        // (CWaterLevel) draws via im3d with tri strips/fans — as triangle lists it tore
        // into flat garbage before this.
        static void im3DDraw(PrimitiveType primType, const uint16* srcIdx, int32 srcCount)
        {
            if (!s_frame.inRenderPass || !s_frame.renderPass) return;
            if (!s_im3d.hasTransform || !s_im3d.vertexBuffer || s_im3d.numVertices <= 0) return;

            bool isLine = (primType == PRIMTYPELINELIST || primType == PRIMTYPEPOLYLINE);
            uint16* listIdx = imScratchIndices((srcCount > 2 ? srcCount : 3) * 3);
            int32 listCount = isLine
                                  ? imBuildLineIndices(primType, srcIdx, srcCount, listIdx)
                                  : imBuildTriListIndices(primType, srcIdx, srcCount, listIdx);
            if (listCount == 0) return;

            uint64_t ibDataBytes = (uint64_t)listCount * sizeof(uint16_t);
            uint64_t ibBoundBytes = WGPU_ALIGN4(ibDataBytes);
            WGPUBuffer ib;
            uint64_t ibOffset = 0;
            bool ibPrivate = false;
            uint32 iroff = wgpuGeomAlloc(listIdx, (uint32)ibDataBytes);
            if (iroff != 0xFFFFFFFFu)
            {
                ib = wgpuGeomRingBuffer();
                ibOffset = iroff;
            }
            else
            {
                listIdx[listCount] = 0; // pad the upload to a 4-byte multiple (scratch has slack)
                WGPUBufferDescriptor ibDesc = {};
                ibDesc.label = WGPU_STR("im3d indices");
                ibDesc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_Index;
                ibDesc.size = ibBoundBytes;
                ib = wgpuDeviceCreateBuffer(wgpuGlobals.device, &ibDesc);
                wgpuQueueWriteBuffer(wgpuGlobals.queue, ib, 0, listIdx, ibBoundBytes);
                ibPrivate = true;
            }

            im3DFlush(primType, ib, ibOffset, listCount, ibBoundBytes);
            if (ibPrivate) wgpuBufferRelease(ib);
        }

        void im3DRenderPrimitive(PrimitiveType primType)
        {
            im3DDraw(primType, nullptr, s_im3d.numVertices);
        }

        void im3DRenderIndexedPrimitive(PrimitiveType primType, void* indices, int32 numIndices)
        {
            if (!indices || numIndices <= 0) return;
            im3DDraw(primType, (const uint16*)indices, numIndices);
        }

        void im3DEnd(void)
        {
            if (s_im3d.vertexBuffer && s_im3d.vertexPrivate)
                wgpuBufferRelease(s_im3d.vertexBuffer);
            s_im3d.vertexBuffer = nullptr;
            s_im3d.vertexPrivate = false;
            s_im3d.numVertices = 0;
            s_im3d.hasTransform = false;
        }

        // ========================================================================
        // Инициализация Im2D (нетекстурированный)
        // ========================================================================
        void openIm2D(void)
        {
            WGPUDevice dev = wgpuGlobals.device;

            // Shader
            WGPUShaderSourceWGSL wgslDesc = {};
            wgslDesc.chain.sType = WGPUSType_ShaderSourceWGSL;
            wgslDesc.code = WGPU_STR(k_im2dWGSL);
            WGPUShaderModuleDescriptor shaderDesc = {};
            shaderDesc.nextInChain = &wgslDesc.chain;
            shaderDesc.label = WGPU_STR("im2d shader");
            WGPUShaderModule shaderMod = wgpuDeviceCreateShaderModule(dev, &shaderDesc);

            // Uniform buffer (16 bytes: xScl, yScl, xOff, yOff)
            WGPUBufferDescriptor ubDesc = {};
            ubDesc.label = WGPU_STR("im2d xform uniform");
            ubDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
            ubDesc.size = 16;
            s_im2d.uniformBuf = wgpuDeviceCreateBuffer(dev, &ubDesc);
            s_im2dXformW = 0.0f;
            s_im2dXformH = 0.0f; // fresh buffer — force a rewrite
            im2dUpdateXform((float)wgpuGlobals.winWidth, (float)wgpuGlobals.winHeight);

            // BindGroupLayout: binding 0 = uniform
            WGPUBindGroupLayoutEntry bglEntry = {};
            bglEntry.binding = 0;
            bglEntry.visibility = WGPUShaderStage_Vertex;
            bglEntry.buffer.type = WGPUBufferBindingType_Uniform;
            bglEntry.buffer.minBindingSize = 16;
            WGPUBindGroupLayoutDescriptor bglDesc = {};
            bglDesc.label = WGPU_STR("im2d bgl");
            bglDesc.entryCount = 1;
            bglDesc.entries = &bglEntry;
            s_im2d.bindGroupLayout = wgpuDeviceCreateBindGroupLayout(dev, &bglDesc);

            // BindGroup
            WGPUBindGroupEntry bgEntry = {};
            bgEntry.binding = 0;
            bgEntry.buffer = s_im2d.uniformBuf;
            bgEntry.offset = 0;
            bgEntry.size = 16;
            WGPUBindGroupDescriptor bgDesc = {};
            bgDesc.label = WGPU_STR("im2d bg");
            bgDesc.layout = s_im2d.bindGroupLayout;
            bgDesc.entryCount = 1;
            bgDesc.entries = &bgEntry;
            s_im2d.bindGroup = wgpuDeviceCreateBindGroup(dev, &bgDesc);

            // PipelineLayout
            WGPUPipelineLayoutDescriptor plDesc = {};
            plDesc.label = WGPU_STR("im2d pl");
            plDesc.bindGroupLayoutCount = 1;
            plDesc.bindGroupLayouts = &s_im2d.bindGroupLayout;
            WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(dev, &plDesc);

            // Vertex attributes: pos(Float32x4), color(Unorm8x4), uv(Float32x2)
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
            vbLayout.arrayStride = sizeof(Im2DVertex);
            vbLayout.stepMode = WGPUVertexStepMode_Vertex;
            vbLayout.attributeCount = 3;
            vbLayout.attributes = attrs;

            WGPUBlendState blend = makeAlphaBlend();
            WGPUColorTargetState colorTarget = {};
            colorTarget.format = wgpuGlobals.surfaceFormat;
            colorTarget.blend = &blend;
            colorTarget.writeMask = WGPUColorWriteMask_All;
            WGPUFragmentState fragState = {};
            fragState.module = shaderMod;
            fragState.entryPoint = WGPU_STR("fs_main");
            fragState.targetCount = 1;
            fragState.targets = &colorTarget;

            WGPUDepthStencilState ds = makeDepthState(false);

            WGPURenderPipelineDescriptor rpDesc = {};
            rpDesc.label = WGPU_STR("im2d pipeline");
            rpDesc.layout = pl;
            rpDesc.vertex.module = shaderMod;
            rpDesc.vertex.entryPoint = WGPU_STR("vs_main");
            rpDesc.vertex.bufferCount = 1;
            rpDesc.vertex.buffers = &vbLayout;
            rpDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
            rpDesc.primitive.stripIndexFormat = WGPUIndexFormat_Undefined;
            rpDesc.primitive.frontFace = WGPUFrontFace_CCW;
            rpDesc.primitive.cullMode = WGPUCullMode_None;
            rpDesc.depthStencil = &ds;
            rpDesc.multisample.count = 1;
            rpDesc.multisample.mask = 0xFFFFFFFF;
            rpDesc.fragment = &fragState;
            s_im2d.pipeline = wgpuDeviceCreateRenderPipeline(dev, &rpDesc);

            // Keep the shader + layout for lazily building blend-variant pipelines.
            // Drop stale entries WITHOUT releasing them: on a device restart the old
            // pipelines belong to a destroyed device and must not be touched (the
            // arrays themselves survive, capacity is reused).
            s_im2d.pc.shader = shaderMod;
            s_im2d.pc.playout = pl;
            s_im2d.pc.count = 0;
        }

        void closeIm2D(void)
        {
            imPipeCacheRelease(&s_im2d.pc);
            if (s_im2d.pipeline)
            {
                wgpuRenderPipelineRelease(s_im2d.pipeline);
                s_im2d.pipeline = nullptr;
            }
            if (s_im2d.bindGroup)
            {
                wgpuBindGroupRelease(s_im2d.bindGroup);
                s_im2d.bindGroup = nullptr;
            }
            if (s_im2d.bindGroupLayout)
            {
                wgpuBindGroupLayoutRelease(s_im2d.bindGroupLayout);
                s_im2d.bindGroupLayout = nullptr;
            }
            if (s_im2d.uniformBuf)
            {
                wgpuBufferRelease(s_im2d.uniformBuf);
                s_im2d.uniformBuf = nullptr;
            }
            free(s_imScratchIdx);
            s_imScratchIdx = nullptr;
            s_imScratchCap = 0;
        }

        // ========================================================================
        // Инициализация Im2D (текстурированный)
        // ========================================================================
        void openIm2DText(void)
        {
            WGPUDevice dev = wgpuGlobals.device;

            // Shader
            WGPUShaderSourceWGSL wgslDesc = {};
            wgslDesc.chain.sType = WGPUSType_ShaderSourceWGSL;
            wgslDesc.code = WGPU_STR(k_im2dTextWGSL);
            WGPUShaderModuleDescriptor shaderDesc = {};
            shaderDesc.nextInChain = &wgslDesc.chain;
            shaderDesc.label = WGPU_STR("im2d text shader");
            WGPUShaderModule shaderMod = wgpuDeviceCreateShaderModule(dev, &shaderDesc);

            // Sampler
            WGPUSamplerDescriptor samplerDesc = {};
            samplerDesc.label = WGPU_STR("im2d sampler");
            samplerDesc.addressModeU = WGPUAddressMode_ClampToEdge;
            samplerDesc.addressModeV = WGPUAddressMode_ClampToEdge;
            samplerDesc.addressModeW = WGPUAddressMode_ClampToEdge;
            // Linear filtering: the ImGui font atlas (and camera/im3d textures that
            // reuse this sampler) is authored for bilinear sampling — Nearest makes
            // text look harshly pixelated, especially when the frame is scaled.
            samplerDesc.magFilter = WGPUFilterMode_Linear;
            samplerDesc.minFilter = WGPUFilterMode_Linear;
            samplerDesc.mipmapFilter = WGPUMipmapFilterMode_Linear;
            samplerDesc.maxAnisotropy = 1;
            s_im2dText.sampler = wgpuDeviceCreateSampler(dev, &samplerDesc);

            // (P2-13) Nearest variant for rwFILTERNEAREST (pixel-art HUD/radar
            // elements asked for NEAREST but were always sampled linearly).
            WGPUSamplerDescriptor nearestDesc = {};
            nearestDesc.label = WGPU_STR("im2d nearest sampler");
            nearestDesc.addressModeU = WGPUAddressMode_ClampToEdge;
            nearestDesc.addressModeV = WGPUAddressMode_ClampToEdge;
            nearestDesc.addressModeW = WGPUAddressMode_ClampToEdge;
            nearestDesc.magFilter = WGPUFilterMode_Nearest;
            nearestDesc.minFilter = WGPUFilterMode_Nearest;
            nearestDesc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
            nearestDesc.maxAnisotropy = 1;
            s_im2dText.samplerNearest = wgpuDeviceCreateSampler(dev, &nearestDesc);

            // BGL: binding0=uniform, binding1=texture, binding2=sampler
            WGPUBindGroupLayoutEntry bglEntries[3] = {};
            bglEntries[0].binding = 0;
            bglEntries[0].visibility = WGPUShaderStage_Vertex;
            bglEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
            bglEntries[0].buffer.minBindingSize = 16;
            bglEntries[1].binding = 1;
            bglEntries[1].visibility = WGPUShaderStage_Fragment;
            bglEntries[1].texture.sampleType = WGPUTextureSampleType_Float;
            bglEntries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
            bglEntries[2].binding = 2;
            bglEntries[2].visibility = WGPUShaderStage_Fragment;
            bglEntries[2].sampler.type = WGPUSamplerBindingType_Filtering;
            WGPUBindGroupLayoutDescriptor bglDesc = {};
            bglDesc.label = WGPU_STR("im2d text bgl");
            bglDesc.entryCount = 3;
            bglDesc.entries = bglEntries;
            s_im2dText.bindGroupLayout = wgpuDeviceCreateBindGroupLayout(dev, &bglDesc);

            // Default 1x1 white texture (will be replaced when Charset uploads)
            WGPUTextureDescriptor texDesc = {};
            texDesc.label = WGPU_STR("im2d text default tex");
            texDesc.size = {1, 1, 1};
            texDesc.format = WGPUTextureFormat_RGBA8Unorm;
            texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
            texDesc.dimension = WGPUTextureDimension_2D;
            texDesc.mipLevelCount = 1;
            texDesc.sampleCount = 1;
            s_im2dText.texture = wgpuDeviceCreateTexture(dev, &texDesc);
            {
                uint8_t white[4] = {255, 255, 255, 255};
                WGPUTexelCopyTextureInfo dst = {};
                dst.texture = s_im2dText.texture;
                WGPUTexelCopyBufferLayout lay = {};
                lay.bytesPerRow = 4;
                lay.rowsPerImage = 1;
                WGPUExtent3D sz = {1, 1, 1};
                wgpuQueueWriteTexture(wgpuGlobals.queue, &dst, white, 4, &lay, &sz);
            }
            WGPUTextureViewDescriptor viewDesc = {};
            viewDesc.format = WGPUTextureFormat_RGBA8Unorm;
            viewDesc.dimension = WGPUTextureViewDimension_2D;
            viewDesc.aspect = WGPUTextureAspect_All;
            viewDesc.mipLevelCount = 1;
            viewDesc.arrayLayerCount = 1;
            s_im2dText.textureView = wgpuTextureCreateView(s_im2dText.texture, &viewDesc);

            // BindGroup
            WGPUBindGroupEntry bgEntries[3] = {};
            bgEntries[0].binding = 0;
            bgEntries[0].buffer = s_im2d.uniformBuf;
            bgEntries[0].offset = 0;
            bgEntries[0].size = 16;
            bgEntries[1].binding = 1;
            bgEntries[1].textureView = s_im2dText.textureView;
            bgEntries[2].binding = 2;
            bgEntries[2].sampler = s_im2dText.sampler;
            WGPUBindGroupDescriptor bgDesc = {};
            bgDesc.label = WGPU_STR("im2d text bg");
            bgDesc.layout = s_im2dText.bindGroupLayout;
            bgDesc.entryCount = 3;
            bgDesc.entries = bgEntries;
            s_im2dText.bindGroup = wgpuDeviceCreateBindGroup(dev, &bgDesc);

            // Pipeline
            WGPUPipelineLayoutDescriptor plDesc = {};
            plDesc.label = WGPU_STR("im2d text pl");
            plDesc.bindGroupLayoutCount = 1;
            plDesc.bindGroupLayouts = &s_im2dText.bindGroupLayout;
            WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(dev, &plDesc);

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
            vbLayout.arrayStride = sizeof(Im2DVertex);
            vbLayout.stepMode = WGPUVertexStepMode_Vertex;
            vbLayout.attributeCount = 3;
            vbLayout.attributes = attrs;

            WGPUBlendState blend = makeAlphaBlend();
            WGPUColorTargetState colorTarget = {};
            colorTarget.format = wgpuGlobals.surfaceFormat;
            colorTarget.blend = &blend;
            colorTarget.writeMask = WGPUColorWriteMask_All;
            WGPUFragmentState fragState = {};
            fragState.module = shaderMod;
            fragState.entryPoint = WGPU_STR("fs_main");
            fragState.targetCount = 1;
            fragState.targets = &colorTarget;

            WGPUDepthStencilState ds = makeDepthState(false);

            WGPURenderPipelineDescriptor rpDesc = {};
            rpDesc.label = WGPU_STR("im2d text pipeline");
            rpDesc.layout = pl;
            rpDesc.vertex.module = shaderMod;
            rpDesc.vertex.entryPoint = WGPU_STR("vs_main");
            rpDesc.vertex.bufferCount = 1;
            rpDesc.vertex.buffers = &vbLayout;
            rpDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
            rpDesc.primitive.stripIndexFormat = WGPUIndexFormat_Undefined;
            rpDesc.primitive.frontFace = WGPUFrontFace_CCW;
            rpDesc.primitive.cullMode = WGPUCullMode_None;
            rpDesc.depthStencil = &ds;
            rpDesc.multisample.count = 1;
            rpDesc.multisample.mask = 0xFFFFFFFF;
            rpDesc.fragment = &fragState;
            s_im2dText.pipeline = wgpuDeviceCreateRenderPipeline(dev, &rpDesc);

            s_im2dText.pc.shader = shaderMod;
            s_im2dText.pc.playout = pl;
            // Drop stale entries without releasing (device-restart safe, see openIm2D)
            s_im2dText.pc.count = 0;
        }

        void closeIm2DText(void)
        {
            imPipeCacheRelease(&s_im2dText.pc);
            if (s_im2dText.pipeline)
            {
                wgpuRenderPipelineRelease(s_im2dText.pipeline);
                s_im2dText.pipeline = nullptr;
            }
            if (s_im2dText.bindGroup)
            {
                wgpuBindGroupRelease(s_im2dText.bindGroup);
                s_im2dText.bindGroup = nullptr;
            }
            if (s_im2dText.bindGroupLayout)
            {
                wgpuBindGroupLayoutRelease(s_im2dText.bindGroupLayout);
                s_im2dText.bindGroupLayout = nullptr;
            }
            if (s_im2dText.textureView)
            {
                wgpuTextureViewRelease(s_im2dText.textureView);
                s_im2dText.textureView = nullptr;
            }
            if (s_im2dText.texture)
            {
                wgpuTextureRelease(s_im2dText.texture);
                s_im2dText.texture = nullptr;
            }
            if (s_im2dText.sampler)
            {
                wgpuSamplerRelease(s_im2dText.sampler);
                s_im2dText.sampler = nullptr;
            }
            if (s_im2dText.samplerNearest)
            {
                wgpuSamplerRelease(s_im2dText.samplerNearest);
                s_im2dText.samplerNearest = nullptr;
            }
            // (P2-13) drop the NEAREST bind-group cache (belongs to this device)
            if (s_im2dNearestBg)
            {
                wgpuBindGroupRelease(s_im2dNearestBg);
                s_im2dNearestBg = nullptr;
            }
            s_im2dNearestView = nullptr;
        }

        // ========================================================================
        // Инициализация Im3D
        // ========================================================================
        void openIm3D(void)
        {
            WGPUDevice dev = wgpuGlobals.device;

            // Shader
            WGPUShaderSourceWGSL wgslDesc = {};
            wgslDesc.chain.sType = WGPUSType_ShaderSourceWGSL;
            wgslDesc.code = WGPU_STR(k_im3dWGSL);
            WGPUShaderModuleDescriptor shaderDesc = {};
            shaderDesc.nextInChain = &wgslDesc.chain;
            shaderDesc.label = WGPU_STR("im3d shader");
            WGPUShaderModule shaderMod = wgpuDeviceCreateShaderModule(dev, &shaderDesc);

            // Scene uniform buffer: viewProj(64) + world(64) = 128 bytes
            WGPUBufferDescriptor ubDesc = {};
            ubDesc.label = WGPU_STR("im3d scene uniform");
            ubDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
            ubDesc.size = 160;
            s_im3d.sceneBuffer = wgpuDeviceCreateBuffer(dev, &ubDesc);

            // BGL: binding0=uniform, binding1=texture, binding2=sampler
            WGPUBindGroupLayoutEntry bglEntries[3] = {};
            bglEntries[0].binding = 0;
            bglEntries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
            bglEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
            bglEntries[0].buffer.minBindingSize = 160;
            bglEntries[0].buffer.hasDynamicOffset = true;
            bglEntries[1].binding = 1;
            bglEntries[1].visibility = WGPUShaderStage_Fragment;
            bglEntries[1].texture.sampleType = WGPUTextureSampleType_Float;
            bglEntries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
            bglEntries[2].binding = 2;
            bglEntries[2].visibility = WGPUShaderStage_Fragment;
            bglEntries[2].sampler.type = WGPUSamplerBindingType_Filtering;
            WGPUBindGroupLayoutDescriptor bglDesc = {};
            bglDesc.label = WGPU_STR("im3d bgl");
            bglDesc.entryCount = 3;
            bglDesc.entries = bglEntries;
            s_im3d.bindGroupLayout = wgpuDeviceCreateBindGroupLayout(dev, &bglDesc);

            // Default 1x1 white texture
            WGPUTextureDescriptor texDesc = {};
            texDesc.size = {1, 1, 1};
            texDesc.format = WGPUTextureFormat_RGBA8Unorm;
            texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
            texDesc.dimension = WGPUTextureDimension_2D;
            texDesc.mipLevelCount = 1;
            texDesc.sampleCount = 1;
            s_im3d.defaultTexture = wgpuDeviceCreateTexture(dev, &texDesc);
            {
                uint8_t white[4] = {255, 255, 255, 255};
                WGPUTexelCopyTextureInfo dst = {};
                dst.texture = s_im3d.defaultTexture;
                WGPUTexelCopyBufferLayout lay = {};
                lay.bytesPerRow = 4;
                lay.rowsPerImage = 1;
                WGPUExtent3D sz = {1, 1, 1};
                wgpuQueueWriteTexture(wgpuGlobals.queue, &dst, white, 4, &lay, &sz);
            }
            WGPUTextureViewDescriptor dvd = {};
            dvd.format = WGPUTextureFormat_RGBA8Unorm;
            dvd.dimension = WGPUTextureViewDimension_2D;
            dvd.aspect = WGPUTextureAspect_All;
            dvd.mipLevelCount = 1;
            dvd.arrayLayerCount = 1;
            s_im3d.defaultView = wgpuTextureCreateView(s_im3d.defaultTexture, &dvd);

            // Reuse the ClampToEdge sampler from Im2DText; add a Repeat/WRAP one for
            // tiled im3d geometry (GTA's far/LOD flat water tiles the water texture with
            // UVs 0..2 and sets rwTEXTUREADDRESSWRAP — clamping it makes each tile show a
            // stretched edge, i.e. the "water breaks into squares" artifact).
            s_im3d.defaultSampler = s_im2dText.sampler;
            {
                WGPUSamplerDescriptor sd = {};
                sd.label = WGPU_STR("im3d wrap sampler");
                sd.addressModeU = WGPUAddressMode_Repeat;
                sd.addressModeV = WGPUAddressMode_Repeat;
                sd.addressModeW = WGPUAddressMode_Repeat;
                sd.magFilter = WGPUFilterMode_Linear;
                sd.minFilter = WGPUFilterMode_Linear;
                sd.mipmapFilter = WGPUMipmapFilterMode_Linear;
                sd.lodMaxClamp = 32.0f;
                sd.maxAnisotropy = 1;
                s_im3d.wrapSampler = wgpuDeviceCreateSampler(dev, &sd);
            }
            // (P2-13) Nearest variant (Repeat address — tiled im3d geometry like
            // water stays tiled; decals that set NEAREST + CLAMP get the clamp
            // behaviour via their UVs being inside [0,1]).
            {
                WGPUSamplerDescriptor sd = {};
                sd.label = WGPU_STR("im3d nearest sampler");
                sd.addressModeU = WGPUAddressMode_Repeat;
                sd.addressModeV = WGPUAddressMode_Repeat;
                sd.addressModeW = WGPUAddressMode_Repeat;
                sd.magFilter = WGPUFilterMode_Nearest;
                sd.minFilter = WGPUFilterMode_Nearest;
                sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
                sd.lodMaxClamp = 32.0f;
                sd.maxAnisotropy = 1;
                s_im3d.nearestSampler = wgpuDeviceCreateSampler(dev, &sd);
            }


            // PipelineLayout
            WGPUPipelineLayoutDescriptor plDesc = {};
            plDesc.label = WGPU_STR("im3d pl");
            plDesc.bindGroupLayoutCount = 1;
            plDesc.bindGroupLayouts = &s_im3d.bindGroupLayout;
            WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(dev, &plDesc);

            // Vertex attributes: pos(Float32x3), normal(Float32x3), color(Unorm8x4), uv(Float32x2)
            WGPUVertexAttribute attrs[4] = {};
            attrs[0].shaderLocation = 0;
            attrs[0].format = WGPUVertexFormat_Float32x3;
            attrs[0].offset = 0;
            attrs[1].shaderLocation = 1;
            attrs[1].format = WGPUVertexFormat_Float32x3;
            attrs[1].offset = offsetof(Im3DVertex, normal);
            attrs[2].shaderLocation = 2;
            attrs[2].format = WGPUVertexFormat_Unorm8x4;
            attrs[2].offset = offsetof(Im3DVertex, r);
            attrs[3].shaderLocation = 3;
            attrs[3].format = WGPUVertexFormat_Float32x2;
            attrs[3].offset = offsetof(Im3DVertex, u);
            WGPUVertexBufferLayout vbLayout = {};
            vbLayout.arrayStride = sizeof(Im3DVertex);
            vbLayout.stepMode = WGPUVertexStepMode_Vertex;
            vbLayout.attributeCount = 4;
            vbLayout.attributes = attrs;

            WGPUBlendState blend = makeAlphaBlend();
            WGPUColorTargetState colorTarget = {};
            colorTarget.format = wgpuGlobals.surfaceFormat;
            colorTarget.blend = &blend;
            colorTarget.writeMask = WGPUColorWriteMask_All;
            WGPUFragmentState fragState = {};
            fragState.module = shaderMod;
            fragState.entryPoint = WGPU_STR("fs_main");
            fragState.targetCount = 1;
            fragState.targets = &colorTarget;

            WGPUDepthStencilState ds = makeDepthState(true); // Im3D writes depth

            WGPURenderPipelineDescriptor rpDesc = {};
            rpDesc.label = WGPU_STR("im3d pipeline");
            rpDesc.layout = pl;
            rpDesc.vertex.module = shaderMod;
            rpDesc.vertex.entryPoint = WGPU_STR("vs_main");
            rpDesc.vertex.bufferCount = 1;
            rpDesc.vertex.buffers = &vbLayout;
            rpDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
            rpDesc.primitive.stripIndexFormat = WGPUIndexFormat_Undefined;
            rpDesc.primitive.frontFace = WGPUFrontFace_CCW;
            rpDesc.primitive.cullMode = WGPUCullMode_None; // let app control via flags
            rpDesc.depthStencil = &ds;
            rpDesc.multisample.count = 1;
            rpDesc.multisample.mask = 0xFFFFFFFF;
            rpDesc.fragment = &fragState;
            s_im3d.pipeline = wgpuDeviceCreateRenderPipeline(dev, &rpDesc);

            // Keep shader + layout for building state-variant pipelines (blend/depth).
            // Drop stale entries without releasing (device-restart safe, see openIm2D)
            s_im3d.pc.shader = shaderMod;
            s_im3d.pc.playout = pl;
            s_im3d.pc.count = 0;
        }

        void closeIm3D(void)
        {
            im3DFreeBindGroupCache();
            imPipeCacheRelease(&s_im3d.pc);
            if (s_im3d.pipeline)
            {
                wgpuRenderPipelineRelease(s_im3d.pipeline);
                s_im3d.pipeline = nullptr;
            }
            if (s_im3d.bindGroup)
            {
                wgpuBindGroupRelease(s_im3d.bindGroup);
                s_im3d.bindGroup = nullptr;
            }
            if (s_im3d.bindGroupLayout)
            {
                wgpuBindGroupLayoutRelease(s_im3d.bindGroupLayout);
                s_im3d.bindGroupLayout = nullptr;
            }
            if (s_im3d.sceneBuffer)
            {
                wgpuBufferRelease(s_im3d.sceneBuffer);
                s_im3d.sceneBuffer = nullptr;
            }
            if (s_im3d.defaultView)
            {
                wgpuTextureViewRelease(s_im3d.defaultView);
                s_im3d.defaultView = nullptr;
            }
            if (s_im3d.defaultTexture)
            {
                wgpuTextureRelease(s_im3d.defaultTexture);
                s_im3d.defaultTexture = nullptr;
            }
            if (s_im3d.wrapSampler)
            {
                wgpuSamplerRelease(s_im3d.wrapSampler);
                s_im3d.wrapSampler = nullptr;
            }
            if (s_im3d.nearestSampler)
            {
                wgpuSamplerRelease(s_im3d.nearestSampler);
                s_im3d.nearestSampler = nullptr;
            }
            if (s_im3d.vertexBuffer)
            {
                wgpuBufferRelease(s_im3d.vertexBuffer);
                s_im3d.vertexBuffer = nullptr;
            }
        }
    } // namespace wgpu
} // namespace rw
#endif
