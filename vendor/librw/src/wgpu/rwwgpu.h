#pragma once

#ifdef LIBRW_GLFW
#include "GLFW/glfw3.h"
#endif

namespace rw
{
#ifdef RW_WGPU
    struct EngineOpenParams
    {
        GLFWwindow** window;
        int width, height;
        const char* windowtitle;
    };
#endif

    namespace wgpu
    {
        extern Device renderdevice;

        // Graphics backend selection. Set before the device is opened (RwEngineOpen);
        // changing it afterwards needs a device re-init (restart), since all GPU
        // resources live on the current device. Can also be overridden by the
        // LIBRW_WGPU_BACKEND=auto|vulkan|gl|d3d12|metal environment variable.
        enum
        {
            BACKEND_AUTO = 0, // let wgpu-native pick (Vulkan-first on desktop)
            BACKEND_VULKAN,
            BACKEND_GL,
            BACKEND_D3D12,
            BACKEND_METAL,
        };

        void setBackend(int backend); // one of the BACKEND_* values
        int getBackend(void); // the requested preference
        const char* getBackendName(void); // the backend actually in use (after open)

        void registerPlatformPlugins(void);
        void initMatFX(void);
        void initSkin(void);

        // ========================================================================
        // Custom im2d shaders (game post-FX: colour filter, screen droplets).
        // Mirrors gl3's im2dOverrideShader: while `im2dOverrideShader` is set, all
        // im2d draws (RwIm2DRenderIndexedPrimitive etc.) run with the custom shader
        // instead of the stock textured/untextured one.
        //
        // createIm2DShader() takes a WGSL *fragment* stage only — the backend
        // prepends the standard im2d vertex shader and these bindings:
        //   @group(0) @binding(0) var<uniform> u_xform : Xform;             (vertex)
        //   @group(0) @binding(1) var t0 : texture_2d<f32>;   // current raster
        //   @group(0) @binding(2) var s0 : sampler;
        //   @group(0) @binding(3) var<uniform> u_params : Params;  // 8 x vec4f
        //   @group(0) @binding(4) var t1 : texture_2d<f32>;   // setIm2DTexture2
        //   @group(0) @binding(5) var s1 : sampler;
        // The fragment source defines: @fragment fn fs_main(f : VsOut) ... and can
        // read u_params.p[i], sample t0/s0 (bound from the TEXTURERASTER render
        // state) and t1/s1 (bound via setIm2DTexture2, white 1x1 when unset).
        // Interstage inputs: f.color (vertex colour), f.uv.
        // ========================================================================
        struct Im2DShader;
        Im2DShader* createIm2DShader(const char* fragmentWGSL);
        void destroyIm2DShader(Im2DShader* sh);
        // Stage user params (copied; uploaded at draw time). Max 128 bytes (8 vec4).
        void setIm2DShaderParams(Im2DShader* sh, const void* data, uint32 size);
        // Second texture for custom shaders (t1/s1). nil resets to white.
        void setIm2DTexture2(Texture* tex);
        extern Im2DShader* im2dOverrideShader;

        // Extended variant for custom vertex formats (screen droplets' UV2 quads).
        // `fullWGSL` is a complete module: it must declare the same @group(0)
        // bindings it uses (see the contract above) plus vs_main/fs_main with its
        // own VsIn matching `attribs`. Attribute formats:
        enum
        {
            IM2DATTR_FLOAT2 = 0,
            IM2DATTR_FLOAT4,
            IM2DATTR_UNORM8X4,
        };

        struct Im2DCustomAttrib
        {
            uint32 location; // @location(n) in the WGSL VsIn
            uint32 format; // IM2DATTR_*
            uint32 offset; // byte offset within the vertex
        };

        Im2DShader* createIm2DShaderEx(const char* fullWGSL, const Im2DCustomAttrib* attribs,
                                       uint32 numAttribs, uint32 vertexStride);
        // Direct draw with a custom shader (bypasses the RwIm2D path and the override
        // global). Triangle lists only; blend/depth follow the current render state.
        void im2DRenderCustom(Im2DShader* sh, const void* verts, uint32 numVerts,
                              const uint16* indices, uint32 numIndices);

        // ========================================================================
        // Custom object-pipeline shaders (the game's EXTENDED_PIPELINES neo pipes).
        // Mirrors the gl3 custom-pipe pattern: the game supplies a WGSL *body*
        // (its own VsIn/VsOut structs plus vs_main/fs_main) and the backend
        // prepends the standard object uniforms, bindings and helpers:
        //   @binding(0) uniforms  : Uniforms   — viewProj, world, ambLight, matColor,
        //               surfProps, alphaTest, fogColor, fogData + 4 light arrays[8]
        //   @binding(1)/(2) t0/s0 — material texture (or setObjTexture0 override)
        //   @binding(3) u_params  : 16 x vec4f — game params (setObjShaderParams)
        //   @binding(4)/(5) t1/s1 — second texture (setObjTexture2)
        //   fn DoDynamicLight(V, N) -> vec3f, fn AlphaTestOK(a) -> bool,
        //   fn DoFog(w) -> f32
        // Vertex inputs (createObjShader):
        //   @location(0) pos:vec3f, (1) normal:vec3f, (2) color:vec4f,
        //   @location(3) uv:vec2f,  (4) uv1:vec2f
        // Skinned variant (createObjSkinShader) instead has
        //   @location(4) weights:vec4f, (5) indices:vec4u and
        //   @binding(6) boneMatrices : array<mat4x4f, 64>.
        // ========================================================================
        struct ObjShader;
        // Per-mesh hook for objShaderRender: update params / texture overrides for
        // this material, or return false to skip the mesh.
        typedef bool (*ObjShaderMeshCB)(ObjShader* sh, Atomic* atomic, Material* mat, void* user);

        enum
        {
            OBJRENDER_FORCEBLEND = 1, // force alpha blending on (e.g. additive gloss pass)
        };

        ObjShader* createObjShader(const char* bodyWGSL);
        ObjShader* createObjSkinShader(const char* bodyWGSL);
        void destroyObjShader(ObjShader* sh);
        // Stage user params (copied; uploaded per mesh at draw time). Max 256 bytes.
        void setObjShaderParams(ObjShader* sh, const void* data, uint32 size);
        // Second texture (t1/s1) for subsequent objShaderRender draws. nil = white.
        void setObjTexture2(Texture* tex);
        // Override t0 for the *current* mesh (call from the mesh callback; reset
        // automatically after each mesh). nil restores the material texture.
        void setObjTexture0(Texture* tex);
        // Render all meshes of the atomic with the custom shader: instances the
        // geometry, fills the standard uniforms (lights/fog/material colour), applies
        // the blend + PS2 GS alpha-test pass logic of the stock pipeline.
        void objShaderRender(ObjShader* sh, Atomic* atomic, ObjShaderMeshCB meshCB,
                             void* user, uint32 flags);
        // Stock object pipeline entry points, for custom ObjPipeline impls and
        // fallbacks (e.g. neo pipes with their effect toggled off).
        void objDefaultRender(ObjPipeline* pipe, Atomic* atomic);
        void objDefaultInstance(ObjPipeline* pipe, Atomic* atomic);
        void objSkinInstance(ObjPipeline* pipe, Atomic* atomic);

        // Per-mesh control over the stock object shader, for the game's NEW_RENDERER
        // (leeds-style world rendering: opaque meshes drawn immediately, blended ones
        // deferred and drawn later in sorted passes).
        // objMeshesNeedingBlend instances the atomic and returns a bitmask of meshes
        // with vertex/material/texture alpha (bit i = mesh i, first 32 meshes).
        uint32 objMeshesNeedingBlend(Atomic* atomic);
        // Render the selected meshes with the stock shader. `world` overrides the
        // frame LTM (nil = use the atomic's), `fadeAlpha` modulates material alpha,
        // `ambient` switches to ambient-only lighting (nil = full world lights).
        void objRenderMeshes(Atomic* atomic, const Matrix* world, uint32 whichMeshes,
                             uint8 fadeAlpha, const RGBAf* ambient);

        struct Im2DVertex
        {
            float32 x, y, z, w;
            uint8 r, g, b, a;
            float32 u, v;

            void setScreenX(float32 x) { this->x = x; }
            void setScreenY(float32 y) { this->y = y; }
            void setScreenZ(float32 z) { this->z = z; }
            void setCameraZ(float32 z) { this->w = z; }
            void setRecipCameraZ(float32 recipz) { this->w = 1.0f / recipz; }

            void setColor(uint8 r, uint8 g, uint8 b, uint8 a)
            {
                this->r = r;
                this->g = g;
                this->b = b;
                this->a = a;
            }

            void setU(float32 u, float recipz) { this->u = u; }
            void setV(float32 v, float recipz) { this->v = v; }

            float getScreenX(void) { return this->x; }
            float getScreenY(void) { return this->y; }
            float getScreenZ(void) { return this->z; }
            float getCameraZ(void) { return this->w; }
            float getRecipCameraZ(void) { return 1.0f / this->w; }
            RGBA getColor(void) { return makeRGBA(this->r, this->g, this->b, this->a); }
            float getU(void) { return this->u; }
            float getV(void) { return this->v; }
        };

        struct Im3DVertex
        {
            V3d position;
            V3d normal;
            uint8 r, g, b, a;
            float32 u, v;

            void setX(float32 x) { this->position.x = x; }
            void setY(float32 y) { this->position.y = y; }
            void setZ(float32 z) { this->position.z = z; }
            void setNormalX(float32 x) { this->normal.x = x; }
            void setNormalY(float32 y) { this->normal.y = y; }
            void setNormalZ(float32 z) { this->normal.z = z; }

            void setColor(uint8 r, uint8 g, uint8 b, uint8 a)
            {
                this->r = r;
                this->g = g;
                this->b = b;
                this->a = a;
            }

            void setU(float32 u) { this->u = u; }
            void setV(float32 v) { this->v = v; }

            float getX(void) { return this->position.x; }
            float getY(void) { return this->position.y; }
            float getZ(void) { return this->position.z; }
            RGBA getColor(void) { return makeRGBA(this->r, this->g, this->b, this->a); }
            float getU(void) { return this->u; }
            float getV(void) { return this->v; }
        };
    } // namespace wgpu
} // namespace rw
