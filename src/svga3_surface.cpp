/*
 * SVGA3=VLKN - SVGA3D Surface & Resource Manager Implementation
 */

#include "svga3_surface.h"
#include "svga3_context.h"
#include "svga3_guest_mem.h"
#include "../data/svga3d_reference.h"
#include <cstring>
#include <algorithm>

extern "C" void log_msg(const char *fmt, ...);

namespace svga3_vlkn {

size_t svga3_format_bytes_per_pixel(SVGA3dSurfaceFormat format) {
    switch (format) {
        case SVGA3D_BUFFER:
        case SVGA3D_LUMINANCE8:
        case SVGA3D_ALPHA8:
            return 1;

        case SVGA3D_R5G6B5:
        case SVGA3D_X1R5G5B5:
        case SVGA3D_A1R5G5B5:
        case SVGA3D_A4R4G4B4:
        case SVGA3D_Z_D16:
        case SVGA3D_Z_D15S1:
        case SVGA3D_Z_DF16:
        case SVGA3D_LUMINANCE16:
        case SVGA3D_LUMINANCE8_ALPHA8:
        case SVGA3D_BUMPU8V8:
        case SVGA3D_V8U8:
        case SVGA3D_CxV8U8:
        case SVGA3D_R_S10E5:
            return 2;

        case SVGA3D_X8R8G8B8:
        case SVGA3D_A8R8G8B8:
        case SVGA3D_Z_D32:
        case SVGA3D_Z_D24S8:
        case SVGA3D_Z_D24X8:
        case SVGA3D_Z_DF24:
        case SVGA3D_Z_D24S8_INT:
        case SVGA3D_A2R10G10B10:
        case SVGA3D_A2W10V10U10:
        case SVGA3D_BUMPX8L8V8U8:
        case SVGA3D_X8L8V8U8:
        case SVGA3D_Q8W8V8U8:
        case SVGA3D_R8G8B8A8_UNORM:
        case SVGA3D_R_S23E8:
        case SVGA3D_RG_S10E5:
        case SVGA3D_V16U16:
        case SVGA3D_G16R16:
            return 4;

        case SVGA3D_ARGB_S10E5:
        case SVGA3D_RG_S23E8:
        case SVGA3D_A16B16G16R16:
            return 8;

        case SVGA3D_ARGB_S23E8:
            return 16;

        case SVGA3D_DXT1:
        case SVGA3D_BC4_UNORM:
            return 8; /* 8 bytes per 4x4 block */

        case SVGA3D_DXT2:
        case SVGA3D_DXT3:
        case SVGA3D_DXT4:
        case SVGA3D_DXT5:
        case SVGA3D_BC5_UNORM:
            return 16; /* 16 bytes per 4x4 block */

        default:
            return 4;
    }
}

bool svga3_format_has_stencil(SVGA3dSurfaceFormat format) {
    switch (format) {
        case SVGA3D_Z_D24S8:
        case SVGA3D_Z_D15S1:
        case SVGA3D_Z_D24S8_INT:
            return true;
        default:
            return false;
    }
}

bool svga3_format_is_depth_stencil(SVGA3dSurfaceFormat format) {
    switch (format) {
        case SVGA3D_Z_D32:
        case SVGA3D_Z_D16:
        case SVGA3D_Z_D24S8:
        case SVGA3D_Z_D15S1:
        case SVGA3D_Z_D24X8:
        case SVGA3D_Z_DF16:
        case SVGA3D_Z_DF24:
        case SVGA3D_Z_D24S8_INT:
            return true;
        default:
            return false;
    }
}

bool svga3_format_is_compressed(SVGA3dSurfaceFormat format) {
    switch (format) {
        case SVGA3D_DXT1:
        case SVGA3D_DXT2:
        case SVGA3D_DXT3:
        case SVGA3D_DXT4:
        case SVGA3D_DXT5:
        case SVGA3D_BC4_UNORM:
        case SVGA3D_BC5_UNORM:
            return true;
        default:
            return false;
    }
}

VlknSurface::VlknSurface(VlknBackend *backend,
                         uint32_t sid,
                         uint32_t surfaceFlags,
                         SVGA3dSurfaceFormat format,
                         const SVGA3dSize *sizes,
                         uint32_t numSizes,
                         uint32_t multisampleCount,
                         SVGA3dTextureFilter autogenFilter)
    : m_backend(backend)
    , m_sid(sid)
    , m_flags(surfaceFlags)
    , m_svgaFormat(format)
    , m_vkFormat(VK_FORMAT_UNDEFINED)
    , m_width(0)
    , m_height(0)
    , m_depth(1)
    , m_mipLevels(1)
    , m_arrayLayers(1)
    , m_multisampleCount(multisampleCount ? multisampleCount : 1)
    , m_autogenFilter(autogenFilter)
    , m_isDepthStencil(false)
    , m_isCubeMap(false)
    , m_active(true)
    , m_image(VK_NULL_HANDLE)
    , m_memory(VK_NULL_HANDLE)
    , m_imageView(VK_NULL_HANDLE)
    , m_viewMipLevels(1)
    , m_currentLayout(VK_IMAGE_LAYOUT_UNDEFINED)
    , m_buffer(VK_NULL_HANDLE)
    , m_bufferMemory(VK_NULL_HANDLE)
    , m_bufferSize(0)
    , m_readbackValid(false)
    , m_readbackW(0)
    , m_readbackH(0)
    , m_readbackPitch(0)
{
    m_vkFormat = (VkFormat)svga3d_to_vk_format((uint32_t)format);
    if (m_vkFormat == VK_FORMAT_UNDEFINED) {
        m_vkFormat = VK_FORMAT_B8G8R8A8_UNORM; /* Fallback for unrecognized formats */
    }

    m_isDepthStencil = svga3_format_is_depth_stencil(format);
    m_isCubeMap = (surfaceFlags & SVGA3D_SURFACE_CUBEMAP) != 0;

    if (numSizes > 0 && sizes) {
        /* Cap guest-controlled dimensions: uncapped 32-bit dimensions
         * overflowed the pitch arithmetic below (wrapping to small values)
         * and could request absurd Vulkan images.
         * Note: 1D surfaces (h<=1, d<=1) with large width are legitimate
         * linear buffers (e.g. 32768x1x1 vertex data); they take the
         * pureBuffer path in allocate(), which has its own size cap. */
        uint32_t w0 = sizes[0].width ? sizes[0].width : 1;
        uint32_t h0 = sizes[0].height ? sizes[0].height : 1;
        uint32_t d0 = sizes[0].depth ? sizes[0].depth : 1;
        bool is1D = (h0 <= 1 && d0 <= 1);
        if ((w0 > SVGA3_MAX_SURFACE_DIM && !is1D) || h0 > SVGA3_MAX_SURFACE_DIM ||
            d0 > SVGA3_MAX_SURFACE_DIM) {
            log_msg("[libqemu_svga3d] defineSurface error: dimensions %ux%ux%u exceed max %u (sid=%u)\n",
                    w0, h0, d0, SVGA3_MAX_SURFACE_DIM, sid);
            m_width = 1; m_height = 1; m_depth = 1;
            m_mipLevels = 1; m_arrayLayers = 1;
            m_mips.push_back(SurfaceMipLevel{1, 1, 1, 4, 4, 4});
            m_allocFailed = true;
            m_budgetedBytes = estimatedBytes();
            return;
        }
        m_width = w0;
        m_height = h0;
        m_depth = d0;

        if (m_isCubeMap) {
            m_arrayLayers = 6;
            m_mipLevels = numSizes / 6;
            if (m_mipLevels == 0) m_mipLevels = 1;
        } else {
            m_arrayLayers = 1;
            m_mipLevels = numSizes;
        }
        if (m_mipLevels == 0 || m_mipLevels > SVGA3_MAX_MIP_LEVELS) {
            log_msg("[libqemu_svga3d] defineSurface error: mip levels %u exceeds max %u (sid=%u)\n",
                    m_mipLevels, SVGA3_MAX_MIP_LEVELS, sid);
            m_width = 1; m_height = 1; m_depth = 1;
            m_mipLevels = 1; m_arrayLayers = 1;
            m_mips.push_back(SurfaceMipLevel{1, 1, 1, 4, 4, 4});
            m_allocFailed = true;
            m_budgetedBytes = estimatedBytes();
            return;
        }

        size_t bpp = svga3_format_bytes_per_pixel(format);
        bool compressed = svga3_format_is_compressed(format);

        for (uint32_t i = 0; i < m_mipLevels; ++i) {
            SurfaceMipLevel mip = {};
            mip.width = std::max(1u, m_width >> i);
            mip.height = std::max(1u, m_height >> i);
            mip.depth = std::max(1u, m_depth >> i);

            /* Overflow-checked: guest dimensions are capped above, but
             * belt-and-braces against future changes. */
            size_t rowPitch = 0, slicePitch = 0, totalBytes = 0;
            bool overflow = false;
            if (compressed) {
                uint32_t blocksW = (mip.width + 3) / 4;
                uint32_t blocksH = (mip.height + 3) / 4;
                overflow = __builtin_mul_overflow((size_t)blocksW, bpp, &rowPitch) ||
                           __builtin_mul_overflow(rowPitch, (size_t)blocksH, &slicePitch) ||
                           __builtin_mul_overflow(slicePitch, (size_t)mip.depth, &totalBytes);
            } else {
                overflow = __builtin_mul_overflow((size_t)mip.width, bpp, &rowPitch) ||
                           __builtin_mul_overflow(rowPitch, (size_t)mip.height, &slicePitch) ||
                           __builtin_mul_overflow(slicePitch, (size_t)mip.depth, &totalBytes);
            }
            if (overflow) {
                log_msg("[libqemu_svga3d] defineSurface error: mip pitch overflow (sid=%u)\n", sid);
                m_allocFailed = true;
                m_budgetedBytes = estimatedBytes();
                return;
            }
            mip.rowPitch = rowPitch;
            mip.slicePitch = slicePitch;
            mip.totalBytes = totalBytes;
            m_mips.push_back(mip);
        }
    } else {
        m_width = 1;
        m_height = 1;
        m_depth = 1;
        m_mipLevels = 1;
        m_arrayLayers = 1;
        SurfaceMipLevel mip = { 1, 1, 1, 4, 4, 4 };
        m_mips.push_back(mip);
    }
    m_budgetedBytes = estimatedBytes();
}

VlknSurface::~VlknSurface() {
    destroy();
}

/* Upper bound on the host/device memory this surface will consume, used
 * for the device-wide aggregate budget. Mirrors the pureBuffer decision in
 * allocate(): 1D/buffer surfaces allocate a host-visible buffer of at
 * least 256 KiB; image surfaces allocate one image covering all mips,
 * array layers, and MSAA samples. */
size_t VlknSurface::estimatedBytes() const {
    bool pureBuffer = (m_svgaFormat == SVGA3D_BUFFER) ||
                      ((m_flags & (SVGA3D_SURFACE_HINT_VERTEXBUFFER | SVGA3D_SURFACE_HINT_INDEXBUFFER)) != 0 && m_height <= 1 && m_depth <= 1) ||
                      (m_height <= 1 && m_depth <= 1 && m_width > 16384);
    if (pureBuffer) {
        size_t mip0Bytes = m_mips.empty() ? (size_t)m_width : m_mips[0].totalBytes;
        return std::max((size_t)262144, mip0Bytes);
    }
    /* Clamp the sample factor the way allocate() does: it maps the guest
     * count to a VkSampleCountFlagBits, so charging the same factor keeps
     * the estimate aligned with the real image. */
    uint64_t samples = 1;
    if (m_multisampleCount >= 16) samples = 16;
    else if (m_multisampleCount >= 8) samples = 8;
    else if (m_multisampleCount >= 4) samples = 4;
    else if (m_multisampleCount >= 2) samples = 2;
    uint64_t total = 0;
    for (const auto &mip : m_mips) {
        total += (uint64_t)mip.totalBytes;
    }
    total *= (uint64_t)m_arrayLayers * samples;
    return total > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)total;
}

Svga3VlknStatus VlknSurface::allocate() {
    if (m_allocFailed) {
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }
    bool pureBuffer = (m_svgaFormat == SVGA3D_BUFFER) ||
                      ((m_flags & (SVGA3D_SURFACE_HINT_VERTEXBUFFER | SVGA3D_SURFACE_HINT_INDEXBUFFER)) != 0 && m_height <= 1 && m_depth <= 1) ||
                      (m_height <= 1 && m_depth <= 1 && m_width > 16384);
    if (pureBuffer) {
        m_image = VK_NULL_HANDLE;
        m_memory = VK_NULL_HANDLE;
        m_imageView = VK_NULL_HANDLE;
        m_currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        size_t mip0Bytes = m_mips.empty() ? m_width : m_mips[0].totalBytes;
        /* Cap buffer size: a 1D surface with width 2^30 would otherwise
         * allocate 1GB+ of host memory (H3). 256MB is generous for
         * vertex/index/constant buffers. */
        static const size_t MAX_BUFFER_BYTES = 256 * 1024 * 1024;
        if (mip0Bytes > MAX_BUFFER_BYTES) {
            log_msg("[libqemu_svga3d] allocate error: buffer size %zu exceeds max %zu (sid=%u)\n",
                    mip0Bytes, MAX_BUFFER_BYTES, m_sid);
            m_allocFailed = true;
            return SVGA3_VLKN_ERROR_INVALID_PARAM;
        }
        m_bufferSize = std::max((size_t)262144, mip0Bytes);
        Svga3VlknStatus st = m_backend->createBuffer(
            m_bufferSize,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &m_buffer, &m_bufferMemory
        );
        if (st == SVGA3_VLKN_SUCCESS && m_bufferMemory) {
            void *p = nullptr;
            if (m_backend->dispatch().vkMapMemory(m_backend->device(), m_bufferMemory, 0, m_bufferSize, 0, &p) == VK_SUCCESS) {
                memset(p, 0, m_bufferSize);
                m_backend->dispatch().vkUnmapMemory(m_backend->device(), m_bufferMemory);
            }
        }
        return st;
    }

    VkImageCreateInfo imgInfo = {};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = (m_depth > 1 && !m_isCubeMap) ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    imgInfo.format = m_vkFormat;
    imgInfo.extent.width = m_width;
    imgInfo.extent.height = m_height;
    imgInfo.extent.depth = m_depth;
    imgInfo.mipLevels = m_mipLevels;
    imgInfo.arrayLayers = m_arrayLayers;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    if (m_multisampleCount >= 16) samples = (VkSampleCountFlagBits)0x00000010;
    else if (m_multisampleCount >= 8) samples = (VkSampleCountFlagBits)0x00000008;
    else if (m_multisampleCount >= 4) samples = (VkSampleCountFlagBits)0x00000004;
    else if (m_multisampleCount >= 2) samples = (VkSampleCountFlagBits)0x00000002;
    imgInfo.samples = samples;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT;

    if (m_isDepthStencil) {
        imgInfo.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    } else {
        imgInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }

    if (m_isCubeMap) {
        imgInfo.flags |= 0x00000010; /* VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT */
    }

    VkResult res = m_backend->dispatch().vkCreateImage(m_backend->device(), &imgInfo, nullptr, &m_image);
    if (res != VK_SUCCESS) {
        return SVGA3_VLKN_ERROR_OUT_OF_MEMORY;
    }

    VkMemoryRequirements memReqs;
    m_backend->dispatch().vkGetImageMemoryRequirements(m_backend->device(), m_image, &memReqs);

    int memType = m_backend->findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memType < 0) {
        m_backend->dispatch().vkDestroyImage(m_backend->device(), m_image, nullptr);
        m_image = VK_NULL_HANDLE;
        return SVGA3_VLKN_ERROR_OUT_OF_MEMORY;
    }
    Svga3VlknStatus st = m_backend->allocateMemory(memReqs.size, memType, &m_memory);
    if (st != SVGA3_VLKN_SUCCESS) {
        m_backend->dispatch().vkDestroyImage(m_backend->device(), m_image, nullptr);
        m_image = VK_NULL_HANDLE;
        return st;
    }

    m_backend->dispatch().vkBindImageMemory(m_backend->device(), m_image, m_memory, 0);

    /* Create default ImageView for full resource sampling */
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_image;
    if (m_isCubeMap) {
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    } else if (m_depth > 1) {
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
    } else {
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    }
    viewInfo.format = m_vkFormat;
    viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.a = (m_svgaFormat == SVGA3D_X8R8G8B8 || m_svgaFormat == SVGA3D_X1R5G5B5) ?
                            VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_IDENTITY;

    viewInfo.subresourceRange.aspectMask = m_isDepthStencil ?
        (svga3_format_has_stencil(m_svgaFormat) ?
            (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) :
            VK_IMAGE_ASPECT_DEPTH_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    m_viewMipLevels = (m_autogenFilter != SVGA3D_TEX_FILTER_NONE) ? m_mipLevels : 1;
    viewInfo.subresourceRange.levelCount = m_viewMipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = m_arrayLayers;

    res = m_backend->dispatch().vkCreateImageView(m_backend->device(), &viewInfo, nullptr, &m_imageView);
    if (res != VK_SUCCESS) {
        return SVGA3_VLKN_ERROR_OUT_OF_MEMORY;
    }

    m_currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    /* Also create backing VkBuffer for vertex/index buffer / linear surface operations */
    size_t mip0Bytes = m_mips.empty() ? 4096 : m_mips[0].totalBytes;
    m_bufferSize = std::max((size_t)262144, mip0Bytes);
    m_backend->createBuffer(
        m_bufferSize,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &m_buffer,
        &m_bufferMemory
    );

    if (m_bufferMemory) {
        void *p = nullptr;
        if (m_backend->dispatch().vkMapMemory(m_backend->device(), m_bufferMemory, 0, m_bufferSize, 0, &p) == VK_SUCCESS) {
            memset(p, 0, m_bufferSize);
            m_backend->dispatch().vkUnmapMemory(m_backend->device(), m_bufferMemory);
        }
    }

    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknSurface::ensureBufferSize(size_t requiredSize) {
    if (requiredSize <= m_bufferSize) {
        return SVGA3_VLKN_SUCCESS;
    }
    /* Cap growth: a single guest command must not be able to demand
     * gigabytes of host memory. */
    if (requiredSize > SVGA3_MAX_DMA_BYTES) {
        log_msg("[libqemu_svga3d] ensureBufferSize error: %zu exceeds max %zu (sid=%u)\n",
                requiredSize, SVGA3_MAX_DMA_BYTES, m_sid);
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }

    size_t newSize = m_bufferSize ? m_bufferSize : 262144;
    while (newSize < requiredSize) {
        /* Overflow-checked growth: newSize*3 used to wrap past SIZE_MAX
         * for huge requiredSize, spinning forever. */
        size_t grown = 0;
        if (__builtin_mul_overflow(newSize, 3, &grown)) {
            grown = requiredSize;
        } else {
            grown = grown / 2;
        }
        if (grown <= newSize) {
            grown = requiredSize;
        }
        newSize = grown;
    }
    newSize = (newSize + 65535) & ~((size_t)65535);

    /* Aggregate budget: buffer growth is guest-triggered host memory.
     * Reserve the delta before allocating; release it if creation fails. */
    size_t growthDelta = 0;
    if (newSize > m_budgetedBytes) {
        growthDelta = newSize - m_budgetedBytes;
        if (!m_backend->resourceBudgets().tryReserveSurfaceBytes(growthDelta)) {
            log_msg("[libqemu_svga3d] ensureBufferSize error: aggregate surface budget exhausted (sid=%u)\n",
                    m_sid);
            return SVGA3_VLKN_ERROR_OUT_OF_MEMORY;
        }
    }

    /* The old allocation can be in flight, and its contents must be stable
     * while copied into the replacement buffer. */
    m_backend->flushCommandBuffer();

    VkBuffer newBuffer = VK_NULL_HANDLE;
    VkDeviceMemory newMemory = VK_NULL_HANDLE;

    Svga3VlknStatus st = m_backend->createBuffer(
        newSize,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &newBuffer,
        &newMemory
    );
    if (st != SVGA3_VLKN_SUCCESS) {
        if (growthDelta > 0) {
            m_backend->resourceBudgets().releaseSurfaceBytes(growthDelta);
        }
        log_msg("[libqemu_svga3d] ERROR: Failed to expand buffer for sid=%u to %zu bytes\n", m_sid, newSize);
        return st;
    }

    void *newMapped = nullptr;
    if (m_backend->dispatch().vkMapMemory(m_backend->device(), newMemory, 0, newSize, 0, &newMapped) == VK_SUCCESS) {
        memset(newMapped, 0, newSize);
        if (m_bufferMemory && m_bufferSize > 0) {
            void *oldMapped = nullptr;
            if (m_backend->dispatch().vkMapMemory(m_backend->device(), m_bufferMemory, 0, m_bufferSize, 0, &oldMapped) == VK_SUCCESS) {
                memcpy(newMapped, oldMapped, m_bufferSize);
                m_backend->dispatch().vkUnmapMemory(m_backend->device(), m_bufferMemory);
            }
        }
        m_backend->dispatch().vkUnmapMemory(m_backend->device(), newMemory);
    }

    if (m_buffer) {
        m_backend->dispatch().vkDestroyBuffer(m_backend->device(), m_buffer, nullptr);
    }
    if (m_bufferMemory) {
        m_backend->freeMemory(m_bufferMemory);
    }

    m_buffer = newBuffer;
    m_bufferMemory = newMemory;
    m_bufferSize = newSize;
    if (growthDelta > 0) {
        m_budgetedBytes = newSize;
    }

    log_msg("[libqemu_svga3d] Expanded buffer for sid=%u to %zu bytes\n", m_sid, m_bufferSize);
    return SVGA3_VLKN_SUCCESS;
}

void VlknSurface::ensureViewMipLevels(uint32_t levels) {
    uint32_t targetLevels = std::min(levels, m_mipLevels);
    if (targetLevels <= m_viewMipLevels || m_image == VK_NULL_HANDLE) {
        return;
    }
    if (m_imageView != VK_NULL_HANDLE) {
        m_backend->dispatch().vkDestroyImageView(m_backend->device(), m_imageView, nullptr);
        m_imageView = VK_NULL_HANDLE;
    }
    m_viewMipLevels = targetLevels;
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_image;
    if (m_isCubeMap) {
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    } else if (m_depth > 1) {
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
    } else {
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    }
    viewInfo.format = m_vkFormat;
    viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.a = (m_svgaFormat == SVGA3D_X8R8G8B8 || m_svgaFormat == SVGA3D_X1R5G5B5) ?
                            VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_IDENTITY;

    viewInfo.subresourceRange.aspectMask = m_isDepthStencil ?
        (svga3_format_has_stencil(m_svgaFormat) ?
            (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) :
            VK_IMAGE_ASPECT_DEPTH_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = m_viewMipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = m_arrayLayers;

    m_backend->dispatch().vkCreateImageView(m_backend->device(), &viewInfo, nullptr, &m_imageView);
}

void VlknSurface::destroy() {
    if (m_backend) {
        m_backend->flushCommandBuffer();
    }

    if (m_buffer) {
        m_backend->dispatch().vkDestroyBuffer(m_backend->device(), m_buffer, nullptr);
        m_buffer = VK_NULL_HANDLE;
    }
    if (m_bufferMemory) {
        m_backend->freeMemory(m_bufferMemory);
        m_bufferMemory = VK_NULL_HANDLE;
    }
    m_bufferSize = 0;

    for (auto &pair : m_rtViews) {
        if (pair.second) {
            m_backend->dispatch().vkDestroyImageView(m_backend->device(), pair.second, nullptr);
        }
    }
    m_rtViews.clear();

    if (m_imageView) {
        m_backend->dispatch().vkDestroyImageView(m_backend->device(), m_imageView, nullptr);
        m_imageView = VK_NULL_HANDLE;
    }

    if (m_image) {
        m_backend->dispatch().vkDestroyImage(m_backend->device(), m_image, nullptr);
        m_image = VK_NULL_HANDLE;
    }

    if (m_memory) {
        m_backend->freeMemory(m_memory);
        m_memory = VK_NULL_HANDLE;
    }
}

VkImageView VlknSurface::getRenderTargetView(uint32_t mip, uint32_t face) {
    /* Validate against the surface: out-of-range levels/faces make
     * vkCreateImageView fail or misbehave, and each unique (mip,face)
     * caches a view — unbounded without this check. */
    if (mip >= m_mipLevels || face >= m_arrayLayers) {
        return VK_NULL_HANDLE;
    }
    /* Bound the view cache: each entry holds a driver-side VkImageView. */
    if (m_rtViews.size() >= SVGA3_MAX_RT_VIEWS) {
        return VK_NULL_HANDLE;
    }
    uint64_t key = ((uint64_t)mip << 32) | (uint64_t)face;
    auto it = m_rtViews.find(key);
    if (it != m_rtViews.end()) {
        return it->second;
    }

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = m_vkFormat;
    viewInfo.subresourceRange.aspectMask = m_isDepthStencil ?
        (svga3_format_has_stencil(m_svgaFormat) ?
            (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) :
            VK_IMAGE_ASPECT_DEPTH_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = mip;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = face;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    m_backend->dispatch().vkCreateImageView(m_backend->device(), &viewInfo, nullptr, &view);
    m_rtViews[key] = view;
    return view;
}

const SurfaceMipLevel* VlknSurface::getMipInfo(uint32_t mipLevel) const {
    if (mipLevel < m_mips.size()) {
        return &m_mips[mipLevel];
    }
    return nullptr;
}

Svga3VlknStatus VlknSurface::dmaUpload(uint32_t mipLevel,
                                      const SVGA3dBox *box,
                                      const void *guestData,
                                      size_t guestStride,
                                      bool isLinear)
{
    if (!guestData || mipLevel >= m_mipLevels) {
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }
    invalidateReadback();

    const SurfaceMipLevel &mip = m_mips[mipLevel];
    uint32_t bw = box ? box->w : mip.width;
    uint32_t bh = box ? box->h : mip.height;
    uint32_t bd = box ? box->d : mip.depth;
    uint32_t bx = box ? box->x : 0;
    uint32_t by = box ? box->y : 0;
    uint32_t bz = box ? box->z : 0;

    bool isLinearBuffer = isLinear || (m_svgaFormat == SVGA3D_BUFFER) ||
                          (m_image == VK_NULL_HANDLE) ||
                          (((m_flags & (SVGA3D_SURFACE_HINT_VERTEXBUFFER | SVGA3D_SURFACE_HINT_INDEXBUFFER)) != 0) && m_height <= 1 && m_depth <= 1) ||
                          (m_height <= 1 && m_depth <= 1 && (bw > mip.width || (bx + bw) > mip.width));

    if (isLinearBuffer) {
        /* 64-bit: bx + bw wrapped in 32-bit for huge boxes, bypassing the
         * ensureBufferSize growth and making copyLen read past guestData. */
        uint64_t offset64 = bx;
        uint64_t len64 = bw;
        uint64_t end64 = offset64 + len64;
        if (end64 > m_bufferSize) {
            if (end64 > SVGA3_MAX_DMA_BYTES) {
                log_msg("[libqemu_svga3d] dmaUpload error: linear range %lu exceeds max %lu (sid=%u)\n",
                        (unsigned long)end64, (unsigned long)SVGA3_MAX_DMA_BYTES, m_sid);
                return SVGA3_VLKN_ERROR_INVALID_PARAM;
            }
            Svga3VlknStatus ensSt = ensureBufferSize((size_t)end64);
            if (ensSt != SVGA3_VLKN_SUCCESS) return ensSt;
        }
        uint32_t offset = bx;
        size_t len = bw;
        if (!m_buffer || !m_bufferMemory) return SVGA3_VLKN_ERROR_INVALID_PARAM;
        if (offset < m_bufferSize) {
            void *bufMapped = nullptr;
            if (m_backend->dispatch().vkMapMemory(m_backend->device(), m_bufferMemory, 0, m_bufferSize, 0, &bufMapped) == VK_SUCCESS) {
                size_t copyLen = std::min(static_cast<size_t>(len), m_bufferSize - offset);
                memcpy(static_cast<uint8_t*>(bufMapped) + offset, guestData, copyLen);
                m_backend->dispatch().vkUnmapMemory(m_backend->device(), m_bufferMemory);

                static uint32_t buf_upload_cnt = 0;
                buf_upload_cnt++;
                if (buf_upload_cnt <= 10 || (buf_upload_cnt % 500) == 0) {
                    log_msg("[libqemu_svga3d] Buffer upload #%u: sid=%u, off=%u, len=%zu\n",
                            buf_upload_cnt, m_sid, offset, copyLen);
                    char hexbuf[256];
                    size_t dumpLen = std::min(copyLen, (size_t)64);
                    size_t hpos = 0;
                    for (size_t k = 0; k < dumpLen && hpos + 4 < sizeof(hexbuf); ++k) {
                        hpos += snprintf(hexbuf + hpos, sizeof(hexbuf) - hpos, "%02x ", ((const uint8_t*)guestData)[k]);
                    }
                    log_msg("[libqemu_svga3d]   bytes: %s\n", hexbuf);

                    if (copyLen == 64) {
                        const uint8_t *p = (const uint8_t*)guestData;
                        for (int v = 0; v < 4; ++v) {
                            const int16_t *pos = (const int16_t*)(p + v * 16 + 0);
                            const float *tex = (const float*)(p + v * 16 + 4);
                            const uint32_t *col = (const uint32_t*)(p + v * 16 + 12);
                            log_msg("[libqemu_svga3d]   v%d: pos=(%d, %d), uv=(%f, %f), color=0x%08x\n",
                                    v, pos[0], pos[1], tex[0], tex[1], *col);
                        }
                    } else if (copyLen == 8) {
                        const uint16_t *idx = (const uint16_t*)guestData;
                        log_msg("[libqemu_svga3d]   indices: %u, %u, %u, %u\n",
                                idx[0], idx[1], idx[2], idx[3]);
                    }
                }
            } else {
                log_msg("[libqemu_svga3d] dmaUpload error: vkMapMemory failed (sid=%u, bufSize=%zu)\n",
                        m_sid, m_bufferSize);
                return SVGA3_VLKN_ERROR_OUT_OF_MEMORY;
            }
        }
        return SVGA3_VLKN_SUCCESS;
    }

    size_t bpp = svga3_format_bytes_per_pixel(m_svgaFormat);
    size_t copyRowBytes = bw * bpp;
    size_t totalBytes = copyRowBytes * bh * bd;

    /* 64-bit comparisons: the 32-bit sums wrapped for large boxes,
     * letting out-of-image boxes pass and driving negative Vulkan extents. */
    bool fitsInImage = (m_image != VK_NULL_HANDLE) &&
                       ((uint64_t)bx + bw <= mip.width) &&
                       ((uint64_t)by + bh <= mip.height) &&
                       ((uint64_t)bz + bd <= mip.depth);

    if (!fitsInImage) {
        log_msg("[libqemu_svga3d] dmaUpload error: box does not fit in image (sid=%u, box=(%u,%u %ux%u), mip=(%ux%u))\n",
                m_sid, bx, by, bw, bh, mip.width, mip.height);
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }

    if (mipLevel > 0) {
        ensureViewMipLevels(mipLevel + 1);
    }

    /* Transition image to TRANSFER_DST_OPTIMAL */
    VkCommandBuffer cb = m_backend->getActiveCommandBuffer();

    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = m_currentLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.image = m_image;
    barrier.subresourceRange.aspectMask = m_isDepthStencil ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = mipLevel;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = m_arrayLayers;

    m_backend->dispatch().vkCmdPipelineBarrier(
        cb,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier
    );

    /* Allocate staging buffer space */
    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    void *mapped = nullptr;
    std::unique_lock<std::mutex> stagingLock;
    bool usingPersistentStaging = false;

    if (totalBytes <= m_backend->stagingSize() && m_backend->stagingBuffer() && m_backend->stagingMapped()) {
        stagingLock = std::unique_lock<std::mutex>(m_backend->stagingMutex());
        stagingBuf = m_backend->stagingBuffer();
        mapped = m_backend->stagingMapped();
        usingPersistentStaging = true;
    } else {
        Svga3VlknStatus st = m_backend->createBuffer(
            totalBytes,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &stagingBuf,
            &stagingMem
        );
        if (st != SVGA3_VLKN_SUCCESS) return st;
        if (m_backend->dispatch().vkMapMemory(m_backend->device(), stagingMem, 0,
                                              totalBytes, 0, &mapped) != VK_SUCCESS ||
            !mapped) {
            m_backend->destroyBuffer(stagingBuf, stagingMem);
            return SVGA3_VLKN_ERROR_OUT_OF_MEMORY;
        }
    }

    if (guestStride == 0 || guestStride == copyRowBytes) {
        memcpy(mapped, guestData, totalBytes);
    } else {
        /* Strided row-by-row copy */
        uint8_t *dst = (uint8_t*)mapped;
        const uint8_t *src = (const uint8_t*)guestData;
        for (uint32_t z = 0; z < bd; ++z) {
            for (uint32_t y = 0; y < bh; ++y) {
                memcpy(dst, src, copyRowBytes);
                dst += copyRowBytes;
                src += guestStride;
            }
        }
    }

    /* Also copy into vertex/index backing VkBuffer */
    if (mipLevel == 0) {
        /* 64-bit: (bx+bw) wrapped in 32-bit, undersizing the buffer. */
        uint64_t bxw = (uint64_t)bx + bw;
        uint64_t bzw = (uint64_t)bz + bd;
        uint64_t byh = (uint64_t)by + bh;
        uint64_t maxOffset64 = 0;
        bool ov = (bd == 0 || bzw == 0) ||
                  __builtin_mul_overflow(bzw - 1, (uint64_t)mip.height, &maxOffset64) ||
                  __builtin_add_overflow(maxOffset64, byh - (bh ? 1 : 0), &maxOffset64) ||
                  __builtin_mul_overflow(maxOffset64, (uint64_t)mip.rowPitch, &maxOffset64) ||
                  __builtin_add_overflow(maxOffset64, bxw * bpp, &maxOffset64);
        if (ov || maxOffset64 > SVGA3_MAX_DMA_BYTES) {
            return SVGA3_VLKN_ERROR_INVALID_PARAM;
        }
        Svga3VlknStatus ensSt = ensureBufferSize((size_t)maxOffset64);
        if (ensSt != SVGA3_VLKN_SUCCESS) return ensSt;
        if (m_buffer && m_bufferMemory) {
            void *bufMapped = nullptr;
            if (m_backend->dispatch().vkMapMemory(m_backend->device(), m_bufferMemory, 0, m_bufferSize, 0, &bufMapped) == VK_SUCCESS) {
            uint8_t *dstBuf = static_cast<uint8_t*>(bufMapped);
            const uint8_t *srcBuf = static_cast<const uint8_t*>(mapped);
            for (uint32_t z = 0; z < bd; ++z) {
                for (uint32_t y = 0; y < bh; ++y) {
                    size_t dstOffset = (static_cast<size_t>(bz + z) * mip.height + (by + y)) * mip.rowPitch + static_cast<size_t>(bx) * bpp;
                    if (dstOffset < m_bufferSize) {
                        size_t copyLen = std::min(copyRowBytes, m_bufferSize - dstOffset);
                        memcpy(dstBuf + dstOffset, srcBuf + (z * bh + y) * copyRowBytes, copyLen);
                    }
                }
            }
            m_backend->dispatch().vkUnmapMemory(m_backend->device(), m_bufferMemory);

                static uint32_t vbuf_upload_cnt = 0;
                vbuf_upload_cnt++;
                if (vbuf_upload_cnt <= 10 || (vbuf_upload_cnt % 500) == 0) {
                    log_msg("[libqemu_svga3d] 2D Buffer upload #%u: sid=%u, box=(%u,%u %ux%u), bpp=%zu, totalBytes=%zu\n",
                            vbuf_upload_cnt, m_sid, bx, by, bw, bh, bpp, totalBytes);
                }
            }
        }
    }

    if (!usingPersistentStaging) {
        m_backend->dispatch().vkUnmapMemory(m_backend->device(), stagingMem);
    }

    if (m_svgaFormat == SVGA3D_BUFFER) {
        if (!usingPersistentStaging) {
            m_backend->destroyBuffer(stagingBuf, stagingMem);
        }
        return SVGA3_VLKN_SUCCESS;
    }

    /* Record buffer to image copy */
    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = bw;
    region.bufferImageHeight = bh;
    region.imageSubresource.aspectMask = m_isDepthStencil ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = mipLevel;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset.x = (int32_t)bx;
    region.imageOffset.y = (int32_t)by;
    region.imageOffset.z = (int32_t)bz;
    region.imageExtent.width = bw;
    region.imageExtent.height = bh;
    region.imageExtent.depth = bd;

    m_backend->dispatch().vkCmdCopyBufferToImage(
        cb, stagingBuf, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region
    );

    /* Transition back to SHADER_READ_ONLY_OPTIMAL or COLOR_ATTACHMENT_OPTIMAL */
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = m_isDepthStencil ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL :
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    m_backend->dispatch().vkCmdPipelineBarrier(
        cb,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier
    );

    m_currentLayout = barrier.newLayout;
    m_backend->flushCommandBuffer();

    if (!usingPersistentStaging) {
        m_backend->destroyBuffer(stagingBuf, stagingMem);
    }
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknSurface::dmaDownloadToStaging(uint32_t mipLevel,
                                                const SVGA3dBox *box,
                                                const void **outMappedData,
                                                size_t *outRowPitch,
                                                std::unique_lock<std::mutex> &outLock)
{
    if (!outMappedData || !outRowPitch || mipLevel >= m_mipLevels) {
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }
    /* Buffer surfaces have no VkImage; the copy below would null-deref. */
    if (m_image == VK_NULL_HANDLE) {
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }

    const SurfaceMipLevel &mip = m_mips[mipLevel];
    uint32_t bw = box ? box->w : mip.width;
    uint32_t bh = box ? box->h : mip.height;
    uint32_t bd = box ? box->d : mip.depth;
    uint32_t bx = box ? box->x : 0;
    uint32_t by = box ? box->y : 0;
    uint32_t bz = box ? box->z : 0;

    size_t bpp = svga3_format_bytes_per_pixel(m_svgaFormat);
    size_t copyRowBytes = bw * bpp;
    size_t totalBytes = copyRowBytes * bh * bd;

    if (totalBytes > m_backend->stagingSize() || !m_backend->stagingBuffer() || !m_backend->stagingMapped()) {
        return SVGA3_VLKN_ERROR_OUT_OF_MEMORY;
    }

    std::unique_lock<std::mutex> lock(m_backend->stagingMutex());

    VkCommandBuffer cb = m_backend->getActiveCommandBuffer();

    /* Transition image to TRANSFER_SRC_OPTIMAL */
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = m_currentLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.image = m_image;
    barrier.subresourceRange.aspectMask = m_isDepthStencil ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = mipLevel;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    m_backend->dispatch().vkCmdPipelineBarrier(
        cb,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier
    );

    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = bw;
    region.bufferImageHeight = bh;
    region.imageSubresource.aspectMask = m_isDepthStencil ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = mipLevel;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset.x = (int32_t)bx;
    region.imageOffset.y = (int32_t)by;
    region.imageOffset.z = (int32_t)bz;
    region.imageExtent.width = bw;
    region.imageExtent.height = bh;
    region.imageExtent.depth = bd;

    m_backend->dispatch().vkCmdCopyImageToBuffer(
        cb, m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_backend->stagingBuffer(), 1, &region
    );

    /* Transition back */
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = m_isDepthStencil ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL :
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    m_backend->dispatch().vkCmdPipelineBarrier(
        cb,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier
    );

    m_currentLayout = barrier.newLayout;
    Svga3VlknStatus flushSt = m_backend->flushCommandBuffer();
    if (flushSt != SVGA3_VLKN_SUCCESS) {
        return flushSt;
    }

    *outMappedData = m_backend->stagingMapped();
    *outRowPitch = copyRowBytes;
    outLock = std::move(lock);

    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknSurface::dmaDownload(uint32_t mipLevel,
                                        const SVGA3dBox *box,
                                        void *outGuestData,
                                        size_t guestStride,
                                        bool isLinear)
{
    if (!outGuestData || mipLevel >= m_mipLevels) {
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }

    const SurfaceMipLevel &mip = m_mips[mipLevel];
    uint32_t bw = box ? box->w : mip.width;
    uint32_t bh = box ? box->h : mip.height;
    uint32_t bd = box ? box->d : mip.depth;
    uint32_t bx = box ? box->x : 0;
    uint32_t by = box ? box->y : 0;
    uint32_t bz = box ? box->z : 0;

    bool isLinearBuffer = isLinear || (m_svgaFormat == SVGA3D_BUFFER) ||
                          (m_image == VK_NULL_HANDLE) ||
                          (((m_flags & (SVGA3D_SURFACE_HINT_VERTEXBUFFER | SVGA3D_SURFACE_HINT_INDEXBUFFER)) != 0) && m_height <= 1 && m_depth <= 1) ||
                          (m_height <= 1 && m_depth <= 1 && (bw > mip.width || (bx + bw) > mip.width));

    if (isLinearBuffer) {
        if (!m_buffer || !m_bufferMemory) return SVGA3_VLKN_ERROR_INVALID_PARAM;
        uint32_t offset = bx;
        uint32_t len = bw;
        if (offset < m_bufferSize) {
            void *bufMapped = nullptr;
            if (m_backend->dispatch().vkMapMemory(m_backend->device(), m_bufferMemory, 0, m_bufferSize, 0, &bufMapped) == VK_SUCCESS) {
                size_t copyLen = std::min(static_cast<size_t>(len), m_bufferSize - offset);
                memcpy(outGuestData, static_cast<const uint8_t*>(bufMapped) + offset, copyLen);
                m_backend->dispatch().vkUnmapMemory(m_backend->device(), m_bufferMemory);
            }
        }
        return SVGA3_VLKN_SUCCESS;
    }

    size_t bpp = svga3_format_bytes_per_pixel(m_svgaFormat);
    size_t copyRowBytes = bw * bpp;
    size_t totalBytes = copyRowBytes * bh * bd;

    /* 64-bit comparisons: the 32-bit sums wrapped for large boxes,
     * letting out-of-image boxes pass and driving negative Vulkan extents. */
    bool fitsInImage = (m_image != VK_NULL_HANDLE) &&
                       ((uint64_t)bx + bw <= mip.width) &&
                       ((uint64_t)by + bh <= mip.height) &&
                       ((uint64_t)bz + bd <= mip.depth);

    if (!fitsInImage) {
        log_msg("[libqemu_svga3d] dmaDownload error: box does not fit in image (sid=%u, box=(%u,%u %ux%u), mip=(%ux%u))\n",
                m_sid, bx, by, bw, bh, mip.width, mip.height);
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }

    if (totalBytes <= m_backend->stagingSize() && m_backend->stagingBuffer() && m_backend->stagingMapped()) {
        const void *mapped = nullptr;
        size_t rowPitch = 0;
        std::unique_lock<std::mutex> lock;
        Svga3VlknStatus st = dmaDownloadToStaging(mipLevel, box, &mapped, &rowPitch, lock);
        if (st != SVGA3_VLKN_SUCCESS) {
            return st;
        }

        if (guestStride == 0 || guestStride == copyRowBytes) {
            memcpy(outGuestData, mapped, totalBytes);
        } else {
            const uint8_t *src = (const uint8_t*)mapped;
            uint8_t *dst = (uint8_t*)outGuestData;
            for (uint32_t z = 0; z < bd; ++z) {
                for (uint32_t y = 0; y < bh; ++y) {
                    memcpy(dst, src, copyRowBytes);
                    src += copyRowBytes;
                    dst += guestStride;
                }
            }
        }
        bool fullMip = bx == 0 && by == 0 && bz == 0 &&
                       bw == mip.width && bh == mip.height && bd == 1;
        if (mipLevel == 0 && !m_isDepthStencil && fullMip) {
            storeReadback(bw, bh, rowPitch, mapped);
        }
        return SVGA3_VLKN_SUCCESS;
    }

    VkBuffer stagingBuf;
    VkDeviceMemory stagingMem;
    Svga3VlknStatus st = m_backend->createBuffer(
        totalBytes,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &stagingBuf,
        &stagingMem
    );
    if (st != SVGA3_VLKN_SUCCESS) return st;

    VkCommandBuffer cb = m_backend->getActiveCommandBuffer();

    /* Transition image to TRANSFER_SRC_OPTIMAL */
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = m_currentLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.image = m_image;
    barrier.subresourceRange.aspectMask = m_isDepthStencil ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = mipLevel;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    m_backend->dispatch().vkCmdPipelineBarrier(
        cb,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier
    );

    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = bw;
    region.bufferImageHeight = bh;
    region.imageSubresource.aspectMask = m_isDepthStencil ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = mipLevel;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset.x = (int32_t)bx;
    region.imageOffset.y = (int32_t)by;
    region.imageOffset.z = (int32_t)bz;
    region.imageExtent.width = bw;
    region.imageExtent.height = bh;
    region.imageExtent.depth = bd;

    m_backend->dispatch().vkCmdCopyImageToBuffer(
        cb, m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuf, 1, &region
    );

    /* Transition back */
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = m_isDepthStencil ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL :
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    m_backend->dispatch().vkCmdPipelineBarrier(
        cb,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier
    );

    m_currentLayout = barrier.newLayout;
    m_backend->flushCommandBuffer();

    void *mapped = nullptr;
    if (m_backend->dispatch().vkMapMemory(m_backend->device(), stagingMem, 0,
                                          totalBytes, 0, &mapped) != VK_SUCCESS ||
        !mapped) {
        m_backend->destroyBuffer(stagingBuf, stagingMem);
        return SVGA3_VLKN_ERROR_OUT_OF_MEMORY;
    }

    if (guestStride == 0 || guestStride == copyRowBytes) {
        memcpy(outGuestData, mapped, totalBytes);
    } else {
        const uint8_t *src = (const uint8_t*)mapped;
        uint8_t *dst = (uint8_t*)outGuestData;
        for (uint32_t z = 0; z < bd; ++z) {
            for (uint32_t y = 0; y < bh; ++y) {
                memcpy(dst, src, copyRowBytes);
                src += copyRowBytes;
                dst += guestStride;
            }
        }
    }

    bool fullMip = bx == 0 && by == 0 && bz == 0 &&
                   bw == mip.width && bh == mip.height && bd == 1;
    if (mipLevel == 0 && !m_isDepthStencil && fullMip) {
        storeReadback(bw, bh, copyRowBytes, mapped);
    }

    m_backend->dispatch().vkUnmapMemory(m_backend->device(), stagingMem);
    m_backend->destroyBuffer(stagingBuf, stagingMem);

    return SVGA3_VLKN_SUCCESS;
}

void VlknSurface::storeReadback(uint32_t w, uint32_t h, size_t pitch, const void *src) {
    if (!src || w == 0 || h == 0 || pitch == 0) {
        m_readbackValid = false;
        return;
    }
    m_readback.resize(pitch * h);
    memcpy(m_readback.data(), src, pitch * h);
    m_readbackW = w;
    m_readbackH = h;
    m_readbackPitch = pitch;
    m_readbackValid = true;
}

VlknSurfaceManager::VlknSurfaceManager(VlknBackend *backend)
    : m_backend(backend)
{}

VlknSurfaceManager::~VlknSurfaceManager() {
    clear();
}

Svga3VlknStatus VlknSurfaceManager::defineSurface(uint32_t sid,
                                                 uint32_t surfaceFlags,
                                                 SVGA3dSurfaceFormat format,
                                                 const SVGA3dSize *sizes,
                                                 uint32_t numSizes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_surfaces.find(sid);
    if (it != m_surfaces.end()) {
        if (m_contextMgr) m_contextMgr->invalidateSurface(sid);
        /* Redefine replaces the old surface: release its budget first so
         * the new reservation is not charged on top of the old one. */
        m_backend->resourceBudgets().releaseSurfaceBytes(it->second->budgetedBytes());
        m_surfaces.erase(it);
    } else {
        /* Cap surfaces: each holds a Vulkan image/buffer, so an unbounded
         * count exhausts host and device memory. */
        if (m_surfaces.size() >= SVGA3_MAX_SURFACES) {
            log_msg("[libqemu_svga3d] defineSurface error: surface limit %u reached\n",
                    SVGA3_MAX_SURFACES);
            return SVGA3_VLKN_ERROR_OUT_OF_MEMORY;
        }
    }

    auto surf = std::make_unique<VlknSurface>(m_backend, sid, surfaceFlags, format, sizes, numSizes);
    /* Aggregate device budget: reject before touching the host allocator,
     * so a guest cannot exhaust host memory with many per-object-legal
     * surfaces. */
    size_t needBytes = surf->budgetedBytes();
    if (!m_backend->resourceBudgets().tryReserveSurfaceBytes(needBytes)) {
        log_msg("[libqemu_svga3d] defineSurface error: aggregate surface budget exhausted (%zu bytes, sid=%u)\n",
                needBytes, sid);
        return SVGA3_VLKN_ERROR_OUT_OF_MEMORY;
    }
    Svga3VlknStatus st = surf->allocate();
    if (st != SVGA3_VLKN_SUCCESS) {
        m_backend->resourceBudgets().releaseSurfaceBytes(needBytes);
        return st;
    }

    m_surfaces[sid] = std::move(surf);
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknSurfaceManager::destroySurface(uint32_t sid) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_surfaces.find(sid);
    if (it == m_surfaces.end()) {
        return SVGA3_VLKN_ERROR_NOT_FOUND;
    }

    if (m_contextMgr) m_contextMgr->invalidateSurface(sid);
    m_backend->resourceBudgets().releaseSurfaceBytes(it->second->budgetedBytes());
    m_surfaces.erase(it);
    return SVGA3_VLKN_SUCCESS;
}

VlknSurface* VlknSurfaceManager::getSurface(uint32_t sid) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_surfaces.find(sid);
    if (it != m_surfaces.end()) {
        return it->second.get();
    }
    return nullptr;
}

bool VlknSurfaceManager::exists(uint32_t sid) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_surfaces.find(sid) != m_surfaces.end();
}

void VlknSurfaceManager::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_backend) m_backend->waitIdle();
    if (m_contextMgr) {
        for (const auto &pair : m_surfaces) {
            m_contextMgr->invalidateSurface(pair.first);
        }
    }
    if (m_backend) {
        for (const auto &pair : m_surfaces) {
            m_backend->resourceBudgets().releaseSurfaceBytes(pair.second->budgetedBytes());
        }
    }
    m_surfaces.clear();
}

size_t VlknSurfaceManager::count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_surfaces.size();
}

Svga3VlknStatus VlknSurfaceManager::copy(uint32_t srcSid,
                                        uint32_t dstSid,
                                        const SVGA3dCopyBox *boxes,
                                        uint32_t numBoxes)
{
    VlknSurface *src = getSurface(srcSid);
    VlknSurface *dst = getSurface(dstSid);
    if (!src || !dst) {
        return SVGA3_VLKN_ERROR_NOT_FOUND;
    }

    VkCommandBuffer cb = m_backend->getActiveCommandBuffer();

    for (uint32_t i = 0; i < numBoxes; ++i) {
        const SVGA3dCopyBox &b = boxes[i];

        /* Copy backing buffer if present */
        if (src->buffer() && dst->buffer()) {
            /* Overflow-checked: b.w*b.h*b.d*bpp wrapped for huge boxes,
             * shrinking copyBytes and bypassing the bounds checks below. */
            size_t copyBytes = b.w;
            if (src->image() != VK_NULL_HANDLE) {
                size_t bpp = svga3_format_bytes_per_pixel(src->svgaFormat());
                size_t whd = 0;
                if (__builtin_mul_overflow((size_t)b.w, (size_t)(b.h ? b.h : 1), &whd) ||
                    __builtin_mul_overflow(whd, (size_t)(b.d ? b.d : 1), &whd) ||
                    __builtin_mul_overflow(whd, bpp, &copyBytes)) {
                    continue;
                }
            }
            /* 64-bit: b.srcx + copyBytes wrapped in 32-bit for huge boxes. */
            uint64_t srcEnd = (uint64_t)b.srcx + copyBytes;
            uint64_t dstEnd = (uint64_t)b.x + copyBytes;
            if (srcEnd <= src->bufferSize() && dstEnd <= dst->bufferSize()) {
                VkBufferCopy bufCopy = {};
                bufCopy.srcOffset = b.srcx;
                bufCopy.dstOffset = b.x;
                bufCopy.size = copyBytes;
                m_backend->dispatch().vkCmdCopyBuffer(cb, src->buffer(), dst->buffer(), 1, &bufCopy);
            }
        }

        /* Copy VkImage if both surfaces have images.
         * 64-bit box validation (C5): guest box fields went verbatim into
         * VkImageCopy; out-of-extents copies are Vulkan-level OOB. */
        if (src->image() != VK_NULL_HANDLE && dst->image() != VK_NULL_HANDLE) {
            uint32_t bd = b.d ? b.d : 1;
            if ((uint64_t)b.srcx + b.w > src->width() ||
                (uint64_t)b.srcy + (b.h ? b.h : 1) > src->height() ||
                (uint64_t)b.srcz + bd > src->depth() ||
                (uint64_t)b.x + b.w > dst->width() ||
                (uint64_t)b.y + (b.h ? b.h : 1) > dst->height() ||
                (uint64_t)b.z + bd > dst->depth()) {
                log_msg("[libqemu_svga3d] copy error: box %u out of image bounds\n", i);
                return SVGA3_VLKN_ERROR_INVALID_PARAM;
            }
            VkImageCopy copyRegion = {};
            copyRegion.srcSubresource.aspectMask = src->isDepthStencil() ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            copyRegion.srcSubresource.mipLevel = 0;
            copyRegion.srcSubresource.baseArrayLayer = 0;
            copyRegion.srcSubresource.layerCount = 1;
            copyRegion.srcOffset.x = (int32_t)b.srcx;
            copyRegion.srcOffset.y = (int32_t)b.srcy;
            copyRegion.srcOffset.z = (int32_t)b.srcz;

            copyRegion.dstSubresource.aspectMask = dst->isDepthStencil() ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            copyRegion.dstSubresource.mipLevel = 0;
            copyRegion.dstSubresource.baseArrayLayer = 0;
            copyRegion.dstSubresource.layerCount = 1;
            copyRegion.dstOffset.x = (int32_t)b.x;
            copyRegion.dstOffset.y = (int32_t)b.y;
            copyRegion.dstOffset.z = (int32_t)b.z;

            copyRegion.extent.width = b.w;
            copyRegion.extent.height = b.h;
            copyRegion.extent.depth = b.d ? b.d : 1;

            m_backend->dispatch().vkCmdCopyImage(
                cb,
                src->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                dst->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &copyRegion
            );
        }
    }

    m_backend->flushCommandBuffer();
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknSurfaceManager::defineSurfaceV2(uint32_t sid,
                                                  uint32_t surfaceFlags,
                                                  SVGA3dSurfaceFormat format,
                                                  uint32_t multisampleCount,
                                                  SVGA3dTextureFilter autogenFilter,
                                                  const SVGA3dSize *sizes,
                                                  uint32_t numSizes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_surfaces.find(sid);
    if (it != m_surfaces.end()) {
        if (m_contextMgr) m_contextMgr->invalidateSurface(sid);
        m_backend->resourceBudgets().releaseSurfaceBytes(it->second->budgetedBytes());
        m_surfaces.erase(it);
    }

    auto surf = std::make_unique<VlknSurface>(m_backend, sid, surfaceFlags, format, sizes, numSizes, multisampleCount, autogenFilter);
    size_t needBytes = surf->budgetedBytes();
    if (!m_backend->resourceBudgets().tryReserveSurfaceBytes(needBytes)) {
        log_msg("[libqemu_svga3d] defineSurfaceV2 error: aggregate surface budget exhausted (%zu bytes, sid=%u)\n",
                needBytes, sid);
        return SVGA3_VLKN_ERROR_OUT_OF_MEMORY;
    }
    Svga3VlknStatus st = surf->allocate();
    if (st != SVGA3_VLKN_SUCCESS) {
        m_backend->resourceBudgets().releaseSurfaceBytes(needBytes);
        return st;
    }

    m_surfaces[sid] = std::move(surf);
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknSurfaceManager::stretchBlt(uint32_t srcSid,
                                              uint32_t dstSid,
                                              const SVGA3dBox &boxSrc,
                                              const SVGA3dBox &boxDest,
                                              SVGA3dStretchBltMode mode)
{
    VlknSurface *src = getSurface(srcSid);
    VlknSurface *dst = getSurface(dstSid);
    if (!src || !dst) {
        return SVGA3_VLKN_ERROR_NOT_FOUND;
    }
    /* Buffer surfaces have no VkImage; blitting would null-deref. */
    if (src->image() == VK_NULL_HANDLE || dst->image() == VK_NULL_HANDLE) {
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }
    /* 64-bit box validation: (x+w) wrapped in 32-bit, producing negative
     * Vulkan extents and out-of-image blits. */
    if ((uint64_t)boxSrc.x + boxSrc.w > src->width() ||
        (uint64_t)boxSrc.y + boxSrc.h > src->height() ||
        (uint64_t)boxSrc.z + (boxSrc.d ? boxSrc.d : 1) > src->depth() ||
        (uint64_t)boxDest.x + boxDest.w > dst->width() ||
        (uint64_t)boxDest.y + boxDest.h > dst->height() ||
        (uint64_t)boxDest.z + (boxDest.d ? boxDest.d : 1) > dst->depth()) {
        log_msg("[libqemu_svga3d] stretchBlt error: box out of image bounds\n");
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }

    VkCommandBuffer cb = m_backend->getActiveCommandBuffer();

    VkImageBlit blit = {};
    blit.srcSubresource.aspectMask = src->isDepthStencil() ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.mipLevel = 0;
    blit.srcSubresource.baseArrayLayer = 0;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[0] = { (int32_t)boxSrc.x, (int32_t)boxSrc.y, (int32_t)boxSrc.z };
    blit.srcOffsets[1] = { (int32_t)(boxSrc.x + boxSrc.w), (int32_t)(boxSrc.y + boxSrc.h), (int32_t)(boxSrc.z + (boxSrc.d ? boxSrc.d : 1)) };

    blit.dstSubresource.aspectMask = dst->isDepthStencil() ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.mipLevel = 0;
    blit.dstSubresource.baseArrayLayer = 0;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[0] = { (int32_t)boxDest.x, (int32_t)boxDest.y, (int32_t)boxDest.z };
    blit.dstOffsets[1] = { (int32_t)(boxDest.x + boxDest.w), (int32_t)(boxDest.y + boxDest.h), (int32_t)(boxDest.z + (boxDest.d ? boxDest.d : 1)) };

    VkFilter filter = (mode == SVGA3D_STRETCH_BLT_LINEAR) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;

    m_backend->dispatch().vkCmdBlitImage(
        cb,
        src->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blit, filter
    );

    m_backend->flushCommandBuffer();
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknSurfaceManager::surfaceDMA(const SVGA3dGuestImage &guest,
                                              const SVGA3dSurfaceImageId &host,
                                              SVGA3dTransferType transfer,
                                              const SVGA3dCopyBox *boxes,
                                              uint32_t numBoxes,
                                              GuestMemoryManager *guestMem,
                                              const void *guestBuffer,
                                              size_t guestBufferSize)
{
    VlknSurface *surf = getSurface(host.sid);
    if (!surf) {
        return SVGA3_VLKN_ERROR_NOT_FOUND;
    }

    if (numBoxes == 0 || !boxes) {
        return SVGA3_VLKN_SUCCESS;
    }

    size_t baseBpp = svga3_format_bytes_per_pixel(surf->svgaFormat());
    for (uint32_t i = 0; i < numBoxes; ++i) {
        const SVGA3dCopyBox &box = boxes[i];
        uint32_t bw = box.w;
        uint32_t bh = box.h;
        uint32_t bd = box.d ? box.d : 1;
        if (bw == 0 || bh == 0) continue;

        bool isLinearBuffer = (surf->svgaFormat() == SVGA3D_BUFFER) ||
                              (surf->image() == VK_NULL_HANDLE) ||
                              (((surf->flags() & (SVGA3D_SURFACE_HINT_VERTEXBUFFER | SVGA3D_SURFACE_HINT_INDEXBUFFER)) != 0) && surf->height() <= 1 && surf->depth() <= 1) ||
                              (surf->height() <= 1 && surf->depth() <= 1 && (bw > surf->width() || (box.x + bw) > surf->width()));
        if (isLinearBuffer && surf->height() <= 1 && surf->depth() <= 1) {
            surf->addFlags(SVGA3D_SURFACE_HINT_VERTEXBUFFER);
        }

        size_t bpp = isLinearBuffer ? 1 : baseBpp;
        size_t rowBytes = isLinearBuffer ? bw : (bw * bpp);
        size_t guestStride = (guest.pitch != 0) ? guest.pitch : rowBytes;
        if (guest.pitch != 0 && guest.pitch < rowBytes) {
            return SVGA3_VLKN_ERROR_INVALID_PARAM;
        }

        SVGA3dBox sBox;
        sBox.x = box.x;
        sBox.y = box.y;
        sBox.z = box.z;
        sBox.w = bw;
        sBox.h = bh;
        sBox.d = bd;

        if (guestMem) {
            /* Overflow-checked: bw/bh/bd are guest-controlled, and a wrapped
             * product would undersize `staging`, overflowing the heap in the
             * row loops below. */
            size_t totalBytes = 0;
            if (__builtin_mul_overflow(rowBytes, static_cast<size_t>(bh), &totalBytes) ||
                __builtin_mul_overflow(totalBytes, static_cast<size_t>(bd), &totalBytes)) {
                return SVGA3_VLKN_ERROR_INVALID_PARAM;
            }
            /* Cap the staging allocation: one guest DMA must not allocate
             * gigabytes of host memory (DoS). */
            if (totalBytes > SVGA3_MAX_DMA_BYTES) {
                log_msg("[libqemu_svga3d] surfaceDMA error: transfer %zu bytes exceeds max %zu (sid=%u)\n",
                        totalBytes, SVGA3_MAX_DMA_BYTES, surf->sid());
                return SVGA3_VLKN_ERROR_INVALID_PARAM;
            }
            std::vector<uint8_t> staging(totalBytes);

            if (transfer == SVGA3D_WRITE_HOST_VRAM) {
                for (uint32_t z = 0; z < bd; ++z) {
                    for (uint32_t y = 0; y < bh; ++y) {
                        uint64_t rowGuestOffset = static_cast<uint64_t>(guest.ptr.offset) +
                            static_cast<uint64_t>(box.srcz + z) * (guestStride * bh) +
                            static_cast<uint64_t>(box.srcy + y) * guestStride +
                            (isLinearBuffer ? static_cast<uint64_t>(box.srcx) : static_cast<uint64_t>(box.srcx) * bpp);
                        if (rowGuestOffset > UINT32_MAX) return SVGA3_VLKN_ERROR_INVALID_PARAM;
                        SVGAGuestPtr rowPtr = { guest.ptr.gmrId, static_cast<uint32_t>(rowGuestOffset) };
                        uint8_t *dstRow = staging.data() + (z * bh + y) * rowBytes;
                        Svga3VlknStatus st = guestMem->readGuest(rowPtr, dstRow, rowBytes);
                        if (st != SVGA3_VLKN_SUCCESS) {
                            static uint32_t dma_read_err_cnt = 0;
                            if (++dma_read_err_cnt <= 10) {
                                log_msg("[libqemu_svga3d] surfaceDMA error: readGuest failed (st=%d, gmrId=%u, offset=%lu, rowBytes=%zu)\n",
                                        st, rowPtr.gmrId, (unsigned long)rowGuestOffset, rowBytes);
                            }
                            return st;
                        }
                    }
                }
                Svga3VlknStatus st = surf->dmaUpload(host.mipmap, &sBox, staging.data(), rowBytes, isLinearBuffer);
                if (st != SVGA3_VLKN_SUCCESS) {
                    static uint32_t dma_upload_err_cnt = 0;
                    if (++dma_upload_err_cnt <= 10) {
                        log_msg("[libqemu_svga3d] surfaceDMA error: dmaUpload failed (st=%d, sid=%u, mip=%u)\n",
                                st, surf->sid(), host.mipmap);
                    }
                    return st;
                }
            } else if (transfer == SVGA3D_READ_HOST_VRAM) {
                Svga3VlknStatus st = surf->dmaDownload(host.mipmap, &sBox, staging.data(), rowBytes, isLinearBuffer);
                if (st != SVGA3_VLKN_SUCCESS) {
                    static uint32_t dma_dl_err_cnt = 0;
                    if (++dma_dl_err_cnt <= 10) {
                        log_msg("[libqemu_svga3d] surfaceDMA error: dmaDownload failed (st=%d, sid=%u, mip=%u)\n",
                                st, surf->sid(), host.mipmap);
                    }
                    return st;
                }

                for (uint32_t z = 0; z < bd; ++z) {
                    for (uint32_t y = 0; y < bh; ++y) {
                        uint64_t rowGuestOffset = static_cast<uint64_t>(guest.ptr.offset) +
                            static_cast<uint64_t>(box.srcz + z) * (guestStride * bh) +
                            static_cast<uint64_t>(box.srcy + y) * guestStride +
                            (isLinearBuffer ? static_cast<uint64_t>(box.srcx) : static_cast<uint64_t>(box.srcx) * bpp);
                        if (rowGuestOffset > UINT32_MAX) return SVGA3_VLKN_ERROR_INVALID_PARAM;
                        SVGAGuestPtr rowPtr = { guest.ptr.gmrId, static_cast<uint32_t>(rowGuestOffset) };
                        const uint8_t *srcRow = staging.data() + (z * bh + y) * rowBytes;
                        st = guestMem->writeGuest(rowPtr, srcRow, rowBytes);
                        if (st != SVGA3_VLKN_SUCCESS) {
                            static uint32_t dma_write_err_cnt = 0;
                            if (++dma_write_err_cnt <= 10) {
                                log_msg("[libqemu_svga3d] surfaceDMA error: writeGuest failed (st=%d, gmrId=%u, offset=%lu, rowBytes=%zu)\n",
                                        st, rowPtr.gmrId, (unsigned long)rowGuestOffset, rowBytes);
                            }
                            return st;
                        }
                    }
                }
            }
        } else if (guestBuffer) {
            uint64_t guestOffset = static_cast<uint64_t>(guest.ptr.offset) +
                (isLinearBuffer ? static_cast<uint64_t>(box.srcx) : static_cast<uint64_t>(box.srcx) * bpp);
            /* Upper bound: the transfer touches (bh*bd) rows of guestStride
             * bytes starting at guestOffset. The old code only checked the
             * lower bound, allowing a host-side OOB read/write past
             * guestBuffer. All arithmetic is overflow-checked. */
            uint64_t xferBytes = 0;
            bool rangeOk = !__builtin_mul_overflow(static_cast<uint64_t>(bh),
                                                   static_cast<uint64_t>(bd),
                                                   &xferBytes) &&
                           !__builtin_mul_overflow(xferBytes,
                                                   static_cast<uint64_t>(guestStride),
                                                   &xferBytes) &&
                           guestOffset <= guestBufferSize &&
                           xferBytes <= guestBufferSize - guestOffset;
            if (transfer == SVGA3D_WRITE_HOST_VRAM) {
                const uint8_t *srcData = nullptr;
                if (rangeOk) {
                    srcData = reinterpret_cast<const uint8_t*>(guestBuffer) + guestOffset;
                }
                if (srcData) {
                    surf->dmaUpload(host.mipmap, &sBox, srcData, guestStride, isLinearBuffer);
                }
            } else if (transfer == SVGA3D_READ_HOST_VRAM) {
                uint8_t *dstData = nullptr;
                if (rangeOk) {
                    dstData = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(guestBuffer) + guestOffset);
                }
                if (dstData) {
                    surf->dmaDownload(host.mipmap, &sBox, dstData, guestStride, isLinearBuffer);
                }
            }
        }
    }

    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknSurfaceManager::generateMipmaps(uint32_t sid, SVGA3dTextureFilter filter) {
    VlknSurface *surf = getSurface(sid);
    if (!surf) return SVGA3_VLKN_ERROR_NOT_FOUND;
    if (surf->mipLevels() <= 1) return SVGA3_VLKN_SUCCESS;
    /* Buffer surfaces have no VkImage; blitting would null-deref. */
    if (surf->image() == VK_NULL_HANDLE) return SVGA3_VLKN_ERROR_INVALID_PARAM;

    VkCommandBuffer cb = m_backend->getActiveCommandBuffer();
    VkFilter vkFilt = (filter == SVGA3D_TEX_FILTER_NEAREST) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;

    for (uint32_t i = 1; i < surf->mipLevels(); ++i) {
        const SurfaceMipLevel *prevMip = surf->getMipInfo(i - 1);
        const SurfaceMipLevel *curMip = surf->getMipInfo(i);
        if (!prevMip || !curMip) break;

        VkImageBlit blit = {};
        blit.srcSubresource.aspectMask = surf->isDepthStencil() ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = i - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = surf->arrayLayers();
        blit.srcOffsets[0] = { 0, 0, 0 };
        blit.srcOffsets[1] = { (int32_t)prevMip->width, (int32_t)prevMip->height, (int32_t)prevMip->depth };

        blit.dstSubresource.aspectMask = surf->isDepthStencil() ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = i;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = surf->arrayLayers();
        blit.dstOffsets[0] = { 0, 0, 0 };
        blit.dstOffsets[1] = { (int32_t)curMip->width, (int32_t)curMip->height, (int32_t)curMip->depth };

        m_backend->dispatch().vkCmdBlitImage(
            cb,
            surf->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            surf->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, vkFilt
        );
    }

    surf->ensureViewMipLevels(surf->mipLevels());
    m_backend->flushCommandBuffer();
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknSurfaceManager::blitSurfaceToScreen(const SVGA3dSurfaceImageId &srcImage,
                                                      const SVGASignedRect &srcRect,
                                                      uint32_t destScreenId,
                                                      const SVGASignedRect &destRect,
                                                      const SVGASignedRect *clipRects,
                                                      uint32_t numClipRects,
                                                      GuestMemoryManager *guestMem)
{
    (void)destScreenId;
    VlknSurface *surf = getSurface(srcImage.sid);
    if (!surf) return SVGA3_VLKN_ERROR_NOT_FOUND;

    if (guestMem && guestMem->getFramebuffer().hva) {
        const auto &fb = guestMem->getFramebuffer();
        uint32_t surfW = surf->width();
        uint32_t surfH = surf->height();
        size_t bpp = svga3_format_bytes_per_pixel(surf->svgaFormat());
        if (bpp == 0) bpp = 4;

        const void *mappedData = nullptr;
        size_t rowPitch = 0;
        std::unique_lock<std::mutex> lock;
        Svga3VlknStatus st = surf->dmaDownloadToStaging(srcImage.mipmap, nullptr, &mappedData, &rowPitch, lock);
        if (st != SVGA3_VLKN_SUCCESS) {
            return st;
        }

        uint32_t dstW = fb.width ? fb.width : surfW;
        uint32_t dstH = fb.height ? fb.height : surfH;
        uint32_t dstBpp = fb.bpp ? fb.bpp : 4;
        uint32_t dstPitch = fb.pitch ? fb.pitch : (dstW * dstBpp);

        int32_t sx = srcRect.left;
        int32_t sy = srcRect.top;
        int32_t sw = srcRect.right - srcRect.left;
        int32_t sh = srcRect.bottom - srcRect.top;

        int32_t dx = destRect.left;
        int32_t dy = destRect.top;
        int32_t dw = destRect.right - destRect.left;
        int32_t dh = destRect.bottom - destRect.top;

        if (sw > 0 && sh > 0 && dw > 0 && dh > 0) {
            uint32_t copyW = std::min(static_cast<uint32_t>(sw), static_cast<uint32_t>(dw));
            uint32_t copyH = std::min(static_cast<uint32_t>(sh), static_cast<uint32_t>(dh));

            for (uint32_t y = 0; y < copyH; ++y) {
                int32_t curDy = dy + static_cast<int32_t>(y);
                int32_t curSy = sy + static_cast<int32_t>(y);
                if (curDy < 0 || curDy >= static_cast<int32_t>(dstH)) continue;
                if (curSy < 0 || curSy >= static_cast<int32_t>(surfH)) continue;

                for (uint32_t x = 0; x < copyW; ++x) {
                    int32_t curDx = dx + static_cast<int32_t>(x);
                    int32_t curSx = sx + static_cast<int32_t>(x);
                    if (curDx < 0 || curDx >= static_cast<int32_t>(dstW)) continue;
                    if (curSx < 0 || curSx >= static_cast<int32_t>(surfW)) continue;

                    if (numClipRects > 0 && clipRects) {
                        bool inside = false;
                        for (uint32_t c = 0; c < numClipRects; ++c) {
                            if (curDx >= clipRects[c].left && curDx < clipRects[c].right &&
                                curDy >= clipRects[c].top && curDy < clipRects[c].bottom) {
                                inside = true;
                                break;
                            }
                        }
                        if (!inside) continue;
                    }

                    /* 64-bit destination offset: the old 32-bit product wrapped
                     * for large pitches. Never write outside the registered
                     * framebuffer. */
                    uint64_t dstOff = (uint64_t)curDy * dstPitch + (uint64_t)curDx * dstBpp;
                    size_t pxBytes = std::min(bpp, static_cast<size_t>(dstBpp));
                    if (dstOff > fb.size || pxBytes > fb.size - dstOff) {
                        continue;
                    }
                    uint8_t *dst = fb.hva + (size_t)dstOff;
                    const uint8_t *src = static_cast<const uint8_t*>(mappedData) + curSy * rowPitch + curSx * bpp;
                    memcpy(dst, src, pxBytes);
                }
            }

            if (numClipRects == 0 || !clipRects) {
                guestMem->notifyDisplayUpdate(dx, dy, copyW, copyH);
            } else {
                for (uint32_t c = 0; c < numClipRects; ++c) {
                    int32_t ix0 = std::max(dx, static_cast<int32_t>(clipRects[c].left));
                    int32_t iy0 = std::max(dy, static_cast<int32_t>(clipRects[c].top));
                    int32_t ix1 = std::min(dx + static_cast<int32_t>(copyW), static_cast<int32_t>(clipRects[c].right));
                    int32_t iy1 = std::min(dy + static_cast<int32_t>(copyH), static_cast<int32_t>(clipRects[c].bottom));
                    if (ix1 > ix0 && iy1 > iy0) {
                        guestMem->notifyDisplayUpdate(ix0, iy0, ix1 - ix0, iy1 - iy0);
                    }
                }
            }
        }
    }

    m_backend->flushCommandBuffer();
    return SVGA3_VLKN_SUCCESS;
}

bool is_buffer_all_zero(const void *data, uint32_t w, uint32_t h, size_t rowPitch, size_t bpp) {
    if (!data) return true;
    const uint8_t *row = static_cast<const uint8_t*>(data);
    size_t rowBytes = w * bpp;
    for (uint32_t y = 0; y < h; ++y) {
        const uint64_t *p64 = reinterpret_cast<const uint64_t*>(row + y * rowPitch);
        size_t n64 = rowBytes / 8;
        for (size_t i = 0; i < n64; ++i) {
            if (p64[i] != 0) return false;
        }
        size_t rem = rowBytes % 8;
        const uint8_t *p8 = reinterpret_cast<const uint8_t*>(p64 + n64);
        for (size_t i = 0; i < rem; ++i) {
            if (p8[i] != 0) return false;
        }
    }
    return true;
}

bool is_buffer_all_black_or_zero(const void *data, uint32_t w, uint32_t h, size_t rowPitch, size_t bpp) {
    if (!data) return true;
    const uint8_t *row = static_cast<const uint8_t*>(data);
    if (bpp == 4) {
        for (uint32_t y = 0; y < h; ++y) {
            const uint32_t *p32 = reinterpret_cast<const uint32_t*>(row + y * rowPitch);
            for (uint32_t x = 0; x < w; ++x) {
                if ((p32[x] & 0x00FFFFFF) != 0) return false;
            }
        }
        return true;
    }
    return is_buffer_all_zero(data, w, h, rowPitch, bpp);
}

Svga3VlknStatus VlknSurfaceManager::present(uint32_t sid,
                                            const SVGA3dCopyRect *rects,
                                            uint32_t numRects,
                                            GuestMemoryManager *guestMem)
{
    VlknSurface *surf = getSurface(sid);
    if (!surf) return SVGA3_VLKN_ERROR_NOT_FOUND;

    if (guestMem && guestMem->getFramebuffer().hva) {
        const auto &fb = guestMem->getFramebuffer();
        uint32_t surfW = surf->width();
        uint32_t surfH = surf->height();
        size_t bpp = svga3_format_bytes_per_pixel(surf->svgaFormat());
        if (bpp == 0) bpp = 4;

        const void *mappedData = nullptr;
        size_t rowPitch = 0;
        std::unique_lock<std::mutex> lock;
        Svga3VlknStatus st = surf->dmaDownloadToStaging(0, nullptr, &mappedData, &rowPitch, lock);
        if (st != SVGA3_VLKN_SUCCESS) {
            return st;
        }

        uint32_t dstW = fb.width ? fb.width : surfW;
        uint32_t dstH = fb.height ? fb.height : surfH;
        uint32_t dstBpp = fb.bpp ? fb.bpp : 4;
        uint32_t dstPitch = fb.pitch ? fb.pitch : (dstW * dstBpp);

        static uint32_t log_pres = 0;
        log_pres++;
        uint32_t p0 = mappedData ? *reinterpret_cast<const uint32_t*>(mappedData) : 0;
        if (log_pres <= 5 || (log_pres % 500) == 0) {
            log_msg("[libqemu_svga3d] VlknSurfaceManager::present #%u: sid=%u, surf=%ux%u, fb=%ux%u, numRects=%u, p0=0x%08x\n",
                    log_pres, sid, surfW, surfH, dstW, dstH, numRects, p0);
        }

        uint32_t copyW = std::min(surfW, dstW);
        uint32_t copyH = std::min(surfH, dstH);
        bool isBlank = (p0 == 0 || (p0 & 0x00FFFFFF) == 0) &&
                       is_buffer_all_black_or_zero(mappedData, copyW, copyH, rowPitch, bpp);

        if (isBlank) {
            static uint32_t blank_warn = 0;
            if (blank_warn++ < 5 || (blank_warn % 500) == 0) {
                log_msg("[libqemu_svga3d] WARNING: present sid=%u is completely blank/zero, preserving fb.hva\n", sid);
            }
            guestMem->notifyDisplayUpdate(0, 0, copyW, copyH);
        } else {
            if (numRects == 0 || !rects) {
                size_t bytesToCopy = copyW * std::min(bpp, static_cast<size_t>(dstBpp));
                for (uint32_t y = 0; y < copyH; ++y) {
                    /* 64-bit offset: the old 32-bit y*dstPitch wrapped for
                     * large pitches. Skip rows outside the framebuffer. */
                    uint64_t dstOff = (uint64_t)y * dstPitch;
                    if (dstOff > fb.size || bytesToCopy > fb.size - dstOff) {
                        continue;
                    }
                    uint8_t *dst = fb.hva + (size_t)dstOff;
                    const uint8_t *src = static_cast<const uint8_t*>(mappedData) + y * rowPitch;
                    memcpy(dst, src, bytesToCopy);
                }
                guestMem->notifyDisplayUpdate(0, 0, copyW, copyH);
            } else {
                for (uint32_t i = 0; i < numRects; ++i) {
                    const auto &r = rects[i];
                    if (r.srcx >= surfW || r.srcy >= surfH) continue;
                    if (r.x >= dstW || r.y >= dstH) continue;

                    uint32_t cw = std::min(r.w, surfW - r.srcx);
                    cw = std::min(cw, dstW - r.x);
                    uint32_t ch = std::min(r.h, surfH - r.srcy);
                    ch = std::min(ch, dstH - r.y);

                    size_t bytesToCopy = cw * std::min(bpp, static_cast<size_t>(dstBpp));
                    for (uint32_t y = 0; y < ch; ++y) {
                        /* 64-bit offset: the old 32-bit product wrapped for
                         * large pitches. Skip rows outside the framebuffer. */
                        uint64_t dstOff = ((uint64_t)r.y + y) * dstPitch + (uint64_t)r.x * dstBpp;
                        if (dstOff > fb.size || bytesToCopy > fb.size - dstOff) {
                            continue;
                        }
                        uint8_t *dst = fb.hva + (size_t)dstOff;
                        const uint8_t *src = static_cast<const uint8_t*>(mappedData) + (r.srcy + y) * rowPitch + r.srcx * bpp;
                        memcpy(dst, src, bytesToCopy);
                    }
                    guestMem->notifyDisplayUpdate(r.x, r.y, cw, ch);
                }
            }
        }
    }

    m_backend->flushCommandBuffer();
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknSurfaceManager::setSurfaceActive(uint32_t sid, bool active) {
    VlknSurface *surf = getSurface(sid);
    if (!surf) return SVGA3_VLKN_ERROR_NOT_FOUND;
    surf->setActive(active);
    return SVGA3_VLKN_SUCCESS;
}

} // namespace svga3_vlkn
