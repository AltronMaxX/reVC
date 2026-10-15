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

#ifdef RW_WGPU

#include "wgpu_internal.h"

#define PLUGIN_ID 0

namespace rw
{
    namespace wgpu
    {
        // ========================================================================
        // Atomic pipeline state
        // ========================================================================
        struct AtomicState
        {
            WGPUBindGroupLayout bindGroupLayout;
            WGPUTexture defaultTexture;
            WGPUTextureView defaultView;
            WGPUSampler defaultSampler;
        };

        AtomicState s_atomic = {};

        static WGPUShaderModule s_objShaderModule = nullptr;

        // ObjectUniforms (736 bytes, declared in wgpu_internal.h): per-draw uniform
        // block for the object pipeline. The four light arrays (128 bytes each)
        // dominate — this is what the object shaders declare, so the ring-buffer
        // binding must cover the whole struct.

        // ========================================================================
        // Per-texture bind group cache
        // ========================================================================
        // Each bind group binds the shared uniform ring (at a dynamic offset supplied
        // per draw), a texture view and the shared sampler. The uniform data varies per
        // draw but is addressed via the dynamic offset, so a bind group only depends on
        // the texture — cache one per view and reuse across draws and frames. Rebuilt
        // when the ring buffer is recreated (generation bump).
        static std::unordered_map<WGPUTextureView, WGPUBindGroup> s_objBindGroups;
        static uint32 s_objBindGroupsGen = 0xFFFFFFFFu;

        static void
        invalidateObjectBindGroups(void)
        {
            for (auto& kv : s_objBindGroups)
                wgpuBindGroupRelease(kv.second);
            s_objBindGroups.clear();
        }

#define OBJ_BG_CACHE_MAX 512

        static WGPUBindGroup
        getObjectBindGroup(WGPUTextureView view, WGPUSampler sampler)
        {
            uint32 gen = wgpuUniformRingGeneration();
            if (gen != s_objBindGroupsGen)
            {
                invalidateObjectBindGroups();
                s_objBindGroupsGen = gen;
            }
            if (s_objBindGroups.size() >= OBJ_BG_CACHE_MAX) // (P2-10)
                invalidateObjectBindGroups();
            auto it = s_objBindGroups.find(view);
            if (it != s_objBindGroups.end())
                return it->second;

            WGPUBindGroupEntry e[3] = {};
            e[0].binding = 0;
            e[0].buffer = wgpuUniformRingBuffer();
            e[0].offset = 0; // base; per-draw offset is dynamic
            e[0].size = sizeof(ObjectUniforms); // full 736-byte block the shader reads
            e[1].binding = 1;
            e[1].textureView = view;
            e[2].binding = 2;
            e[2].sampler = sampler;

            WGPUBindGroupDescriptor d = {};
            d.layout = s_atomic.bindGroupLayout;
            d.entryCount = 3;
            d.entries = e;
            WGPUBindGroup bg = wgpuDeviceCreateBindGroup(wgpuGlobals.device, &d);
            s_objBindGroups[view] = bg;
            return bg;
        }

        // ========================================================================
        // Atomic — Initialize pipeline
        // ========================================================================
        void openAtomic(void)
        {
            WGPUDevice dev = wgpuGlobals.device;

            // BindGroupLayout
            WGPUBindGroupLayoutEntry bglEntries[3] = {};

            // binding 0: uniform buffer
            bglEntries[0].binding = 0;
            bglEntries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
            bglEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
            // 0 = check binding size at draw time. The cached object pipelines share this
            // layout but use a larger (224-byte) Uniforms struct than k_atomicWGSL (192).
            bglEntries[0].buffer.minBindingSize = 0;
            // Draws bind the shared uniform ring buffer at a per-draw dynamic offset
            // (see wgpuUniformAlloc) instead of a private buffer each mesh/pass/frame.
            bglEntries[0].buffer.hasDynamicOffset = true;

            // binding 1: texture
            bglEntries[1].binding = 1;
            bglEntries[1].visibility = WGPUShaderStage_Fragment;
            bglEntries[1].texture.sampleType = WGPUTextureSampleType_Float;
            bglEntries[1].texture.viewDimension = WGPUTextureViewDimension_2D;

            // binding 2: sampler
            bglEntries[2].binding = 2;
            bglEntries[2].visibility = WGPUShaderStage_Fragment;
            bglEntries[2].sampler.type = WGPUSamplerBindingType_Filtering;

            WGPUBindGroupLayoutDescriptor bglDesc = {};
            bglDesc.label = WGPU_STR("atomic bgl");
            bglDesc.entryCount = 3;
            bglDesc.entries = bglEntries;
            s_atomic.bindGroupLayout = wgpuDeviceCreateBindGroupLayout(dev, &bglDesc);

            // Default 1x1 white texture
            WGPUTextureDescriptor texDesc = {};
            texDesc.label = WGPU_STR("atomic default tex");
            texDesc.size = {1, 1, 1};
            texDesc.format = WGPUTextureFormat_RGBA8Unorm;
            texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
            texDesc.dimension = WGPUTextureDimension_2D;
            texDesc.mipLevelCount = 1;
            texDesc.sampleCount = 1;
            s_atomic.defaultTexture = wgpuDeviceCreateTexture(dev, &texDesc);

            uint8_t white[4] = {255, 255, 255, 255};
            WGPUTexelCopyTextureInfo dst = {};
            dst.texture = s_atomic.defaultTexture;
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
            s_atomic.defaultView = wgpuTextureCreateView(s_atomic.defaultTexture, &viewDesc);

            // Sampler
            WGPUSamplerDescriptor samplerDesc = {};
            samplerDesc.label = WGPU_STR("atomic sampler");
            samplerDesc.addressModeU = WGPUAddressMode_Repeat;
            samplerDesc.addressModeV = WGPUAddressMode_Repeat;
            samplerDesc.addressModeW = WGPUAddressMode_Repeat;
            samplerDesc.magFilter = WGPUFilterMode_Linear;
            samplerDesc.minFilter = WGPUFilterMode_Linear;
            samplerDesc.mipmapFilter = WGPUMipmapFilterMode_Linear;
            samplerDesc.maxAnisotropy = 16;
            // Without an explicit lodMaxClamp the descriptor's zero value pins sampling
            // to mip 0, disabling mipmapping. 32 covers any real texture's mip count;
            // textures with a single level clamp to it via their view's mipLevelCount.
            samplerDesc.lodMaxClamp = 32.0f;
            s_atomic.defaultSampler = wgpuDeviceCreateSampler(dev, &samplerDesc);
        }

        void flushPipelineCache(void);

        void closeAtomic(void)
        {
            flushPipelineCache();
            // (P3-21) release the cached object shader module while the device is
            // still alive (termWGPU calls this before stopWGPU).
            if (s_objShaderModule)
            {
                wgpuShaderModuleRelease(s_objShaderModule);
                s_objShaderModule = nullptr;
            }
            invalidateObjectBindGroups();
            s_objBindGroupsGen = 0xFFFFFFFFu;
            if (s_atomic.bindGroupLayout)
            {
                wgpuBindGroupLayoutRelease(s_atomic.bindGroupLayout);
                s_atomic.bindGroupLayout = nullptr;
            }
            if (s_atomic.defaultView)
            {
                wgpuTextureViewRelease(s_atomic.defaultView);
                s_atomic.defaultView = nullptr;
            }
            if (s_atomic.defaultTexture)
            {
                wgpuTextureRelease(s_atomic.defaultTexture);
                s_atomic.defaultTexture = nullptr;
            }
            if (s_atomic.defaultSampler)
            {
                wgpuSamplerRelease(s_atomic.defaultSampler);
                s_atomic.defaultSampler = nullptr;
            }
        }

        // ========================================================================
        // Instancing (создание WGPU буферов для Geometry)
        // ========================================================================

        void
        freeInstanceData(Geometry* geometry)
        {
            if (geometry->instData == nullptr ||
                geometry->instData->platform != PLATFORM_WGPU)
                return;

            WGPUInstanceDataHeader* header = (WGPUInstanceDataHeader*)geometry->instData;
            geometry->instData = nullptr;

            for (uint32 i = 0; i < header->numMeshes; i++)
            {
                if (header->inst[i].indexBuffer)
                    wgpuBufferRelease(header->inst[i].indexBuffer);
            }
            if (header->sharedVbo)
                wgpuBufferRelease(header->sharedVbo);

            rwFree(header->inst);
            rwFree(header);
        }

        void*
        destroyNativeData(void* object, int32, int32)
        {
            freeInstanceData((Geometry*)object);
            return object;
        }

        static void fillVertexData(Geometry* geo, float* verts)
        {
            const int F = OBJ_VERTEX_STRIDE / 4; // 14 float slots per vertex

            // Positions
            for (int32 i = 0; i < geo->numVertices; i++)
            {
                verts[i * F + 0] = geo->morphTargets[0].vertices[i].x;
                verts[i * F + 1] = geo->morphTargets[0].vertices[i].y;
                verts[i * F + 2] = geo->morphTargets[0].vertices[i].z;
            }

            // Normals
            if (geo->flags & Geometry::NORMALS)
            {
                for (int32 i = 0; i < geo->numVertices; i++)
                {
                    verts[i * F + 3] = geo->morphTargets[0].normals[i].x;
                    verts[i * F + 4] = geo->morphTargets[0].normals[i].y;
                    verts[i * F + 5] = geo->morphTargets[0].normals[i].z;
                }
            }

            // Colors (prelight). The buffer is uninitialized (malloc), so we must
            // fill the color for every vertex. Non-prelit geometry defaults to
            // (0,0,0,1): rgb=0 makes the shader fall back to the material colour,
            // alpha=1 keeps it opaque (vertex alpha only applies to prelit meshes).
            if (geo->flags & Geometry::PRELIT)
            {
                for (int32 i = 0; i < geo->numVertices; i++)
                {
                    verts[i * F + 6] = geo->colors[i].red / 255.0f;
                    verts[i * F + 7] = geo->colors[i].green / 255.0f;
                    verts[i * F + 8] = geo->colors[i].blue / 255.0f;
                    verts[i * F + 9] = geo->colors[i].alpha / 255.0f;
                }
            }
            else
            {
                for (int32 i = 0; i < geo->numVertices; i++)
                {
                    verts[i * F + 6] = 0.0f;
                    verts[i * F + 7] = 0.0f;
                    verts[i * F + 8] = 0.0f;
                    verts[i * F + 9] = 1.0f;
                }
            }

            // Texcoords: uv0 always; uv1 = second set if present, else a copy of uv0
            // (dual-texture materials on single-UV geometry sample with uv0, like gl3).
            {
                int32 uv1set = geo->numTexCoordSets > 1 ? 1 : 0;
                for (int32 i = 0; i < geo->numVertices; i++)
                {
                    if (geo->numTexCoordSets > 0)
                    {
                        verts[i * F + 10] = geo->texCoords[0][i].u;
                        verts[i * F + 11] = geo->texCoords[0][i].v;
                        verts[i * F + 12] = geo->texCoords[uv1set][i].u;
                        verts[i * F + 13] = geo->texCoords[uv1set][i].v;
                    }
                    else
                    {
                        verts[i * F + 10] = 0.0f;
                        verts[i * F + 11] = 0.0f;
                        verts[i * F + 12] = 0.0f;
                        verts[i * F + 13] = 0.0f;
                    }
                }
            }
        }

        static WGPUInstanceDataHeader*
        instanceMesh(Geometry* geo)
        {
            WGPUInstanceDataHeader* header = rwNewT(WGPUInstanceDataHeader, 1, MEMDUR_EVENT | ID_GEOMETRY);
            MeshHeader* meshh = geo->meshHeader;
            geo->instData = (InstanceDataHeader*)header;
            header->platform = PLATFORM_WGPU;

            header->numMeshes = meshh->numMeshes;
            header->totalNumVertex = geo->numVertices;
            header->totalNumIndex = meshh->totalIndices;
            header->inst = rwNewT(WGPUInstanceData, header->numMeshes, MEMDUR_EVENT | ID_GEOMETRY);

            // Создаём vertex buffer для всей геометрии
            // Формат: position(3) + normal(3) + color(4) + texcoord0(2) + texcoord1(2)
            //       = 14 floats = 56 bytes (uv1 нужен кастомным пайпам: lightmap/dual)
            uint32 stride = OBJ_VERTEX_STRIDE;
            uint8* vertexData = rwNewT(uint8, header->totalNumVertex * stride, MEMDUR_EVENT | ID_GEOMETRY);

            // Заполняем vertex buffer
            float* verts = (float*)vertexData;
            fillVertexData(geo, verts);

            // Создаём WGPU vertex buffer
            WGPUBufferDescriptor vbDesc = {};
            vbDesc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
            vbDesc.size = header->totalNumVertex * stride;
            vbDesc.mappedAtCreation = false;

            // Geometry has per-vertex alpha (prelit colours with alpha < 255)? Such
            // meshes must be alpha-blended (mirrors gl3's inst->vertexAlpha).
            bool geoVertexAlpha = false;
            if (geo->flags & Geometry::PRELIT)
            {
                for (int32 v = 0; v < geo->numVertices; v++)
                    if (geo->colors[v].alpha != 255)
                    {
                        geoVertexAlpha = true;
                        break;
                    }
            }

            WGPUInstanceData* inst = header->inst;
            Mesh* mesh = meshh->getMeshes();

            header->sharedVbo = wgpuDeviceCreateBuffer(wgpuGlobals.device, &vbDesc);
            wgpuQueueWriteBuffer(wgpuGlobals.queue, header->sharedVbo, 0, vertexData, vbDesc.size);

            for (uint32 i = 0; i < header->numMeshes; i++)
            {
                inst[i].vertexBuffer = header->sharedVbo; // alias, not owned

                // GTA/RW meshes are usually triangle STRIPS (meshHeader flags == TRISTRIP),
                // but our pipeline bakes TriangleList topology (gl3 instead picks
                // GL_TRIANGLE_STRIP, gl3pipe.cpp:60). Un-strip to a triangle list at
                // instance time so a single list pipeline renders everything correctly.
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

        static void refreshInstanceData(Geometry* geo, WGPUInstanceDataHeader* h)
        {
            uint32 stride = OBJ_VERTEX_STRIDE;
            float* verts = rwNewT(float, h->totalNumVertex * stride, MEMDUR_EVENT);
            fillVertexData(geo, verts);
            if (h->sharedVbo)
                wgpuQueueWriteBuffer(wgpuGlobals.queue, h->sharedVbo, 0, verts,
                                     h->totalNumVertex * stride);
            rwFree(verts);
        }

        void
        instance(ObjPipeline* pipe, Atomic* atomic)
        {
            Geometry* geo = atomic->geometry;
            if (geo->flags & Geometry::NATIVE)
                return;

            WGPUInstanceDataHeader* header = (WGPUInstanceDataHeader*)geo->instData;
            if (geo->instData)
            {
                if (header->platform != PLATFORM_WGPU)
                {
                    freeInstanceData(geo);
                    header = nullptr;
                }
            }
            if (geo->instData == nullptr)
            {
                geo->instData = (InstanceDataHeader*)instanceMesh(geo);
            }
            else if (geo->lockedSinceInst)
                refreshInstanceData(geo, header);
            geo->lockedSinceInst = 0;
        }

        // ========================================================================
        // Render Pipeline
        // ========================================================================

        static WGPUCullMode toWGPUCullMode(uint32 rwCull)
        {
            switch (rwCull)
            {
            case CULLBACK: return WGPUCullMode_Back;
            case CULLFRONT: return WGPUCullMode_Front;
            default: return WGPUCullMode_None; // CULLNONE
            }
        }

        static WGPURenderPipeline
        createRenderPipeline(bool hasTexture, bool hasNormals, bool hasPrelight,
                             bool blendEnable, WGPUBlendFactor srcFactor,
                             WGPUBlendFactor dstFactor, bool zWrite, WGPUCullMode cullMode,
                             bool zTest)
        {
            // WGSL shader
            std::string shaderSrc = std::string(R"(
struct Uniforms {
    viewProj: mat4x4<f32>,
    world: mat4x4<f32>,
    ambLight: vec4<f32>,
    matColor: vec4<f32>,
    surfProps: vec4<f32>,  // ambient, specular, diffuse, matAlpha
    alphaTest: vec4<f32>,  // x=ref(0..1), y=enable, z=func(1=GEqual,2=Less)
    fogColor: vec4<f32>,   // rgb fog color
    fogData:  vec4<f32>,   // x=start, y=end, z=range(1/(start-end)), w=disable(0 on, 1 off)
    lightParams:    array<vec4<f32>, 8>,  // x=type(0 end,1 dir,2 point,3 spot), y=radius, z=minusCosAngle, w=hardSpot
    lightPosition:  array<vec4<f32>, 8>,
    lightDirection: array<vec4<f32>, 8>,
    lightColor:     array<vec4<f32>, 8>,
};
)") + k_doDynamicLightWGSL + R"(
@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var t_diffuse: texture_2d<f32>;
@group(0) @binding(2) var s_diffuse: sampler;

struct VertexInput {
    @location(0) position: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) color: vec4<f32>,
    @location(3) uv: vec2<f32>,
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
    let worldPos = uniforms.world * vec4<f32>(input.position, 1.0);
    output.position = uniforms.viewProj * worldPos;
    let N = normalize((uniforms.world * vec4<f32>(input.normal, 0.0)).xyz);

    // RW lighting: prelight + ambient*surfAmbient + dynamic*surfDiffuse,
    // clamped, then modulated by the material colour.
    var col = input.color;   // prelight (0,0,0,1 for non-prelit)
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
            if (!s_objShaderModule)
            {
                WGPUShaderSourceWGSL wgslDesc = {};
                wgslDesc.chain.sType = WGPUSType_ShaderSourceWGSL;
                wgslDesc.code = WGPU_STR(shaderSrc.c_str());
                WGPUShaderModuleDescriptor shaderDesc = {};
                shaderDesc.nextInChain = &wgslDesc.chain;
                shaderDesc.label = WGPU_STR("object shader");
                s_objShaderModule = wgpuDeviceCreateShaderModule(wgpuGlobals.device, &shaderDesc);
            }
            WGPUShaderModule shaderModule = s_objShaderModule;

            // Vertex buffer layout
            WGPUVertexAttribute attrs[4] = {};
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

            WGPUVertexBufferLayout vbLayout = {};
            vbLayout.arrayStride = OBJ_VERTEX_STRIDE; // uv1 at offset 48 unused here
            vbLayout.attributeCount = 4;
            vbLayout.attributes = attrs;

            // Bind group layout
            WGPUBindGroupLayoutEntry bglEntries[3] = {};
            bglEntries[0].binding = 0;
            bglEntries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
            bglEntries[0].buffer.type = WGPUBufferBindingType_Uniform;

            bglEntries[1].binding = 1;
            bglEntries[1].visibility = WGPUShaderStage_Fragment;
            bglEntries[1].texture.sampleType = WGPUTextureSampleType_Float;
            bglEntries[1].texture.viewDimension = WGPUTextureViewDimension_2D;

            bglEntries[2].binding = 2;
            bglEntries[2].visibility = WGPUShaderStage_Fragment;
            bglEntries[2].sampler.type = WGPUSamplerBindingType_Filtering;

            WGPUBindGroupLayoutDescriptor bglDesc = {};
            bglDesc.entryCount = 3;
            bglDesc.entries = bglEntries;

            // Pipeline layout
            WGPUPipelineLayoutDescriptor plDesc = {};
            plDesc.bindGroupLayoutCount = 1;
            plDesc.bindGroupLayouts = &s_atomic.bindGroupLayout;
            WGPUPipelineLayout pipelineLayout = wgpuDeviceCreatePipelineLayout(wgpuGlobals.device, &plDesc);

            // Color target. When blending is disabled we pass a null blend state
            // (WGPU writes the source color directly).
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

            // Fragment state
            WGPUFragmentState fragState = {};
            fragState.module = shaderModule;
            fragState.entryPoint = WGPU_STR("fs_main");
            fragState.targetCount = 1;
            fragState.targets = &colorTarget;

            // Depth stencil. Depth WRITE follows ZWRITEENABLE; the depth COMPARE follows
            // ZTESTENABLE independently (makeDepthState couples compare to write, which is
            // wrong for the transparent pass: ZTEST on + ZWRITE off must still occlude —
            // otherwise translucent geometry draws through walls).
            WGPUDepthStencilState depthStencil = makeDepthState(zWrite);
            depthStencil.depthCompare = zTest
                                            ? WGPUCompareFunction_LessEqual
                                            : WGPUCompareFunction_Always;

            // Pipeline
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
            rpDesc.multisample.count = currentSampleCount();
            rpDesc.multisample.mask = 0xFFFFFFFF;
            rpDesc.fragment = &fragState;

            WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(wgpuGlobals.device, &rpDesc);

            wgpuPipelineLayoutRelease(pipelineLayout);

            return pipeline;
        }

        // ========================================================================
        // Pipeline Cache
        // ========================================================================

        // WGPU pipelines are immutable, so every distinct combination of geometry
        // flags and render state (blend factors, blend enable, zwrite) needs its own
        // baked pipeline. We cache them keyed by a packed key and linear-scan.
        // Growable (Im2DPipeCache): the old fixed 64-slot array asserted when full
        // and, in release builds, wrote past its end (free could be -1).
        static Im2DPipeCache s_pipelineCache;

        static uint32 pipelineCacheKey(bool hasTexture, bool hasNormals, bool hasPrelight,
                                       bool blendEnable, uint32 srcBlend, uint32 destBlend,
                                       bool zWrite, uint32 cullMode, bool zTest)
        {
            return (uint32)hasTexture
                | ((uint32)hasNormals << 1)
                | ((uint32)hasPrelight << 2)
                | ((uint32)blendEnable << 3)
                | ((uint32)zWrite << 4)
                | ((srcBlend & 0xF) << 5)
                | ((destBlend & 0xF) << 9)
                | ((cullMode & 0x3) << 13)
                | ((uint32)zTest << 15)
                | (currentSampleCount() << 28);
        }

        static WGPURenderPipeline
        getOrCreatePipeline(bool hasTexture, bool hasNormals, bool hasPrelight,
                            bool blendEnable, uint32 srcBlend, uint32 destBlend, bool zWrite,
                            uint32 cullMode, bool zTest)
        {
            uint32 key = pipelineCacheKey(hasTexture, hasNormals, hasPrelight,
                                          blendEnable, srcBlend, destBlend, zWrite, cullMode, zTest);
            WGPURenderPipeline cached = imPipeCacheFind(&s_pipelineCache, key);
            if (cached) return cached;

            WGPURenderPipeline pipe = createRenderPipeline(
                hasTexture, hasNormals, hasPrelight, blendEnable,
                toWGPUBlendFactor(srcBlend), toWGPUBlendFactor(destBlend), zWrite,
                toWGPUCullMode(cullMode), zTest);
            imPipeCacheInsert(&s_pipelineCache, key, pipe);
            return pipe;
        }

        void
        flushPipelineCache(void)
        {
            imPipeCacheRelease(&s_pipelineCache);
        }
        // (Forward declaration above changed from static to external so the
        // MSAA-recreate path in wgpudevice.cpp can flush the atomic cache.
        // Function body unchanged.)

        // ========================================================================
        // Render Callback
        // ========================================================================

        // Fill the per-atomic part of ObjectUniforms: viewProj, world, gathered
        // lights, alpha-test render state and fog. Per-mesh fields (ambient=matColor,
        // matProps=surfProps) are the caller's job. Shared with the custom object
        // shaders (wgpucustom.cpp).
        void
        fillObjectUniforms(Atomic* atomic, ObjectUniforms* u)
        {
            Frame* frame = atomic->getFrame();
            Matrix worldMat = *frame->getLTM();

            Camera* cam = (Camera*)engine->currentCamera;
            if (cam)
            {
                float* V = (float*)&cam->devView;
                float* P = (float*)&cam->devProj;
                float viewProj[16];
                mat4Mult(viewProj, V, P);
                memcpy(u->viewProj, viewProj, 64);
            }
            else
            {
                memset(u->viewProj, 0, 64);
                u->viewProj[0] = 1.0f;
                u->viewProj[5] = 1.0f;
                u->viewProj[10] = 1.0f;
                u->viewProj[15] = 1.0f;
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
            memcpy(u->world, world, 64);

            // ---- Shared (per-atomic) lighting ----
            // Gather the world lights affecting this atomic; ambLight + the 4 light
            // arrays come straight from it (matches gl3 enumerateLights/setLights).
            LightDataGPU lights;
            gatherLights(atomic, &lights);
            memcpy(u->lightDir, lights.ambient, sizeof(lights.ambient)); // -> ambLight
            memcpy(u->lightParams, lights.params, sizeof(lights.params));
            memcpy(u->lightPosition, lights.position, sizeof(lights.position));
            memcpy(u->lightDirection, lights.direction, sizeof(lights.direction));
            memcpy(u->lightColor, lights.color, sizeof(lights.color));

            // Alpha test: включён, если функция не ALPHAALWAYS
            uint32 atFunc = s_renderState.alphaTestFunc;
            u->alphaTest[0] = s_renderState.alphaTestRef / 255.0f;
            u->alphaTest[1] = (atFunc != ALPHAALWAYS) ? 1.0f : 0.0f;
            u->alphaTest[2] = (float)atFunc;
            u->alphaTest[3] = 0.0f;

            // Fog: same model as gl3 — fog factor = clamp((w - end)*range, disable, 1).
            // start = cam->fogPlane, end = cam->farPlane, range = 1/(start-end).
            u->fogColor[0] = s_renderState.fogColor.red / 255.0f;
            u->fogColor[1] = s_renderState.fogColor.green / 255.0f;
            u->fogColor[2] = s_renderState.fogColor.blue / 255.0f;
            u->fogColor[3] = s_renderState.fogColor.alpha / 255.0f;
            if (cam && s_renderState.fogEnable)
            {
                float fogStart = cam->fogPlane;
                float fogEnd = cam->farPlane;
                u->fogData[0] = fogStart;
                u->fogData[1] = fogEnd;
                u->fogData[2] = 1.0f / (fogStart - fogEnd);
                u->fogData[3] = 0.0f; // enabled
            }
            else
            {
                u->fogData[0] = 0.0f;
                u->fogData[1] = 0.0f;
                u->fogData[2] = 0.0f;
                u->fogData[3] = 1.0f; // disabled → factor clamps to 1 (no fog)
            }
        }

        // Shared per-mesh draw loop for the stock object shader. `baseUniforms` has
        // the per-atomic fields filled (fillObjectUniforms or a caller override);
        // per-mesh material fields are set here. `whichMeshes` is a bitmask (mesh i
        // drawn when bit i is set; meshes >= 32 always draw) and `fadeAlpha`
        // modulates material alpha (255 = none) — both serve the game's NEW_RENDERER
        // deferred blend pass (objRenderMeshes below).
        static void
        renderMeshes(Geometry* geo, WGPUInstanceDataHeader* header,
                     const ObjectUniforms* baseUniforms, uint32 whichMeshes, uint8 fadeAlpha)
        {
            bool hasTexture = (geo->flags & Geometry::TEXTURED) != 0;
            bool hasNormals = (geo->flags & Geometry::NORMALS) != 0;
            bool hasPrelight = (geo->flags & Geometry::PRELIT) != 0;

            // Local copy: per-mesh fields (ambient=matColor, matProps, alphaTest) are
            // rewritten every iteration.
            ObjectUniforms uniforms = *baseUniforms;

            // ---- Per-mesh: each mesh has its own material (colour + texture), so it
            // needs its own uniform buffer and bind group. ----
            for (uint32 i = 0; i < header->numMeshes; i++)
            {
                if (i < 32 && !(whichMeshes & (1u << i)))
                    continue;
                WGPUInstanceData* inst = &header->inst[i];
                Material* mat = inst->material;

                // Blend enable (the game's VERTEXALPHA). Original RenderWare enables it
                // when the geometry has vertex alpha, a translucent material colour, OR
                // the material texture has an alpha channel. gl3render.cpp:168 drops the
                // texture check, but the reference GL renderer still shows alpha-textured
                // glass/shadows/foliage blended — because the PS2 GS alpha-test emulation
                // (below) is what actually makes them look right: it splits each blended
                // mesh into an opaque high-alpha pass (keeps depth) and a soft low-alpha
                // pass (no depth write). Without the texture check these render as opaque
                // grey/black splats; with it plus GSemu they blend correctly with no dark
                // fringe and without hiding what's behind their soft edges.
                bool texHasAlpha = false;
                if (mat && mat->texture && mat->texture->raster)
                {
                    uint32 cf = mat->texture->raster->format & 0xF00;
                    texHasAlpha = (cf == Raster::C8888 || cf == Raster::C1555 || cf == Raster::C4444);
                }
                bool meshBlend = inst->vertexAlpha || (mat && mat->color.alpha != 255) ||
                    texHasAlpha || fadeAlpha != 255;

                // matColor (rgb + alpha) and surfProps (material surface lighting coefs)
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
                if (fadeAlpha != 255)
                {
                    // NEW_RENDERER fading entities: modulate material alpha
                    uniforms.ambient[3] = uniforms.ambient[3] * fadeAlpha / 255.0f;
                    uniforms.matProps[3] = uniforms.ambient[3];
                }

                WGPUTextureView textureView = s_atomic.defaultView;
                WGPUSampler sampler = s_atomic.defaultSampler;
                if (hasTexture && mat && mat->texture && mat->texture->raster)
                {
                    WGPURaster* ext = GETWGPURASTEREXT(mat->texture->raster);
                    if (ext && ext->view)
                        textureView = ext->view;
                }

                // Draw passes — faithful port of librw gl3render.cpp drawInst_GSemu, the
                // PS2 GS alpha-test emulation reVC renders with (gPS2alphaTest, on by
                // default; GSALPHATESTREF=128). When a blended mesh is drawn with depth
                // write on it renders in TWO passes: an opaque-alpha pass (alpha >= gsref,
                // keeps depth) and a blended-alpha pass (alpha < gsref, no depth write).
                // This is what makes vertex/material-alpha surfaces — shadow decals,
                // glass, water — look smooth without occluding what's behind their soft
                // parts. Meshes without blend (alpha-textured foliage/fences: alpha only
                // in the texture) fall through to a single opaque pass with the ordinary
                // render-state alpha test (ref 3) — exactly as the reference GL renderer.
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
                    // blended but no depth write: single pass, alpha test disabled
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

                    WGPURenderPipeline pipeline = getOrCreatePipeline(
                        hasTexture, hasNormals, hasPrelight, passes[p].blend,
                        s_renderState.srcBlend, s_renderState.destBlend,
                        passes[p].zWrite, s_renderState.cullMode,
                        s_renderState.zTest != 0);
                    wgpuRenderPassEncoderSetPipeline(s_frame.renderPass, pipeline);

                    // Sub-allocate this draw's uniforms from the shared ring and bind it
                    // with a dynamic offset + a per-texture cached bind group. On ring
                    // overflow (rare, only until it grows to the frame's peak) fall back
                    // to a private buffer + bind group for this draw.
                    WGPUBuffer fallbackBuffer = nullptr;
                    WGPUBindGroup fallbackGroup = nullptr;
                    uint32 dynOffset = wgpuUniformAlloc(&uniforms, sizeof(ObjectUniforms));
                    if (dynOffset != 0xFFFFFFFFu)
                    {
                        WGPUBindGroup bindGroup = getObjectBindGroup(textureView, sampler);
                        wgpuRenderPassEncoderSetBindGroup(s_frame.renderPass, 0, bindGroup, 1, &dynOffset);
                    }
                    else
                    {
                        WGPUBufferDescriptor ubDesc = {};
                        ubDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
                        ubDesc.size = sizeof(ObjectUniforms);
                        fallbackBuffer = wgpuDeviceCreateBuffer(wgpuGlobals.device, &ubDesc);
                        wgpuQueueWriteBuffer(wgpuGlobals.queue, fallbackBuffer, 0, &uniforms, sizeof(ObjectUniforms));

                        WGPUBindGroupEntry bgEntries[3] = {};
                        bgEntries[0].binding = 0;
                        bgEntries[0].buffer = fallbackBuffer;
                        bgEntries[0].offset = 0;
                        bgEntries[0].size = sizeof(ObjectUniforms);
                        bgEntries[1].binding = 1;
                        bgEntries[1].textureView = textureView;
                        bgEntries[2].binding = 2;
                        bgEntries[2].sampler = sampler;

                        WGPUBindGroupDescriptor bgDesc = {};
                        bgDesc.layout = s_atomic.bindGroupLayout;
                        bgDesc.entryCount = 3;
                        bgDesc.entries = bgEntries;
                        fallbackGroup = wgpuDeviceCreateBindGroup(wgpuGlobals.device, &bgDesc);
                        uint32 zero = 0;
                        wgpuRenderPassEncoderSetBindGroup(s_frame.renderPass, 0, fallbackGroup, 1, &zero);
                    }

                    wgpuRenderPassEncoderSetVertexBuffer(s_frame.renderPass, 0, inst->vertexBuffer, 0, WGPU_WHOLE_SIZE);
                    wgpuRenderPassEncoderSetIndexBuffer(s_frame.renderPass, inst->indexBuffer, WGPUIndexFormat_Uint16,
                                                        0, WGPU_WHOLE_SIZE);
                    wgpuRenderPassEncoderDrawIndexed(s_frame.renderPass, inst->numIndices, 1, 0, 0, 0);

                    if (fallbackGroup) wgpuBindGroupRelease(fallbackGroup);
                    if (fallbackBuffer) wgpuBufferRelease(fallbackBuffer);
                }
            }
        }

        static void
        defaultRenderCB(Atomic* atomic, WGPUInstanceDataHeader* header)
        {
            // Uniform data for this draw call. CPU field names lightDir/ambient/
            // matProps map by offset to the WGSL ambLight/matColor/surfProps; the
            // four light arrays follow fogData.
            ObjectUniforms uniforms;
            fillObjectUniforms(atomic, &uniforms);
            renderMeshes(atomic->geometry, header, &uniforms, 0xFFFFFFFFu, 255);
        }

        // ========================================================================
        // NEW_RENDERER helpers (public API, rwwgpu.h) — the game's leeds-style world
        // renderer draws opaque meshes immediately and defers blended ones, so it
        // needs per-mesh control over the stock object shader.
        // ========================================================================

        uint32
        objMeshesNeedingBlend(Atomic* atomic)
        {
            instance(nil, atomic);
            Geometry* geo = atomic->geometry;
            if (geo->instData == nil || geo->instData->platform != PLATFORM_WGPU)
                return 0;
            WGPUInstanceDataHeader* header = (WGPUInstanceDataHeader*)geo->instData;
            uint32 mask = 0;
            for (uint32 i = 0; i < header->numMeshes && i < 32; i++)
            {
                WGPUInstanceData* inst = &header->inst[i];
                Material* mat = inst->material;
                bool texHasAlpha = false;
                if (mat && mat->texture && mat->texture->raster)
                {
                    uint32 cf = mat->texture->raster->format & 0xF00;
                    texHasAlpha = (cf == Raster::C8888 || cf == Raster::C1555 || cf == Raster::C4444);
                }
                if (inst->vertexAlpha || (mat && mat->color.alpha != 255) || texHasAlpha)
                    mask |= 1u << i;
            }
            return mask;
        }

        void
        objRenderMeshes(Atomic* atomic, const Matrix* world, uint32 whichMeshes,
                        uint8 fadeAlpha, const RGBAf* ambient)
        {
            if (whichMeshes == 0)
                return;
            if (!s_frame.inRenderPass || !s_frame.renderPass)
                return;
            instance(nil, atomic);
            Geometry* geo = atomic->geometry;
            if (geo->instData == nil || geo->instData->platform != PLATFORM_WGPU)
                return;
            WGPUInstanceDataHeader* header = (WGPUInstanceDataHeader*)geo->instData;

            ObjectUniforms uniforms;
            fillObjectUniforms(atomic, &uniforms);
            if (world)
            {
                // deferred draws render with the matrix captured at defer time
                uniforms.world[0] = world->right.x;
                uniforms.world[1] = world->right.y;
                uniforms.world[2] = world->right.z;
                uniforms.world[3] = 0.0f;
                uniforms.world[4] = world->up.x;
                uniforms.world[5] = world->up.y;
                uniforms.world[6] = world->up.z;
                uniforms.world[7] = 0.0f;
                uniforms.world[8] = world->at.x;
                uniforms.world[9] = world->at.y;
                uniforms.world[10] = world->at.z;
                uniforms.world[11] = 0.0f;
                uniforms.world[12] = world->pos.x;
                uniforms.world[13] = world->pos.y;
                uniforms.world[14] = world->pos.z;
                uniforms.world[15] = 1.0f;
            }
            if (ambient)
            {
                // explicit ambient-only lighting (no dynamic lights)
                uniforms.lightDir[0] = ambient->red;
                uniforms.lightDir[1] = ambient->green;
                uniforms.lightDir[2] = ambient->blue;
                uniforms.lightDir[3] = ambient->alpha;
                memset(uniforms.lightParams, 0, sizeof(uniforms.lightParams));
            }
            renderMeshes(geo, header, &uniforms, whichMeshes, fadeAlpha);
        }

        // ========================================================================
        // Atomic render callback
        // ========================================================================

        // White 1x1 fallback texture + shared object sampler, for the custom object
        // shaders (wgpucustom.cpp) — AtomicState's fields are private to this file.
        WGPUTextureView
        objDefaultTextureView(void)
        {
            return s_atomic.defaultView;
        }

        WGPUSampler
        objDefaultSampler(void)
        {
            return s_atomic.defaultSampler;
        }

        void
        atomicRender(ObjPipeline* pipe, Atomic* atomic)
        {
            Geometry* geo = atomic->geometry;
            instance(pipe, atomic);

            if (geo->instData == nullptr || geo->instData->platform != PLATFORM_WGPU)
            {
                static bool warned = false;
                if (!warned)
                {
                    fprintf(stderr, "[wgpu] atomic with NATIVE/foreign geometry skipped\n");
                    warned = true;
                }
                return;
            }

            WGPUInstanceDataHeader* header = (WGPUInstanceDataHeader*)geo->instData;
            defaultRenderCB(atomic, header);
        }
    } // namespace wgpu
} // namespace rw
#endif
