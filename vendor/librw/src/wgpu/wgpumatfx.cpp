#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
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
        // MatFX pipeline — environment mapping (single pass: base + reflection)
        //
        // Mirrors gl3matfx.cpp. Instancing reuses the default object pipeline's
        // 56-byte vertex layout (instance() in wgpupipe.cpp). The env reflection is
        // added on top of the base in the fragment shader; blending is premultiplied
        // (src=ONE, dst=INVSRCALPHA) so non-env meshes (shininess=0) render normally.
        // Ref: gl3matfx.cpp, shaders/matfx_env.{vert,frag}.
        // ========================================================================

        struct MatFXState
        {
            WGPUBindGroupLayout bindGroupLayout; // uniforms, tex0, tex1, sampler
            WGPUTexture defaultTexture;
            WGPUTextureView defaultView;
            WGPUSampler defaultSampler;
            bool ready;
        };

        static MatFXState s_matfx = {};

        static RawMatrix normal2texcoord = {
            {0.5f, 0.0f, 0.0f}, 0.0f,
            {0.0f, -0.5f, 0.0f}, 0.0f,
            {0.0f, 0.0f, 1.0f}, 0.0f,
            {0.5f, 0.5f, 0.0f}, 1.0f
        };

        static WGPUCullMode
        toWGPUCullModeMatFX(uint32 rwCull)
        {
            switch (rwCull)
            {
            case CULLBACK: return WGPUCullMode_Back;
            case CULLFRONT: return WGPUCullMode_Front;
            default: return WGPUCullMode_None;
            }
        }

        static void
        ensureMatFXGPU(void)
        {
            if (s_matfx.ready)
                return;
            WGPUDevice dev = wgpuGlobals.device;

            WGPUBindGroupLayoutEntry bglEntries[4] = {};
            bglEntries[0].binding = 0;
            bglEntries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
            bglEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
            bglEntries[0].buffer.minBindingSize = 0;
            // Uniforms are sub-allocated from the shared ring at a per-draw dynamic offset.
            bglEntries[0].buffer.hasDynamicOffset = true;

            bglEntries[1].binding = 1; // base texture
            bglEntries[1].visibility = WGPUShaderStage_Fragment;
            bglEntries[1].texture.sampleType = WGPUTextureSampleType_Float;
            bglEntries[1].texture.viewDimension = WGPUTextureViewDimension_2D;

            bglEntries[2].binding = 2; // env texture
            bglEntries[2].visibility = WGPUShaderStage_Fragment;
            bglEntries[2].texture.sampleType = WGPUTextureSampleType_Float;
            bglEntries[2].texture.viewDimension = WGPUTextureViewDimension_2D;

            bglEntries[3].binding = 3; // shared sampler
            bglEntries[3].visibility = WGPUShaderStage_Fragment;
            bglEntries[3].sampler.type = WGPUSamplerBindingType_Filtering;

            WGPUBindGroupLayoutDescriptor bglDesc = {};
            bglDesc.label = WGPU_STR("matfx bgl");
            bglDesc.entryCount = 4;
            bglDesc.entries = bglEntries;
            s_matfx.bindGroupLayout = wgpuDeviceCreateBindGroupLayout(dev, &bglDesc);

            // Default 1x1 white texture
            WGPUTextureDescriptor texDesc = {};
            texDesc.label = WGPU_STR("matfx default tex");
            texDesc.size = {1, 1, 1};
            texDesc.format = WGPUTextureFormat_RGBA8Unorm;
            texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
            texDesc.dimension = WGPUTextureDimension_2D;
            texDesc.mipLevelCount = 1;
            texDesc.sampleCount = 1;
            s_matfx.defaultTexture = wgpuDeviceCreateTexture(dev, &texDesc);

            uint8_t white[4] = {255, 255, 255, 255};
            WGPUTexelCopyTextureInfo dst = {};
            dst.texture = s_matfx.defaultTexture;
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
            s_matfx.defaultView = wgpuTextureCreateView(s_matfx.defaultTexture, &viewDesc);

            WGPUSamplerDescriptor samplerDesc = {};
            samplerDesc.label = WGPU_STR("matfx sampler");
            samplerDesc.addressModeU = WGPUAddressMode_Repeat;
            samplerDesc.addressModeV = WGPUAddressMode_Repeat;
            samplerDesc.addressModeW = WGPUAddressMode_Repeat;
            samplerDesc.magFilter = WGPUFilterMode_Linear;
            samplerDesc.minFilter = WGPUFilterMode_Linear;
            samplerDesc.mipmapFilter = WGPUMipmapFilterMode_Linear;
            samplerDesc.maxAnisotropy = 16;
            samplerDesc.lodMaxClamp = 32.0f; // enable mip sampling (0 would pin to mip 0)
            s_matfx.defaultSampler = wgpuDeviceCreateSampler(dev, &samplerDesc);

            s_matfx.ready = true;
        }

        // ========================================================================
        // Pipeline (blend is always premultiplied src=ONE/dst=INVSRCALPHA)
        // ========================================================================
        static WGPURenderPipeline
        createMatFXPipeline(bool zWrite, WGPUCullMode cullMode, bool zTest)
        {
            std::string shaderSrc = std::string(R"(
struct Uniforms {
    viewProj:   mat4x4<f32>,
    world:      mat4x4<f32>,
    ambLight:   vec4<f32>,
    matColor:   vec4<f32>,
    surfProps:  vec4<f32>,  // ambCoef, spec, diffCoef, matAlpha
    alphaTest:  vec4<f32>,  // ref, enable, func
    fogColor:   vec4<f32>,
    fogData:    vec4<f32>,  // start, end, range, disable
    texMatrix:  mat4x4<f32>,
    fxparams:   vec4<f32>,  // shininess, disableFBA
    envColor:   vec4<f32>,
    colorClamp: vec4<f32>,
    lightParams:    array<vec4<f32>, 8>,
    lightPosition:  array<vec4<f32>, 8>,
    lightDirection: array<vec4<f32>, 8>,
    lightColor:     array<vec4<f32>, 8>,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var t_base: texture_2d<f32>;
@group(0) @binding(2) var t_env: texture_2d<f32>;
@group(0) @binding(3) var s_samp: sampler;
)") + k_doDynamicLightWGSL + R"(
struct VertexInput {
    @location(0) position: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) color: vec4<f32>,
    @location(3) uv: vec2<f32>,
};

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) color: vec4<f32>,
    @location(1) envColor: vec4<f32>,
    @location(2) uv0: vec2<f32>,
    @location(3) uv1: vec2<f32>,
    @location(4) fog: f32,
};

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    let worldPos = uniforms.world * vec4<f32>(input.position, 1.0);
    output.position = uniforms.viewProj * worldPos;
    let N = normalize((uniforms.world * vec4<f32>(input.normal, 0.0)).xyz);

    var col = input.color;
    col = vec4<f32>(clamp(col.rgb
                          + uniforms.ambLight.rgb * uniforms.surfProps.x
                          + DoDynamicLight(worldPos.xyz, N) * uniforms.surfProps.z,
                          vec3<f32>(0.0), vec3<f32>(1.0)), col.a);

    output.envColor = max(col, uniforms.colorClamp) * uniforms.envColor;
    output.color = col * uniforms.matColor;

    output.uv0 = input.uv;
    output.uv1 = (uniforms.texMatrix * vec4<f32>(N, 1.0)).xy;

    output.fog = clamp((output.position.w - uniforms.fogData.y) * uniforms.fogData.z,
                       uniforms.fogData.w, 1.0);
    return output;
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    let shininess  = uniforms.fxparams.x;
    let disableFBA = uniforms.fxparams.y;

    var pass1 = input.color;
    pass1 = pass1 * textureSample(t_base, s_samp, input.uv0);

    let pass2 = input.envColor * shininess * textureSample(t_env, s_samp, input.uv1);

    let p1rgb = mix(uniforms.fogColor.rgb, pass1.rgb, input.fog);
    let p2rgb = mix(vec3<f32>(0.0), pass2.rgb, input.fog);

    let fba = max(pass1.a, disableFBA);

    // Premultiplied output (blend is src=ONE / dst=INVSRCALPHA).
    let outAlpha = pass1.a;

    // Alpha test on the base alpha
    if (uniforms.alphaTest.y > 0.5) {
        if (uniforms.alphaTest.z < 1.5) {
            if (outAlpha < uniforms.alphaTest.x) { discard; }
        } else {
            if (outAlpha >= uniforms.alphaTest.x) { discard; }
        }
    }

    let rgb = p1rgb * pass1.a + p2rgb * fba;
    return vec4<f32>(rgb, outAlpha);
}
)";
            const char* shaderCode = shaderSrc.c_str();

            WGPUShaderSourceWGSL wgslDesc = {};
            wgslDesc.chain.sType = WGPUSType_ShaderSourceWGSL;
            wgslDesc.code = WGPU_STR(shaderCode);
            WGPUShaderModuleDescriptor shaderDesc = {};
            shaderDesc.nextInChain = &wgslDesc.chain;
            WGPUShaderModule shaderModule = wgpuDeviceCreateShaderModule(wgpuGlobals.device, &shaderDesc);

            // Vertex layout = default object layout (56 bytes; uv1 at offset 48 unused)
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
            vbLayout.arrayStride = OBJ_VERTEX_STRIDE;
            vbLayout.attributeCount = 4;
            vbLayout.attributes = attrs;

            WGPUPipelineLayoutDescriptor plDesc = {};
            plDesc.bindGroupLayoutCount = 1;
            plDesc.bindGroupLayouts = &s_matfx.bindGroupLayout;
            WGPUPipelineLayout pipelineLayout = wgpuDeviceCreatePipelineLayout(wgpuGlobals.device, &plDesc);

            WGPUBlendState blend = {};
            blend.color.srcFactor = WGPUBlendFactor_One;
            blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
            blend.color.operation = WGPUBlendOperation_Add;
            blend.alpha.srcFactor = WGPUBlendFactor_One;
            blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
            blend.alpha.operation = WGPUBlendOperation_Add;

            WGPUColorTargetState colorTarget = {};
            colorTarget.format = wgpuGlobals.surfaceFormat;
            colorTarget.blend = &blend;
            colorTarget.writeMask = WGPUColorWriteMask_All;

            WGPUFragmentState fragState = {};
            fragState.module = shaderModule;
            fragState.entryPoint = WGPU_STR("fs_main");
            fragState.targetCount = 1;
            fragState.targets = &colorTarget;

            // Depth WRITE from ZWRITEENABLE; depth COMPARE from ZTESTENABLE (independent).
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
            wgpuShaderModuleRelease(shaderModule);
            return pipeline;
        }

#define MATFX_CACHE_SIZE 16

        static struct
        {
            uint32 key;
            WGPURenderPipeline pipeline;
            bool valid;
        } s_matfxCache[MATFX_CACHE_SIZE];

        static WGPURenderPipeline
        getOrCreateMatFXPipeline(bool zWrite, uint32 cullMode, bool zTest)
        {
            uint32 key = (uint32)zWrite | ((cullMode & 0x3) << 1) | ((uint32)zTest << 3);
            int free = -1;
            for (int i = 0; i < MATFX_CACHE_SIZE; i++)
            {
                if (s_matfxCache[i].valid)
                {
                    if (s_matfxCache[i].key == key)
                        return s_matfxCache[i].pipeline;
                }
                else if (free < 0)
                    free = i;
            }
            assert(free >= 0 && "wgpu matfx pipeline cache full");
            WGPURenderPipeline pipe = createMatFXPipeline(zWrite, toWGPUCullModeMatFX(cullMode), zTest);
            s_matfxCache[free].key = key;
            s_matfxCache[free].pipeline = pipe;
            s_matfxCache[free].valid = true;
            return pipe;
        }

        static void
        flushMatFXCache(void)
        {
            for (int i = 0; i < MATFX_CACHE_SIZE; i++)
            {
                if (s_matfxCache[i].valid)
                {
                    wgpuRenderPipelineRelease(s_matfxCache[i].pipeline);
                    s_matfxCache[i].pipeline = nullptr;
                    s_matfxCache[i].valid = false;
                }
            }
        }

        // ========================================================================
        // Env texture matrix (mirror gl3 uploadEnvMatrix)
        // ========================================================================
        static void
        computeEnvMatrix(Frame* frame, float* out16)
        {
            if (frame == nil)
                frame = engine->currentCamera->getFrame();

            Matrix invMat;
            Matrix::invert(&invMat, frame->getLTM());
            RawMatrix invMtx;
            convMatrix(&invMtx, &invMat);
            invMtx.pos.set(0.0f, 0.0f, 0.0f);

            RawMatrix n2t = normal2texcoord;
            float uscale = fabsf(n2t.right.x);
            n2t.right.x = MatFX::envMapFlipU ? -uscale : uscale;

            RawMatrix envMtx;
            RawMatrix::mult(&envMtx, &invMtx, &n2t); // = n2t * invMtx
            memcpy(out16, &envMtx, 64);
        }

        // ========================================================================
        // Render callback
        // ========================================================================
        struct MatFXUniforms
        {
            float viewProj[16];
            float world[16];
            float ambLight[4];
            float matColor[4];
            float surfProps[4];
            float alphaTest[4];
            float fogColor[4];
            float fogData[4];
            float texMatrix[16];
            float fxparams[4];
            float envColor[4];
            float colorClamp[4];
            float lightParams[WGPU_MAX_LIGHTS * 4];
            float lightPosition[WGPU_MAX_LIGHTS * 4];
            float lightDirection[WGPU_MAX_LIGHTS * 4];
            float lightColor[WGPU_MAX_LIGHTS * 4];
        };

        // Bind group cache keyed by the (base, env) texture pair — matfx binds a base
        // texture (1) and an env-map texture (2); the uniforms (0) are addressed by a
        // per-draw dynamic offset into the shared ring, so a cached bind group depends
        // only on the two textures. Rebuilt when the ring buffer is recreated.
        struct MatFXKey
        {
            WGPUTextureView base;
            WGPUTextureView env;
            bool operator==(const MatFXKey& o) const { return base == o.base && env == o.env; }
        };

        struct MatFXKeyHash
        {
            size_t operator()(const MatFXKey& k) const
            {
                return std::hash<void*>()((void*)k.base) ^ (std::hash<void*>()((void*)k.env) << 1);
            }
        };

        static std::unordered_map<MatFXKey, WGPUBindGroup, MatFXKeyHash> s_matfxBindGroups;
        static uint32 s_matfxBindGroupsGen = 0xFFFFFFFFu;

        static void
        invalidateMatFXBindGroups(void)
        {
            for (auto& kv : s_matfxBindGroups)
                wgpuBindGroupRelease(kv.second);
            s_matfxBindGroups.clear();
        }

#define MATFX_BG_CACHE_MAX 512

        static WGPUBindGroup
        getMatFXBindGroup(WGPUTextureView baseView, WGPUTextureView envView, WGPUSampler sampler)
        {
            uint32 gen = wgpuUniformRingGeneration();
            if (gen != s_matfxBindGroupsGen)
            {
                invalidateMatFXBindGroups();
                s_matfxBindGroupsGen = gen;
            }
            if (s_matfxBindGroups.size() >= MATFX_BG_CACHE_MAX) // (P2-10)
                invalidateMatFXBindGroups();
            MatFXKey key = {baseView, envView};
            auto it = s_matfxBindGroups.find(key);
            if (it != s_matfxBindGroups.end())
                return it->second;

            WGPUBindGroupEntry e[4] = {};
            e[0].binding = 0;
            e[0].buffer = wgpuUniformRingBuffer();
            e[0].size = sizeof(MatFXUniforms);
            e[1].binding = 1;
            e[1].textureView = baseView;
            e[2].binding = 2;
            e[2].textureView = envView;
            e[3].binding = 3;
            e[3].sampler = sampler;

            WGPUBindGroupDescriptor d = {};
            d.layout = s_matfx.bindGroupLayout;
            d.entryCount = 4;
            d.entries = e;
            WGPUBindGroup bg = wgpuDeviceCreateBindGroup(wgpuGlobals.device, &d);
            s_matfxBindGroups[key] = bg;
            return bg;
        }

        static void
        matfxRenderCB(Atomic* atomic, WGPUInstanceDataHeader* header)
        {
            Geometry* geo = atomic->geometry;
            Frame* frame = atomic->getFrame();
            Matrix worldMat = *frame->getLTM();

            MatFXUniforms u;
            memset(&u, 0, sizeof(u));

            Camera* cam = (Camera*)engine->currentCamera;
            if (cam)
            {
                float* V = (float*)&cam->devView;
                float* P = (float*)&cam->devProj;
                float viewProj[16];
                mat4Mult(viewProj, V, P);
                memcpy(u.viewProj, viewProj, 64);
            }
            else
            {
                u.viewProj[0] = u.viewProj[5] = u.viewProj[10] = u.viewProj[15] = 1.0f;
            }

            u.world[0] = worldMat.right.x;
            u.world[1] = worldMat.right.y;
            u.world[2] = worldMat.right.z;
            u.world[3] = 0.0f;
            u.world[4] = worldMat.up.x;
            u.world[5] = worldMat.up.y;
            u.world[6] = worldMat.up.z;
            u.world[7] = 0.0f;
            u.world[8] = worldMat.at.x;
            u.world[9] = worldMat.at.y;
            u.world[10] = worldMat.at.z;
            u.world[11] = 0.0f;
            u.world[12] = worldMat.pos.x;
            u.world[13] = worldMat.pos.y;
            u.world[14] = worldMat.pos.z;
            u.world[15] = 1.0f;

            // Shared lighting (matColor/surfProps per-mesh below)
            LightDataGPU lights;
            gatherLights(atomic, &lights);
            memcpy(u.ambLight, lights.ambient, sizeof(lights.ambient));
            memcpy(u.lightParams, lights.params, sizeof(lights.params));
            memcpy(u.lightPosition, lights.position, sizeof(lights.position));
            memcpy(u.lightDirection, lights.direction, sizeof(lights.direction));
            memcpy(u.lightColor, lights.color, sizeof(lights.color));

            // u.alphaTest[0..2] set per draw-pass in the loop below (PS2 GS alpha-test).

            u.fogColor[0] = s_renderState.fogColor.red / 255.0f;
            u.fogColor[1] = s_renderState.fogColor.green / 255.0f;
            u.fogColor[2] = s_renderState.fogColor.blue / 255.0f;
            u.fogColor[3] = s_renderState.fogColor.alpha / 255.0f;
            if (cam && s_renderState.fogEnable)
            {
                float fs = cam->fogPlane, fe = cam->farPlane;
                u.fogData[0] = fs;
                u.fogData[1] = fe;
                u.fogData[2] = 1.0f / (fs - fe);
                u.fogData[3] = 0.0f;
            }
            else
            {
                u.fogData[3] = 1.0f;
            }

            for (uint32 i = 0; i < header->numMeshes; i++)
            {
                WGPUInstanceData* inst = &header->inst[i];
                Material* mat = inst->material;

                // material colour / surface lighting coefficients
                if (mat)
                {
                    u.matColor[0] = mat->color.red / 255.0f;
                    u.matColor[1] = mat->color.green / 255.0f;
                    u.matColor[2] = mat->color.blue / 255.0f;
                    u.matColor[3] = mat->color.alpha / 255.0f;
                    u.surfProps[0] = mat->surfaceProps.ambient;
                    u.surfProps[1] = mat->surfaceProps.specular;
                    u.surfProps[2] = mat->surfaceProps.diffuse;
                    u.surfProps[3] = mat->color.alpha / 255.0f;
                }
                else
                {
                    u.matColor[0] = u.matColor[1] = u.matColor[2] = u.matColor[3] = 1.0f;
                    u.surfProps[0] = 1.0f;
                    u.surfProps[1] = 0.0f;
                    u.surfProps[2] = 1.0f;
                    u.surfProps[3] = 1.0f;
                }

                // base texture
                WGPUTextureView baseView = s_matfx.defaultView;
                if ((geo->flags & Geometry::TEXTURED) && mat && mat->texture && mat->texture->raster)
                {
                    WGPURaster* ext = GETWGPURASTEREXT(mat->texture->raster);
                    if (ext && ext->view) baseView = ext->view;
                }

                // env effect (if any)
                WGPUTextureView envView = s_matfx.defaultView;
                u.fxparams[0] = 0.0f; // shininess (0 → no reflection)
                u.fxparams[1] = 1.0f; // disableFBA default
                u.envColor[0] = u.envColor[1] = u.envColor[2] = u.envColor[3] = 1.0f;
                u.colorClamp[0] = u.colorClamp[1] = u.colorClamp[2] = u.colorClamp[3] = 1.0f;
                memset(u.texMatrix, 0, 64);
                u.texMatrix[0] = u.texMatrix[5] = u.texMatrix[10] = u.texMatrix[15] = 1.0f;

                MatFX* matfx = mat ? MatFX::get(mat) : nil;
                if (matfx && matfx->type == MatFX::ENVMAP)
                {
                    MatFX::Env* env = &matfx->fx[0].env;
                    if (env->tex && env->coefficient != 0.0f)
                    {
                        WGPURaster* ext = env->tex->raster ? GETWGPURASTEREXT(env->tex->raster) : nullptr;
                        if (ext && ext->view) envView = ext->view;

                        u.fxparams[0] = env->coefficient;
                        u.fxparams[1] = env->fbAlpha ? 0.0f : 1.0f;

                        // colorClamp: 0 → modulate env by lighting, 1 → constant
                        float cc = MatFX::envMapApplyLight ? 0.0f : 1.0f;
                        u.colorClamp[0] = u.colorClamp[1] = u.colorClamp[2] = u.colorClamp[3] = cc;

                        RGBA ec = MatFX::envMapUseMatColor && mat ? mat->color : MatFX::envMapColor;
                        u.envColor[0] = ec.red / 255.0f;
                        u.envColor[1] = ec.green / 255.0f;
                        u.envColor[2] = ec.blue / 255.0f;
                        u.envColor[3] = ec.alpha / 255.0f;

                        computeEnvMatrix(env->frame, u.texMatrix);
                    }
                }

                // Same PS2 GS alpha-test emulation as the default/skin pipelines. MatFX
                // always blends (premultiplied), so a translucent (vertex/mat alpha or
                // alpha texture) mesh drawn with depth write gets the two-pass split:
                // opaque core keeps depth, soft remainder blends without depth write —
                // needed for vehicle glass/windows and alpha-textured car parts.
                bool texHasAlpha = false;
                if (mat && mat->texture && mat->texture->raster)
                {
                    uint32 cf = mat->texture->raster->format & 0xF00;
                    texHasAlpha = (cf == Raster::C8888 || cf == Raster::C1555 || cf == Raster::C4444);
                }
                bool meshBlend = inst->vertexAlpha || (mat && mat->color.alpha != 255) || texHasAlpha;

                struct DrawPass
                {
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
                    passes[0] = {true, gsref, 1.0f, (float)ALPHAGREATEREQUAL};
                    passes[1] = {false, gsref, 1.0f, (float)ALPHALESS};
                    numPasses = 2;
                }
                else if (gsEmu)
                {
                    passes[0] = {false, 0.0f, 0.0f, (float)ALPHAALWAYS};
                }
                else
                {
                    passes[0] = {
                        s_renderState.zWrite != 0,
                        s_renderState.alphaTestRef / 255.0f,
                        (s_renderState.alphaTestFunc != ALPHAALWAYS) ? 1.0f : 0.0f,
                        (float)s_renderState.alphaTestFunc
                    };
                }

                for (int p = 0; p < numPasses; p++)
                {
                    u.alphaTest[0] = passes[p].atRef;
                    u.alphaTest[1] = passes[p].atEnable;
                    u.alphaTest[2] = passes[p].atFunc;

                    WGPURenderPipeline pipeline = getOrCreateMatFXPipeline(
                        passes[p].zWrite, s_renderState.cullMode, s_renderState.zTest != 0);
                    wgpuRenderPassEncoderSetPipeline(s_frame.renderPass, pipeline);

                    // Sub-allocate uniforms from the ring + cached bind group keyed by the
                    // (base,env) texture pair; fall back to a private buffer on overflow.
                    WGPUBuffer fallbackBuffer = nullptr;
                    WGPUBindGroup throwaway = nullptr;
                    WGPUBindGroup bindGroup;
                    uint32 dynOffset = wgpuUniformAlloc(&u, sizeof(MatFXUniforms));
                    if (dynOffset != 0xFFFFFFFFu)
                    {
                        bindGroup = getMatFXBindGroup(baseView, envView, s_matfx.defaultSampler);
                    }
                    else
                    {
                        WGPUBufferDescriptor ubDesc = {};
                        ubDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
                        ubDesc.size = sizeof(MatFXUniforms);
                        fallbackBuffer = wgpuDeviceCreateBuffer(wgpuGlobals.device, &ubDesc);
                        wgpuQueueWriteBuffer(wgpuGlobals.queue, fallbackBuffer, 0, &u, sizeof(MatFXUniforms));

                        WGPUBindGroupEntry bgEntries[4] = {};
                        bgEntries[0].binding = 0;
                        bgEntries[0].buffer = fallbackBuffer;
                        bgEntries[0].size = sizeof(MatFXUniforms);
                        bgEntries[1].binding = 1;
                        bgEntries[1].textureView = baseView;
                        bgEntries[2].binding = 2;
                        bgEntries[2].textureView = envView;
                        bgEntries[3].binding = 3;
                        bgEntries[3].sampler = s_matfx.defaultSampler;

                        WGPUBindGroupDescriptor bgDesc = {};
                        bgDesc.layout = s_matfx.bindGroupLayout;
                        bgDesc.entryCount = 4;
                        bgDesc.entries = bgEntries;
                        throwaway = wgpuDeviceCreateBindGroup(wgpuGlobals.device, &bgDesc);
                        bindGroup = throwaway;
                        dynOffset = 0;
                    }

                    wgpuRenderPassEncoderSetBindGroup(s_frame.renderPass, 0, bindGroup, 1, &dynOffset);
                    wgpuRenderPassEncoderSetVertexBuffer(s_frame.renderPass, 0, inst->vertexBuffer, 0, WGPU_WHOLE_SIZE);
                    wgpuRenderPassEncoderSetIndexBuffer(s_frame.renderPass, inst->indexBuffer, WGPUIndexFormat_Uint16,
                                                        0, WGPU_WHOLE_SIZE);
                    wgpuRenderPassEncoderDrawIndexed(s_frame.renderPass, inst->numIndices, 1, 0, 0, 0);

                    if (throwaway) wgpuBindGroupRelease(throwaway);
                    if (fallbackBuffer) wgpuBufferRelease(fallbackBuffer);
                }
            }
        }

        static void
        matfxRender(ObjPipeline* pipe, Atomic* atomic)
        {
            ensureMatFXGPU();
            Geometry* geo = atomic->geometry;
            instance(pipe, atomic); // reuse default object-layout instancing
            if (geo->instData == nullptr || geo->instData->platform != PLATFORM_WGPU)
            {
                static bool warned = false;
                if (!warned)
                {
                    fprintf(stderr, "[wgpu] matfx atomic with NATIVE/foreign geometry skipped\n");
                    warned = true;
                }
                return;
            }
            matfxRenderCB(atomic, (WGPUInstanceDataHeader*)geo->instData);
        }

        // ========================================================================
        // Pipeline / plugin registration
        // ========================================================================
        static ObjPipeline*
        makeMatFXPipeline(void)
        {
            ObjPipeline* pipe = ObjPipeline::create();
            pipe->impl.instance = instance;
            pipe->impl.uninstance = nullptr;
            pipe->impl.render = matfxRender;
            pipe->pluginID = ID_MATFX;
            pipe->pluginData = 0;
            return pipe;
        }

        static void*
        matfxOpen(void* o, int32, int32)
        {
            matFXGlobals.pipelines[PLATFORM_WGPU] = makeMatFXPipeline();
            return o;
        }

        static void*
        matfxClose(void* o, int32, int32)
        {
            closeMatFX();
            if (matFXGlobals.pipelines[PLATFORM_WGPU])
            {
                ((ObjPipeline*)matFXGlobals.pipelines[PLATFORM_WGPU])->destroy();
                matFXGlobals.pipelines[PLATFORM_WGPU] = nil;
            }
            return o;
        }

        void
        closeMatFX(void)
        {
            flushMatFXCache();
            invalidateMatFXBindGroups();
            s_matfxBindGroupsGen = 0xFFFFFFFFu;
            if (!s_matfx.ready)
                return;
            if (s_matfx.defaultSampler)
            {
                wgpuSamplerRelease(s_matfx.defaultSampler);
                s_matfx.defaultSampler = nullptr;
            }
            if (s_matfx.defaultView)
            {
                wgpuTextureViewRelease(s_matfx.defaultView);
                s_matfx.defaultView = nullptr;
            }
            if (s_matfx.defaultTexture)
            {
                wgpuTextureRelease(s_matfx.defaultTexture);
                s_matfx.defaultTexture = nullptr;
            }
            if (s_matfx.bindGroupLayout)
            {
                wgpuBindGroupLayoutRelease(s_matfx.bindGroupLayout);
                s_matfx.bindGroupLayout = nullptr;
            }
            s_matfx.ready = false;
        }

        void
        initMatFX(void)
        {
            Driver::registerPlugin(PLATFORM_WGPU, 0, ID_MATFX, matfxOpen, matfxClose);
        }
    }
}

#else

namespace rw
{
    namespace wgpu
    {
        void initMatFX(void)
        {
        }

        void closeMatFX(void)
        {
        }
    }
}

#endif
