/*
 * SVGA3=VLKN - SVGA3D Surface & Resource Manager
 */

#ifndef ___SVGA3_SURFACE_H___
#define ___SVGA3_SURFACE_H___

#include "svga3_vlkn.h"
#include "vlkn_backend.h"
#include <unordered_map>
#include <vector>
#include <memory>
#include <mutex>

namespace svga3_vlkn {

class VlknContextManager;

class GuestMemoryManager;

struct SurfaceMipLevel {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    size_t rowPitch;
    size_t slicePitch;
    size_t totalBytes;
};

class VlknSurface {
public:
    VlknSurface(VlknBackend *backend,
                uint32_t sid,
                uint32_t surfaceFlags,
                SVGA3dSurfaceFormat format,
                const SVGA3dSize *sizes,
                uint32_t numSizes,
                uint32_t multisampleCount = 1,
                SVGA3dTextureFilter autogenFilter = SVGA3D_TEX_FILTER_NONE);
    ~VlknSurface();

    Svga3VlknStatus allocate();
    void destroy();

    uint32_t sid() const { return m_sid; }
    uint32_t flags() const { return m_flags; }
    void addFlags(uint32_t flags) { m_flags |= flags; }
    SVGA3dSurfaceFormat svgaFormat() const { return m_svgaFormat; }
    VkFormat vkFormat() const { return m_vkFormat; }
    VkImage image() const { return m_image; }
    VkImageView imageView() const { return m_imageView; }
    VkImageView getRenderTargetView(uint32_t mip, uint32_t face);
    VkImageLayout currentLayout() const { return m_currentLayout; }
    void setLayout(VkImageLayout layout) { m_currentLayout = layout; }

    VkBuffer buffer() const { return m_buffer; }
    VkDeviceMemory bufferMemory() const { return m_bufferMemory; }
    size_t bufferSize() const { return m_bufferSize; }
    Svga3VlknStatus ensureBufferSize(size_t requiredSize);

    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }
    uint32_t depth() const { return m_depth; }
    uint32_t mipLevels() const { return m_mipLevels; }
    uint32_t viewMipLevels() const { return m_viewMipLevels; }
    void ensureViewMipLevels(uint32_t levels);
    uint32_t arrayLayers() const { return m_arrayLayers; }
    uint32_t multisampleCount() const { return m_multisampleCount; }
    SVGA3dTextureFilter autogenFilter() const { return m_autogenFilter; }
    bool isDepthStencil() const { return m_isDepthStencil; }
    bool isCubeMap() const { return m_isCubeMap; }
    bool isActive() const { return m_active; }
    void setActive(bool active) { m_active = active; }

    const SurfaceMipLevel* getMipInfo(uint32_t mipLevel) const;

    Svga3VlknStatus dmaUpload(uint32_t mipLevel,
                              const SVGA3dBox *box,
                              const void *guestData,
                              size_t guestStride,
                              bool isLinear);
    Svga3VlknStatus dmaUpload(uint32_t mipLevel,
                              const SVGA3dBox *box,
                              const void *guestData,
                              size_t guestStride) {
        return dmaUpload(mipLevel, box, guestData, guestStride, false);
    }

    Svga3VlknStatus dmaDownload(uint32_t mipLevel,
                                const SVGA3dBox *box,
                                void *outGuestData,
                                size_t guestStride,
                                bool isLinear);
    Svga3VlknStatus dmaDownload(uint32_t mipLevel,
                                const SVGA3dBox *box,
                                void *outGuestData,
                                size_t guestStride) {
        return dmaDownload(mipLevel, box, outGuestData, guestStride, false);
    }

    Svga3VlknStatus dmaDownloadToStaging(uint32_t mipLevel,
                                        const SVGA3dBox *box,
                                        const void **outMappedData,
                                        size_t *outRowPitch,
                                        std::unique_lock<std::mutex> &outLock);

private:
    VlknBackend *m_backend;
    uint32_t m_sid;
    uint32_t m_flags;
    SVGA3dSurfaceFormat m_svgaFormat;
    VkFormat m_vkFormat;

    uint32_t m_width;
    uint32_t m_height;
    uint32_t m_depth;
    uint32_t m_mipLevels;
    uint32_t m_arrayLayers;
    uint32_t m_multisampleCount;
    SVGA3dTextureFilter m_autogenFilter;
    bool m_isDepthStencil;
    bool m_isCubeMap;
    bool m_active;

    VkImage m_image;
    VkDeviceMemory m_memory;
    VkImageView m_imageView;
    uint32_t m_viewMipLevels;
    VkImageLayout m_currentLayout;

    VkBuffer m_buffer;
    VkDeviceMemory m_bufferMemory;
    size_t m_bufferSize;

    std::vector<SurfaceMipLevel> m_mips;
    std::unordered_map<uint64_t, VkImageView> m_rtViews;
};

class VlknSurfaceManager {
public:
    explicit VlknSurfaceManager(VlknBackend *backend);
    ~VlknSurfaceManager();

    Svga3VlknStatus defineSurface(uint32_t sid,
                                  uint32_t surfaceFlags,
                                  SVGA3dSurfaceFormat format,
                                  const SVGA3dSize *sizes,
                                  uint32_t numSizes);

    Svga3VlknStatus defineSurfaceV2(uint32_t sid,
                                    uint32_t surfaceFlags,
                                    SVGA3dSurfaceFormat format,
                                    uint32_t multisampleCount,
                                    SVGA3dTextureFilter autogenFilter,
                                    const SVGA3dSize *sizes,
                                    uint32_t numSizes);

    Svga3VlknStatus destroySurface(uint32_t sid);
    VlknSurface* getSurface(uint32_t sid);
    bool exists(uint32_t sid) const;
    void clear();

    Svga3VlknStatus copy(uint32_t srcSid,
                         uint32_t dstSid,
                         const SVGA3dCopyBox *boxes,
                         uint32_t numBoxes);

    Svga3VlknStatus stretchBlt(uint32_t srcSid,
                               uint32_t dstSid,
                               const SVGA3dBox &boxSrc,
                               const SVGA3dBox &boxDest,
                               SVGA3dStretchBltMode mode);

    Svga3VlknStatus surfaceDMA(const SVGA3dGuestImage &guest,
                               const SVGA3dSurfaceImageId &host,
                               SVGA3dTransferType transfer,
                               const SVGA3dCopyBox *boxes,
                               uint32_t numBoxes,
                               GuestMemoryManager *guestMem = nullptr,
                               const void *guestBuffer = nullptr,
                               size_t guestBufferSize = 0);

    Svga3VlknStatus generateMipmaps(uint32_t sid, SVGA3dTextureFilter filter);

    Svga3VlknStatus blitSurfaceToScreen(const SVGA3dSurfaceImageId &srcImage,
                                        const SVGASignedRect &srcRect,
                                        uint32_t destScreenId,
                                        const SVGASignedRect &destRect,
                                        const SVGASignedRect *clipRects,
                                        uint32_t numClipRects,
                                        GuestMemoryManager *guestMem = nullptr);

    Svga3VlknStatus present(uint32_t sid,
                            const SVGA3dCopyRect *rects,
                            uint32_t numRects,
                            GuestMemoryManager *guestMem = nullptr);

    Svga3VlknStatus setSurfaceActive(uint32_t sid, bool active);

    void setContextManager(VlknContextManager *ctxMgr) { m_contextMgr = ctxMgr; }

    size_t count() const;

private:
    VlknBackend *m_backend;
    VlknContextManager *m_contextMgr = nullptr;
    std::unordered_map<uint32_t, std::unique_ptr<VlknSurface>> m_surfaces;
    mutable std::mutex m_mutex;
};

/* Format helpers */
size_t svga3_format_bytes_per_pixel(SVGA3dSurfaceFormat format);
bool   svga3_format_has_stencil(SVGA3dSurfaceFormat format);
bool   svga3_format_is_depth_stencil(SVGA3dSurfaceFormat format);
bool   svga3_format_is_compressed(SVGA3dSurfaceFormat format);

bool is_buffer_all_zero(const void *data, uint32_t w, uint32_t h, size_t rowPitch, size_t bpp);
bool is_buffer_all_black_or_zero(const void *data, uint32_t w, uint32_t h, size_t rowPitch, size_t bpp);

} // namespace svga3_vlkn

#endif /* ___SVGA3_SURFACE_H___ */
