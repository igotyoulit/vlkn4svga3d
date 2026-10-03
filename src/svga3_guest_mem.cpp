/*
 * SVGA3=VLKN - Guest Memory & GMR Management Subsystem Implementation
 *
 * Source: svga3_guest_mem.cpp
 */

#include "svga3_guest_mem.h"
#include <iostream>

extern "C" void log_msg(const char *fmt, ...);

namespace svga3_vlkn {

/* Overflow-safe range containment: [gpa, gpa+size) \subseteq [base, base+extent).
 * Structured so that no addition can wrap: gpa and size are guest-controlled.
 * All-unsigned; the subtractions are safe because of the preceding comparisons. */
static bool rangeWithin(uint64_t gpa, size_t size, uint64_t base, uint64_t extent)
{
    if (gpa < base) {
        return false;
    }
    uint64_t off = gpa - base; /* safe: gpa >= base */
    if (off > extent) {
        return false;
    }
    /* safe: off <= extent, so extent - off cannot wrap */
    return static_cast<uint64_t>(size) <= extent - off;
}

GuestMemoryManager::GuestMemoryManager() {
    m_fb = FramebufferInfo{};
}

GuestMemoryManager::~GuestMemoryManager() {
    clearRamBlocks();
}

Svga3VlknStatus GuestMemoryManager::registerRamBlock(uint64_t gpaBase, void *hvaBase, size_t size) {
    if (!hvaBase || size == 0) {
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }
    /* Reject a wrapping [gpaBase, gpaBase+size) range outright: every range
     * check below assumes block extents do not wrap. */
    if (size > UINT64_MAX - gpaBase) {
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }
    std::lock_guard<std::mutex> lock(m_mutex);

    /* Check for overlapping blocks */
    for (const auto &b : m_ramBlocks) {
        uint64_t bEnd = b.gpaBase + b.size; /* safe: existing blocks never wrap */
        uint64_t newEnd = gpaBase + size;   /* safe: checked above */
        if (gpaBase < bEnd && newEnd > b.gpaBase) {
            return SVGA3_VLKN_ERROR_ALREADY_EXISTS;
        }
    }

    RamBlock block;
    block.gpaBase = gpaBase;
    block.hvaBase = reinterpret_cast<uint8_t*>(hvaBase);
    block.size = size;
    m_ramBlocks.push_back(block);
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus GuestMemoryManager::unregisterRamBlock(uint64_t gpaBase) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_ramBlocks.begin(); it != m_ramBlocks.end(); ++it) {
        if (it->gpaBase == gpaBase) {
            m_ramBlocks.erase(it);
            return SVGA3_VLKN_SUCCESS;
        }
    }
    return SVGA3_VLKN_ERROR_NOT_FOUND;
}

void GuestMemoryManager::clearRamBlocks() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_ramBlocks.clear();
}

void GuestMemoryManager::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_ramBlocks.clear();
    m_gmrs.clear();
}

void GuestMemoryManager::setCallbacks(void *opaque,
                                      Svga3GpaToHvaFn gpaToHva,
                                      Svga3DmaReadFn dmaRead,
                                      Svga3DmaWriteFn dmaWrite)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cbOpaque = opaque;
    m_gpaToHva = gpaToHva;
    m_dmaRead  = dmaRead;
    m_dmaWrite = dmaWrite;
}

void GuestMemoryManager::setDisplayCallback(void *opaque, Svga3DisplayUpdateFn displayUpdate)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_displayOpaque = opaque;
    m_displayUpdate = displayUpdate;
}

void GuestMemoryManager::notifyDisplayUpdate(int32_t x, int32_t y, int32_t w, int32_t h)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_displayUpdate && w > 0 && h > 0) {
        m_displayUpdate(m_displayOpaque, x, y, w, h);
    }
}

Svga3VlknStatus GuestMemoryManager::setFramebuffer(void *hva,
                                                  uint64_t gpa,
                                                  size_t size,
                                                  uint32_t width,
                                                  uint32_t height,
                                                  uint32_t pitch,
                                                  uint32_t bpp,
                                                  uint32_t scanoutOffset)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    /* Validate before accepting: the blit/present paths do raw pointer
     * arithmetic against these fields, so inconsistent values would be a
     * host-side out-of-bounds write. A zero bpp is rejected outright rather
     * than defaulted. All products are computed in 64-bit; with 32-bit
     * inputs they cannot wrap. */
    if (bpp == 0) {
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }
    uint32_t bytesPerPixel = (bpp > 8) ? (bpp / 8) : bpp;
    if (!hva || width == 0 || height == 0 || pitch == 0 ||
        bytesPerPixel == 0 || size == 0) {
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }
    uint64_t rowBytes = static_cast<uint64_t>(width) * bytesPerPixel;
    if (pitch < rowBytes) {
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }
    uint64_t fbBytes = static_cast<uint64_t>(pitch) * height;
    if (scanoutOffset >= size || fbBytes > (size - scanoutOffset)) {
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }
    m_fb.hva = reinterpret_cast<uint8_t*>(hva);
    m_fb.gpa = gpa;
    m_fb.size = size;
    m_fb.width = width;
    m_fb.height = height;
    m_fb.pitch = pitch;
    m_fb.bpp = bytesPerPixel;
    m_fb.scanoutOffset = scanoutOffset;
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus GuestMemoryManager::setScanoutOffset(uint32_t offset) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const uint64_t footprint = uint64_t(m_fb.pitch) * m_fb.height;
    if (!m_fb.hva || offset >= m_fb.size || footprint > m_fb.size - offset)
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    m_fb.scanoutOffset = offset;
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus GuestMemoryManager::defineGMR2(uint32_t gmrId, uint32_t numPages) {
    if (gmrId >= SVGA3_MAX_GMR_IDS || numPages > SVGA3_MAX_GMR_PAGES) {
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    GuestMemoryRegion &region = m_gmrs[gmrId];
    region.gmrId = gmrId;
    region.numPages = numPages;
    region.ppns.assign(numPages, 0);
    region.isDefined = true;
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus GuestMemoryManager::remapGMR2(uint32_t gmrId,
                                             uint32_t flags,
                                             uint32_t offsetPages,
                                             uint32_t numPages,
                                             const void *descriptors,
                                             size_t descriptorBytes,
                                             size_t *outBytesConsumed)
{
    if (outBytesConsumed) *outBytesConsumed = 0;
    if (gmrId >= SVGA3_MAX_GMR_IDS) {
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_gmrs.find(gmrId);
    if (it == m_gmrs.end() || !it->second.isDefined) {
        return SVGA3_VLKN_ERROR_NOT_FOUND;
    }

    GuestMemoryRegion &region = it->second;
    if (offsetPages > region.numPages || numPages > (region.numPages - offsetPages)) {
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }

    if (numPages == 0) {
        return SVGA3_VLKN_SUCCESS;
    }

    bool isPPN64 = (flags & SVGA_REMAP_GMR2_PPN64) != 0;
    bool isSinglePPN = (flags & SVGA_REMAP_GMR2_SINGLE_PPN) != 0;
    bool isViaGMR = (flags & SVGA_REMAP_GMR2_VIA_GMR) != 0;

    if (isSinglePPN) {
        uint64_t singlePpn = 0;
        if (isPPN64) {
            if (descriptorBytes < sizeof(uint64_t)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            singlePpn = *reinterpret_cast<const uint64_t*>(descriptors);
            if (outBytesConsumed) *outBytesConsumed = sizeof(uint64_t);
        } else {
            if (descriptorBytes < sizeof(uint32_t)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            singlePpn = *reinterpret_cast<const uint32_t*>(descriptors);
            if (outBytesConsumed) *outBytesConsumed = sizeof(uint32_t);
        }

        for (uint32_t i = 0; i < numPages; ++i) {
            region.ppns[offsetPages + i] = singlePpn + i;
        }
        return SVGA3_VLKN_SUCCESS;
    }

    if (isViaGMR) {
        if (descriptorBytes < sizeof(SVGAGuestPtr)) {
            return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
        }
        const auto &srcPtr = *reinterpret_cast<const SVGAGuestPtr*>(descriptors);
        if (outBytesConsumed) *outBytesConsumed = sizeof(SVGAGuestPtr);

        if (isPPN64) {
            std::vector<uint64_t> pageList(numPages);
            size_t bytesToRead = numPages * sizeof(uint64_t);
            // Read using internal unlock/relock or helper
            // Note: readGuest acquires m_mutex, so call locked variant
            size_t bytesLeft = bytesToRead;
            size_t curOffset = srcPtr.offset;
            uint8_t *curDst = reinterpret_cast<uint8_t*>(pageList.data());

            auto srcIt = m_gmrs.find(srcPtr.gmrId);
            if (srcIt == m_gmrs.end() || !srcIt->second.isDefined) {
                return SVGA3_VLKN_ERROR_NOT_FOUND;
            }
            const auto &srcRegion = srcIt->second;
            if ((uint64_t)srcPtr.offset + bytesToRead > (uint64_t)srcRegion.numPages * SVGA3_PAGE_SIZE) {
                return SVGA3_VLKN_ERROR_INVALID_PARAM;
            }

            while (bytesLeft > 0) {
                uint32_t pIdx = curOffset / SVGA3_PAGE_SIZE;
                uint32_t pOff = curOffset % SVGA3_PAGE_SIZE;
                uint64_t ppn = srcRegion.ppns[pIdx];
                if (ppn == 0) return SVGA3_VLKN_ERROR_INVALID_PARAM;
                size_t chunk = std::min(bytesLeft, static_cast<size_t>(SVGA3_PAGE_SIZE - pOff));
                uint64_t gpa = (ppn << SVGA3_PAGE_SHIFT) + pOff;
                if (!readPhysicalLocked(gpa, curDst, chunk)) return SVGA3_VLKN_ERROR_INVALID_PARAM;
                bytesLeft -= chunk;
                curOffset += chunk;
                curDst += chunk;
            }

            for (uint32_t i = 0; i < numPages; ++i) {
                region.ppns[offsetPages + i] = pageList[i];
            }
        } else {
            std::vector<uint32_t> pageList(numPages);
            size_t bytesToRead = numPages * sizeof(uint32_t);
            size_t bytesLeft = bytesToRead;
            size_t curOffset = srcPtr.offset;
            uint8_t *curDst = reinterpret_cast<uint8_t*>(pageList.data());

            auto srcIt = m_gmrs.find(srcPtr.gmrId);
            if (srcIt == m_gmrs.end() || !srcIt->second.isDefined) {
                return SVGA3_VLKN_ERROR_NOT_FOUND;
            }
            const auto &srcRegion = srcIt->second;
            if ((uint64_t)srcPtr.offset + bytesToRead > (uint64_t)srcRegion.numPages * SVGA3_PAGE_SIZE) {
                return SVGA3_VLKN_ERROR_INVALID_PARAM;
            }

            while (bytesLeft > 0) {
                uint32_t pIdx = curOffset / SVGA3_PAGE_SIZE;
                uint32_t pOff = curOffset % SVGA3_PAGE_SIZE;
                uint64_t ppn = srcRegion.ppns[pIdx];
                if (ppn == 0) return SVGA3_VLKN_ERROR_INVALID_PARAM;
                size_t chunk = std::min(bytesLeft, static_cast<size_t>(SVGA3_PAGE_SIZE - pOff));
                uint64_t gpa = (ppn << SVGA3_PAGE_SHIFT) + pOff;
                if (!readPhysicalLocked(gpa, curDst, chunk)) return SVGA3_VLKN_ERROR_INVALID_PARAM;
                bytesLeft -= chunk;
                curOffset += chunk;
                curDst += chunk;
            }

            for (uint32_t i = 0; i < numPages; ++i) {
                region.ppns[offsetPages + i] = pageList[i];
            }
        }
        return SVGA3_VLKN_SUCCESS;
    }

    /* Direct PPN array in command buffer */
    if (isPPN64) {
        size_t requiredBytes = numPages * sizeof(uint64_t);
        if (descriptorBytes < requiredBytes) {
            return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
        }
        const auto *ppnArray = reinterpret_cast<const uint64_t*>(descriptors);
        for (uint32_t i = 0; i < numPages; ++i) {
            region.ppns[offsetPages + i] = ppnArray[i];
        }
        if (outBytesConsumed) *outBytesConsumed = requiredBytes;
    } else {
        size_t requiredBytes = numPages * sizeof(uint32_t);
        if (descriptorBytes < requiredBytes) {
            return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
        }
        const auto *ppnArray = reinterpret_cast<const uint32_t*>(descriptors);
        for (uint32_t i = 0; i < numPages; ++i) {
            region.ppns[offsetPages + i] = ppnArray[i];
        }
        if (outBytesConsumed) *outBytesConsumed = requiredBytes;
    }

    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus GuestMemoryManager::registerLegacyGMR(uint32_t gmrId, uint32_t descriptorPPN) {
    if (gmrId >= SVGA3_MAX_GMR_IDS) {
        log_msg("[libqemu_svga3d] registerLegacyGMR error: gmrId=%u >= max %u\n", gmrId, SVGA3_MAX_GMR_IDS);
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (descriptorPPN == 0) {
        m_gmrs.erase(gmrId);
        log_msg("[libqemu_svga3d] GMR id=%u unregistered\n", gmrId);
        return SVGA3_VLKN_SUCCESS;
    }

    uint64_t curGpa = static_cast<uint64_t>(descriptorPPN) << SVGA3_PAGE_SHIFT;
    std::vector<uint64_t> ppns;
    uint32_t descCount = 0;

    while (descCount < SVGA3_MAX_DESCRIPTOR_CHAIN) {
        SVGAGuestMemDescriptor desc = {};
        if (!readPhysicalLocked(curGpa, &desc, sizeof(desc))) {
            log_msg("[libqemu_svga3d] registerLegacyGMR error: readPhysicalLocked failed at GPA 0x%lx (gmrId=%u, descCount=%u)\n",
                    (unsigned long)curGpa, gmrId, descCount);
            return SVGA3_VLKN_ERROR_INVALID_PARAM;
        }
        curGpa += sizeof(desc);
        descCount++;

        if (desc.ppn == 0 && desc.numPages == 0) {
            break; /* End of descriptor list */
        }

        if (desc.ppn != 0 && desc.numPages != 0) {
            for (uint32_t p = 0; p < desc.numPages; ++p) {
                ppns.push_back(static_cast<uint64_t>(desc.ppn) + p);
                if (ppns.size() > SVGA3_MAX_GMR_PAGES) {
                    log_msg("[libqemu_svga3d] registerLegacyGMR error: ppns.size() %zu > max %u (gmrId=%u)\n",
                            ppns.size(), SVGA3_MAX_GMR_PAGES, gmrId);
                    return SVGA3_VLKN_ERROR_INVALID_PARAM;
                }
            }
            /* The SVGA device will never automatically cross a page boundary.
             * If curGpa reaches a 4KB page boundary, we must terminate unless
             * an explicit jump descriptor redirect was encountered. */
            if ((curGpa & (SVGA3_PAGE_SIZE - 1)) == 0) {
                break;
            }
        } else if (desc.ppn != 0 && desc.numPages == 0) {
            /* PPN points to the next page of descriptors */
            curGpa = static_cast<uint64_t>(desc.ppn) << SVGA3_PAGE_SHIFT;
        } else {
            /* Invalid or terminating descriptor (ppn == 0 && numPages != 0), terminate */
            log_msg("[libqemu_svga3d] registerLegacyGMR: invalid descriptor ppn=0, numPages=%u, terminating chain\n",
                    desc.numPages);
            break;
        }
    }

    GuestMemoryRegion &region = m_gmrs[gmrId];
    region.gmrId = gmrId;
    region.numPages = static_cast<uint32_t>(ppns.size());
    region.ppns = std::move(ppns);
    region.isDefined = true;
    log_msg("[libqemu_svga3d] GMR id=%u registered: %u pages (%zu KB), %u descs parsed\n",
            gmrId, region.numPages, (size_t)region.numPages * 4, descCount);
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus GuestMemoryManager::destroyGMR(uint32_t gmrId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_gmrs.find(gmrId);
    if (it != m_gmrs.end()) {
        m_gmrs.erase(it);
        return SVGA3_VLKN_SUCCESS;
    }
    return SVGA3_VLKN_ERROR_NOT_FOUND;
}

bool GuestMemoryManager::hasGMR(uint32_t gmrId) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_gmrs.find(gmrId);
    return (it != m_gmrs.end() && it->second.isDefined);
}

size_t GuestMemoryManager::getGMRNumPages(uint32_t gmrId) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_gmrs.find(gmrId);
    if (it != m_gmrs.end() && it->second.isDefined) {
        return it->second.numPages;
    }
    return 0;
}

Svga3VlknStatus GuestMemoryManager::readGuest(const SVGAGuestPtr &ptr, void *dstHost, size_t size) {
    if (size == 0) return SVGA3_VLKN_SUCCESS;
    if (!dstHost) return SVGA3_VLKN_ERROR_INVALID_PARAM;

    std::lock_guard<std::mutex> lock(m_mutex);

    if (ptr.gmrId == SVGA_GMR_NULL) {
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }

    if (ptr.gmrId == SVGA_GMR_FRAMEBUFFER) {
        if (!m_fb.hva || ptr.offset >= m_fb.size || size > (m_fb.size - ptr.offset)) {
            return SVGA3_VLKN_ERROR_INVALID_PARAM;
        }
        memcpy(dstHost, m_fb.hva + ptr.offset, size);
        return SVGA3_VLKN_SUCCESS;
    }

    auto it = m_gmrs.find(ptr.gmrId);
    if (it == m_gmrs.end() || !it->second.isDefined) {
        static uint32_t not_found_cnt = 0;
        if (++not_found_cnt <= 10) {
            log_msg("[libqemu_svga3d] readGuest error: GMR id=%u not found or not defined\n", ptr.gmrId);
        }
        return SVGA3_VLKN_ERROR_NOT_FOUND;
    }

    const auto &region = it->second;
    uint64_t totalBytes = static_cast<uint64_t>(region.numPages) * SVGA3_PAGE_SIZE;
    if (static_cast<uint64_t>(ptr.offset) + size > totalBytes) {
        static uint32_t oob_cnt = 0;
        if (++oob_cnt <= 10) {
            log_msg("[libqemu_svga3d] readGuest error: GMR id=%u offset %u + size %zu > totalBytes %lu\n",
                    ptr.gmrId, ptr.offset, size, (unsigned long)totalBytes);
        }
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }

    size_t bytesLeft = size;
    size_t curOffset = ptr.offset;
    uint8_t *curDst = reinterpret_cast<uint8_t*>(dstHost);

    while (bytesLeft > 0) {
        uint32_t pageIndex = static_cast<uint32_t>(curOffset / SVGA3_PAGE_SIZE);
        uint32_t pageOffset = static_cast<uint32_t>(curOffset % SVGA3_PAGE_SIZE);
        if (pageIndex >= region.ppns.size()) {
            static uint32_t page_idx_err_cnt = 0;
            if (++page_idx_err_cnt <= 10) {
                log_msg("[libqemu_svga3d] readGuest error: pageIndex %u >= ppns.size() %zu\n",
                        pageIndex, region.ppns.size());
            }
            return SVGA3_VLKN_ERROR_INVALID_PARAM;
        }

        uint64_t ppn = region.ppns[pageIndex];
        if (ppn == 0) {
            static uint32_t ppn_zero_cnt = 0;
            if (++ppn_zero_cnt <= 10) {
                log_msg("[libqemu_svga3d] readGuest error: ppn is 0 at pageIndex %u\n", pageIndex);
            }
            return SVGA3_VLKN_ERROR_INVALID_PARAM;
        }

        size_t chunk = std::min(bytesLeft, static_cast<size_t>(SVGA3_PAGE_SIZE - pageOffset));
        uint64_t gpa = (ppn << SVGA3_PAGE_SHIFT) + pageOffset;

        if (!readPhysicalLocked(gpa, curDst, chunk)) {
            static uint32_t read_phys_err_cnt = 0;
            if (++read_phys_err_cnt <= 10) {
                log_msg("[libqemu_svga3d] readGuest error: readPhysicalLocked failed for GPA 0x%lx\n",
                        (unsigned long)gpa);
            }
            return SVGA3_VLKN_ERROR_INVALID_PARAM;
        }

        bytesLeft -= chunk;
        curOffset += chunk;
        curDst += chunk;
    }

    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus GuestMemoryManager::writeGuest(const SVGAGuestPtr &ptr, const void *srcHost, size_t size) {
    if (size == 0) return SVGA3_VLKN_SUCCESS;
    if (!srcHost) return SVGA3_VLKN_ERROR_INVALID_PARAM;

    std::lock_guard<std::mutex> lock(m_mutex);

    if (ptr.gmrId == SVGA_GMR_NULL) {
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }

    if (ptr.gmrId == SVGA_GMR_FRAMEBUFFER) {
        if (!m_fb.hva || ptr.offset >= m_fb.size || size > (m_fb.size - ptr.offset)) {
            return SVGA3_VLKN_ERROR_INVALID_PARAM;
        }
        memcpy(m_fb.hva + ptr.offset, srcHost, size);
        return SVGA3_VLKN_SUCCESS;
    }

    auto it = m_gmrs.find(ptr.gmrId);
    if (it == m_gmrs.end() || !it->second.isDefined) {
        return SVGA3_VLKN_ERROR_NOT_FOUND;
    }

    const auto &region = it->second;
    uint64_t totalBytes = static_cast<uint64_t>(region.numPages) * SVGA3_PAGE_SIZE;
    if (static_cast<uint64_t>(ptr.offset) + size > totalBytes) {
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }

    size_t bytesLeft = size;
    size_t curOffset = ptr.offset;
    const uint8_t *curSrc = reinterpret_cast<const uint8_t*>(srcHost);

    while (bytesLeft > 0) {
        uint32_t pageIndex = static_cast<uint32_t>(curOffset / SVGA3_PAGE_SIZE);
        uint32_t pageOffset = static_cast<uint32_t>(curOffset % SVGA3_PAGE_SIZE);
        if (pageIndex >= region.ppns.size()) {
            return SVGA3_VLKN_ERROR_INVALID_PARAM;
        }

        uint64_t ppn = region.ppns[pageIndex];
        if (ppn == 0) {
            return SVGA3_VLKN_ERROR_INVALID_PARAM;
        }

        size_t chunk = std::min(bytesLeft, static_cast<size_t>(SVGA3_PAGE_SIZE - pageOffset));
        uint64_t gpa = (ppn << SVGA3_PAGE_SHIFT) + pageOffset;

        if (!writePhysicalLocked(gpa, curSrc, chunk)) {
            return SVGA3_VLKN_ERROR_INVALID_PARAM;
        }

        bytesLeft -= chunk;
        curOffset += chunk;
        curSrc += chunk;
    }

    return SVGA3_VLKN_SUCCESS;
}

bool GuestMemoryManager::readPhysical(uint64_t gpa, void *dstHost, size_t size) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return readPhysicalLocked(gpa, dstHost, size);
}

bool GuestMemoryManager::writePhysical(uint64_t gpa, const void *srcHost, size_t size) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return writePhysicalLocked(gpa, srcHost, size);
}

bool GuestMemoryManager::readPhysicalLocked(uint64_t gpa, void *dstHost, size_t size) {
    if (size == 0) return true;

    /* 1. Direct RAM block lookup */
    for (const auto &block : m_ramBlocks) {
        if (rangeWithin(gpa, size, block.gpaBase, block.size)) {
            memcpy(dstHost, block.hvaBase + (gpa - block.gpaBase), size);
            return true;
        }
    }

    /* 2. Framebuffer range */
    if (m_fb.hva && rangeWithin(gpa, size, m_fb.gpa, m_fb.size)) {
        memcpy(dstHost, m_fb.hva + (gpa - m_fb.gpa), size);
        return true;
    }

    /* 3. Translation callback */
    if (m_gpaToHva) {
        void *hva = m_gpaToHva(m_cbOpaque, gpa, size, false);
        if (hva) {
            memcpy(dstHost, hva, size);
            return true;
        }
    }

    /* 4. Direct DMA read callback */
    if (m_dmaRead) {
        return m_dmaRead(m_cbOpaque, gpa, dstHost, size);
    }

    return false;
}

bool GuestMemoryManager::writePhysicalLocked(uint64_t gpa, const void *srcHost, size_t size) {
    if (size == 0) return true;

    /* 1. Direct RAM block lookup */
    for (const auto &block : m_ramBlocks) {
        if (rangeWithin(gpa, size, block.gpaBase, block.size)) {
            memcpy(block.hvaBase + (gpa - block.gpaBase), srcHost, size);
            return true;
        }
    }

    /* 2. Framebuffer range */
    if (m_fb.hva && rangeWithin(gpa, size, m_fb.gpa, m_fb.size)) {
        memcpy(m_fb.hva + (gpa - m_fb.gpa), srcHost, size);
        return true;
    }

    /* 3. Translation callback */
    if (m_gpaToHva) {
        void *hva = m_gpaToHva(m_cbOpaque, gpa, size, true);
        if (hva) {
            memcpy(hva, srcHost, size);
            return true;
        }
    }

    /* 4. Direct DMA write callback */
    if (m_dmaWrite) {
        return m_dmaWrite(m_cbOpaque, gpa, srcHost, size);
    }

    return false;
}

void* GuestMemoryManager::gpaToHva(uint64_t gpa, size_t size, bool isWrite) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto &block : m_ramBlocks) {
        if (rangeWithin(gpa, size, block.gpaBase, block.size)) {
            return block.hvaBase + (gpa - block.gpaBase);
        }
    }
    if (m_fb.hva && rangeWithin(gpa, size, m_fb.gpa, m_fb.size)) {
        return m_fb.hva + (gpa - m_fb.gpa);
    }
    if (m_gpaToHva) {
        return m_gpaToHva(m_cbOpaque, gpa, size, isWrite);
    }
    return nullptr;
}

} // namespace svga3_vlkn
