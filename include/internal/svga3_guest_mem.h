/*
 * SVGA3=VLKN - Guest Memory & GMR Management Subsystem
 *
 * Header: svga3_guest_mem.h
 * Description: Real Guest Physical Address (GPA) to Host Virtual Address (HVA)
 *              translation, GMR descriptor tables, multi-page boundary crossing,
 *              noncontiguous physical page scatter-gather, and safe bounds validation.
 */

#ifndef ___SVGA3_GUEST_MEM_H___
#define ___SVGA3_GUEST_MEM_H___

#include "svga3_vlkn.h"
#include <vector>
#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <algorithm>

namespace svga3_vlkn {

constexpr size_t SVGA3_PAGE_SHIFT = 12;
constexpr size_t SVGA3_PAGE_SIZE  = (1ULL << SVGA3_PAGE_SHIFT); /* 4096 bytes */
constexpr size_t SVGA3_PAGE_MASK  = (SVGA3_PAGE_SIZE - 1);
constexpr uint32_t SVGA3_MAX_GMR_IDS = 1024;
constexpr uint32_t SVGA3_MAX_GMR_PAGES = 1048576; /* Up to 4GB per GMR */
constexpr uint32_t SVGA3_MAX_DESCRIPTOR_CHAIN = 2048;

struct RamBlock {
    uint64_t gpaBase;
    uint8_t *hvaBase;
    size_t   size;
};

struct GuestMemoryRegion {
    uint32_t gmrId = 0;
    uint32_t numPages = 0;
    std::vector<uint64_t> ppns;
    bool isDefined = false;
};

struct FramebufferInfo {
    uint8_t *hva = nullptr;
    uint64_t gpa = 0;
    size_t   size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;
    uint32_t bpp = 4;
    uint32_t scanoutOffset = 0;

    uint8_t* scanoutHva() const {
        return (hva && scanoutOffset < size) ? (hva + scanoutOffset) : hva;
    }
    size_t scanoutSize() const {
        return (size > scanoutOffset) ? (size - scanoutOffset) : size;
    }
};

class GuestMemoryManager {
public:
    GuestMemoryManager();
    ~GuestMemoryManager();

    /* RAM Block Registration (for testbeds or direct host RAM mappings) */
    Svga3VlknStatus registerRamBlock(uint64_t gpaBase, void *hvaBase, size_t size);
    Svga3VlknStatus unregisterRamBlock(uint64_t gpaBase);
    void clearRamBlocks();
    void clear();

    /* Host Adapter (replaces the old QEMU-specific callbacks) */
    void setAdapter(const Svga3HostAdapter *adapter);
    void notifyDisplayUpdate(int32_t x, int32_t y, int32_t w, int32_t h);

    /* Framebuffer Mapping (BAR1) */
    Svga3VlknStatus setFramebuffer(void *hva,
                                  uint64_t gpa,
                                  size_t size,
                                  uint32_t width,
                                  uint32_t height,
                                  uint32_t pitch,
                                  uint32_t bpp,
                                  uint32_t scanoutOffset = 0);
    Svga3VlknStatus setScanoutOffset(uint32_t offset);
    const FramebufferInfo& getFramebuffer() const { return m_fb; }

    /* GMR Management */
    Svga3VlknStatus defineGMR2(uint32_t gmrId, uint32_t numPages);
    Svga3VlknStatus remapGMR2(uint32_t gmrId,
                             uint32_t flags,
                             uint32_t offsetPages,
                             uint32_t numPages,
                             const void *descriptors,
                             size_t descriptorBytes,
                             size_t *outBytesConsumed = nullptr);
    Svga3VlknStatus registerLegacyGMR(uint32_t gmrId, uint32_t descriptorPPN);
    Svga3VlknStatus destroyGMR(uint32_t gmrId);
    bool hasGMR(uint32_t gmrId) const;
    size_t getGMRNumPages(uint32_t gmrId) const;

    /* Scatter-gather transfer across page boundaries & noncontiguous physical pages */
    Svga3VlknStatus readGuest(const SVGAGuestPtr &ptr, void *dstHost, size_t size);
    Svga3VlknStatus writeGuest(const SVGAGuestPtr &ptr, const void *srcHost, size_t size);

    /* Direct Physical Memory Access (GPA to HVA) */
    bool readPhysical(uint64_t gpa, void *dstHost, size_t size);
    bool writePhysical(uint64_t gpa, const void *srcHost, size_t size);
    void* gpaToHva(uint64_t gpa, size_t size, bool isWrite);

private:
    bool readPhysicalLocked(uint64_t gpa, void *dstHost, size_t size);
    bool writePhysicalLocked(uint64_t gpa, const void *srcHost, size_t size);

    mutable std::mutex m_mutex;
    std::vector<RamBlock> m_ramBlocks;
    std::unordered_map<uint32_t, GuestMemoryRegion> m_gmrs;
    FramebufferInfo m_fb;

    Svga3HostAdapter m_adapter{};
    bool m_hasAdapter = false;
};

} // namespace svga3_vlkn

#endif /* ___SVGA3_GUEST_MEM_H___ */
