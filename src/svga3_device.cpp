/*
 * SVGA3=VLKN - Device Lifecycle and Public C Interface Implementation
 */

#include "svga3_device.h"
#include <cstring>
#include <memory>
#include <cstdarg>
#include <cstdio>

extern "C" __attribute__((weak)) void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* Capability table definition */
struct CapTableEntry {
    uint32_t id;
    uint32_t val;
    bool supported;
};

static const CapTableEntry kDeviceCaps[] = {
    { SVGA3D_DEVCAP_3D, 1, true },
    { SVGA3D_DEVCAP_MAX_LIGHTS, 8, true },
    { SVGA3D_DEVCAP_MAX_TEXTURES, 8, true },
    { SVGA3D_DEVCAP_MAX_CLIP_PLANES, 6, true },
    { SVGA3D_DEVCAP_VERTEX_SHADER_VERSION, 0x300, true },
    { SVGA3D_DEVCAP_VERTEX_SHADER, 1, true },
    { SVGA3D_DEVCAP_FRAGMENT_SHADER_VERSION, 0x300, true },
    { SVGA3D_DEVCAP_FRAGMENT_SHADER, 1, true },
    { SVGA3D_DEVCAP_MAX_RENDER_TARGETS, 4, true },
    { SVGA3D_DEVCAP_S23E8_TEXTURES, 0, false },
    { SVGA3D_DEVCAP_S10E5_TEXTURES, 0, false },
    { SVGA3D_DEVCAP_MAX_FIXED_VERTEXBLEND, 4, true },
    { SVGA3D_DEVCAP_D16_BUFFER_FORMAT, 1, true },
    { SVGA3D_DEVCAP_D24S8_BUFFER_FORMAT, 1, true },
    { SVGA3D_DEVCAP_D24X8_BUFFER_FORMAT, 1, true },
    { SVGA3D_DEVCAP_QUERY_TYPES, 1, true },
    { SVGA3D_DEVCAP_TEXTURE_GRADIENT_SAMPLING, 1, true },
    { SVGA3D_DEVCAP_MAX_POINT_SIZE, 256, true },
    { SVGA3D_DEVCAP_MAX_SHADER_TEXTURES, 0, false },
    { SVGA3D_DEVCAP_MAX_TEXTURE_WIDTH, 8192, true },
    { SVGA3D_DEVCAP_MAX_TEXTURE_HEIGHT, 8192, true },
    { SVGA3D_DEVCAP_MAX_VOLUME_EXTENT, 2048, true },
    { SVGA3D_DEVCAP_MAX_TEXTURE_REPEAT, 8192, true },
    { SVGA3D_DEVCAP_MAX_TEXTURE_ASPECT_RATIO, 8192, true },
    { SVGA3D_DEVCAP_MAX_TEXTURE_ANISOTROPY, 16, true },
    { SVGA3D_DEVCAP_MAX_PRIMITIVE_COUNT, 0x100000, true },
    { SVGA3D_DEVCAP_MAX_VERTEX_INDEX, 0xffffff, true },
    { SVGA3D_DEVCAP_MAX_VERTEX_SHADER_INSTRUCTIONS, 4096, true },
    { SVGA3D_DEVCAP_MAX_FRAGMENT_SHADER_INSTRUCTIONS, 4096, true },
    { SVGA3D_DEVCAP_MAX_VERTEX_SHADER_TEMPS, 32, true },
    { SVGA3D_DEVCAP_MAX_FRAGMENT_SHADER_TEMPS, 32, true },
    { SVGA3D_DEVCAP_TEXTURE_OPS, 0x03ffffff, true },
    { SVGA3D_DEVCAP_SURFACEFMT_X8R8G8B8, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_A8R8G8B8, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_A2R10G10B10, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_X1R5G5B5, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_A1R5G5B5, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_A4R4G4B4, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_R5G6B5, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_LUMINANCE16, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_LUMINANCE8_ALPHA8, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_ALPHA8, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_LUMINANCE8, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_Z_D16, 0x10, true },
    { SVGA3D_DEVCAP_SURFACEFMT_Z_D24S8, 0x10, true },
    { SVGA3D_DEVCAP_SURFACEFMT_Z_D24X8, 0x10, true },
    { SVGA3D_DEVCAP_SURFACEFMT_DXT1, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_DXT2, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_DXT3, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_DXT4, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_DXT5, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_BUMPX8L8V8U8, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_A2W10V10U10, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_BUMPU8V8, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_Q8W8V8U8, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_CxV8U8, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_R_S10E5, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_R_S23E8, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_RG_S10E5, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_RG_S23E8, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_ARGB_S10E5, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_ARGB_S23E8, 0x4f, true },
    { SVGA3D_DEVCAP_MAX_VERTEX_SHADER_TEXTURES, 4, true },
    { SVGA3D_DEVCAP_MAX_SIMULTANEOUS_RENDER_TARGETS, 4, true },
    { SVGA3D_DEVCAP_SURFACEFMT_V16U16, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_G16R16, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_A16B16G16R16, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_UYVY, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_YUY2, 0x4f, true },
    { SVGA3D_DEVCAP_MULTISAMPLE_NONMASKABLESAMPLES, 0x0, true },
    { SVGA3D_DEVCAP_MULTISAMPLE_MASKABLESAMPLES, 0x14, true },
    { SVGA3D_DEVCAP_ALPHATOCOVERAGE, 1, true },
    { SVGA3D_DEVCAP_SUPERSAMPLE, 1, true },
    { SVGA3D_DEVCAP_AUTOGENMIPMAPS, 1, true },
    { SVGA3D_DEVCAP_SURFACEFMT_NV12, 0x4f, true },
    { SVGA3D_DEVCAP_SURFACEFMT_AYUV, 0x4f, true },
    { SVGA3D_DEVCAP_MAX_CONTEXT_IDS, 64, true },
    { SVGA3D_DEVCAP_MAX_SURFACE_IDS, 1024, true },
    { SVGA3D_DEVCAP_SURFACEFMT_Z_DF16, 0x10, true },
    { SVGA3D_DEVCAP_SURFACEFMT_Z_DF24, 0x10, true },
    { SVGA3D_DEVCAP_SURFACEFMT_Z_D24S8_INT, 0x10, true },
    { SVGA3D_DEVCAP_SURFACEFMT_BC4_UNORM, 0, false },
    { SVGA3D_DEVCAP_SURFACEFMT_BC5_UNORM, 0, false }
};

extern "C" {

Svga3VlknDevice *svga3_vlkn_device_create(const Svga3VlknConfig *config)
{
    std::unique_ptr<Svga3VlknDevice> dev = std::make_unique<Svga3VlknDevice>();
    dev->backend = std::make_unique<svga3_vlkn::VlknBackend>();

    Svga3VlknConfig cfg;
    if (config) {
        cfg = *config;
    } else {
        memset(&cfg, 0, sizeof(cfg));
        cfg.appName = "SVGA3=VLKN Application";
        cfg.apiVersion = VK_API_VERSION_1_0;
        cfg.stagingBufferSize = 64 * 1024 * 1024;
    }

    dev->dxContextLimit = cfg.maxContexts ? cfg.maxContexts : 256;
    Svga3VlknStatus st = dev->backend->init(&cfg);
    if (st != SVGA3_VLKN_SUCCESS) {
        return nullptr;
    }

    dev->surfaceMgr = std::make_unique<svga3_vlkn::VlknSurfaceManager>(dev->backend.get());
    dev->contextMgr = std::make_unique<svga3_vlkn::VlknContextManager>(dev->backend.get(), dev->surfaceMgr.get());
    dev->guestMem   = std::make_unique<svga3_vlkn::GuestMemoryManager>();

    dev->surfaceMgr->setContextManager(dev->contextMgr.get());

    svga3_vlkn::VlknContextManager *ctxMgr = dev->contextMgr.get();
    dev->backend->setPreFlushHook([ctxMgr]() {
        if (ctxMgr) {
            ctxMgr->endAllRenderPasses();
        }
    });

    return dev.release();
}

void svga3_vlkn_device_destroy(Svga3VlknDevice *dev)
{
    if (!dev) return;
    {
        std::lock_guard<std::mutex> lock(dev->mutex);
        if (dev->backend) {
            dev->backend->waitIdle();
        }
        if (dev->surfaceMgr) {
            dev->surfaceMgr->setContextManager(nullptr);
        }
        dev->guestMem.reset();
        dev->contextMgr.reset();
        dev->surfaceMgr.reset();
        dev->backend.reset();
    }
    delete dev;
}

Svga3VlknStatus svga3_vlkn_device_reset(Svga3VlknDevice *dev)
{
    if (!dev) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    if (dev->backend) {
        Svga3VlknStatus waitStatus = dev->backend->waitIdle();
        if (waitStatus != SVGA3_VLKN_SUCCESS) return waitStatus;
    }
    dev->dxContexts.clear();
    if (dev->contextMgr) {
        dev->contextMgr->clear();
    }
    if (dev->surfaceMgr) {
        dev->surfaceMgr->clear();
    }
    if (dev->guestMem) {
        dev->guestMem->clear();
    }
    memset(&dev->stats, 0, sizeof(dev->stats));
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus svga3_vlkn_device_wait_idle(Svga3VlknDevice *dev)
{
    if (!dev || !dev->backend) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    return dev->backend->waitIdle();
}

void svga3_vlkn_get_stats(Svga3VlknDevice *dev, Svga3VlknStats *outStats)
{
    if (!dev || !outStats) return;
    std::lock_guard<std::mutex> lock(dev->mutex);
    *outStats = dev->stats;
}

uint32_t svga3_vlkn_query_cap(Svga3VlknDevice *dev, uint32_t capIndex, uint32_t *outCapValue)
{
    (void)dev;
    if (!outCapValue) return 0;

    for (size_t i = 0; i < sizeof(kDeviceCaps) / sizeof(kDeviceCaps[0]); ++i) {
        if (kDeviceCaps[i].id == capIndex) {
            if (!kDeviceCaps[i].supported) {
                return 0;
            }
            uint32_t val = kDeviceCaps[i].val;
            /* Mesa requires SVGA3DFORMAT_OP_ZSTENCIL before it exposes a
             * depth visual. 0x10 (SAME_FORMAT_RENDERTARGET) is not enough.
             * (Mirrors advertised_devcap in qemu_svga3d_preload.cpp.) */
            switch (capIndex) {
            case SVGA3D_DEVCAP_SURFACEFMT_Z_D16:
            case SVGA3D_DEVCAP_SURFACEFMT_Z_D24S8:
            case SVGA3D_DEVCAP_SURFACEFMT_Z_D24X8:
            case SVGA3D_DEVCAP_SURFACEFMT_Z_DF16:
            case SVGA3D_DEVCAP_SURFACEFMT_Z_DF24:
            case SVGA3D_DEVCAP_SURFACEFMT_Z_D24S8_INT:
                val = SVGA3DFORMAT_OP_ZSTENCIL
                    | SVGA3DFORMAT_OP_ZSTENCIL_WITH_ARBITRARY_COLOR_DEPTH
                    | SVGA3DFORMAT_OP_TEXTURE;
                break;
            default:
                break;
            }
            *outCapValue = val;
            return 1;
        }
    }
    return 0;
}

Svga3VlknStatus svga3_vlkn_context_create(Svga3VlknDevice *dev, uint32_t cid)
{
    if (!dev || !dev->contextMgr) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    return dev->contextMgr->createContext(cid);
}

Svga3VlknStatus svga3_vlkn_context_destroy(Svga3VlknDevice *dev, uint32_t cid)
{
    if (!dev || !dev->contextMgr) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    return dev->contextMgr->destroyContext(cid);
}

bool svga3_vlkn_context_exists(Svga3VlknDevice *dev, uint32_t cid)
{
    if (!dev || !dev->contextMgr) return false;
    std::lock_guard<std::mutex> lock(dev->mutex);
    return dev->contextMgr->exists(cid);
}

Svga3VlknStatus svga3_vlkn_surface_define(Svga3VlknDevice *dev,
                                          uint32_t sid,
                                          uint32_t surfaceFlags,
                                          SVGA3dSurfaceFormat format,
                                          const SVGA3dSize *sizes,
                                          uint32_t numSizes)
{
    if (!dev || !dev->surfaceMgr) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    if (dev->contextMgr) dev->contextMgr->endAllRenderPasses();
    Svga3VlknStatus st = dev->surfaceMgr->defineSurface(sid, surfaceFlags, format, sizes, numSizes);
    if (st == SVGA3_VLKN_SUCCESS) {
        dev->stats.surfacesCreated++;
    }
    return st;
}

Svga3VlknStatus svga3_vlkn_surface_destroy(Svga3VlknDevice *dev, uint32_t sid)
{
    if (!dev || !dev->surfaceMgr) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    if (dev->contextMgr) dev->contextMgr->endAllRenderPasses();
    Svga3VlknStatus st = dev->surfaceMgr->destroySurface(sid);
    if (st == SVGA3_VLKN_SUCCESS) {
        dev->stats.surfacesDestroyed++;
    }
    return st;
}

bool svga3_vlkn_surface_exists(Svga3VlknDevice *dev, uint32_t sid)
{
    if (!dev || !dev->surfaceMgr) return false;
    std::lock_guard<std::mutex> lock(dev->mutex);
    return dev->surfaceMgr->exists(sid);
}

Svga3VlknStatus svga3_vlkn_surface_dma_upload(Svga3VlknDevice *dev,
                                              uint32_t sid,
                                              uint32_t mipLevel,
                                              const SVGA3dBox *box,
                                              const void *guestData,
                                              size_t guestStride)
{
    if (!dev || !dev->surfaceMgr) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    if (dev->contextMgr) dev->contextMgr->endAllRenderPasses();
    svga3_vlkn::VlknSurface *surf = dev->surfaceMgr->getSurface(sid);
    if (!surf) return SVGA3_VLKN_ERROR_NOT_FOUND;

    Svga3VlknStatus st = surf->dmaUpload(mipLevel, box, guestData, guestStride);
    if (st == SVGA3_VLKN_SUCCESS) {
        uint32_t w = box ? box->w : surf->width();
        uint32_t h = box ? box->h : surf->height();
        uint32_t d = box ? box->d : surf->depth();
        size_t bpp = svga3_vlkn::svga3_format_bytes_per_pixel(surf->svgaFormat());
        dev->stats.dmaBytesTransferred += (w * h * d * (bpp ? bpp : 4));
    }
    return st;
}

Svga3VlknStatus svga3_vlkn_surface_dma_download(Svga3VlknDevice *dev,
                                                uint32_t sid,
                                                uint32_t mipLevel,
                                                const SVGA3dBox *box,
                                                void *outGuestData,
                                                size_t guestStride)
{
    if (!dev || !dev->surfaceMgr) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    if (dev->contextMgr) dev->contextMgr->endAllRenderPasses();
    svga3_vlkn::VlknSurface *surf = dev->surfaceMgr->getSurface(sid);
    if (!surf) return SVGA3_VLKN_ERROR_NOT_FOUND;

    Svga3VlknStatus st = surf->dmaDownload(mipLevel, box, outGuestData, guestStride);
    if (st == SVGA3_VLKN_SUCCESS) {
        uint32_t w = box ? box->w : surf->width();
        uint32_t h = box ? box->h : surf->height();
        uint32_t d = box ? box->d : surf->depth();
        size_t bpp = svga3_vlkn::svga3_format_bytes_per_pixel(surf->svgaFormat());
        dev->stats.dmaBytesTransferred += (w * h * d * (bpp ? bpp : 4));
    }
    return st;
}

Svga3VlknStatus svga3_vlkn_surface_define_v2(Svga3VlknDevice *dev,
                                             uint32_t sid,
                                             uint32_t surfaceFlags,
                                             SVGA3dSurfaceFormat format,
                                             uint32_t multisampleCount,
                                             SVGA3dTextureFilter autogenFilter,
                                             const SVGA3dSize *sizes,
                                             uint32_t numSizes)
{
    if (!dev || !dev->surfaceMgr) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    Svga3VlknStatus st = dev->surfaceMgr->defineSurfaceV2(
        sid, surfaceFlags, format, multisampleCount, autogenFilter, sizes, numSizes
    );
    if (st == SVGA3_VLKN_SUCCESS) {
        dev->stats.surfacesCreated++;
    }
    return st;
}

Svga3VlknStatus svga3_vlkn_surface_set_active(Svga3VlknDevice *dev, uint32_t sid, bool active)
{
    if (!dev || !dev->surfaceMgr) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    return dev->surfaceMgr->setSurfaceActive(sid, active);
}

Svga3VlknStatus svga3_vlkn_surface_copy(Svga3VlknDevice *dev,
                                        uint32_t srcSid,
                                        uint32_t dstSid,
                                        const SVGA3dCopyBox *boxes,
                                        uint32_t numBoxes)
{
    if (!dev || !dev->surfaceMgr) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    if (dev->contextMgr) dev->contextMgr->endAllRenderPasses();
    return dev->surfaceMgr->copy(srcSid, dstSid, boxes, numBoxes);
}

Svga3VlknStatus svga3_vlkn_surface_stretch_blt(Svga3VlknDevice *dev,
                                               uint32_t srcSid,
                                               uint32_t dstSid,
                                               const SVGA3dBox *boxSrc,
                                               const SVGA3dBox *boxDest,
                                               SVGA3dStretchBltMode mode)
{
    if (!dev || !dev->surfaceMgr || !boxSrc || !boxDest) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    if (dev->contextMgr) dev->contextMgr->endAllRenderPasses();
    return dev->surfaceMgr->stretchBlt(srcSid, dstSid, *boxSrc, *boxDest, mode);
}

Svga3VlknStatus svga3_vlkn_surface_generate_mipmaps(Svga3VlknDevice *dev,
                                                     uint32_t sid,
                                                     SVGA3dTextureFilter filter)
{
    if (!dev || !dev->surfaceMgr) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    return dev->surfaceMgr->generateMipmaps(sid, filter);
}

Svga3VlknStatus svga3_vlkn_surface_blit_to_screen(Svga3VlknDevice *dev,
                                                   const SVGA3dSurfaceImageId *srcImage,
                                                   const SVGASignedRect *srcRect,
                                                   uint32_t destScreenId,
                                                   const SVGASignedRect *destRect,
                                                   const SVGASignedRect *clipRects,
                                                   uint32_t numClipRects)
{
    if (!dev || !dev->surfaceMgr || !srcImage || !srcRect || !destRect) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    if (dev->contextMgr) dev->contextMgr->endAllRenderPasses();
    return dev->surfaceMgr->blitSurfaceToScreen(*srcImage, *srcRect, destScreenId, *destRect, clipRects, numClipRects, dev->guestMem.get());
}

Svga3VlknStatus svga3_vlkn_surface_present(Svga3VlknDevice *dev,
                                            uint32_t sid,
                                            const SVGA3dCopyRect *rects,
                                            uint32_t numRects)
{
    if (!dev || !dev->surfaceMgr) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    if (dev->contextMgr) dev->contextMgr->endAllRenderPasses();
    return dev->surfaceMgr->present(sid, rects, numRects, dev->guestMem.get());
}

Svga3VlknStatus svga3_vlkn_context_set_render_state(Svga3VlknDevice *dev,
                                                    uint32_t cid,
                                                    SVGA3dRenderStateName state,
                                                    uint32_t value)
{
    if (!dev || !dev->contextMgr) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    svga3_vlkn::VlknContext *ctx = dev->contextMgr->getContext(cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;
    return ctx->setRenderState(state, value);
}

Svga3VlknStatus svga3_vlkn_context_get_render_state(Svga3VlknDevice *dev,
                                                    uint32_t cid,
                                                    SVGA3dRenderStateName state,
                                                    uint32_t *outValue)
{
    if (!dev || !dev->contextMgr || !outValue) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    svga3_vlkn::VlknContext *ctx = dev->contextMgr->getContext(cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;
    return ctx->getRenderState(state, outValue);
}

Svga3VlknStatus svga3_vlkn_context_set_render_target(Svga3VlknDevice *dev,
                                                     uint32_t cid,
                                                     SVGA3dRenderTargetType type,
                                                     uint32_t sid,
                                                     uint32_t face,
                                                     uint32_t mipmap)
{
    if (!dev || !dev->contextMgr) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    svga3_vlkn::VlknContext *ctx = dev->contextMgr->getContext(cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;
    return ctx->setRenderTarget(type, sid, face, mipmap);
}

Svga3VlknStatus svga3_vlkn_context_set_viewport(Svga3VlknDevice *dev,
                                                uint32_t cid,
                                                const SVGA3dRect *rect)
{
    if (!dev || !dev->contextMgr) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    svga3_vlkn::VlknContext *ctx = dev->contextMgr->getContext(cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;
    return ctx->setViewport(rect);
}

Svga3VlknStatus svga3_vlkn_context_set_scissor_rect(Svga3VlknDevice *dev,
                                                    uint32_t cid,
                                                    const SVGA3dRect *rect)
{
    if (!dev || !dev->contextMgr) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    svga3_vlkn::VlknContext *ctx = dev->contextMgr->getContext(cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;
    return ctx->setScissorRect(rect);
}

Svga3VlknStatus svga3_vlkn_context_set_transform(Svga3VlknDevice *dev,
                                                 uint32_t cid,
                                                 SVGA3dTransformType type,
                                                 const float matrix[16])
{
    if (!dev || !dev->contextMgr || !matrix) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    svga3_vlkn::VlknContext *ctx = dev->contextMgr->getContext(cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;
    return ctx->setTransform(type, matrix);
}

Svga3VlknStatus svga3_vlkn_context_get_transform(Svga3VlknDevice *dev,
                                                 uint32_t cid,
                                                 SVGA3dTransformType type,
                                                 float outMatrix[16])
{
    if (!dev || !dev->contextMgr || !outMatrix) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    svga3_vlkn::VlknContext *ctx = dev->contextMgr->getContext(cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;
    return ctx->getTransform(type, outMatrix);
}

Svga3VlknStatus svga3_vlkn_context_set_zrange(Svga3VlknDevice *dev,
                                              uint32_t cid,
                                              const SVGA3dZRange *zRange)
{
    if (!dev || !dev->contextMgr || !zRange) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    svga3_vlkn::VlknContext *ctx = dev->contextMgr->getContext(cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;
    return ctx->setZRange(zRange);
}

Svga3VlknStatus svga3_vlkn_context_get_zrange(Svga3VlknDevice *dev,
                                              uint32_t cid,
                                              SVGA3dZRange *outZRange)
{
    if (!dev || !dev->contextMgr || !outZRange) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    svga3_vlkn::VlknContext *ctx = dev->contextMgr->getContext(cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;
    *outZRange = ctx->getZRange();
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus svga3_vlkn_context_set_texture_stage_state(Svga3VlknDevice *dev,
                                                           uint32_t cid,
                                                           uint32_t stage,
                                                           SVGA3dTextureStateName name,
                                                           uint32_t value)
{
    if (!dev || !dev->contextMgr) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    svga3_vlkn::VlknContext *ctx = dev->contextMgr->getContext(cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;
    return ctx->setTextureStageState(stage, name, value);
}

Svga3VlknStatus svga3_vlkn_context_set_texture(Svga3VlknDevice *dev,
                                               uint32_t cid,
                                               uint32_t stage,
                                               uint32_t sid)
{
    if (!dev || !dev->contextMgr) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    svga3_vlkn::VlknContext *ctx = dev->contextMgr->getContext(cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;
    return ctx->setTexture(stage, sid);
}

Svga3VlknStatus svga3_vlkn_context_set_clip_plane(Svga3VlknDevice *dev,
                                                  uint32_t cid,
                                                  uint32_t index,
                                                  const float plane[4])
{
    if (!dev || !dev->contextMgr || !plane) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    svga3_vlkn::VlknContext *ctx = dev->contextMgr->getContext(cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;
    return ctx->setClipPlane(index, plane);
}

Svga3VlknStatus svga3_vlkn_context_set_material(Svga3VlknDevice *dev,
                                                uint32_t cid,
                                                SVGA3dFace face,
                                                const SVGA3dMaterial *material)
{
    if (!dev || !dev->contextMgr || !material) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    svga3_vlkn::VlknContext *ctx = dev->contextMgr->getContext(cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;
    return ctx->setMaterial(face, material);
}

Svga3VlknStatus svga3_vlkn_context_set_light_data(Svga3VlknDevice *dev,
                                                  uint32_t cid,
                                                  uint32_t index,
                                                  const SVGA3dLightData *data)
{
    if (!dev || !dev->contextMgr || !data) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    svga3_vlkn::VlknContext *ctx = dev->contextMgr->getContext(cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;
    return ctx->setLightData(index, data);
}

Svga3VlknStatus svga3_vlkn_context_set_light_enabled(Svga3VlknDevice *dev,
                                                     uint32_t cid,
                                                     uint32_t index,
                                                     uint32_t enabled)
{
    if (!dev || !dev->contextMgr) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    svga3_vlkn::VlknContext *ctx = dev->contextMgr->getContext(cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;
    return ctx->setLightEnabled(index, enabled);
}

Svga3VlknStatus svga3_vlkn_context_define_shader(Svga3VlknDevice *dev,
                                                  uint32_t cid,
                                                  uint32_t shid,
                                                  SVGA3dShaderType type,
                                                  const uint32_t *bytecode,
                                                  uint32_t numDwords)
{
    if (!dev || !dev->contextMgr || !bytecode || numDwords == 0) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    svga3_vlkn::VlknContext *ctx = dev->contextMgr->getContext(cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;
    return ctx->defineShader(shid, type, bytecode, numDwords);
}

Svga3VlknStatus svga3_vlkn_context_destroy_shader(Svga3VlknDevice *dev,
                                                   uint32_t cid,
                                                   uint32_t shid,
                                                   SVGA3dShaderType type)
{
    if (!dev || !dev->contextMgr) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    svga3_vlkn::VlknContext *ctx = dev->contextMgr->getContext(cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;
    return ctx->destroyShader(shid, type);
}

Svga3VlknStatus svga3_vlkn_context_set_shader(Svga3VlknDevice *dev,
                                               uint32_t cid,
                                               SVGA3dShaderType type,
                                               uint32_t shid)
{
    if (!dev || !dev->contextMgr) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    svga3_vlkn::VlknContext *ctx = dev->contextMgr->getContext(cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;
    return ctx->setShader(type, shid);
}

Svga3VlknStatus svga3_vlkn_context_set_shader_const(Svga3VlknDevice *dev,
                                                     uint32_t cid,
                                                     uint32_t reg,
                                                     SVGA3dShaderType type,
                                                     SVGA3dShaderConstType ctype,
                                                     const uint32_t values[4])
{
    if (!dev || !dev->contextMgr || !values) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    svga3_vlkn::VlknContext *ctx = dev->contextMgr->getContext(cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;
    return ctx->setShaderConst(reg, type, ctype, values);
}

Svga3VlknStatus svga3_vlkn_context_get_shader_const(Svga3VlknDevice *dev,
                                                     uint32_t cid,
                                                     uint32_t reg,
                                                     SVGA3dShaderType type,
                                                     SVGA3dShaderConstType ctype,
                                                     uint32_t outValues[4])
{
    if (!dev || !dev->contextMgr || !outValues) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    svga3_vlkn::VlknContext *ctx = dev->contextMgr->getContext(cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;
    return ctx->getShaderConst(reg, type, ctype, outValues);
}

Svga3VlknStatus svga3_vlkn_context_clear(Svga3VlknDevice *dev,
                                         uint32_t cid,
                                         SVGA3dClearFlag flags,
                                         uint32_t colorRGBA,
                                         float depth,
                                         uint32_t stencil,
                                         const SVGA3dRect *rects,
                                         uint32_t numRects)
{
    if (!dev || !dev->contextMgr) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    dev->contextMgr->endAllRenderPassesExcept(cid);
    svga3_vlkn::VlknContext *ctx = dev->contextMgr->getContext(cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;
    Svga3VlknStatus st = ctx->clear(flags, colorRGBA, depth, stencil, rects, numRects);
    if (st == SVGA3_VLKN_SUCCESS) {
        dev->stats.renderPassesExecuted++;
    }
    return st;
}

Svga3VlknStatus svga3_vlkn_context_draw(Svga3VlknDevice *dev,
                                        uint32_t cid,
                                        SVGA3dPrimitiveType primitiveType,
                                        const SVGA3dVertexDecl *decls,
                                        uint32_t numDecls,
                                        const SVGA3dPrimitiveRange *ranges,
                                        uint32_t numRanges)
{
    if (!dev || !dev->contextMgr) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    dev->contextMgr->endAllRenderPassesExcept(cid);
    svga3_vlkn::VlknContext *ctx = dev->contextMgr->getContext(cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;
    Svga3VlknStatus st = ctx->draw(primitiveType, decls, numDecls, ranges, numRanges);
    if (st == SVGA3_VLKN_SUCCESS) {
        dev->stats.drawCallsSubmitted += numRanges;
        for (uint32_t i = 0; i < numRanges; ++i) {
            dev->stats.primitivesRendered += ranges[i].primitiveCount;
            dev->stats.verticesRendered += ranges[i].primitiveCount * 3;
        }
    }
    return st;
}

Svga3VlknStatus svga3_vlkn_context_begin_query(Svga3VlknDevice *dev,
                                                uint32_t cid,
                                                SVGA3dQueryType type)
{
    if (!dev || !dev->contextMgr) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    svga3_vlkn::VlknContext *ctx = dev->contextMgr->getContext(cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;
    return ctx->beginQuery(type);
}

Svga3VlknStatus svga3_vlkn_context_end_query(Svga3VlknDevice *dev,
                                              uint32_t cid,
                                              SVGA3dQueryType type)
{
    if (!dev || !dev->contextMgr) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    svga3_vlkn::VlknContext *ctx = dev->contextMgr->getContext(cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;
    return ctx->endQuery(type);
}

Svga3VlknStatus svga3_vlkn_context_wait_for_query(Svga3VlknDevice *dev,
                                                    uint32_t cid,
                                                    SVGA3dQueryType type,
                                                    uint32_t *outResult)
{
    if (!dev || !dev->contextMgr) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    svga3_vlkn::VlknContext *ctx = dev->contextMgr->getContext(cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;
    return ctx->waitForQuery(type, outResult);
}

/*
 * Guest Memory Management C API
 */
Svga3VlknStatus svga3_vlkn_device_set_guest_memory_callbacks(
    Svga3VlknDevice *dev,
    void *opaque,
    Svga3GpaToHvaFn gpaToHva,
    Svga3DmaReadFn dmaRead,
    Svga3DmaWriteFn dmaWrite)
{
    if (!dev || !dev->guestMem) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    dev->guestMem->setCallbacks(opaque, gpaToHva, dmaRead, dmaWrite);
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus svga3_vlkn_device_set_display_callback(
    Svga3VlknDevice *dev,
    void *opaque,
    Svga3DisplayUpdateFn displayUpdate)
{
    if (!dev || !dev->guestMem) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    dev->guestMem->setDisplayCallback(opaque, displayUpdate);
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus svga3_vlkn_device_map_guest_ram(
    Svga3VlknDevice *dev,
    uint64_t gpaBase,
    void *hvaBase,
    size_t size)
{
    if (!dev || !dev->guestMem) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    return dev->guestMem->registerRamBlock(gpaBase, hvaBase, size);
}

Svga3VlknStatus svga3_vlkn_device_unmap_guest_ram(
    Svga3VlknDevice *dev,
    uint64_t gpaBase)
{
    if (!dev || !dev->guestMem) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    return dev->guestMem->unregisterRamBlock(gpaBase);
}

Svga3VlknStatus svga3_vlkn_device_set_framebuffer(
    Svga3VlknDevice *dev,
    void *fbHva,
    uint64_t fbGpa,
    size_t fbSize,
    uint32_t width,
    uint32_t height,
    uint32_t pitch,
    uint32_t bpp)
{
    if (!dev || !dev->guestMem) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    return dev->guestMem->setFramebuffer(fbHva, fbGpa, fbSize, width, height, pitch, bpp);
}

Svga3VlknStatus svga3_vlkn_device_set_scanout_offset(
    Svga3VlknDevice *dev,
    uint32_t scanoutOffset)
{
    if (!dev || !dev->guestMem) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    return dev->guestMem->setScanoutOffset(scanoutOffset);
}

Svga3VlknStatus svga3_vlkn_gmr_define(
    Svga3VlknDevice *dev,
    uint32_t gmrId,
    uint32_t numPages)
{
    if (!dev || !dev->guestMem) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    return dev->guestMem->defineGMR2(gmrId, numPages);
}

Svga3VlknStatus svga3_vlkn_gmr_remap(
    Svga3VlknDevice *dev,
    uint32_t gmrId,
    uint32_t flags,
    uint32_t offsetPages,
    uint32_t numPages,
    const void *descriptors,
    size_t descriptorBytes)
{
    if (!dev || !dev->guestMem) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    size_t consumed = 0;
    return dev->guestMem->remapGMR2(gmrId, flags, offsetPages, numPages, descriptors, descriptorBytes, &consumed);
}

Svga3VlknStatus svga3_vlkn_gmr_register_legacy(
    Svga3VlknDevice *dev,
    uint32_t gmrId,
    uint32_t descriptorPPN)
{
    if (!dev || !dev->guestMem) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    return dev->guestMem->registerLegacyGMR(gmrId, descriptorPPN);
}

Svga3VlknStatus svga3_vlkn_gmr_destroy(
    Svga3VlknDevice *dev,
    uint32_t gmrId)
{
    if (!dev || !dev->guestMem) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(dev->mutex);
    return dev->guestMem->destroyGMR(gmrId);
}

Svga3VlknStatus svga3_vlkn_guest_read(
    Svga3VlknDevice *dev,
    SVGAGuestPtr ptr,
    void *dstHost,
    size_t size)
{
    if (!dev || !dev->guestMem) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    return dev->guestMem->readGuest(ptr, dstHost, size);
}

Svga3VlknStatus svga3_vlkn_guest_write(
    Svga3VlknDevice *dev,
    SVGAGuestPtr ptr,
    const void *srcHost,
    size_t size)
{
    if (!dev || !dev->guestMem) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    return dev->guestMem->writeGuest(ptr, srcHost, size);
}

} // extern "C"
