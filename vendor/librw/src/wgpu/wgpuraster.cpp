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

#include <atomic>
#include <chrono>

#include "wgpu_internal.h"

#define PLUGIN_ID 0

namespace rw
{
    namespace wgpu
    {
        // ========================================================================
        // Raster support
        // ========================================================================

        bool32 imageFindRasterFormat(Image* img, int32 type,
                                     int32* pWidth, int32* pHeight, int32* pDepth, int32* pFormat)
        {
            assert((type&0xF) == Raster::TEXTURE);
            int32 w = img->width, h = img->height, d = img->depth;
            if (d <= 8) d = 32;
            int32 fmt;
            switch (d)
            {
            case 32: fmt = img->hasAlpha() ? Raster::C8888 : Raster::C888;
                if (fmt == Raster::C888) d = 24;
                break;
            case 24: fmt = Raster::C888;
                break;
            case 16: fmt = Raster::C1555;
                break;
            default: RWERROR((ERR_INVRASTER));
                return 0;
            }
            *pWidth = w;
            *pHeight = h;
            *pDepth = d;
            *pFormat = fmt | type;
            return 1;
        }

        static uint32 padRowPitch(uint32 pitch) { return (pitch + 255u) & ~255u; }

        // Upload one tightly packed RGBA8 block (w*4 bytes per row) into a mip level.
        static void uploadRGBA8Rows(WGPUTexture texture, uint32 level, uint32 w, uint32 h,
                                    const uint8* src)
        {
            uint32 tight = w * 4;
            uint32 pitch = padRowPitch(tight);
            WGPUTexelCopyTextureInfo dst = {};
            dst.texture = texture;
            dst.aspect = WGPUTextureAspect_All;
            dst.mipLevel = level;
            WGPUTexelCopyBufferLayout lay = {};
            lay.bytesPerRow = pitch;
            lay.rowsPerImage = h;
            WGPUExtent3D sz = {w, h, 1};
            if (pitch == tight)
            {
                wgpuQueueWriteTexture(wgpuGlobals.queue, &dst, src, (size_t)pitch * h, &lay, &sz);
                return;
            }
            uint8* tmp = new uint8[(size_t)pitch * h];
            for (uint32 y = 0; y < h; y++)
                memcpy(tmp + (size_t)y * pitch, src + (size_t)y * tight, tight);
            wgpuQueueWriteTexture(wgpuGlobals.queue, &dst, tmp, (size_t)pitch * h, &lay, &sz);
            delete[] tmp;
        }

        // CAMERATEXTURE rasters are created in the surface format (BGRA8Unorm on
        // Vulkan). The RW lock contract hands out RGBA bytes, so swap R/B on
        // read/write (P2-12). Regular texture rasters are RGBA8Unorm — no swap.
        static bool rasterNeedsRBSwap(Raster* raster)
        {
            return raster->type == Raster::CAMERATEXTURE &&
                wgpuGlobals.surfaceFormat == WGPUTextureFormat_BGRA8Unorm;
        }

        static void swapRB(uint8* px, uint32 n)
        {
            for (uint32 i = 0; i < n; i++)
            {
                uint8 t = px[i * 4 + 0];
                px[i * 4 + 0] = px[i * 4 + 2];
                px[i * 4 + 2] = t;
            }
        }

        // Lazily allocate the GPU texture for a TEXTURE/NORMAL raster (P3-22):
        // RwRasterCreate no longer pre-allocates, so the TXD load path
        // (Raster::create + setFromImage) doesn't allocate a texture twice.
        // rasterLock calls this on demand; rasterFromImage creates its own.
        static void ensureTextureRaster(Raster* raster)
        {
            WGPURaster* ext = GETWGPURASTEREXT(raster);
            if (ext->texture || raster->width <= 0 || raster->height <= 0)
                return;
            WGPUDevice dev = wgpuGlobals.device;
            int32 w = raster->width, h = raster->height;
            raster->stride = w * 4;

            int32 numLevels = 1;
            if (raster->format & Raster::MIPMAP)
            {
                int mw = w, mh = h;
                while (mw != 1 || mh != 1)
                {
                    numLevels++;
                    if (mw > 1) mw /= 2;
                    if (mh > 1) mh /= 2;
                }
            }
            ext->numLevels = numLevels;

            WGPUTextureDescriptor texDesc = {};
            texDesc.label = WGPU_STR("texture raster");
            texDesc.size = {(uint32_t)w, (uint32_t)h, 1};
            texDesc.format = WGPUTextureFormat_RGBA8Unorm;
            texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst |
                WGPUTextureUsage_CopySrc;
            texDesc.dimension = WGPUTextureDimension_2D;
            texDesc.mipLevelCount = (uint32_t)numLevels;
            texDesc.sampleCount = 1;
            ext->texture = wgpuDeviceCreateTexture(dev, &texDesc);

            WGPUTextureViewDescriptor viewDesc = {};
            viewDesc.format = WGPUTextureFormat_RGBA8Unorm;
            viewDesc.dimension = WGPUTextureViewDimension_2D;
            viewDesc.aspect = WGPUTextureAspect_All;
            viewDesc.baseMipLevel = 0;
            viewDesc.mipLevelCount = (uint32_t)numLevels;
            viewDesc.baseArrayLayer = 0;
            viewDesc.arrayLayerCount = 1;
            ext->view = wgpuTextureCreateView(ext->texture, &viewDesc);

            if (s_im2dText.bindGroupLayout)
            {
                WGPUBindGroupEntry bgEntries[3] = {};
                bgEntries[0].binding = 0;
                bgEntries[0].buffer = s_im2d.uniformBuf;
                bgEntries[0].offset = 0;
                bgEntries[0].size = 16;
                bgEntries[1].binding = 1;
                bgEntries[1].textureView = ext->view;
                bgEntries[2].binding = 2;
                bgEntries[2].sampler = s_im2dText.sampler;
                WGPUBindGroupDescriptor bgDesc = {};
                bgDesc.label = WGPU_STR("texture raster bg");
                bgDesc.layout = s_im2dText.bindGroupLayout;
                bgDesc.entryCount = 3;
                bgDesc.entries = bgEntries;
                ext->bindGroup = wgpuDeviceCreateBindGroup(dev, &bgDesc);
            }
        }

        Raster* rasterCreate(Raster* raster)
        {
            if (!raster) return nil;
            if (wgpuRasterOffset > 0)
            {
                WGPURaster* ext = GETWGPURASTEREXT(raster);
                ext->texture = nullptr;
                ext->view = nullptr;
                ext->bindGroup = nullptr;
                ext->lockBuffer = nullptr;
                ext->lockLevel = 0;
                ext->lockMode = 0;
                ext->stagingBuffer = nullptr;
                ext->stagingSize = 0;
                ext->lockSavedTexture = nullptr;
                ext->numLevels = 1;

                // A zero-size raster allocates no backing texture; it becomes a
                // sub-raster of another via Raster::subRaster(), which only takes
                // effect when DONTALLOCATE is set (see raster.cpp). gl3's rasterCreate
                // does the same. Without this, camera sub-raster insets (e.g. the
                // camera tool's mini-view) silently stay 0x0 with parent==self, so
                // they aren't recognised as sub-rects and clear the whole frame.
                if (raster->width == 0 || raster->height == 0)
                {
                    raster->flags |= Raster::DONTALLOCATE;
                    raster->stride = 0;
                    return raster;
                }

                WGPUDevice dev = wgpuGlobals.device;

                if (raster->type == Raster::CAMERATEXTURE && raster->width > 0 && raster->height > 0)
                {
                    // Camera render target texture
                    int32 w = raster->width, h = raster->height;
                    raster->stride = w * 4;

                    WGPUTextureDescriptor texDesc = {};
                    texDesc.label = WGPU_STR("camera texture");
                    texDesc.size = {(uint32_t)w, (uint32_t)h, 1};
                    texDesc.format = wgpuGlobals.surfaceFormat;
                    texDesc.usage = WGPUTextureUsage_TextureBinding
                        | WGPUTextureUsage_RenderAttachment
                        | WGPUTextureUsage_CopyDst
                        | WGPUTextureUsage_CopySrc;
                    texDesc.dimension = WGPUTextureDimension_2D;
                    texDesc.mipLevelCount = 1;
                    texDesc.sampleCount = 1;
                    ext->texture = wgpuDeviceCreateTexture(dev, &texDesc);

                    WGPUTextureViewDescriptor viewDesc = {};
                    viewDesc.format = wgpuGlobals.surfaceFormat;
                    viewDesc.dimension = WGPUTextureViewDimension_2D;
                    viewDesc.aspect = WGPUTextureAspect_All;
                    viewDesc.baseMipLevel = 0;
                    viewDesc.mipLevelCount = 1;
                    viewDesc.arrayLayerCount = 1;
                    ext->view = wgpuTextureCreateView(ext->texture, &viewDesc);

                    // BindGroup for Im2DText sampling
                    if (s_im2dText.bindGroupLayout)
                    {
                        WGPUBindGroupEntry bgEntries[3] = {};
                        bgEntries[0].binding = 0;
                        bgEntries[0].buffer = s_im2d.uniformBuf;
                        bgEntries[0].offset = 0;
                        bgEntries[0].size = 16;
                        bgEntries[1].binding = 1;
                        bgEntries[1].textureView = ext->view;
                        bgEntries[2].binding = 2;
                        bgEntries[2].sampler = s_im2dText.sampler;
                        WGPUBindGroupDescriptor bgDesc = {};
                        bgDesc.label = WGPU_STR("camera raster bg");
                        bgDesc.layout = s_im2dText.bindGroupLayout;
                        bgDesc.entryCount = 3;
                        bgDesc.entries = bgEntries;
                        ext->bindGroup = wgpuDeviceCreateBindGroup(dev, &bgDesc);
                    }
                }

                if (raster->type == Raster::ZBUFFER && raster->width > 0 && raster->height > 0)
                {
                    // Depth/stencil texture
                    int32 w = raster->width, h = raster->height;
                    raster->stride = 0;

                    WGPUTextureDescriptor texDesc = {};
                    texDesc.label = WGPU_STR("zbuffer texture");
                    texDesc.size = {(uint32_t)w, (uint32_t)h, 1};
                    texDesc.format = WGPUTextureFormat_Depth32Float;
                    texDesc.usage = WGPUTextureUsage_RenderAttachment;
                    texDesc.dimension = WGPUTextureDimension_2D;
                    texDesc.mipLevelCount = 1;
                    texDesc.sampleCount = 1;
                    ext->texture = wgpuDeviceCreateTexture(dev, &texDesc);

                    WGPUTextureViewDescriptor viewDesc = {};
                    viewDesc.format = WGPUTextureFormat_Depth32Float;
                    viewDesc.dimension = WGPUTextureViewDimension_2D;
                    viewDesc.aspect = WGPUTextureAspect_All;
                    viewDesc.baseMipLevel = 0;
                    viewDesc.mipLevelCount = 1;
                    viewDesc.arrayLayerCount = 1;
                    ext->view = wgpuTextureCreateView(ext->texture, &viewDesc);
                }

                // Regular texture raster created without an image, then locked to write
                // pixels (e.g. CRadar::LoadTextures does RwRasterCreate + RwRasterLock).
                // Allocate the GPU texture now so rasterLock/Unlock have a target;
                // rasterFromImage releases and replaces this if an image is assigned.
                if ((raster->type == Raster::TEXTURE || raster->type == Raster::NORMAL) &&
                    raster->width > 0 && raster->height > 0)
                {
                    raster->stride = raster->width * 4;
                }
            }
            return raster;
        }

        void* destroyNativeRaster(void *object, int32 offset, int32 size)
        {
            (void)offset;
            (void)size;
            rasterDestroy((Raster*)object);
            return object;
        }

        void rasterDestroy(Raster* raster)
        {
            if (!raster || wgpuRasterOffset == 0) return;
            if (raster->platform != PLATFORM_WGPU) return;
            WGPURaster* ext = GETWGPURASTEREXT(raster);
            if (!ext) return;
            // Clean up any pending lock state
            if (ext->stagingBuffer)
            {
                wgpuBufferUnmap(ext->stagingBuffer);
                wgpuBufferRelease(ext->stagingBuffer);
                ext->stagingBuffer = nullptr;
                ext->lockBuffer = nullptr;
            }
            else if (ext->lockBuffer)
            {
                rwFree(ext->lockBuffer);
                ext->lockBuffer = nullptr;
            }
            if (ext->bindGroup)
            {
                wgpuBindGroupRelease(ext->bindGroup);
                ext->bindGroup = nullptr;
            }
            if (ext->view)
            {
                wgpuTextureViewRelease(ext->view);
                ext->view = nullptr;
            }
            if (ext->texture)
            {
                wgpuTextureRelease(ext->texture);
                ext->texture = nullptr;
            }
        }

        // Helper: synchronous buffer map for readback
        static uint8_t* mapBufferSync(WGPUBuffer buf, uint64_t size)
        {
            struct UD
            {
                std::atomic<bool> done{false};
            } ud;
            auto cb = [](WGPUMapAsyncStatus s, WGPUStringView msg, void* u1, void* u2)
            {
                (void)s;
                (void)msg;
                ((UD*)u1)->done.store(true, std::memory_order_release);
            };
            WGPUBufferMapCallbackInfo ci = {};
            ci.mode = WGPUCallbackMode_AllowSpontaneous;
            ci.callback = cb;
            ci.userdata1 = &ud;
            wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, size, ci);
            auto t0 = std::chrono::steady_clock::now();
            while (!ud.done.load(std::memory_order_acquire))
            {
                wgpuInstanceProcessEvents(wgpuGlobals.instance);
                if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(15))
                {
                    fprintf(stderr, "[wgpu] mapBufferSync: no answer after 15s (driver hang?) — giving up\n");
                    return nullptr;
                }
            }
            const WGPUBufferMapState mapState = wgpuBufferGetMapState(buf);
            if (mapState != WGPUBufferMapState_Mapped) return nullptr;
            return (uint8_t*)wgpuBufferGetMappedRange(buf, 0, size);
        }

        uint8* rasterLock(Raster* raster, int32 level, int32 lockMode)
        {
            if (!raster || wgpuRasterOffset == 0) return nullptr;
            WGPURaster* ext = GETWGPURASTEREXT(raster);
            if (!ext) return nullptr;
            if (!ext->texture)
                ensureTextureRaster(raster);
            if (!ext->texture) return nullptr;

            ext->lockLevel = level;
            ext->lockMode = lockMode;
            ext->lockSavedTexture = ext->texture;

            // Per-level dimensions (mip level `level`), not the base size — otherwise
            // locking/uploading levels > 0 uses the wrong extent.
            uint32 w = (uint32_t)raster->width >> level;
            if (w == 0) w = 1;
            uint32 h = (uint32_t)raster->height >> level;
            if (h == 0) h = 1;
            uint32 bpp = 4; // RGBA8Unorm
            uint32 rowPitch = w * bpp;
            uint32 totalSize = rowPitch * h;

            if (lockMode & Raster::LOCKWRITE)
            {
                // Allocate CPU memory for write
                ext->lockBuffer = (uint8_t*)rwMalloc(totalSize, MEMDUR_EVENT | ID_GEOMETRY);
                if (!ext->lockBuffer) return nullptr;
                return ext->lockBuffer;
            }

            if (lockMode & Raster::LOCKREAD)
            {
                uint32 pitch = padRowPitch(rowPitch);
                uint64_t stagingSize = (uint64_t)pitch * h;


                // Create staging buffer for readback
                WGPUBufferDescriptor sbDesc = {};
                sbDesc.label = WGPU_STR("raster lock staging");
                sbDesc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
                sbDesc.size = stagingSize;
                ext->stagingBuffer = wgpuDeviceCreateBuffer(wgpuGlobals.device, &sbDesc);
                ext->stagingSize = (uint32_t)stagingSize;

                // Copy texture to staging buffer
                WGPUCommandEncoderDescriptor encDesc = {};
                encDesc.label = WGPU_STR("raster lock encoder");
                WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(wgpuGlobals.device, &encDesc);

                WGPUTexelCopyTextureInfo src = {};
                src.texture = ext->texture;
                src.aspect = WGPUTextureAspect_All;
                src.mipLevel = (uint32_t)level;

                WGPUTexelCopyBufferInfo dst = {};
                dst.buffer = ext->stagingBuffer;
                dst.layout.bytesPerRow = pitch;
                dst.layout.rowsPerImage = h;
                dst.layout.offset = 0;

                WGPUExtent3D copySize = {w, h, 1};
                wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &copySize);

                WGPUCommandBufferDescriptor cmdDesc = {};
                cmdDesc.label = WGPU_STR("raster lock cmd");
                WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, &cmdDesc);
                wgpuQueueSubmit(wgpuGlobals.queue, 1, &cmd);
                wgpuCommandBufferRelease(cmd);
                wgpuCommandEncoderRelease(enc);

                // Map synchronously, then repack the padded rows into a tight CPU
                //buffer (the RW lock contract hands out tightly packed pixels).
                uint8_t* mapped = mapBufferSync(ext->stagingBuffer, stagingSize);
                if (!mapped)
                {
                    wgpuBufferUnmap(ext->stagingBuffer);
                    wgpuBufferRelease(ext->stagingBuffer);
                    ext->stagingBuffer = nullptr;
                    return nullptr;
                }
                ext->lockBuffer = (uint8_t*)rwMalloc(totalSize, MEMDUR_EVENT | ID_GEOMETRY);
                if (ext->lockBuffer)
                {
                    for (uint32 y = 0; y < h; y++)
                        memcpy(ext->lockBuffer + (size_t)y * rowPitch,
                               mapped + (size_t)y * pitch, rowPitch);
                    // CAMERATEXTURE on a BGRA surface: hand out RGBA bytes (P2-12).
                    if (rasterNeedsRBSwap(raster))
                        swapRB(ext->lockBuffer, w * h);
                }
                wgpuBufferUnmap(ext->stagingBuffer);
                wgpuBufferRelease(ext->stagingBuffer);
                ext->stagingBuffer = nullptr;
                return ext->lockBuffer;
            }

            return nullptr;
        }

        void rasterUnlock(Raster* raster, int32 level)
        {
            (void)level;
            if (!raster || wgpuRasterOffset == 0) return;
            WGPURaster* ext = GETWGPURASTEREXT(raster);
            if (!ext || !ext->texture) return;

            if (ext->lockMode & Raster::LOCKWRITE)
            {
                if (!ext->lockBuffer)
                {
                    ext->lockLevel = 0;
                    ext->lockMode = 0;
                    ext->lockSavedTexture = nullptr;
                    return;
                }
                if (ext->texture != ext->lockSavedTexture)
                {
                    rwFree(ext->lockBuffer);
                    ext->lockBuffer = nullptr;
                    ext->lockLevel = 0;
                    ext->lockMode = 0;
                    ext->lockSavedTexture = nullptr;
                    return;
                }
                // Upload CPU buffer back to texture at the locked mip level (per-level
                // dimensions, matching rasterLock).
                uint32 w = (uint32_t)raster->width >> ext->lockLevel;
                if (w == 0) w = 1;
                uint32 h = (uint32_t)raster->height >> ext->lockLevel;
                uint32 rowPitch = w * 4;

                // CAMERATEXTURE on a BGRA surface: the game wrote RGBA bytes —
                // swap R/B before the upload (P2-12).
                if (rasterNeedsRBSwap(raster))
                    swapRB(ext->lockBuffer, w * h);

                // Padded upload (P0-1).
                uploadRGBA8Rows(ext->texture, (uint32_t)ext->lockLevel, w, h, ext->lockBuffer);

                rwFree(ext->lockBuffer);
                ext->lockBuffer = nullptr;
            }

            if (ext->lockMode & Raster::LOCKREAD)
            {
                // The staging buffer was already unmapped+released in rasterLock;
                // the tight repacked lockBuffer returned to the caller is ours.
                if (ext->lockBuffer)
                {
                    rwFree(ext->lockBuffer);
                    ext->lockBuffer = nullptr;
                }
                if (ext->stagingBuffer)
                {
                    wgpuBufferUnmap(ext->stagingBuffer);
                    wgpuBufferRelease(ext->stagingBuffer);
                    ext->stagingBuffer = nullptr;
                }
            }

            ext->lockLevel = 0;
            ext->lockMode = 0;
        }

        int32 rasterNumLevels(Raster* raster)
        {
            if (!raster || wgpuRasterOffset == 0) return 1;
            WGPURaster* ext = GETWGPURASTEREXT(raster);
            if (!ext) return 1;
            return ext->numLevels;
        }

        bool32 rasterFromImage(Raster* raster, Image* image)
        {
            if (!raster || !image) return 0;

            WGPUDevice dev = wgpuGlobals.device;

            // Convert to RGBA32
            int32 w = image->width, h = image->height;
            uint8_t* pixels = new uint8_t[w * h * 4];
            if (image->depth == 32)
            {
                memcpy(pixels, image->pixels, w * h * 4);
            }
            else if (image->depth == 24)
            {
                for (int i = 0; i < w * h; i++)
                {
                    pixels[i * 4 + 0] = image->pixels[i * 3 + 0];
                    pixels[i * 4 + 1] = image->pixels[i * 3 + 1];
                    pixels[i * 4 + 2] = image->pixels[i * 3 + 2];
                    pixels[i * 4 + 3] = 255;
                }
            }
            else if (image->depth == 16)
            {
                // librw stores 16-bit images as ARGB1555 (1-bit alpha, 5:5:5 RGB).
                // Un-handled before → uninitialised garbage (or a black/opaque texture),
                // which broke e.g. shadow-blob textures stored with 1-bit alpha.
                for (int i = 0; i < w * h; i++)
                {
                    uint16_t px = (uint16_t)(image->pixels[i * 2] | (image->pixels[i * 2 + 1] << 8));
                    uint8_t r5 = (px >> 10) & 0x1F, g5 = (px >> 5) & 0x1F, b5 = px & 0x1F;
                    pixels[i * 4 + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
                    pixels[i * 4 + 1] = (uint8_t)((g5 << 3) | (g5 >> 2));
                    pixels[i * 4 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
                    pixels[i * 4 + 3] = (px & 0x8000) ? 255 : 0;
                }
            }
            else if (image->depth == 8 && image->palette)
            {
                for (int i = 0; i < w * h; i++)
                {
                    uint8_t idx = image->pixels[i];
                    pixels[i * 4 + 0] = image->palette[idx * 4 + 0];
                    pixels[i * 4 + 1] = image->palette[idx * 4 + 1];
                    pixels[i * 4 + 2] = image->palette[idx * 4 + 2];
                    pixels[i * 4 + 3] = image->palette[idx * 4 + 3];
                }
            }
            else
            {
                // Unknown depth — fill opaque magenta so it's visibly wrong, not garbage.
                for (int i = 0; i < w * h; i++)
                {
                    pixels[i * 4 + 0] = 255;
                    pixels[i * 4 + 1] = 0;
                    pixels[i * 4 + 2] = 255;
                    pixels[i * 4 + 3] = 255;
                }
            }

            // Compute mip level count
            int32 numLevels = 1;
            if (raster->format & Raster::MIPMAP)
            {
                int mw = w, mh = h;
                while (mw != 1 || mh != 1)
                {
                    numLevels++;
                    if (mw > 1) mw /= 2;
                    if (mh > 1) mh /= 2;
                }
            }

            // Texture
            WGPUTextureDescriptor texDesc = {};
            texDesc.label = WGPU_STR("raster texture");
            texDesc.size = {(uint32_t)w, (uint32_t)h, 1};
            texDesc.format = WGPUTextureFormat_RGBA8Unorm;
            texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
            texDesc.dimension = WGPUTextureDimension_2D;
            texDesc.mipLevelCount = (uint32_t)numLevels;
            texDesc.sampleCount = 1;
            WGPUTexture texture = wgpuDeviceCreateTexture(dev, &texDesc);

            // Upload base level, then generate + upload the mip chain (2x2 box filter).
            // WGPU has no automatic mipmap generation and the object/skin samplers enable
            // mip filtering, so any unfilled level would sample as garbage at distance.
            {
                uint8_t* src = pixels; // current level (base level owns `pixels`)
                int lw = w, lh = h;
                for (int level = 0; level < numLevels; level++)
                {
                    // Padded upload — bytesPerRow must be 256-aligned (P0-1); the
                    // sub-64px mip levels of every mipmapped texture failed
                    // validation with the tight pitch before.
                    uploadRGBA8Rows(texture, (uint32_t)level, (uint32_t)lw, (uint32_t)lh, src);

                    if (level + 1 >= numLevels) break;
                    int nw = lw > 1 ? lw / 2 : 1;
                    int nh = lh > 1 ? lh / 2 : 1;
                    uint8_t* nxt = new uint8_t[(size_t)nw * nh * 4];
                    for (int y = 0; y < nh; y++)
                    {
                        int y0 = y * 2, y1 = (lh > 1) ? y0 + 1 : y0;
                        for (int x = 0; x < nw; x++)
                        {
                            int x0 = x * 2, x1 = (lw > 1) ? x0 + 1 : x0;
                            for (int c = 0; c < 4; c++)
                                nxt[(y * nw + x) * 4 + c] = (uint8_t)((
                                    src[(y0 * lw + x0) * 4 + c] + src[(y0 * lw + x1) * 4 + c] +
                                    src[(y1 * lw + x0) * 4 + c] + src[(y1 * lw + x1) * 4 + c]) / 4);
                        }
                    }
                    if (src != pixels) delete[] src;
                    src = nxt;
                    lw = nw;
                    lh = nh;
                }
                if (src != pixels) delete[] src;
            }
            delete[] pixels;

            // View (all mip levels)
            WGPUTextureViewDescriptor viewDesc = {};
            viewDesc.format = WGPUTextureFormat_RGBA8Unorm;
            viewDesc.dimension = WGPUTextureViewDimension_2D;
            viewDesc.aspect = WGPUTextureAspect_All;
            viewDesc.baseMipLevel = 0;
            viewDesc.mipLevelCount = (uint32_t)numLevels;
            viewDesc.arrayLayerCount = 1;
            WGPUTextureView view = wgpuTextureCreateView(texture, &viewDesc);

            // BindGroup (uses Im2DText layout: uniform + texture + sampler)
            WGPUBindGroupEntry bgEntries[3] = {};
            bgEntries[0].binding = 0;
            bgEntries[0].buffer = s_im2d.uniformBuf;
            bgEntries[0].offset = 0;
            bgEntries[0].size = 16;
            bgEntries[1].binding = 1;
            bgEntries[1].textureView = view;
            bgEntries[2].binding = 2;
            bgEntries[2].sampler = s_im2dText.sampler;
            WGPUBindGroupDescriptor bgDesc = {};
            bgDesc.label = WGPU_STR("raster bg");
            bgDesc.layout = s_im2dText.bindGroupLayout;
            bgDesc.entryCount = 3;
            bgDesc.entries = bgEntries;
            WGPUBindGroup bindGroup = wgpuDeviceCreateBindGroup(dev, &bgDesc);

            WGPURaster* ext = GETWGPURASTEREXT(raster);
            // Release any texture rasterCreate pre-allocated for this raster (e.g. the
            // d3d->wgpu convert path does Raster::create then setFromImage).
            if (ext->bindGroup) wgpuBindGroupRelease(ext->bindGroup);
            if (ext->view) wgpuTextureViewRelease(ext->view);
            if (ext->texture) wgpuTextureRelease(ext->texture);
            ext->texture = texture;
            ext->view = view;
            ext->bindGroup = bindGroup;
            ext->numLevels = numLevels;
            return 1;
        }

        Image* rasterToImage(Raster* raster)
        {
            if (!raster || wgpuRasterOffset == 0) return nullptr;
            WGPURaster* ext = GETWGPURASTEREXT(raster);
            if (!ext || !ext->texture) return nullptr;

            uint32 w = (uint32_t)raster->width;
            uint32 h = (uint32_t)raster->height;
            uint32 rowPitch = w * 4;
            // Rows must go through a 256-aligned pitch (P0-1).
            uint32 pitch = padRowPitch(rowPitch);
            uint64_t stagingSize = (uint64_t)pitch * h;

            // Create staging buffer
            WGPUBufferDescriptor sbDesc = {};
            sbDesc.label = WGPU_STR("rasterToImage staging");
            sbDesc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
            sbDesc.size = stagingSize;
            WGPUBuffer staging = wgpuDeviceCreateBuffer(wgpuGlobals.device, &sbDesc);

            // Copy texture to staging
            WGPUCommandEncoderDescriptor encDesc = {};
            encDesc.label = WGPU_STR("rasterToImage encoder");
            WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(wgpuGlobals.device, &encDesc);

            WGPUTexelCopyTextureInfo src = {};
            src.texture = ext->texture;
            src.aspect = WGPUTextureAspect_All;
            src.mipLevel = 0;

            WGPUTexelCopyBufferInfo dst = {};
            dst.buffer = staging;
            dst.layout.bytesPerRow = pitch;
            dst.layout.rowsPerImage = h;

            WGPUExtent3D copySize = {w, h, 1};
            wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &copySize);

            WGPUCommandBufferDescriptor cmdDesc = {};
            cmdDesc.label = WGPU_STR("rasterToImage cmd");
            WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, &cmdDesc);
            wgpuQueueSubmit(wgpuGlobals.queue, 1, &cmd);
            wgpuCommandBufferRelease(cmd);
            wgpuCommandEncoderRelease(enc);

            // Map synchronously
            uint8_t* mapped = mapBufferSync(staging, stagingSize);
            if (!mapped)
            {
                wgpuBufferUnmap(staging);
                wgpuBufferRelease(staging);
                return nullptr;
            }

            // Create Image
            // Image depth must match the raster's format (P2-11, gl3 parity):
            // C888 -> 24-bit, C1555 -> ARGB1555, anything else -> 32-bit RGBA.
            uint32 fmt = raster->format & 0xF00;
            int32 depth = 32;
            if (fmt == Raster::C888) depth = 24;
            else if (fmt == Raster::C1555) depth = 16;
            Image* img = Image::create(w, h, depth);
            if (!img)
            {
                wgpuBufferUnmap(staging);
                wgpuBufferRelease(staging);
                return nullptr;
            }

            // Repack the padded rows, converting from RGBA8 (BGRA for camera
            // textures on a BGRA surface — P2-12) into the target depth.
            bool rbSwap = rasterNeedsRBSwap(raster);
            for (uint32 y = 0; y < h; y++)
            {
                const uint8_t* s = mapped + (size_t)y * pitch;
                uint8_t* d = &img->pixels[(size_t)y * w * (depth / 8)];
                if (depth == 32)
                {
                    for (uint32 x = 0; x < w; x++)
                    {
                        d[x * 4 + 0] = s[x * 4 + (rbSwap ? 2 : 0)];
                        d[x * 4 + 1] = s[x * 4 + 1];
                        d[x * 4 + 2] = s[x * 4 + (rbSwap ? 0 : 2)];
                        d[x * 4 + 3] = s[x * 4 + 3];
                    }
                }
                else if (depth == 24)
                {
                    for (uint32 x = 0; x < w; x++)
                    {
                        d[x * 3 + 0] = s[x * 4 + (rbSwap ? 2 : 0)];
                        d[x * 3 + 1] = s[x * 4 + 1];
                        d[x * 3 + 2] = s[x * 4 + (rbSwap ? 0 : 2)];
                    }
                }
                else // 16: ARGB1555, byte order as in rasterFromImage
                {
                    for (uint32 x = 0; x < w; x++)
                    {
                        uint16_t px = (s[x * 4 + 3] >= 128 ? 0x8000u : 0u)
                            | (uint16_t)((s[x * 4 + (rbSwap ? 2 : 0)] >> 3) << 10)
                            | (uint16_t)((s[x * 4 + 1] >> 3) << 5)
                            | (uint16_t)(s[x * 4 + (rbSwap ? 0 : 2)] >> 3);
                        d[x * 2 + 0] = (uint8_t)(px & 0xFF);
                        d[x * 2 + 1] = (uint8_t)(px >> 8);
                    }
                }
            }

            wgpuBufferUnmap(staging);
            wgpuBufferRelease(staging);

            return img;
        }
    } // namespace wgpu
} // namespace rw
#endif
