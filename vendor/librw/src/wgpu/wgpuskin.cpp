#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <string>
#include <unordered_map>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwengine.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwanim.h"
#include "../rwplugins.h"

#ifdef RW_WGPU

#include "wgpu_internal.h"

#define PLUGIN_ID 0

namespace rw
{
    namespace wgpu
    {
        // ========================================================================
        // Skin pipeline (HAnim / bone skinning)
        //
        // Mirrors the default object pipeline (wgpupipe.cpp) but adds per-vertex
        // bone weights + indices and a 64-entry bone-matrix uniform buffer; the
        // vertex shader transforms position/normal by the weighted bone matrices.
        // Reference: gl3skin.cpp.
        // ========================================================================

#define MAX_BONES 64

        // Skinned vertex layout (bytes):
        //   pos(12) + normal(12) + color(16) + uv(8) + weights(16) + indices(4) = 68
#define SKIN_STRIDE 68

        // Lazily-created GPU resources shared by all skinned draws.
        struct SkinState
        {
            WGPUBindGroupLayout bindGroupLayout; // 4 bindings (uniforms, tex, sampler, bones)
            WGPUTexture defaultTexture;
            WGPUTextureView defaultView;
            WGPUSampler defaultSampler;
            bool ready;
        };

        static SkinState s_skin = {};

        // Per-draw uniform block for the skin pipeline — same 736-byte layout as the
        // object pipeline. Hoisted to file scope so the bind-group cache can size the
        // binding by sizeof.
        struct SkinUniforms
        {
            float viewProj[16];
            float world[16];
            float lightDir[4]; // -> ambLight
            float ambient[4]; // -> matColor
            float matProps[4]; // -> surfProps
            float alphaTest[4];
            float fogColor[4];
            float fogData[4];
            float lightParams[WGPU_MAX_LIGHTS * 4];
            float lightPosition[WGPU_MAX_LIGHTS * 4];
            float lightDirection[WGPU_MAX_LIGHTS * 4];
            float lightColor[WGPU_MAX_LIGHTS * 4];
        };

        // Per-texture bind group cache. Like the object pipeline, but the skin layout
        // also has the bone matrices at binding 3 — those are sub-allocated from the
        // same uniform ring at a per-atomic dynamic offset, so a cached bind group still
        // depends only on the texture (both uniforms and bones are addressed by dynamic
        // offset at draw time). Rebuilt when the ring buffer is recreated.
        static std::unordered_map<WGPUTextureView, WGPUBindGroup> s_skinBindGroups;
        static uint32 s_skinBindGroupsGen = 0xFFFFFFFFu;

        static void
        invalidateSkinBindGroups(void)
        {
            for (auto& kv : s_skinBindGroups)
                wgpuBindGroupRelease(kv.second);
            s_skinBindGroups.clear();
        }

#define SKIN_BG_CACHE_MAX 512

        static WGPUBindGroup
        getSkinBindGroup(WGPUTextureView view, WGPUSampler sampler)
        {
            uint32 gen = wgpuUniformRingGeneration();
            if (gen != s_skinBindGroupsGen)
            {
                invalidateSkinBindGroups();
                s_skinBindGroupsGen = gen;
            }
            if (s_skinBindGroups.size() >= SKIN_BG_CACHE_MAX) // (P2-10)
                invalidateSkinBindGroups();
            auto it = s_skinBindGroups.find(view);
            if (it != s_skinBindGroups.end())
                return it->second;

            WGPUBuffer ring = wgpuUniformRingBuffer();
            WGPUBindGroupEntry e[4] = {};
            e[0].binding = 0;
            e[0].buffer = ring;
            e[0].size = sizeof(SkinUniforms);
            e[1].binding = 1;
            e[1].textureView = view;
            e[2].binding = 2;
            e[2].sampler = sampler;
            e[3].binding = 3;
            e[3].buffer = ring;
            e[3].size = MAX_BONES * 64; // 4096

            WGPUBindGroupDescriptor d = {};
            d.layout = s_skin.bindGroupLayout;
            d.entryCount = 4;
            d.entries = e;
            WGPUBindGroup bg = wgpuDeviceCreateBindGroup(wgpuGlobals.device, &d);
            s_skinBindGroups[view] = bg;
            return bg;
        }

        static WGPUCullMode
        toWGPUCullModeSkin(uint32 rwCull)
        {
            switch (rwCull)
            {
            case CULLBACK: return WGPUCullMode_Back;
            case CULLFRONT: return WGPUCullMode_Front;
            default: return WGPUCullMode_None; // CULLNONE
            }
        }

        // ========================================================================
        // GPU resource setup (lazy: created on first skinned render so we don't
        // depend on driver-plugin open ordering vs. device creation).
        // ========================================================================
        static void
        ensureSkinGPU(void)
        {
            if (s_skin.ready)
                return;
            WGPUDevice dev = wgpuGlobals.device;

            // BindGroupLayout: uniforms(0), texture(1), sampler(2), bones(3)
            WGPUBindGroupLayoutEntry bglEntries[4] = {};
            bglEntries[0].binding = 0;
            bglEntries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
            bglEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
            bglEntries[0].buffer.minBindingSize = 0;
            // Uniforms and bones (binding 3) are sub-allocated from the shared uniform
            // ring and bound at per-draw / per-atomic dynamic offsets.
            bglEntries[0].buffer.hasDynamicOffset = true;

            bglEntries[1].binding = 1;
            bglEntries[1].visibility = WGPUShaderStage_Fragment;
            bglEntries[1].texture.sampleType = WGPUTextureSampleType_Float;
            bglEntries[1].texture.viewDimension = WGPUTextureViewDimension_2D;

            bglEntries[2].binding = 2;
            bglEntries[2].visibility = WGPUShaderStage_Fragment;
            bglEntries[2].sampler.type = WGPUSamplerBindingType_Filtering;

            bglEntries[3].binding = 3;
            bglEntries[3].visibility = WGPUShaderStage_Vertex;
            bglEntries[3].buffer.type = WGPUBufferBindingType_Uniform;
            bglEntries[3].buffer.minBindingSize = MAX_BONES * 64; // 64 mat4
            bglEntries[3].buffer.hasDynamicOffset = true;

            WGPUBindGroupLayoutDescriptor bglDesc = {};
            bglDesc.label = WGPU_STR("skin bgl");
            bglDesc.entryCount = 4;
            bglDesc.entries = bglEntries;
            s_skin.bindGroupLayout = wgpuDeviceCreateBindGroupLayout(dev, &bglDesc);

            // Default 1x1 white texture (for untextured skinned meshes)
            WGPUTextureDescriptor texDesc = {};
            texDesc.label = WGPU_STR("skin default tex");
            texDesc.size = {1, 1, 1};
            texDesc.format = WGPUTextureFormat_RGBA8Unorm;
            texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
            texDesc.dimension = WGPUTextureDimension_2D;
            texDesc.mipLevelCount = 1;
            texDesc.sampleCount = 1;
            s_skin.defaultTexture = wgpuDeviceCreateTexture(dev, &texDesc);

            uint8_t white[4] = {255, 255, 255, 255};
            WGPUTexelCopyTextureInfo dst = {};
            dst.texture = s_skin.defaultTexture;
            WGPUTexelCopyBufferLayout lay = {};
            lay.bytesPerRow = 4;
            lay.rowsPerImage = 1;
            WGPUExtent3D sz = {1, 1, 1};
            wgpuQueueWriteTexture(wgpuGlobals.queue, &dst, white, 4, &lay, &sz);

            WGPUTextureViewDescriptor viewDesc = {};
            viewDesc.format = WGPUTextureFormat_RGBA8Unorm;
            viewDesc.dimension = WGPUTextureViewDimension_2D;
            viewDesc.aspect = WGPUTextureAspect_All;
            viewDesc.mipLevelCount = 1;
            viewDesc.arrayLayerCount = 1;
            s_skin.defaultView = wgpuTextureCreateView(s_skin.defaultTexture, &viewDesc);

            WGPUSamplerDescriptor samplerDesc = {};
            samplerDesc.label = WGPU_STR("skin sampler");
            samplerDesc.addressModeU = WGPUAddressMode_Repeat;
            samplerDesc.addressModeV = WGPUAddressMode_Repeat;
            samplerDesc.addressModeW = WGPUAddressMode_Repeat;
            samplerDesc.magFilter = WGPUFilterMode_Linear;
            samplerDesc.minFilter = WGPUFilterMode_Linear;
            samplerDesc.mipmapFilter = WGPUMipmapFilterMode_Linear;
            samplerDesc.maxAnisotropy = 16;
            samplerDesc.lodMaxClamp = 32.0f; // enable mip sampling (0 would pin to mip 0)
            s_skin.defaultSampler = wgpuDeviceCreateSampler(dev, &samplerDesc);

            s_skin.ready = true;
        }

        static WGPUShaderModule s_skinShaderModule = nullptr;

        // ========================================================================
        // Render pipeline (one baked pipeline per render-state combo, like the
        // default object pipeline).
        // ========================================================================
        static WGPURenderPipeline
        createSkinPipeline(bool blendEnable, WGPUBlendFactor srcFactor,
                           WGPUBlendFactor dstFactor, bool zWrite, WGPUCullMode cullMode,
                           bool zTest)
        {
            std::string shaderSrc = std::string(R"(
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

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var t_diffuse: texture_2d<f32>;
@group(0) @binding(2) var s_diffuse: sampler;
@group(0) @binding(3) var<uniform> boneMatrices: array<mat4x4<f32>, 64>;
)") + k_doDynamicLightWGSL + R"(
struct VertexInput {
    @location(0) position: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) color: vec4<f32>,
    @location(3) uv: vec2<f32>,
    @location(4) weights: vec4<f32>,
    @location(5) indices: vec4<u32>,
};

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) color: vec4<f32>,
    @location(1) uv: vec2<f32>,
    @location(2) fog: f32,
};

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;

    // Bone skinning: weighted sum of bone-transformed position/normal.
    var skinPos = vec3<f32>(0.0);
    var skinNorm = vec3<f32>(0.0);
    for(var i = 0; i < 4; i = i + 1) {
        let m = boneMatrices[input.indices[i]];
        let w = input.weights[i];
        skinPos  = skinPos  + (m * vec4<f32>(input.position, 1.0)).xyz * w;
        let m3 = mat3x3<f32>(m[0].xyz, m[1].xyz, m[2].xyz);
        skinNorm = skinNorm + (m3 * input.normal) * w;
    }

    let worldPos = uniforms.world * vec4<f32>(skinPos, 1.0);
    output.position = uniforms.viewProj * worldPos;
    let N = normalize((uniforms.world * vec4<f32>(skinNorm, 0.0)).xyz);

    var col = input.color;
    col = vec4<f32>(col.rgb
                    + uniforms.ambLight.rgb * uniforms.surfProps.x
                    + DoDynamicLight(worldPos.xyz, N) * uniforms.surfProps.z,
                    col.a);
    col = clamp(col, vec4<f32>(0.0), vec4<f32>(1.0));
    output.color = col * uniforms.matColor;

    output.uv = input.uv;
    output.fog = clamp((output.position.w - uniforms.fogData.y) * uniforms.fogData.z,
                       uniforms.fogData.w, 1.0);
    return output;
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    let texColor = textureSample(t_diffuse, s_diffuse, input.uv);
    var rgb = input.color.rgb * texColor.rgb;
    rgb = mix(uniforms.fogColor.rgb, rgb, input.fog);
    let alpha = input.color.a * texColor.a;

    if (uniforms.alphaTest.y > 0.5) {
        if (uniforms.alphaTest.z < 1.5) {
            if (alpha < uniforms.alphaTest.x) { discard; }
        } else {
            if (alpha >= uniforms.alphaTest.x) { discard; }
        }
    }

    return vec4<f32>(rgb, alpha);
}
)";
            if (!s_skinShaderModule)
            {
                WGPUShaderSourceWGSL wgslDesc = {};
                wgslDesc.chain.sType = WGPUSType_ShaderSourceWGSL;
                wgslDesc.code = WGPU_STR(shaderSrc.c_str());
                WGPUShaderModuleDescriptor shaderDesc = {};
                shaderDesc.nextInChain = &wgslDesc.chain;
                shaderDesc.label = WGPU_STR("skin shader");
                s_skinShaderModule = wgpuDeviceCreateShaderModule(wgpuGlobals.device, &shaderDesc);
            }
            WGPUShaderModule shaderModule = s_skinShaderModule;

            // Vertex layout (68-byte stride)
            WGPUVertexAttribute attrs[6] = {};
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
            attrs[4].shaderLocation = 4;
            attrs[4].format = WGPUVertexFormat_Float32x4;
            attrs[4].offset = 48;
            attrs[5].shaderLocation = 5;
            attrs[5].format = WGPUVertexFormat_Uint8x4;
            attrs[5].offset = 64;

            WGPUVertexBufferLayout vbLayout = {};
            vbLayout.arrayStride = SKIN_STRIDE;
            vbLayout.attributeCount = 6;
            vbLayout.attributes = attrs;

            WGPUPipelineLayoutDescriptor plDesc = {};
            plDesc.bindGroupLayoutCount = 1;
            plDesc.bindGroupLayouts = &s_skin.bindGroupLayout;
            WGPUPipelineLayout pipelineLayout = wgpuDeviceCreatePipelineLayout(wgpuGlobals.device, &plDesc);

            WGPUBlendState blend = {};
            blend.color.srcFactor = srcFactor;
            blend.color.dstFactor = dstFactor;
            blend.color.operation = WGPUBlendOperation_Add;
            blend.alpha.srcFactor = srcFactor;
            blend.alpha.dstFactor = dstFactor;
            blend.alpha.operation = WGPUBlendOperation_Add;

            WGPUColorTargetState colorTarget = {};
            colorTarget.format = wgpuGlobals.surfaceFormat;
            colorTarget.blend = blendEnable ? &blend : nullptr;
            colorTarget.writeMask = WGPUColorWriteMask_All;

            WGPUFragmentState fragState = {};
            fragState.module = shaderModule;
            fragState.entryPoint = WGPU_STR("fs_main");
            fragState.targetCount = 1;
            fragState.targets = &colorTarget;

            // Depth WRITE from ZWRITEENABLE; depth COMPARE from ZTESTENABLE (independent)
            // so ZTEST on + ZWRITE off (transparent ped parts) still occludes.
            WGPUDepthStencilState depthStencil = makeDepthState(zWrite);
            depthStencil.depthCompare = zTest
                                            ? WGPUCompareFunction_LessEqual
                                            : WGPUCompareFunction_Always;

            WGPURenderPipelineDescriptor rpDesc = {};
            rpDesc.layout = pipelineLayout;
            rpDesc.vertex.module = shaderModule;
            rpDesc.vertex.entryPoint = WGPU_STR("vs_main");
            rpDesc.vertex.bufferCount = 1;
            rpDesc.vertex.buffers = &vbLayout;
            rpDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
            rpDesc.primitive.stripIndexFormat = WGPUIndexFormat_Undefined;
            rpDesc.primitive.frontFace = WGPUFrontFace_CCW;
            rpDesc.primitive.cullMode = cullMode;
            rpDesc.depthStencil = &depthStencil;
            rpDesc.multisample.count = 1;
            rpDesc.multisample.mask = 0xFFFFFFFF;
            rpDesc.fragment = &fragState;

            WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(wgpuGlobals.device, &rpDesc);

            wgpuPipelineLayoutRelease(pipelineLayout);
            return pipeline;
        }

        // ------------------------------------------------------------------------
        // Pipeline cache (keyed by render state; pipelines are immutable in WGPU)
        // ------------------------------------------------------------------------

        static Im2DPipeCache s_skinPipeCache;

        static uint32
        skinCacheKey(bool blendEnable, uint32 srcBlend, uint32 destBlend,
                     bool zWrite, uint32 cullMode, bool zTest)
        {
            return (uint32)blendEnable
                | ((uint32)zWrite << 1)
                | ((srcBlend & 0xF) << 2)
                | ((destBlend & 0xF) << 6)
                | ((cullMode & 0x3) << 10)
                | ((uint32)zTest << 12);
        }

        static WGPURenderPipeline
        getOrCreateSkinPipeline(bool blendEnable, uint32 srcBlend, uint32 destBlend,
                                bool zWrite, uint32 cullMode, bool zTest)
        {
            uint32 key = skinCacheKey(blendEnable, srcBlend, destBlend, zWrite, cullMode, zTest);
            WGPURenderPipeline cached = imPipeCacheFind(&s_skinPipeCache, key);
            if (cached) return cached;

            WGPURenderPipeline pipe = createSkinPipeline(
                blendEnable, toWGPUBlendFactor(srcBlend), toWGPUBlendFactor(destBlend),
                zWrite, toWGPUCullModeSkin(cullMode), zTest);
            imPipeCacheInsert(&s_skinPipeCache, key, pipe);
            return pipe;
        }

        static void
        flushSkinCache(void)
        {
            imPipeCacheRelease(&s_skinPipeCache);
            // (P3-21) release the shared shader module while the device is alive
            if (s_skinShaderModule)
            {
                wgpuShaderModuleRelease(s_skinShaderModule);
                s_skinShaderModule = nullptr;
            }
        }

        // ========================================================================
        // Instancing — build the skinned vertex buffer (interleaved, 68B stride)
        // ========================================================================
        static WGPUInstanceDataHeader*
        instanceSkinMesh(Geometry* geo)
        {
            WGPUInstanceDataHeader* header = rwNewT(WGPUInstanceDataHeader, 1, MEMDUR_EVENT | ID_GEOMETRY);
            MeshHeader* meshh = geo->meshHeader;
            geo->instData = (InstanceDataHeader*)header;
            header->platform = PLATFORM_WGPU;
            header->numMeshes = meshh->numMeshes;
            header->totalNumVertex = geo->numVertices;
            header->totalNumIndex = meshh->totalIndices;
            header->inst = rwNewT(WGPUInstanceData, header->numMeshes, MEMDUR_EVENT | ID_GEOMETRY);

            Skin* skin = Skin::get(geo);

            uint32 nverts = geo->numVertices;
            uint8* vertexData = rwNewT(uint8, nverts * SKIN_STRIDE, MEMDUR_EVENT | ID_GEOMETRY);
            // 68 bytes = 17 floats per vertex; indices share the last float slot.
            float* verts = (float*)vertexData;
            const int F = SKIN_STRIDE / 4; // 17 float-sized slots per vertex

            // Position
            for (uint32 i = 0; i < nverts; i++)
            {
                verts[i * F + 0] = geo->morphTargets[0].vertices[i].x;
                verts[i * F + 1] = geo->morphTargets[0].vertices[i].y;
                verts[i * F + 2] = geo->morphTargets[0].vertices[i].z;
            }
            // Normal
            if (geo->flags & Geometry::NORMALS)
            {
                for (uint32 i = 0; i < nverts; i++)
                {
                    verts[i * F + 3] = geo->morphTargets[0].normals[i].x;
                    verts[i * F + 4] = geo->morphTargets[0].normals[i].y;
                    verts[i * F + 5] = geo->morphTargets[0].normals[i].z;
                }
            }
            else
            {
                for (uint32 i = 0; i < nverts; i++)
                {
                    verts[i * F + 3] = 0.0f;
                    verts[i * F + 4] = 0.0f;
                    verts[i * F + 5] = 1.0f;
                }
            }
            // Color (prelight); non-prelit defaults to (0,0,0,1) — see wgpupipe.cpp note.
            if (geo->flags & Geometry::PRELIT)
            {
                for (uint32 i = 0; i < nverts; i++)
                {
                    verts[i * F + 6] = geo->colors[i].red / 255.0f;
                    verts[i * F + 7] = geo->colors[i].green / 255.0f;
                    verts[i * F + 8] = geo->colors[i].blue / 255.0f;
                    verts[i * F + 9] = geo->colors[i].alpha / 255.0f;
                }
            }
            else
            {
                for (uint32 i = 0; i < nverts; i++)
                {
                    verts[i * F + 6] = 0.0f;
                    verts[i * F + 7] = 0.0f;
                    verts[i * F + 8] = 0.0f;
                    verts[i * F + 9] = 1.0f;
                }
            }
            // Texcoords
            if (geo->numTexCoordSets > 0)
            {
                for (uint32 i = 0; i < nverts; i++)
                {
                    verts[i * F + 10] = geo->texCoords[0][i].u;
                    verts[i * F + 11] = geo->texCoords[0][i].v;
                }
            }
            else
            {
                for (uint32 i = 0; i < nverts; i++)
                {
                    verts[i * F + 10] = 0.0f;
                    verts[i * F + 11] = 0.0f;
                }
            }
            // Bone weights (float4) at slot 12..15
            for (uint32 i = 0; i < nverts; i++)
            {
                verts[i * F + 12] = skin->weights[i * 4 + 0];
                verts[i * F + 13] = skin->weights[i * 4 + 1];
                verts[i * F + 14] = skin->weights[i * 4 + 2];
                verts[i * F + 15] = skin->weights[i * 4 + 3];
            }
            // Bone indices (4 x uint8) packed into the last 4 bytes (slot 16)
            for (uint32 i = 0; i < nverts; i++)
            {
                uint8* idx = (uint8*)&verts[i * F + 16];
                idx[0] = skin->indices[i * 4 + 0];
                idx[1] = skin->indices[i * 4 + 1];
                idx[2] = skin->indices[i * 4 + 2];
                idx[3] = skin->indices[i * 4 + 3];
            }

            WGPUBufferDescriptor vbDesc = {};
            vbDesc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
            vbDesc.size = nverts * SKIN_STRIDE;
            vbDesc.mappedAtCreation = false;

            WGPUInstanceData* inst = header->inst;
            Mesh* mesh = meshh->getMeshes();

            // Geometry has per-vertex alpha (prelit colours < 255)? Needs blend
            // (mirrors the default pipeline / gl3 inst->vertexAlpha).
            bool geoVertexAlpha = false;
            if (geo->flags & Geometry::PRELIT)
            {
                for (uint32 v = 0; v < nverts; v++)
                    if (geo->colors[v].alpha != 255)
                    {
                        geoVertexAlpha = true;
                        break;
                    }
            }

            header->sharedVbo = wgpuDeviceCreateBuffer(wgpuGlobals.device, &vbDesc);
            wgpuQueueWriteBuffer(wgpuGlobals.queue, header->sharedVbo, 0, vertexData, vbDesc.size);

            for (uint32 i = 0; i < header->numMeshes; i++)
            {
                inst[i].vertexBuffer = header->sharedVbo;

                // Un-strip TRISTRIP meshes to a triangle list (list-topology pipeline).
                uint32 listCount = 0;
                uint16* listIdx = buildTriListIndices(mesh[i].indices, mesh[i].numIndices,
                                                      (meshh->flags & MeshHeader::TRISTRIP) != 0, &listCount);

                uint64_t ibDataBytes = listCount * sizeof(uint16);
                uint64_t ibAlignedBytes = WGPU_ALIGN4(ibDataBytes);
                if (ibAlignedBytes == 0) ibAlignedBytes = 4;
                WGPUBufferDescriptor ibDesc = {};
                ibDesc.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
                ibDesc.size = ibAlignedBytes;
                ibDesc.mappedAtCreation = false;
                inst[i].indexBuffer = wgpuDeviceCreateBuffer(wgpuGlobals.device, &ibDesc);

                uint8_t* tmpIdx = new uint8_t[ibAlignedBytes];
                memcpy(tmpIdx, listIdx, ibDataBytes);
                if (ibAlignedBytes > ibDataBytes)
                    memset(tmpIdx + ibDataBytes, 0, ibAlignedBytes - ibDataBytes);
                wgpuQueueWriteBuffer(wgpuGlobals.queue, inst[i].indexBuffer, 0, tmpIdx, ibAlignedBytes);
                delete[] tmpIdx;
                if (listIdx != mesh[i].indices) delete[] listIdx;

                inst[i].numIndices = listCount;
                inst[i].material = mesh[i].material;
                inst[i].vertexAlpha = geoVertexAlpha;
            }

            rwFree(vertexData);
            return header;
        }

        static void
        skinInstance(ObjPipeline* pipe, Atomic* atomic)
        {
            (void)pipe;
            Geometry* geo = atomic->geometry;
            if (geo->flags & Geometry::NATIVE)
                return;
            WGPUInstanceDataHeader* header = (WGPUInstanceDataHeader*)geo->instData;
            if (geo->instData && header->platform != PLATFORM_WGPU)
            {
                freeInstanceData(geo);
                geo->instData = nullptr;
            }
            if (geo->instData == nullptr)
                geo->instData = (InstanceDataHeader*)instanceSkinMesh(geo);
            geo->lockedSinceInst = 0;
        }

        // ========================================================================
        // Bone matrices (mirror gl3 uploadSkinMatrices)
        // ========================================================================
        static float s_boneMatrices[MAX_BONES * 16];

        static void
        computeSkinMatrices(Atomic* a, float* dst)
        {
            Skin* skin = Skin::get(a->geometry);
            Matrix* m = (Matrix*)dst;
            HAnimHierarchy* hier = Skin::getHierarchy(a);

            if (hier)
            {
                Matrix* invMats = (Matrix*)skin->inverseMatrices;
                int n = hier->numNodes < MAX_BONES ? hier->numNodes : MAX_BONES;
                if (hier->flags & HAnimHierarchy::LOCALSPACEMATRICES)
                {
                    for (int i = 0; i < n; i++)
                    {
                        invMats[i].flags = 0;
                        Matrix::mult(m, &invMats[i], &hier->matrices[i]);
                        m++;
                    }
                }
                else
                {
                    Matrix invAtmMat, tmp;
                    Matrix::invert(&invAtmMat, a->getFrame()->getLTM());
                    for (int i = 0; i < n; i++)
                    {
                        invMats[i].flags = 0;
                        Matrix::mult(&tmp, &hier->matrices[i], &invAtmMat);
                        Matrix::mult(m, &invMats[i], &tmp);
                        m++;
                    }
                }
            }
            else
            {
                int n = skin->numBones < MAX_BONES ? skin->numBones : MAX_BONES;
                for (int i = 0; i < n; i++)
                {
                    m->setIdentity();
                    m++;
                }
            }
        }

        // ========================================================================
        // Render callback
        // ========================================================================
        static void
        skinRenderCB(Atomic* atomic, WGPUInstanceDataHeader* header)
        {
            Geometry* geo = atomic->geometry;
            Frame* frame = atomic->getFrame();
            Matrix worldMat = *frame->getLTM();

            SkinUniforms uniforms;

            Camera* cam = (Camera*)engine->currentCamera;
            if (cam)
            {
                float* V = (float*)&cam->devView;
                float* P = (float*)&cam->devProj;
                float viewProj[16];
                mat4Mult(viewProj, V, P);
                memcpy(uniforms.viewProj, viewProj, 64);
            }
            else
            {
                memset(uniforms.viewProj, 0, 64);
                uniforms.viewProj[0] = uniforms.viewProj[5] = uniforms.viewProj[10] = uniforms.viewProj[15] = 1.0f;
            }

            float world[16];
            world[0] = worldMat.right.x;
            world[1] = worldMat.right.y;
            world[2] = worldMat.right.z;
            world[3] = 0.0f;
            world[4] = worldMat.up.x;
            world[5] = worldMat.up.y;
            world[6] = worldMat.up.z;
            world[7] = 0.0f;
            world[8] = worldMat.at.x;
            world[9] = worldMat.at.y;
            world[10] = worldMat.at.z;
            world[11] = 0.0f;
            world[12] = worldMat.pos.x;
            world[13] = worldMat.pos.y;
            world[14] = worldMat.pos.z;
            world[15] = 1.0f;
            memcpy(uniforms.world, world, 64);

            // ---- Shared (per-atomic) lighting ----
            LightDataGPU lights;
            gatherLights(atomic, &lights);
            memcpy(uniforms.lightDir, lights.ambient, sizeof(lights.ambient));
            memcpy(uniforms.lightParams, lights.params, sizeof(lights.params));
            memcpy(uniforms.lightPosition, lights.position, sizeof(lights.position));
            memcpy(uniforms.lightDirection, lights.direction, sizeof(lights.direction));
            memcpy(uniforms.lightColor, lights.color, sizeof(lights.color));

            uniforms.alphaTest[3] = 0.0f; // per-pass alphaTest[0..2] set in the draw loop below

            uniforms.fogColor[0] = s_renderState.fogColor.red / 255.0f;
            uniforms.fogColor[1] = s_renderState.fogColor.green / 255.0f;
            uniforms.fogColor[2] = s_renderState.fogColor.blue / 255.0f;
            uniforms.fogColor[3] = s_renderState.fogColor.alpha / 255.0f;
            if (cam && s_renderState.fogEnable)
            {
                float fogStart = cam->fogPlane, fogEnd = cam->farPlane;
                uniforms.fogData[0] = fogStart;
                uniforms.fogData[1] = fogEnd;
                uniforms.fogData[2] = 1.0f / (fogStart - fogEnd);
                uniforms.fogData[3] = 0.0f;
            }
            else
            {
                uniforms.fogData[0] = uniforms.fogData[1] = uniforms.fogData[2] = 0.0f;
                uniforms.fogData[3] = 1.0f;
            }

            // Bone matrices (shared by all meshes of this atomic). Sub-allocate once
            // from the uniform ring at a per-atomic dynamic offset; fall back to a
            // private buffer only if the ring is full this frame.
            memset(s_boneMatrices, 0, sizeof(s_boneMatrices));
            computeSkinMatrices(atomic, s_boneMatrices);
            WGPUBuffer boneFallback = nullptr;
            WGPUBuffer boneRingBuffer = nullptr; // ring buffer if allocation succeeded
            uint32 boneDynOffset = wgpuUniformAlloc(s_boneMatrices, sizeof(s_boneMatrices));
            if (boneDynOffset != 0xFFFFFFFFu)
            {
                boneRingBuffer = wgpuUniformRingBuffer();
            }
            else
            {
                WGPUBufferDescriptor boneDesc = {};
                boneDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
                boneDesc.size = sizeof(s_boneMatrices); // 4096
                boneFallback = wgpuDeviceCreateBuffer(wgpuGlobals.device, &boneDesc);
                wgpuQueueWriteBuffer(wgpuGlobals.queue, boneFallback, 0, s_boneMatrices, sizeof(s_boneMatrices));
                boneDynOffset = 0;
            }

            // ---- Per-mesh: own material (colour + texture) → own uniform + bind group ----
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

                WGPUTextureView textureView = s_skin.defaultView;
                WGPUSampler sampler = s_skin.defaultSampler;
                if ((geo->flags & Geometry::TEXTURED) && mat && mat->texture && mat->texture->raster)
                {
                    WGPURaster* ext = GETWGPURASTEREXT(mat->texture->raster);
                    if (ext && ext->view)
                        textureView = ext->view;
                }

                // Same blend rule + PS2 GS alpha-test emulation as the default pipeline
                // (see wgpupipe.cpp defaultRenderCB) — peds have alpha-textured hair,
                // shadows and translucent parts that need this. meshBlend = vertex/mat
                // alpha or an alpha texture; blended + zwrite → two passes (opaque core
                // keeps depth, soft remainder blends without depth write).
                bool texHasAlpha = false;
                if (mat && mat->texture && mat->texture->raster)
                {
                    uint32 cf = mat->texture->raster->format & 0xF00;
                    texHasAlpha = (cf == Raster::C8888 || cf == Raster::C1555 || cf == Raster::C4444);
                }
                bool meshBlend = inst->vertexAlpha || (mat && mat->color.alpha != 255) || texHasAlpha;

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
                bool gsEmu = s_renderState.gsAlphaTest && meshBlend;
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

                    WGPURenderPipeline pipeline = getOrCreateSkinPipeline(
                        passes[p].blend, s_renderState.srcBlend, s_renderState.destBlend,
                        passes[p].zWrite, s_renderState.cullMode, s_renderState.zTest != 0);
                    wgpuRenderPassEncoderSetPipeline(s_frame.renderPass, pipeline);

                    // Sub-allocate this draw's uniforms from the ring. When both uniforms
                    // and bones come from the ring, use the cached per-texture bind group
                    // (bound with two dynamic offsets: uniforms then bones). On overflow,
                    // build a throwaway bind group with private buffer(s) for this draw.
                    WGPUBuffer uniFallback = nullptr;
                    WGPUBindGroup throwaway = nullptr;
                    uint32 uniDynOffset = wgpuUniformAlloc(&uniforms, sizeof(SkinUniforms));
                    WGPUBuffer uniBufForBind = wgpuUniformRingBuffer();
                    if (uniDynOffset == 0xFFFFFFFFu)
                    {
                        WGPUBufferDescriptor ubDesc = {};
                        ubDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
                        ubDesc.size = sizeof(SkinUniforms);
                        uniFallback = wgpuDeviceCreateBuffer(wgpuGlobals.device, &ubDesc);
                        wgpuQueueWriteBuffer(wgpuGlobals.queue, uniFallback, 0, &uniforms, sizeof(SkinUniforms));
                        uniBufForBind = uniFallback;
                        uniDynOffset = 0;
                    }

                    WGPUBindGroup bindGroup;
                    bool cached = (uniFallback == nullptr && boneFallback == nullptr);
                    if (cached)
                    {
                        bindGroup = getSkinBindGroup(textureView, sampler);
                    }
                    else
                    {
                        WGPUBindGroupEntry bgEntries[4] = {};
                        bgEntries[0].binding = 0;
                        bgEntries[0].buffer = uniBufForBind;
                        bgEntries[0].size = sizeof(SkinUniforms);
                        bgEntries[1].binding = 1;
                        bgEntries[1].textureView = textureView;
                        bgEntries[2].binding = 2;
                        bgEntries[2].sampler = sampler;
                        bgEntries[3].binding = 3;
                        bgEntries[3].buffer = boneFallback ? boneFallback : boneRingBuffer;
                        bgEntries[3].size = sizeof(s_boneMatrices);

                        WGPUBindGroupDescriptor bgDesc = {};
                        bgDesc.layout = s_skin.bindGroupLayout;
                        bgDesc.entryCount = 4;
                        bgDesc.entries = bgEntries;
                        throwaway = wgpuDeviceCreateBindGroup(wgpuGlobals.device, &bgDesc);
                        bindGroup = throwaway;
                    }

                    uint32 dynOffsets[2] = {uniDynOffset, boneDynOffset}; // binding 0, then 3
                    wgpuRenderPassEncoderSetBindGroup(s_frame.renderPass, 0, bindGroup, 2, dynOffsets);
                    wgpuRenderPassEncoderSetVertexBuffer(s_frame.renderPass, 0, inst->vertexBuffer, 0, WGPU_WHOLE_SIZE);
                    wgpuRenderPassEncoderSetIndexBuffer(s_frame.renderPass, inst->indexBuffer, WGPUIndexFormat_Uint16,
                                                        0, WGPU_WHOLE_SIZE);
                    wgpuRenderPassEncoderDrawIndexed(s_frame.renderPass, inst->numIndices, 1, 0, 0, 0);

                    if (throwaway) wgpuBindGroupRelease(throwaway);
                    if (uniFallback) wgpuBufferRelease(uniFallback);
                }
            }

            if (boneFallback) wgpuBufferRelease(boneFallback);
        }

        // Exported for custom (game) skin shaders — see wgpu_internal.h.
        void
        skinInstanceAtomic(Atomic* atomic)
        {
            skinInstance(nil, atomic);
        }

        void
        skinComputeBoneMatrices(Atomic* atomic, float* dst)
        {
            computeSkinMatrices(atomic, dst);
        }

        static void
        skinRender(ObjPipeline* pipe, Atomic* atomic)
        {
            (void)pipe;
            ensureSkinGPU();
            Geometry* geo = atomic->geometry;
            skinInstance(pipe, atomic);
            if (geo->instData == nullptr || geo->instData->platform != PLATFORM_WGPU)
            {
                static bool warned = false;
                if (!warned)
                {
                    fprintf(stderr, "[wgpu] skinned atomic with NATIVE/foreign geometry skipped\n");
                    warned = true;
                }
                return;
            }
            skinRenderCB(atomic, (WGPUInstanceDataHeader*)geo->instData);
        }

        // ========================================================================
        // Pipeline / plugin registration
        // ========================================================================
        static ObjPipeline*
        makeSkinPipeline(void)
        {
            ObjPipeline* pipe = ObjPipeline::create();
            pipe->impl.instance = skinInstance;
            pipe->impl.uninstance = nullptr;
            pipe->impl.render = skinRender;
            pipe->pluginID = ID_SKIN;
            pipe->pluginData = 1;
            return pipe;
        }

        static void*
        skinOpen(void* o, int32, int32)
        {
            skinGlobals.pipelines[PLATFORM_WGPU] = makeSkinPipeline();
            return o;
        }

        static void*
        skinClose(void* o, int32, int32)
        {
            closeSkin();
            if (skinGlobals.pipelines[PLATFORM_WGPU])
            {
                ((ObjPipeline*)skinGlobals.pipelines[PLATFORM_WGPU])->destroy();
                skinGlobals.pipelines[PLATFORM_WGPU] = nil;
            }
            return o;
        }

        void
        closeSkin(void)
        {
            flushSkinCache();
            invalidateSkinBindGroups();
            s_skinBindGroupsGen = 0xFFFFFFFFu;
            if (!s_skin.ready)
                return;
            if (s_skin.defaultSampler)
            {
                wgpuSamplerRelease(s_skin.defaultSampler);
                s_skin.defaultSampler = nullptr;
            }
            if (s_skin.defaultView)
            {
                wgpuTextureViewRelease(s_skin.defaultView);
                s_skin.defaultView = nullptr;
            }
            if (s_skin.defaultTexture)
            {
                wgpuTextureRelease(s_skin.defaultTexture);
                s_skin.defaultTexture = nullptr;
            }
            if (s_skin.bindGroupLayout)
            {
                wgpuBindGroupLayoutRelease(s_skin.bindGroupLayout);
                s_skin.bindGroupLayout = nullptr;
            }
            s_skin.ready = false;
        }

        void
        initSkin(void)
        {
            Driver::registerPlugin(PLATFORM_WGPU, 0, ID_SKIN, skinOpen, skinClose);
        }
    }
}

#else

namespace rw
{
    namespace wgpu
    {
        void initSkin(void)
        {
        }

        void closeSkin(void)
        {
        }
    }
}

#endif
