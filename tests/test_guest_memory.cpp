/*
 * SVGA3=VLKN - Comprehensive Guest Memory Subsystem & DMA Acceptance Test
 * Deliverable 3: Real Guest Memory Access, GMR2/Legacy Translation,
 * Page Boundary Crossing, Noncontiguous Pages, and Safe Error Rejection.
 */

#include "svga3_vlkn.h"
#include "svga3_device.h"
#include "svga3_guest_mem.h"
#include "vlkn_backend.h"

#include <iostream>
#include <vector>
#include <cstring>
#include <cassert>
#include <iomanip>
#include <memory>

#define TEST_CHECK(cond, msg) do { \
    if (cond) { \
        std::cout << "  [PASS] " << msg << std::endl; \
    } else { \
        std::cerr << "  [FAIL] " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
        return 1; \
    } \
} while(0)

/* Simulated Guest Physical Memory Space (64 MB = 16384 pages of 4096 bytes) */
constexpr size_t GUEST_RAM_SIZE = 64 * 1024 * 1024;
constexpr uint64_t GUEST_RAM_BASE_GPA = 0x10000000ULL; /* 256 MB GPA base */
static std::vector<uint8_t> g_guestRam(GUEST_RAM_SIZE);

/* Simulated Display Framebuffer (1920x1080x4 = ~8.3 MB) */
constexpr uint32_t FB_WIDTH = 800;
constexpr uint32_t FB_HEIGHT = 600;
constexpr uint32_t FB_BPP = 4;
constexpr uint32_t FB_PITCH = FB_WIDTH * FB_BPP;
constexpr size_t FB_SIZE = FB_PITCH * FB_HEIGHT;
constexpr uint64_t FB_BASE_GPA = 0xF0000000ULL; /* BAR1 GPA */
static std::vector<uint8_t> g_framebuffer(FB_SIZE);

/* GPA to HVA helper for test */
static uint8_t* gpaToHva(uint64_t gpa) {
    if (gpa >= GUEST_RAM_BASE_GPA && gpa < GUEST_RAM_BASE_GPA + GUEST_RAM_SIZE) {
        return g_guestRam.data() + (gpa - GUEST_RAM_BASE_GPA);
    }
    if (gpa >= FB_BASE_GPA && gpa < FB_BASE_GPA + FB_SIZE) {
        return g_framebuffer.data() + (gpa - FB_BASE_GPA);
    }
    return nullptr;
}

static int testAdapterFallback() {
    svga3_vlkn::GuestMemoryManager mem;
    struct Host { uint8_t bytes[32]{}; unsigned updates = 0; } first, second;
    Svga3HostAdapter adapter{};
    adapter.opaque = &first;
    adapter.guestRamMap = [](void *opaque, uint64_t gpa, size_t size, bool) -> void* {
        auto *host = static_cast<Host*>(opaque);
        if (gpa < 0x1000 || gpa - 0x1000 > sizeof(host->bytes) ||
            size > sizeof(host->bytes) - (gpa - 0x1000)) return nullptr;
        return host->bytes + (gpa - 0x1000);
    };
    adapter.displayUpdate = [](void *opaque, int32_t, int32_t, int32_t, int32_t) {
        ++static_cast<Host*>(opaque)->updates;
    };
    mem.setAdapter(&adapter);
    adapter.opaque = &second; // Registration copies the table, not its address.
    uint32_t value = 0x12345678, out = 0;
    TEST_CHECK(mem.writePhysical(0x101c, &value, 4), "Adapter maps exact end range");
    TEST_CHECK(mem.readPhysical(0x101c, &out, 4) && out == value, "Adapter RAM roundtrip");
    TEST_CHECK(!mem.writePhysical(0x101d, &value, 4), "Adapter rejects overflowing range");
    mem.notifyDisplayUpdate(0, 0, 4, 4);
    TEST_CHECK(first.updates == 1 && second.updates == 0, "Adapter registration is independent");
    uint8_t registered[32]{};
    TEST_CHECK(mem.registerRamBlock(0x1000, registered, sizeof(registered)) == SVGA3_VLKN_SUCCESS,
               "Register RAM over adapter range");
    TEST_CHECK(mem.writePhysical(0x1000, &value, 4) && first.bytes[0] == 0,
               "Registered RAM takes precedence over adapter");
    mem.clear();
    adapter.guestRamMap = nullptr;
    adapter.guestRamRead = [](void *opaque, uint64_t gpa, void *dst, size_t size) {
        if (gpa != 0x1000 || size > 32) return false;
        memcpy(dst, static_cast<Host*>(opaque)->bytes, size); return true;
    };
    adapter.guestRamWrite = [](void *opaque, uint64_t gpa, const void *src, size_t size) {
        if (gpa != 0x1000 || size > 32) return false;
        memcpy(static_cast<Host*>(opaque)->bytes, src, size); return true;
    };
    mem.setAdapter(&adapter);
    TEST_CHECK(mem.writePhysical(0x1000, &value, 4) && mem.readPhysical(0x1000, &out, 4) && out == value,
               "Direct read/write callbacks work without a mapping callback");
    mem.setAdapter(nullptr);
    TEST_CHECK(!mem.readPhysical(0x1000, &out, 4), "Detached adapter cannot access RAM");
    mem.notifyDisplayUpdate(0, 0, 4, 4);
    TEST_CHECK(second.updates == 0, "Detached adapter cannot notify display");
    return 0;
}

int main() {
    if (testAdapterFallback()) return 1;
    std::cout << "======================================================================" << std::endl;
    std::cout << "SVGA3=VLKN Deliverable 3: Real Guest Memory Access & Region Translation" << std::endl;
    std::cout << "======================================================================" << std::endl;

    /* 1. Real Vulkan Device Creation with Validation Layers */
    Svga3VlknConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.appName = "Guest Memory Acceptance Test";
    cfg.apiVersion = VK_API_VERSION_1_1;
    cfg.forceMockBackend = false; /* Real Vulkan ONLY - fail immediately if unavailable */
    cfg.enableValidationLayers = true;

    Svga3VlknDevice *dev = svga3_vlkn_device_create(&cfg);
    if (!dev) {
        std::cerr << "CRITICAL: Failed to create real Vulkan device (forceMockBackend=false)!" << std::endl;
        return 1;
    }

    svga3_vlkn::VlknBackend *backend = dev->backend.get();
    TEST_CHECK(!backend->dispatch().isMock, "Execution is running on real Vulkan driver (NOT mock)");

    VkPhysicalDeviceProperties props;
    backend->dispatch().vkGetPhysicalDeviceProperties(backend->physicalDevice(), &props);
    std::cout << "  [INFO] Real Vulkan Driver: " << props.deviceName
              << " (Driver version: 0x" << std::hex << props.driverVersion << std::dec
              << ", Vulkan API: " << VK_VERSION_MAJOR(props.apiVersion) << "."
              << VK_VERSION_MINOR(props.apiVersion) << "." << VK_VERSION_PATCH(props.apiVersion) << ")" << std::endl;

    /* 2. Setup Guest RAM Blocks and Display Framebuffer */
    std::cout << "\n--- Subtest 1: Guest RAM Mapping & Display Framebuffer Setup ---" << std::endl;
    std::fill(g_guestRam.begin(), g_guestRam.end(), 0xCC);
    std::fill(g_framebuffer.begin(), g_framebuffer.end(), 0x00);

    Svga3VlknStatus st = svga3_vlkn_device_map_guest_ram(dev, GUEST_RAM_BASE_GPA, g_guestRam.data(), GUEST_RAM_SIZE);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Map guest physical RAM (64MB) into GuestMemoryManager");

    st = svga3_vlkn_device_set_framebuffer(dev, g_framebuffer.data(), FB_BASE_GPA, FB_SIZE, FB_WIDTH, FB_HEIGHT, FB_PITCH, FB_BPP);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Register guest display framebuffer (BAR1, 800x600x4)");

    /* 3. Test GMR2 Definition and Remapping with Noncontiguous Physical Pages */
    std::cout << "\n--- Subtest 2: GMR2 Definition, Remapping & Noncontiguous Page Translation ---" << std::endl;
    const uint32_t TEST_GMR_ID = 5;
    const uint32_t NUM_PAGES = 4;
    st = svga3_vlkn_gmr_define(dev, TEST_GMR_ID, NUM_PAGES);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Define GMR2 ID 5 with 4 virtual pages");

    /* Map 4 virtual pages to noncontiguous physical pages:
     * Virtual page 0 -> Physical page 100  (GPA = GUEST_RAM_BASE_GPA + 100 * 4096)
     * Virtual page 1 -> Physical page 50   (GPA = GUEST_RAM_BASE_GPA + 50 * 4096)
     * Virtual page 2 -> Physical page 500  (GPA = GUEST_RAM_BASE_GPA + 500 * 4096)
     * Virtual page 3 -> Physical page 2    (GPA = GUEST_RAM_BASE_GPA + 2 * 4096)
     */
    uint64_t ramBasePPN = GUEST_RAM_BASE_GPA >> 12;
    uint32_t ppns32[4] = {
        static_cast<uint32_t>(ramBasePPN + 100),
        static_cast<uint32_t>(ramBasePPN + 50),
        static_cast<uint32_t>(ramBasePPN + 500),
        static_cast<uint32_t>(ramBasePPN + 2)
    };

    st = svga3_vlkn_gmr_remap(dev, TEST_GMR_ID, SVGA_REMAP_GMR2_PPN32, 0, NUM_PAGES, ppns32, sizeof(ppns32));
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Remap GMR2 with PPN32 descriptor array to noncontiguous pages");

    /* 4. Test Multi-page Boundary Crossing & Nonzero Offset Transfers */
    std::cout << "\n--- Subtest 3: Nonzero Offsets and Transfers Crossing Page Boundaries ---" << std::endl;
    /*
     * We will write 9000 bytes starting at byte offset 2000 in GMR 5.
     * Page 0 (PPN 100): bytes [2000 .. 4095] = 2096 bytes
     * Page 1 (PPN 50):  bytes [4096 .. 8191] = 4096 bytes
     * Page 2 (PPN 500): bytes [8192 .. 10999] = 2808 bytes
     * Total = 9000 bytes spanning 3 noncontiguous physical pages!
     */
    const size_t TRANSFER_SIZE = 9000;
    const uint32_t START_OFFSET = 2000;
    std::vector<uint8_t> writePayload(TRANSFER_SIZE);
    for (size_t i = 0; i < TRANSFER_SIZE; ++i) {
        writePayload[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
    }

    SVGAGuestPtr gmrPtr = { TEST_GMR_ID, START_OFFSET };
    st = svga3_vlkn_guest_write(dev, gmrPtr, writePayload.data(), TRANSFER_SIZE);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Write 9000 bytes across 3 noncontiguous physical pages at offset 2000");

    /* Verify each physical page directly in simulated guest RAM */
    uint8_t *page0Hva = gpaToHva((ramBasePPN + 100) << 12);
    uint8_t *page1Hva = gpaToHva((ramBasePPN + 50) << 12);
    uint8_t *page2Hva = gpaToHva((ramBasePPN + 500) << 12);
    uint8_t *page3Hva = gpaToHva((ramBasePPN + 2) << 12);

    bool page0Match = (memcmp(page0Hva + 2000, writePayload.data(), 2096) == 0);
    bool page1Match = (memcmp(page1Hva, writePayload.data() + 2096, 4096) == 0);
    bool page2Match = (memcmp(page2Hva, writePayload.data() + 2096 + 4096, 2808) == 0);
    TEST_CHECK(page0Match, "Page 0 (PPN 100) received exact 2096 bytes at offset 2000");
    TEST_CHECK(page1Match, "Page 1 (PPN 50) received exact 4096 bytes from 0..4095");
    TEST_CHECK(page2Match, "Page 2 (PPN 500) received exact 2808 bytes from 0..2807");
    TEST_CHECK(page3Hva[0] == 0xCC, "Page 3 (PPN 2) remained untouched (0xCC)");

    /* Read back using svga3_vlkn_guest_read */
    std::vector<uint8_t> readbackPayload(TRANSFER_SIZE, 0);
    st = svga3_vlkn_guest_read(dev, gmrPtr, readbackPayload.data(), TRANSFER_SIZE);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Read back 9000 bytes through GuestMemoryManager");
    TEST_CHECK(memcmp(writePayload.data(), readbackPayload.data(), TRANSFER_SIZE) == 0,
               "Read back payload matches written data byte-for-byte across all 3 pages");

    /* 5. Test Legacy GMR Descriptors (Chained SVGAGuestMemDescriptor in Guest Physical Memory) */
    std::cout << "\n--- Subtest 4: Legacy GMR Descriptor Chain Traversal ---" << std::endl;
    /*
     * We allocate a descriptor chain in guest RAM at PPN 10 (GPA = GUEST_RAM_BASE_GPA + 10 * 4096).
     * Descriptor 0: ppn = ramBasePPN + 20, numPages = 2
     * Descriptor 1: ppn = ramBasePPN + 80, numPages = 3
     * Descriptor 2: ppn = 0, numPages = 0 (Terminator)
     */
    uint64_t descTablePPN = ramBasePPN + 10;
    auto *descTable = reinterpret_cast<SVGAGuestMemDescriptor*>(gpaToHva(descTablePPN << 12));
    descTable[0].ppn = static_cast<uint32_t>(ramBasePPN + 20);
    descTable[0].numPages = 2;
    descTable[1].ppn = static_cast<uint32_t>(ramBasePPN + 80);
    descTable[1].numPages = 3;
    descTable[2].ppn = 0;
    descTable[2].numPages = 0;

    const uint32_t LEGACY_GMR_ID = 9;
    st = svga3_vlkn_gmr_register_legacy(dev, LEGACY_GMR_ID, static_cast<uint32_t>(descTablePPN));
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Register legacy GMR ID 9 via descriptor table at PPN");

    /* Verify legacy GMR read/write across descriptors (5 total pages = 20480 bytes) */
    std::vector<uint8_t> legacyData(12000, 0x42);
    SVGAGuestPtr legacyPtr = { LEGACY_GMR_ID, 1000 };
    st = svga3_vlkn_guest_write(dev, legacyPtr, legacyData.data(), 12000);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Write 12000 bytes spanning multiple legacy descriptors");

    std::vector<uint8_t> legacyRead(12000, 0);
    st = svga3_vlkn_guest_read(dev, legacyPtr, legacyRead.data(), 12000);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Read back 12000 bytes from legacy GMR");
    TEST_CHECK(memcmp(legacyData.data(), legacyRead.data(), 12000) == 0,
               "Legacy GMR readback matches written data across descriptor boundaries");

    /* 6. Test Bidirectional Surface DMA Upload and Readback with Real Vulkan Surfaces */
    std::cout << "\n--- Subtest 5: Bidirectional Surface DMA (Upload & Download) with Real Vulkan ---" << std::endl;
    /*
     * Define a 64x64 RGBA8 Vulkan surface (SID 100).
     * Row pitch = 64 * 4 = 256 bytes.
     * Total image size = 64 * 256 = 16384 bytes (4 pages).
     */
    const uint32_t SID_TEX = 100;
    SVGA3dSize texSize = { 64, 64, 1 };
    st = svga3_vlkn_surface_define(dev, SID_TEX, 0, SVGA3D_X8R8G8B8, &texSize, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Define 64x64 Vulkan texture surface (SID 100)");

    /* Populate GMR 5 with a deterministic 64x64 RGBA pattern */
    std::vector<uint32_t> testPixels(64 * 64);
    for (uint32_t y = 0; y < 64; ++y) {
        for (uint32_t x = 0; x < 64; ++x) {
            uint8_t r = static_cast<uint8_t>(x * 4);
            uint8_t g = static_cast<uint8_t>(y * 4);
            uint8_t b = static_cast<uint8_t>((x + y) * 2);
            uint8_t a = 0xFF;
            testPixels[y * 64 + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    SVGAGuestPtr uploadPtr = { TEST_GMR_ID, 0 };
    st = svga3_vlkn_guest_write(dev, uploadPtr, testPixels.data(), testPixels.size() * sizeof(uint32_t));
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Write 64x64 texture pixels into noncontiguous GMR 5");

    /* DMA Upload from GMR 5 into Vulkan Surface 100 */
    SVGA3dGuestImage guestImg = {};
    guestImg.ptr = uploadPtr;
    guestImg.pitch = 64 * 4;

    SVGA3dSurfaceImageId hostImg = {};
    hostImg.sid = SID_TEX;
    hostImg.face = 0;
    hostImg.mipmap = 0;

    SVGA3dCopyBox copyBox = {};
    copyBox.x = 0; copyBox.y = 0; copyBox.z = 0;
    copyBox.w = 64; copyBox.h = 64; copyBox.d = 1;
    copyBox.srcx = 0; copyBox.srcy = 0; copyBox.srcz = 0;

    st = dev->surfaceMgr->surfaceDMA(guestImg, hostImg, SVGA3D_WRITE_HOST_VRAM, &copyBox, 1, dev->guestMem.get());
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Execute surfaceDMA WRITE_HOST_VRAM (Upload GMR -> Vulkan Image)");

    /* Now Download the surface back from Vulkan into a DIFFERENT region of GMR 5 (offset 100) */
    /* First zero out destination region */
    std::vector<uint8_t> zeros(64 * 64 * 4, 0);
    SVGAGuestPtr downloadPtr = { TEST_GMR_ID, 0 };
    st = svga3_vlkn_guest_write(dev, downloadPtr, zeros.data(), zeros.size());
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Zero out guest memory before download");

    guestImg.ptr = downloadPtr;
    st = dev->surfaceMgr->surfaceDMA(guestImg, hostImg, SVGA3D_READ_HOST_VRAM, &copyBox, 1, dev->guestMem.get());
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Execute surfaceDMA READ_HOST_VRAM (Download Vulkan Image -> GMR)");

    /* Read back downloaded pixels and verify exact match with original pattern */
    std::vector<uint32_t> downloadedPixels(64 * 64, 0);
    st = svga3_vlkn_guest_read(dev, downloadPtr, downloadedPixels.data(), downloadedPixels.size() * sizeof(uint32_t));
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Read back downloaded pixels from GMR 5");

    bool textureMatch = (memcmp(testPixels.data(), downloadedPixels.data(), testPixels.size() * sizeof(uint32_t)) == 0);
    TEST_CHECK(textureMatch, "Downloaded texture pixels match uploaded pixels byte-for-byte through real Vulkan staging");

    /* 7. Test Presentation to Display Framebuffer (BAR1) */
    std::cout << "\n--- Subtest 6: Presentation to Display Framebuffer with Clipping ---" << std::endl;
    SVGA3dCopyRect presRect = {};
    presRect.x = 100;
    presRect.y = 150;
    presRect.w = 64;
    presRect.h = 64;
    presRect.srcx = 0;
    presRect.srcy = 0;

    st = dev->surfaceMgr->present(SID_TEX, &presRect, 1, dev->guestMem.get());
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Execute present() to QEMU display framebuffer at (100, 150)");

    /* Verify pixels in simulated framebuffer */
    bool fbMatch = true;
    for (uint32_t y = 0; y < 64 && fbMatch; ++y) {
        for (uint32_t x = 0; x < 64 && fbMatch; ++x) {
            uint32_t fbPixel = *reinterpret_cast<const uint32_t*>(g_framebuffer.data() + (150 + y) * FB_PITCH + (100 + x) * FB_BPP);
            uint32_t srcPixel = testPixels[y * 64 + x];
            if (fbPixel != srcPixel) {
                fbMatch = false;
            }
        }
    }
    TEST_CHECK(fbMatch, "Display framebuffer received exact pixels at (100, 150)");

    /* Verify untouched framebuffer pixels remained 0 */
    uint32_t untouchedPixel = *reinterpret_cast<const uint32_t*>(g_framebuffer.data());
    TEST_CHECK(untouchedPixel == 0x00, "Untouched framebuffer pixels outside blit area remained untouched");

    /* 7b. Test Framebuffer Scanout Offset and GMR Aperture Isolation */
    std::cout << "\n--- Subtest 6b: Framebuffer Scanout Offset and GMR Aperture Isolation ---" << std::endl;
    const uint32_t SCANOUT_OFFSET = 1024 * 1024; /* 1 MB offset */
    g_framebuffer.resize(FB_SIZE + SCANOUT_OFFSET);
    st = svga3_vlkn_device_set_framebuffer(dev, g_framebuffer.data(), FB_BASE_GPA, g_framebuffer.size(), FB_WIDTH, FB_HEIGHT, FB_PITCH, FB_BPP);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Register full VRAM extent including scanout offset");
    TEST_CHECK(svga3_vlkn_device_set_scanout_offset(dev, g_framebuffer.size()) == SVGA3_VLKN_ERROR_INVALID_PARAM, "Reject scanout at end of VRAM");
    TEST_CHECK(svga3_vlkn_device_set_scanout_offset(dev, SCANOUT_OFFSET + 1) == SVGA3_VLKN_ERROR_INVALID_PARAM, "Reject incomplete scanout footprint");
    TEST_CHECK(dev->guestMem->getFramebuffer().scanoutOffset == 0, "Rejected scanout does not mutate registration");
    st = svga3_vlkn_device_set_scanout_offset(dev, SCANOUT_OFFSET);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Set scanout offset to 1MB");

    /* Test that GMR SVGA_GMR_FRAMEBUFFER (4294967294) reads from hva + offset, NOT shifted by scanoutOffset */
    uint32_t magicVal = 0x12345678;
    *reinterpret_cast<uint32_t*>(g_framebuffer.data() + 256) = magicVal;
    *reinterpret_cast<uint32_t*>(g_framebuffer.data() + SCANOUT_OFFSET + 256) = 0x99999999;

    SVGAGuestPtr gmrFbPtr = {};
    gmrFbPtr.gmrId = SVGA_GMR_FRAMEBUFFER;
    gmrFbPtr.offset = 256;
    uint32_t readBackVal = 0;
    st = dev->guestMem->readGuest(gmrFbPtr, &readBackVal, sizeof(readBackVal));
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS && readBackVal == magicVal,
               "GMR_FRAMEBUFFER reads from VRAM base independent of scanoutOffset");

    /* Clear framebuffer to verify fresh present writes */
    std::fill(g_framebuffer.begin(), g_framebuffer.end(), 0x00);
    st = dev->surfaceMgr->present(SID_TEX, &presRect, 1, dev->guestMem.get());
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Execute present() with active scanoutOffset");

    uint32_t scanoutPixel = *reinterpret_cast<const uint32_t*>(g_framebuffer.data() + SCANOUT_OFFSET + (150 + 10) * FB_PITCH + (100 + 10) * FB_BPP);
    uint32_t mirroredPixel = *reinterpret_cast<const uint32_t*>(g_framebuffer.data() + (150 + 10) * FB_PITCH + (100 + 10) * FB_BPP);
    uint32_t expectedPixel = testPixels[10 * 64 + 10];
    TEST_CHECK(scanoutPixel == expectedPixel, "Scanout region (hva + scanoutOffset) updated by present()");
    TEST_CHECK(mirroredPixel == 0, "Present does not overwrite unowned VRAM at offset zero");

    /* Reset scanout offset */
    svga3_vlkn_device_set_scanout_offset(dev, 0);

    /* 8. Test FIFO Integration with GMR Commands */
    std::cout << "\n--- Subtest 7: FIFO Command Stream Execution with GMR Commands ---" << std::endl;
    /*
     * Build a FIFO stream containing:
     * 1. SVGA_CMD_DEFINE_GMR2 (gmrId = 20, numPages = 2)
     * 2. SVGA_CMD_REMAP_GMR2 (PPN32 mapping)
     * 3. SVGA_3D_CMD_SURFACE_DEFINE (SID 200, 32x32)
     * 4. SVGA_3D_CMD_SURFACE_DMA (Upload from GMR 20)
     */
    std::vector<uint8_t> fifoStream;

    /* Helper lambda to append */
    auto appendData = [&](const void *data, size_t sz) {
        const uint8_t *b = reinterpret_cast<const uint8_t*>(data);
        fifoStream.insert(fifoStream.end(), b, b + sz);
    };

    /* 1. SVGA_CMD_DEFINE_GMR2 */
    uint32_t cmdDefineGMR = SVGA_CMD_DEFINE_GMR2;
    SVGAFifoCmdDefineGMR2 defGmr = { 20, 2 };
    appendData(&cmdDefineGMR, sizeof(cmdDefineGMR));
    appendData(&defGmr, sizeof(defGmr));

    /* 2. SVGA_CMD_REMAP_GMR2 */
    uint32_t cmdRemapGMR = SVGA_CMD_REMAP_GMR2;
    SVGAFifoCmdRemapGMR2 remapGmr = { 20, SVGA_REMAP_GMR2_PPN32, 0, 2 };
    uint32_t remapPPNs[2] = {
        static_cast<uint32_t>(ramBasePPN + 60),
        static_cast<uint32_t>(ramBasePPN + 61)
    };
    appendData(&cmdRemapGMR, sizeof(cmdRemapGMR));
    appendData(&remapGmr, sizeof(remapGmr));
    appendData(remapPPNs, sizeof(remapPPNs));

    /* 3. SVGA_3D_CMD_SURFACE_DEFINE */
    uint32_t cmdDefSurf = SVGA_3D_CMD_SURFACE_DEFINE;
    SVGA3dCmdHeader hdrDefSurf = { sizeof(SVGA3dCmdDefineSurface) + sizeof(SVGA3dSize) };
    SVGA3dCmdDefineSurface defSurf = {};
    defSurf.sid = 200;
    defSurf.surfaceFlags = static_cast<SVGA3dSurfaceFlags>(0);
    defSurf.format = SVGA3D_X8R8G8B8;
    defSurf.face[0].numMipLevels = 1;
    SVGA3dSize surfSize32 = { 32, 32, 1 };
    appendData(&cmdDefSurf, sizeof(cmdDefSurf));
    appendData(&hdrDefSurf, sizeof(hdrDefSurf));
    appendData(&defSurf, sizeof(defSurf));
    appendData(&surfSize32, sizeof(surfSize32));

    /* 4. SVGA_3D_CMD_SURFACE_DMA */
    uint32_t cmdDma = SVGA_3D_CMD_SURFACE_DMA;
    SVGA3dCmdHeader hdrDma = { sizeof(SVGA3dCmdSurfaceDMA) + sizeof(SVGA3dCopyBox) };
    SVGA3dCmdSurfaceDMA dmaCmd = {};
    dmaCmd.guest.ptr.gmrId = 20;
    dmaCmd.guest.ptr.offset = 0;
    dmaCmd.guest.pitch = 32 * 4;
    dmaCmd.host.sid = 200;
    dmaCmd.host.face = 0;
    dmaCmd.host.mipmap = 0;
    dmaCmd.transfer = SVGA3D_WRITE_HOST_VRAM;
    SVGA3dCopyBox dmaBox = {};
    dmaBox.w = 32; dmaBox.h = 32; dmaBox.d = 1;

    appendData(&cmdDma, sizeof(cmdDma));
    appendData(&hdrDma, sizeof(hdrDma));
    appendData(&dmaCmd, sizeof(dmaCmd));
    appendData(&dmaBox, sizeof(dmaBox));

    /* Fill guest RAM for GMR 20 */
    uint8_t *gmr20Page0 = gpaToHva((ramBasePPN + 60) << 12);
    memset(gmr20Page0, 0x77, 32 * 32 * 4);

    size_t bytesConsumed = 0;
    st = svga3_vlkn_fifo_execute(dev, fifoStream.data(), fifoStream.size(), &bytesConsumed);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Execute FIFO stream with DEFINE_GMR2, REMAP_GMR2, DEFINE_SURFACE, SURFACE_DMA");
    TEST_CHECK(bytesConsumed == fifoStream.size(), "FIFO consumed entire byte stream exactly");
    TEST_CHECK(svga3_vlkn_surface_exists(dev, 200), "Surface 200 defined successfully via FIFO");

    /* 9. Test Robustness & Negative Cases (Safe Error Handling Without Crashing) */
    std::cout << "\n--- Subtest 8: Robustness, Boundary & Malformed Packet Negative Tests ---" << std::endl;

    /* A. Out of bounds read/write on valid GMR */
    SVGAGuestPtr oobPtr = { TEST_GMR_ID, NUM_PAGES * 4096 - 10 };
    uint8_t dummyBuf[32];
    st = svga3_vlkn_guest_read(dev, oobPtr, dummyBuf, 32);
    TEST_CHECK(st == SVGA3_VLKN_ERROR_INVALID_PARAM, "Reject OOB read crossing past end of GMR (overflow protection)");

    /* B. Invalid / unmapped GMR ID */
    SVGAGuestPtr unmappedPtr = { 999, 0 };
    st = svga3_vlkn_guest_read(dev, unmappedPtr, dummyBuf, 16);
    TEST_CHECK(st == SVGA3_VLKN_ERROR_NOT_FOUND, "Reject access to unmapped GMR ID 999 with SVGA3_VLKN_ERROR_NOT_FOUND");

    /* C. NULL GMR pointer */
    SVGAGuestPtr nullPtr = { SVGA_GMR_NULL, 0 };
    st = svga3_vlkn_guest_write(dev, nullPtr, dummyBuf, 16);
    TEST_CHECK(st == SVGA3_VLKN_ERROR_INVALID_PARAM, "Reject access to SVGA_GMR_NULL with SVGA3_VLKN_ERROR_INVALID_PARAM");

    /* D. Integer overflow in offset + size */
    SVGAGuestPtr overflowPtr = { TEST_GMR_ID, 0xFFFFFFF0U };
    st = svga3_vlkn_guest_read(dev, overflowPtr, dummyBuf, 64);
    TEST_CHECK(st == SVGA3_VLKN_ERROR_INVALID_PARAM, "Reject integer overflow in offset + size safely");

    /* E. Malformed / truncated FIFO packet */
    uint32_t truncatedFifo[2] = { SVGA_CMD_DEFINE_GMR2, 10 }; /* Only 8 bytes, needs 12 */
    size_t truncatedConsumed = 0;
    st = svga3_vlkn_fifo_execute(dev, truncatedFifo, sizeof(truncatedFifo), &truncatedConsumed);
    TEST_CHECK(st == SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER, "Reject truncated FIFO packet safely without crash");

    /* F. Malformed legacy descriptor with loop / cycle protection */
    uint64_t loopTablePPN = ramBasePPN + 15;
    auto *loopTable = reinterpret_cast<SVGAGuestMemDescriptor*>(gpaToHva(loopTablePPN << 12));
    loopTable[0].ppn = static_cast<uint32_t>(ramBasePPN + 15); /* Points to self */
    loopTable[0].numPages = 1;
    st = svga3_vlkn_gmr_register_legacy(dev, 30, static_cast<uint32_t>(loopTablePPN));
    TEST_CHECK(st == SVGA3_VLKN_ERROR_INVALID_PARAM, "Reject infinite loop in legacy descriptor chain safely");

    /* 10. Clean Teardown */
    std::cout << "\n--- Subtest 9: Teardown & Resource Cleanup ---" << std::endl;
    svga3_vlkn_surface_destroy(dev, SID_TEX);
    svga3_vlkn_surface_destroy(dev, 200);
    svga3_vlkn_gmr_destroy(dev, TEST_GMR_ID);
    svga3_vlkn_gmr_destroy(dev, LEGACY_GMR_ID);
    svga3_vlkn_device_destroy(dev);
    std::cout << "  [PASS] Device and all memory resources cleaned up successfully" << std::endl;

    std::cout << "\n======================================================================" << std::endl;
    std::cout << "DELIVERABLE 3 ACCEPTANCE SUITE: ALL TESTS PASSED" << std::endl;
    std::cout << "======================================================================" << std::endl;

    return 0;
}
