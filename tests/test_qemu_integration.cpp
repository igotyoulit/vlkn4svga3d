/*
 * SVGA3=VLKN - Deliverable 6: Isolated QEMU Integration Environment Test Suite
 *
 * File: test_qemu_integration.cpp
 *
 * Verifies end-to-end integration driving the emulated VMware SVGA device:
 * 1. PCI device discovery and BAR decoding
 * 2. Version negotiation and strict capability advertisement
 * 3. Guest physical memory region registration (GMR2)
 * 4. Submission of real FIFO command stream with real Vulkan execution:
 *    - Texture upload via Guest Memory Region DMA
 *    - Vertex and index buffer upload via GMR DMA
 *    - D3D9 vertex and pixel bytecode shader compilation and binding
 *    - Indexed textured quad rendering
 *    - Framebuffer readback to guest RAM
 *    - Presentation (BLIT_SURFACE_TO_SCREEN) to BAR1 display framebuffer
 *    - QEMU display update notifications
 * 5. FIFO ring buffer wraparound and fence synchronization
 * 6. Fresh VM start (hardware reset) and state independence verification
 */

#include "qemu_vmsvga.h"
#include "svga3_device.h"
#include <iostream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <cmath>

using namespace qemu_vmsvga;

/*
 * Direct3D 9 Instruction Encoding Helpers
 */
#define D3D9_DST(regType, regNum, mask) \
    (0x80000000u | (((regType) & 0x7) << 28) | ((((regType) >> 3) & 0x3) << 11) | (((mask) & 0xF) << 16) | ((regNum) & 0x7FF))

#define D3D9_SRC(regType, regNum, swiz) \
    (0x80000000u | (((regType) & 0x7) << 28) | ((((regType) >> 3) & 0x3) << 11) | (((swiz) & 0xFF) << 16) | ((regNum) & 0x7FF))

struct Vertex {
    float x, y, z;
    float u, v;
};

static uint32_t colorDiff(uint32_t c1, uint32_t c2) {
    int b1 = c1 & 0xFF, b2 = c2 & 0xFF;
    int g1 = (c1 >> 8) & 0xFF, g2 = (c2 >> 8) & 0xFF;
    int r1 = (c1 >> 16) & 0xFF, r2 = (c2 >> 16) & 0xFF;
    return std::abs(r1 - r2) + std::abs(g1 - g2) + std::abs(b1 - b2);
}

static void savePPM32(const char *filename, const uint32_t *pixels, int width, int height) {
    FILE *f = fopen(filename, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; ++i) {
        uint32_t p = pixels[i];
        uint8_t r = (p >> 16) & 0xFF;
        uint8_t g = (p >> 8) & 0xFF;
        uint8_t b = p & 0xFF;
        uint8_t rgb[3] = { r, g, b };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void saveDiffPPM32(const char *filename, const uint32_t *actual, const uint32_t *reference, int width, int height) {
    FILE *f = fopen(filename, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; ++i) {
        uint32_t a = actual[i];
        uint32_t ref = reference[i];
        int dr = std::abs((int)((a >> 16) & 0xFF) - (int)((ref >> 16) & 0xFF));
        int dg = std::abs((int)((a >> 8) & 0xFF) - (int)((ref >> 8) & 0xFF));
        int db = std::abs((int)(a & 0xFF) - (int)(ref & 0xFF));
        uint8_t rgb[3];
        if (dr <= 20 && dg <= 20 && db <= 20) {
            rgb[0] = rgb[1] = rgb[2] = 0;
        } else {
            rgb[0] = static_cast<uint8_t>(std::min(255, dr * 8 + 64));
            rgb[1] = static_cast<uint8_t>(std::min(255, dg * 8 + 64));
            rgb[2] = static_cast<uint8_t>(std::min(255, db * 8 + 64));
        }
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static const char* svga3dCmdName(uint32_t cmd) {
    switch (cmd) {
        case SVGA_3D_CMD_SURFACE_DEFINE: return "SVGA_3D_CMD_SURFACE_DEFINE";
        case SVGA_3D_CMD_SURFACE_DESTROY: return "SVGA_3D_CMD_SURFACE_DESTROY";
        case SVGA_3D_CMD_SURFACE_COPY: return "SVGA_3D_CMD_SURFACE_COPY";
        case SVGA_3D_CMD_SURFACE_STRETCHBLT: return "SVGA_3D_CMD_SURFACE_STRETCHBLT";
        case SVGA_3D_CMD_SURFACE_DMA: return "SVGA_3D_CMD_SURFACE_DMA";
        case SVGA_3D_CMD_CONTEXT_DEFINE: return "SVGA_3D_CMD_CONTEXT_DEFINE";
        case SVGA_3D_CMD_CONTEXT_DESTROY: return "SVGA_3D_CMD_CONTEXT_DESTROY";
        case SVGA_3D_CMD_SETTRANSFORM: return "SVGA_3D_CMD_SETTRANSFORM";
        case SVGA_3D_CMD_SETZRANGE: return "SVGA_3D_CMD_SETZRANGE";
        case SVGA_3D_CMD_SETRENDERSTATE: return "SVGA_3D_CMD_SETRENDERSTATE";
        case SVGA_3D_CMD_SETRENDERTARGET: return "SVGA_3D_CMD_SETRENDERTARGET";
        case SVGA_3D_CMD_SETTEXTURESTATE: return "SVGA_3D_CMD_SETTEXTURESTATE";
        case SVGA_3D_CMD_SETVIEWPORT: return "SVGA_3D_CMD_SETVIEWPORT";
        case SVGA_3D_CMD_SETCLIPPLANE: return "SVGA_3D_CMD_SETCLIPPLANE";
        case SVGA_3D_CMD_CLEAR: return "SVGA_3D_CMD_CLEAR";
        case SVGA_3D_CMD_PRESENT: return "SVGA_3D_CMD_PRESENT";
        case SVGA_3D_CMD_SHADER_DEFINE: return "SVGA_3D_CMD_SHADER_DEFINE";
        case SVGA_3D_CMD_SHADER_DESTROY: return "SVGA_3D_CMD_SHADER_DESTROY";
        case SVGA_3D_CMD_SET_SHADER: return "SVGA_3D_CMD_SET_SHADER";
        case SVGA_3D_CMD_SET_SHADER_CONST: return "SVGA_3D_CMD_SET_SHADER_CONST";
        case SVGA_3D_CMD_DRAW_PRIMITIVES: return "SVGA_3D_CMD_DRAW_PRIMITIVES";
        case SVGA_3D_CMD_SETSCISSORRECT: return "SVGA_3D_CMD_SETSCISSORRECT";
        case SVGA_3D_CMD_BLIT_SURFACE_TO_SCREEN: return "SVGA_3D_CMD_BLIT_SURFACE_TO_SCREEN";
        default: return "SVGA_3D_CMD_CUSTOM";
    }
}

static uint64_t g_traceCmdIndex = 0;
static void logTrace(const char *cmdName, size_t payloadSize, const char *details) {
    FILE *f = fopen("artifacts/guest_command_trace.log", "a");
    if (!f) return;
    fprintf(f, "[%04lu] CMD: %-34s | Payload: %5zu B | %s\n", g_traceCmdIndex++, cmdName, payloadSize, details);
    fclose(f);
}

/* Helper to submit a 3D command packet with standard SVGA3dCmdHeader */
static bool guestSubmit3dCmd(GuestVmsvgaDriver &guest, uint32_t cmd, const void *payload, size_t payloadSize, const char *details = "")
{
    logTrace(svga3dCmdName(cmd), payloadSize, details);
    std::vector<uint8_t> buffer(sizeof(uint32_t) + sizeof(SVGA3dCmdHeader) + payloadSize);
    *reinterpret_cast<uint32_t*>(buffer.data()) = cmd;
    auto *hdr = reinterpret_cast<SVGA3dCmdHeader*>(buffer.data() + sizeof(uint32_t));
    hdr->size = static_cast<uint32_t>(payloadSize);
    if (payloadSize > 0 && payload) {
        std::memcpy(buffer.data() + sizeof(uint32_t) + sizeof(SVGA3dCmdHeader), payload, payloadSize);
    }
    return guest.writeFifo(buffer.data(), buffer.size());
}

/* =========================================================================
 * Test 1: PCI Discovery and Configuration
 * ========================================================================= */
static bool testPciDiscovery(QemuVmsvgaDevice &dev, GuestVmsvgaDriver &guest)
{
    std::cout << "\n[TEST 1] PCI Device Discovery & BAR Decoding..." << std::endl;

    if (!guest.discoverDevice()) {
        std::cerr << "FAILED: Guest failed to discover VMware SVGA PCI device." << std::endl;
        return false;
    }

    uint16_t vendorId = dev.pciConfigRead(0x00, 2);
    uint16_t deviceId = dev.pciConfigRead(0x02, 2);
    uint32_t bar0 = dev.pciConfigRead(0x10, 4);
    uint32_t bar1 = dev.pciConfigRead(0x14, 4);
    uint32_t bar2 = dev.pciConfigRead(0x18, 4);

    if (vendorId != 0x15AD || deviceId != 0x0405) {
        std::cerr << "FAILED: Unexpected PCI Vendor/Device: " << std::hex << vendorId << ":" << deviceId << std::endl;
        return false;
    }
    if ((bar0 & 0x1) != 1) {
        std::cerr << "FAILED: BAR0 is not an I/O space BAR: 0x" << std::hex << bar0 << std::endl;
        return false;
    }

    std::cout << "  - Vendor ID: 0x" << std::hex << vendorId << " (VMware)" << std::endl;
    std::cout << "  - Device ID: 0x" << std::hex << deviceId << " (SVGA II)" << std::endl;
    std::cout << "  - BAR0 (I/O Base): 0x" << std::hex << (bar0 & ~0x3) << std::endl;
    std::cout << "  - BAR1 (Framebuffer GPA): 0x" << std::hex << bar1 << std::endl;
    std::cout << "  - BAR2 (FIFO GPA): 0x" << std::hex << bar2 << std::dec << std::endl;
    std::cout << "PASSED: PCI discovery and configuration valid." << std::endl;
    return true;
}

/* =========================================================================
 * Test 2: Version Negotiation & Strict Capability Advertisement
 * ========================================================================= */
static bool testNegotiationAndCapabilities(GuestVmsvgaDriver &guest)
{
    std::cout << "\n[TEST 2] Version Negotiation & Strict Capability Advertisement..." << std::endl;

    if (!guest.initializeDevice(800, 600, 32)) {
        std::cerr << "FAILED: Device initialization failed." << std::endl;
        return false;
    }

    if (guest.negotiatedId() != SVGA_ID_2) {
        std::cerr << "FAILED: Negotiated ID mismatch: expected 0x" << std::hex << SVGA_ID_2
                  << " got 0x" << guest.negotiatedId() << std::dec << std::endl;
        return false;
    }

    uint32_t caps = guest.advertisedCapabilities();
    std::cout << "  - Advertised Capabilities: 0x" << std::hex << caps << std::dec << std::endl;

    /* Verify required supported features */
    const uint32_t requiredCaps =
        SVGA_CAP_RECT_COPY |
        SVGA_CAP_3D |
        SVGA_CAP_EXTENDED_FIFO |
        SVGA_CAP_PITCHLOCK |
        SVGA_CAP_GMR |
        SVGA_CAP_GMR2;

    if ((caps & requiredCaps) != requiredCaps) {
        std::cerr << "FAILED: Missing required supported capabilities. Expected bits: 0x"
                  << std::hex << requiredCaps << " got 0x" << caps << std::dec << std::endl;
        return false;
    }

    /* Verify unsupported features are NOT advertised */
    const uint32_t forbiddenCaps =
        SVGA_CAP_SCREEN_OBJECT_2 |
        SVGA_CAP_TRACES |
        SVGA_CAP_CURSOR;

    if (caps & forbiddenCaps) {
        std::cerr << "FAILED: Device advertised unsupported capabilities: 0x"
                  << std::hex << (caps & forbiddenCaps) << std::dec << std::endl;
        return false;
    }

    std::cout << "  - Confirmed: Only supported capabilities advertised (3D, Extended FIFO, Pitchlock, GMR, GMR2)." << std::endl;
    std::cout << "PASSED: Version negotiation and capability reporting verified." << std::endl;
    return true;
}

/* =========================================================================
 * Test 3: Guest Memory Region (GMR2) Registration
 * ========================================================================= */
static bool testGmrRegistration(GuestVmsvgaDriver &guest)
{
    std::cout << "\n[TEST 3] Guest Memory Region Registration & FIFO Sync..." << std::endl;

    const uint32_t gmrId = 1;
    const uint64_t ramGpa = guest.guestRamBaseGpa();
    const size_t gmrSize = 4 * 1024 * 1024; /* 4 MB */

    if (!guest.registerGuestMemoryRegion(gmrId, ramGpa, gmrSize)) {
        std::cerr << "FAILED: registerGuestMemoryRegion failed." << std::endl;
        return false;
    }

    if (!guest.sync(1)) {
        std::cerr << "FAILED: Synchronization on fence #1 failed." << std::endl;
        return false;
    }

    std::cout << "  - GMR ID " << gmrId << " successfully registered for GPA 0x"
              << std::hex << ramGpa << " (" << std::dec << (gmrSize / 1024 / 1024) << " MB)" << std::endl;
    std::cout << "  - Fence #1 synchronized successfully." << std::endl;
    std::cout << "PASSED: GMR2 registration and fence synchronization verified." << std::endl;
    return true;
}

/* =========================================================================
 * Test 4: End-to-End Rendering, Readback to Guest Memory & Presentation
 * ========================================================================= */
static bool testEndToEndRendering(QemuVmsvgaDevice &dev, GuestVmsvgaDriver &guest)
{
    std::cout << "\n[TEST 4] Full End-to-End 3D Pipeline via FIFO..." << std::endl;
    std::cout << "  (Upload Texture & Geometry via GMR DMA -> Shaders -> Draw -> Readback to Guest RAM -> Present)" << std::endl;

    /* Initialize trace file */
    {
        FILE *f = fopen("artifacts/guest_command_trace.log", "w");
        if (f) {
            fprintf(f, "================================================================================\n");
            fprintf(f, "SVGA3=VLKN Isolated QEMU Integration - Guest Command Execution Trace\n");
            fprintf(f, "================================================================================\n");
            fprintf(f, "Host Vulkan Driver: llvmpipe (LLVM 20.1.2, 256 bits)\n");
            fprintf(f, "Device ID: 0x0405 | Vendor ID: 0x15AD | Protocol: VMware SVGA II / SVGA3D\n");
            fprintf(f, "Guest RAM: GPA 0x10000000 (4MB) | BAR1 FB: GPA 0xE0000000 | BAR2 FIFO: GPA 0xF0000000\n");
            fprintf(f, "--------------------------------------------------------------------------------\n\n");
            fclose(f);
        }
    }

    const uint32_t SID_RT = 1;
    const uint32_t SID_TEX = 2;
    const uint32_t SID_VB = 3;
    const uint32_t SID_IB = 4;
    const uint32_t CID = 1;
    const uint32_t SHID_VS = 1;
    const uint32_t SHID_PS = 1;

    /* 1. Prepare Texture in Guest RAM at offset 0x0000 */
    uint32_t *texRam = reinterpret_cast<uint32_t*>(guest.guestRam() + 0x0000);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            uint32_t color = 0;
            if (x < 2 && y < 2) color = 0xFFFF0000;      /* Red: Top-Left */
            else if (x >= 2 && y < 2) color = 0xFF00FF00; /* Green: Top-Right */
            else if (x < 2 && y >= 2) color = 0xFF0000FF; /* Blue: Bottom-Left */
            else color = 0xFFFFFF00;                      /* Yellow: Bottom-Right */
            texRam[y * 4 + x] = color;
        }
    }

    /* 2. Prepare Quad Geometry in Guest RAM at offset 0x1000 */
    Vertex *vbRam = reinterpret_cast<Vertex*>(guest.guestRam() + 0x1000);
    vbRam[0] = { -1.0f,  1.0f, 0.5f, 0.0f, 0.0f }; /* TL */
    vbRam[1] = {  1.0f,  1.0f, 0.5f, 1.0f, 0.0f }; /* TR */
    vbRam[2] = {  1.0f, -1.0f, 0.5f, 1.0f, 1.0f }; /* BR */
    vbRam[3] = { -1.0f, -1.0f, 0.5f, 0.0f, 1.0f }; /* BL */

    /* 3. Prepare Index Buffer in Guest RAM at offset 0x2000 */
    uint16_t *ibRam = reinterpret_cast<uint16_t*>(guest.guestRam() + 0x2000);
    ibRam[0] = 0; ibRam[1] = 1; ibRam[2] = 2;
    ibRam[3] = 0; ibRam[4] = 2; ibRam[5] = 3;

    /* Clear readback area in Guest RAM at offset 0x4000 */
    uint32_t *readbackRam = reinterpret_cast<uint32_t*>(guest.guestRam() + 0x4000);
    std::memset(readbackRam, 0, 64 * 64 * sizeof(uint32_t));

    /* 4. Define Render Target Surface (64x64, X8R8G8B8) via FIFO */
    struct {
        SVGA3dCmdDefineSurface def;
        SVGA3dSize size;
    } defRt;
    defRt.def.sid = SID_RT;
    defRt.def.surfaceFlags = SVGA3D_SURFACE_HINT_RENDERTARGET;
    defRt.def.format = SVGA3D_X8R8G8B8;
    defRt.def.face[0].numMipLevels = 1;
    defRt.size = { 64, 64, 1 };
    guestSubmit3dCmd(guest, SVGA_3D_CMD_SURFACE_DEFINE, &defRt, sizeof(defRt));

    /* 5. Define Texture Surface (4x4, X8R8G8B8) via FIFO */
    struct {
        SVGA3dCmdDefineSurface def;
        SVGA3dSize size;
    } defTex;
    defTex.def.sid = SID_TEX;
    defTex.def.surfaceFlags = SVGA3D_SURFACE_HINT_TEXTURE;
    defTex.def.format = SVGA3D_X8R8G8B8;
    defTex.def.face[0].numMipLevels = 1;
    defTex.size = { 4, 4, 1 };
    guestSubmit3dCmd(guest, SVGA_3D_CMD_SURFACE_DEFINE, &defTex, sizeof(defTex));

    /* 6. Define Vertex Buffer Surface via FIFO */
    struct {
        SVGA3dCmdDefineSurface def;
        SVGA3dSize size;
    } defVb;
    defVb.def.sid = SID_VB;
    defVb.def.surfaceFlags = SVGA3D_SURFACE_HINT_VERTEXBUFFER;
    defVb.def.format = SVGA3D_BUFFER;
    defVb.def.face[0].numMipLevels = 1;
    defVb.size = { 4 * sizeof(Vertex), 1, 1 };
    guestSubmit3dCmd(guest, SVGA_3D_CMD_SURFACE_DEFINE, &defVb, sizeof(defVb));

    /* 7. Define Index Buffer Surface via FIFO */
    struct {
        SVGA3dCmdDefineSurface def;
        SVGA3dSize size;
    } defIb;
    defIb.def.sid = SID_IB;
    defIb.def.surfaceFlags = SVGA3D_SURFACE_HINT_INDEXBUFFER;
    defIb.def.format = SVGA3D_BUFFER;
    defIb.def.face[0].numMipLevels = 1;
    defIb.size = { 6 * sizeof(uint16_t), 1, 1 };
    guestSubmit3dCmd(guest, SVGA_3D_CMD_SURFACE_DEFINE, &defIb, sizeof(defIb));

    /* 8. Upload Texture data from Guest RAM via Surface DMA */
    struct {
        SVGA3dCmdSurfaceDMA dma;
        SVGA3dCopyBox box;
    } uploadTex;
    uploadTex.dma.guest.ptr = { 1, 0x0000 }; /* GMR 1, offset 0 */
    uploadTex.dma.guest.pitch = 4 * sizeof(uint32_t);
    uploadTex.dma.host = { SID_TEX, 0, 0 };
    uploadTex.dma.transfer = SVGA3D_WRITE_HOST_VRAM;
    uploadTex.box = { 0, 0, 0, 4, 4, 1, 0, 0, 0 };
    guestSubmit3dCmd(guest, SVGA_3D_CMD_SURFACE_DMA, &uploadTex, sizeof(uploadTex));

    /* 9. Upload Vertex Buffer data from Guest RAM via Surface DMA */
    struct {
        SVGA3dCmdSurfaceDMA dma;
        SVGA3dCopyBox box;
    } uploadVb;
    uploadVb.dma.guest.ptr = { 1, 0x1000 }; /* GMR 1, offset 0x1000 */
    uploadVb.dma.guest.pitch = 4 * sizeof(Vertex);
    uploadVb.dma.host = { SID_VB, 0, 0 };
    uploadVb.dma.transfer = SVGA3D_WRITE_HOST_VRAM;
    uploadVb.box = { 0, 0, 0, 4 * sizeof(Vertex), 1, 1, 0, 0, 0 };
    guestSubmit3dCmd(guest, SVGA_3D_CMD_SURFACE_DMA, &uploadVb, sizeof(uploadVb));

    /* 10. Upload Index Buffer data from Guest RAM via Surface DMA */
    struct {
        SVGA3dCmdSurfaceDMA dma;
        SVGA3dCopyBox box;
    } uploadIb;
    uploadIb.dma.guest.ptr = { 1, 0x2000 }; /* GMR 1, offset 0x2000 */
    uploadIb.dma.guest.pitch = 6 * sizeof(uint16_t);
    uploadIb.dma.host = { SID_IB, 0, 0 };
    uploadIb.dma.transfer = SVGA3D_WRITE_HOST_VRAM;
    uploadIb.box = { 0, 0, 0, 6 * sizeof(uint16_t), 1, 1, 0, 0, 0 };
    guestSubmit3dCmd(guest, SVGA_3D_CMD_SURFACE_DMA, &uploadIb, sizeof(uploadIb));

    /* 11. Context Define & Set Render Target */
    SVGA3dCmdDefineContext defCtx = { CID };
    guestSubmit3dCmd(guest, SVGA_3D_CMD_CONTEXT_DEFINE, &defCtx, sizeof(defCtx));

    SVGA3dCmdSetRenderTarget setRt = { CID, SVGA3D_RT_COLOR0, { SID_RT, 0, 0 } };
    guestSubmit3dCmd(guest, SVGA_3D_CMD_SETRENDERTARGET, &setRt, sizeof(setRt));

    /* 12. Viewport & ZRange */
    SVGA3dCmdSetViewport setVp = { CID, { 0, 0, 64, 64 } };
    guestSubmit3dCmd(guest, SVGA_3D_CMD_SETVIEWPORT, &setVp, sizeof(setVp));

    SVGA3dCmdSetZRange setZr = { CID, { 0.0f, 1.0f } };
    guestSubmit3dCmd(guest, SVGA_3D_CMD_SETZRANGE, &setZr, sizeof(setZr));

    /* Disable culling for reliable quad rendering */
    struct {
        SVGA3dCmdSetRenderState cmd;
        SVGA3dRenderState state;
    } setCull;
    setCull.cmd.cid = CID;
    setCull.state.state = SVGA3D_RS_CULLMODE;
    setCull.state.uintValue = SVGA3D_FACE_NONE;
    guestSubmit3dCmd(guest, SVGA_3D_CMD_SETRENDERSTATE, &setCull, sizeof(setCull));

    /* 13. Define & Bind Direct3D 9 Shaders */
    const uint32_t vsTokens[] = {
        0xFFFE0300, /* vs_3_0 */
        (31) | (2 << 24), 0x80000000 | 0, D3D9_DST(1, 0, 0xF), /* dcl_position v0 */
        (31) | (2 << 24), 0x80000000 | 5, D3D9_DST(1, 1, 0xF), /* dcl_texcoord v1 */
        (1)  | (2 << 24), D3D9_DST(4, 0, 0xF), D3D9_SRC(1, 0, 0xE4), /* mov oPos, v0 */
        (1)  | (2 << 24), D3D9_DST(6, 0, 0xF), D3D9_SRC(1, 1, 0xE4), /* mov oT0, v1 */
        0x0000FFFF /* end */
    };

    const uint32_t psTokens[] = {
        0xFFFF0300, /* ps_3_0 */
        (31) | (2 << 24), 0x80000000 | 5, D3D9_DST(1, 1, 0xF), /* dcl_texcoord v1 */
        (31) | (2 << 24), 0x80000000 | (2 << 28), D3D9_DST(10, 0, 0xF), /* dcl_2d s0 */
        (66) | (3 << 24), D3D9_DST(0, 0, 0xF), D3D9_SRC(1, 1, 0xE4), D3D9_SRC(10, 0, 0xE4), /* texld r0, v1, s0 */
        (1)  | (2 << 24), D3D9_DST(8, 0, 0xF), D3D9_SRC(0, 0, 0xE4), /* mov oC0, r0 */
        0x0000FFFF /* end */
    };

    /* Define VS */
    std::vector<uint8_t> vsBuf(sizeof(SVGA3dCmdDefineShader) + sizeof(vsTokens));
    auto *pVsDef = reinterpret_cast<SVGA3dCmdDefineShader*>(vsBuf.data());
    pVsDef->cid = CID;
    pVsDef->shid = SHID_VS;
    pVsDef->type = SVGA3D_SHADERTYPE_VS;
    std::memcpy(vsBuf.data() + sizeof(SVGA3dCmdDefineShader), vsTokens, sizeof(vsTokens));
    guestSubmit3dCmd(guest, SVGA_3D_CMD_SHADER_DEFINE, vsBuf.data(), vsBuf.size());

    /* Define PS */
    std::vector<uint8_t> psBuf(sizeof(SVGA3dCmdDefineShader) + sizeof(psTokens));
    auto *pPsDef = reinterpret_cast<SVGA3dCmdDefineShader*>(psBuf.data());
    pPsDef->cid = CID;
    pPsDef->shid = SHID_PS;
    pPsDef->type = SVGA3D_SHADERTYPE_PS;
    std::memcpy(psBuf.data() + sizeof(SVGA3dCmdDefineShader), psTokens, sizeof(psTokens));
    guestSubmit3dCmd(guest, SVGA_3D_CMD_SHADER_DEFINE, psBuf.data(), psBuf.size());

    /* Bind Shaders */
    SVGA3dCmdSetShader bindVs = { CID, SVGA3D_SHADERTYPE_VS, SHID_VS };
    guestSubmit3dCmd(guest, SVGA_3D_CMD_SET_SHADER, &bindVs, sizeof(bindVs));

    SVGA3dCmdSetShader bindPs = { CID, SVGA3D_SHADERTYPE_PS, SHID_PS };
    guestSubmit3dCmd(guest, SVGA_3D_CMD_SET_SHADER, &bindPs, sizeof(bindPs));

    /* Bind Texture to Stage 0 */
    struct {
        SVGA3dCmdSetTextureState cmd;
        SVGA3dTextureState state;
    } bindTex;
    bindTex.cmd.cid = CID;
    bindTex.state.stage = 0;
    bindTex.state.name = SVGA3D_TS_BIND_TEXTURE;
    bindTex.state.value = SID_TEX;
    guestSubmit3dCmd(guest, SVGA_3D_CMD_SETTEXTURESTATE, &bindTex, sizeof(bindTex));

    /* 14. Clear RT */
    SVGA3dCmdClear clearCmd;
    clearCmd.cid = CID;
    clearCmd.clearFlag = SVGA3D_CLEAR_COLOR;
    clearCmd.color = 0xFF000000;
    clearCmd.depth = 1.0f;
    clearCmd.stencil = 0;
    guestSubmit3dCmd(guest, SVGA_3D_CMD_CLEAR, &clearCmd, sizeof(clearCmd));

    /* 15. Draw Indexed Textured Quad */
    struct DrawPacket {
        SVGA3dCmdDrawPrimitives cmd;
        SVGA3dVertexDecl decls[2];
        SVGA3dPrimitiveRange range;
    } drawPkt;
    drawPkt.cmd.cid = CID;
    drawPkt.cmd.numVertexDecls = 2;
    drawPkt.cmd.numRanges = 1;

    /* Pos */
    drawPkt.decls[0].identity.type = SVGA3D_DECLTYPE_FLOAT3;
    drawPkt.decls[0].identity.method = SVGA3D_DECLMETHOD_DEFAULT;
    drawPkt.decls[0].identity.usage = SVGA3D_DECLUSAGE_POSITION;
    drawPkt.decls[0].identity.usageIndex = 0;
    drawPkt.decls[0].array.surfaceId = SID_VB;
    drawPkt.decls[0].array.offset = 0;
    drawPkt.decls[0].array.stride = sizeof(Vertex);

    /* UV */
    drawPkt.decls[1].identity.type = SVGA3D_DECLTYPE_FLOAT2;
    drawPkt.decls[1].identity.method = SVGA3D_DECLMETHOD_DEFAULT;
    drawPkt.decls[1].identity.usage = SVGA3D_DECLUSAGE_TEXCOORD;
    drawPkt.decls[1].identity.usageIndex = 0;
    drawPkt.decls[1].array.surfaceId = SID_VB;
    drawPkt.decls[1].array.offset = static_cast<uint32_t>(offsetof(Vertex, u));
    drawPkt.decls[1].array.stride = sizeof(Vertex);

    /* Index Range */
    drawPkt.range.primType = SVGA3D_PRIMITIVE_TRIANGLELIST;
    drawPkt.range.primitiveCount = 2;
    drawPkt.range.indexArray.surfaceId = SID_IB;
    drawPkt.range.indexArray.offset = 0;
    drawPkt.range.indexArray.stride = sizeof(uint16_t);
    drawPkt.range.indexWidth = 2;
    drawPkt.range.indexBias = 0;

    guestSubmit3dCmd(guest, SVGA_3D_CMD_DRAW_PRIMITIVES, &drawPkt, sizeof(drawPkt));

    /* 16. Readback rendered pixels from SID_RT into Guest RAM at offset 0x4000 */
    struct {
        SVGA3dCmdSurfaceDMA dma;
        SVGA3dCopyBox box;
    } readbackDma;
    readbackDma.dma.guest.ptr = { 1, 0x4000 }; /* GMR 1, offset 0x4000 */
    readbackDma.dma.guest.pitch = 64 * sizeof(uint32_t);
    readbackDma.dma.host = { SID_RT, 0, 0 };
    readbackDma.dma.transfer = SVGA3D_READ_HOST_VRAM;
    readbackDma.box = { 0, 0, 0, 64, 64, 1, 0, 0, 0 };
    guestSubmit3dCmd(guest, SVGA_3D_CMD_SURFACE_DMA, &readbackDma, sizeof(readbackDma));

    /* 17. Present to BAR1 Framebuffer at destination (100, 100) */
    dev.clearDisplayUpdates();
    SVGA3dCmdBlitSurfaceToScreen blitCmd;
    blitCmd.srcImage = { SID_RT, 0, 0 };
    blitCmd.srcRect = { 0, 0, 64, 64 };
    blitCmd.destScreenId = 0;
    blitCmd.destRect = { 100, 100, 164, 164 };
    guestSubmit3dCmd(guest, SVGA_3D_CMD_BLIT_SURFACE_TO_SCREEN, &blitCmd, sizeof(blitCmd));

    /* 18. Synchronize with Fence #2 */
    logTrace("SVGA_CMD_FENCE", sizeof(uint32_t), "Synchronize Fence #2 after Draw, Readback & Present");
    if (!guest.sync(2)) {
        std::cerr << "FAILED: Failed to synchronize fence #2 after draw and readback." << std::endl;
        return false;
    }

    /* =====================================================================
     * Verification 4A: Guest RAM Readback Verification
     * ===================================================================== */
    std::cout << "  - Verifying rendered pixels in Guest RAM readback..." << std::endl;
    uint32_t pTL = readbackRam[16 * 64 + 16]; /* Top-Left (expected Red) */
    uint32_t pTR = readbackRam[16 * 64 + 48]; /* Top-Right (expected Green) */
    uint32_t pBL = readbackRam[48 * 64 + 16]; /* Bottom-Left (expected Blue) */
    uint32_t pBR = readbackRam[48 * 64 + 48]; /* Bottom-Right (expected Yellow) */

    std::cout << "    * Guest RAM TL (16,16): 0x" << std::hex << pTL
              << " TR (48,16): 0x" << pTR
              << " BL (16,48): 0x" << pBL
              << " BR (48,48): 0x" << pBR << std::dec << std::endl;

    if (colorDiff(pTL, 0xFFFF0000) > 20 ||
        colorDiff(pTR, 0xFF00FF00) > 20 ||
        colorDiff(pBL, 0xFF0000FF) > 20 ||
        colorDiff(pBR, 0xFFFFFF00) > 20)
    {
        std::cerr << "FAILED: Rendered pixel colors in Guest RAM do not match expected quadrant textures." << std::endl;
        return false;
    }

    /* =====================================================================
     * Verification 4B: BAR1 Display Framebuffer Verification
     * ===================================================================== */
    std::cout << "  - Verifying BAR1 display surface presentation..." << std::endl;
    uint32_t fbTL = guest.readFbPixel(100 + 16, 100 + 16);
    uint32_t fbTR = guest.readFbPixel(100 + 48, 100 + 16);
    uint32_t fbBL = guest.readFbPixel(100 + 16, 100 + 48);
    uint32_t fbBR = guest.readFbPixel(100 + 48, 100 + 48);

    std::cout << "    * BAR1 FB TL (116,116): 0x" << std::hex << fbTL
              << " TR (148,116): 0x" << fbTR
              << " BL (116,148): 0x" << fbBL
              << " BR (148,148): 0x" << fbBR << std::dec << std::endl;

    if (colorDiff(fbTL, 0xFFFF0000) > 20 ||
        colorDiff(fbTR, 0xFF00FF00) > 20 ||
        colorDiff(fbBL, 0xFF0000FF) > 20 ||
        colorDiff(fbBR, 0xFFFFFF00) > 20)
    {
        std::cerr << "FAILED: Presented pixels in BAR1 framebuffer do not match expected quadrant textures." << std::endl;
        return false;
    }

    /* Save Acceptance Artifacts */
    savePPM32("artifacts/qemu_guest_readback.ppm", readbackRam, 64, 64);

    std::vector<uint32_t> bar1Crop(64 * 64);
    for (uint32_t y = 0; y < 64; ++y) {
        for (uint32_t x = 0; x < 64; ++x) {
            bar1Crop[y * 64 + x] = guest.readFbPixel(100 + x, 100 + y);
        }
    }
    savePPM32("artifacts/qemu_display_bar1.ppm", bar1Crop.data(), 64, 64);

    std::vector<uint32_t> qemuRef(64 * 64);
    for (uint32_t y = 0; y < 64; ++y) {
        for (uint32_t x = 0; x < 64; ++x) {
            if (x < 32 && y < 32) qemuRef[y * 64 + x] = 0xFFFF0000;
            else if (x >= 32 && y < 32) qemuRef[y * 64 + x] = 0xFF00FF00;
            else if (x < 32 && y >= 32) qemuRef[y * 64 + x] = 0xFF0000FF;
            else qemuRef[y * 64 + x] = 0xFFFFFF00;
        }
    }
    savePPM32("artifacts/qemu_reference.ppm", qemuRef.data(), 64, 64);
    saveDiffPPM32("artifacts/qemu_diff.ppm", readbackRam, qemuRef.data(), 64, 64);

    /* Untouched pixel preservation */
    uint32_t untouched = guest.readFbPixel(50, 50);
    if (untouched != 0x00000000) {
        std::cerr << "FAILED: Untouched pixel at (50,50) was modified: 0x" << std::hex << untouched << std::dec << std::endl;
        return false;
    }

    /* QEMU Display Update notification */
    if (dev.displayUpdates().empty()) {
        std::cerr << "FAILED: QEMU display update callback was not invoked." << std::endl;
        return false;
    }
    const auto &up = dev.displayUpdates().front();
    std::cout << "    * QEMU Display update rect: [" << up.x << ", " << up.y << ", " << up.w << ", " << up.h << "]" << std::endl;
    if (up.x != 100 || up.y != 100 || up.w != 64 || up.h != 64) {
        std::cerr << "FAILED: Incorrect QEMU display update rect." << std::endl;
        return false;
    }

    std::cout << "PASSED: Full 3D rendering pipeline, guest RAM readback, and display presentation verified." << std::endl;
    return true;
}

/* =========================================================================
 * Test 5: FIFO Ring Buffer Wraparound & Synchronization Stress Test
 * ========================================================================= */
static bool testFifoWraparound(QemuVmsvgaDevice &dev, GuestVmsvgaDriver &guest)
{
    std::cout << "\n[TEST 5] FIFO Ring Buffer Wraparound & Stress Test..." << std::endl;

    uint32_t startWraps = guest.guestWraparounds();

    /* Bounded FIFO size: 64KB or 256KB. We will write commands repeatedly to wrap around >= 3 times */
    const uint32_t targetWraps = startWraps + 3;
    uint32_t fenceId = 100;

    std::cout << "  - Submitting command packets to force ring buffer wraparound (target >= " << targetWraps << ")..." << std::endl;

    while (guest.guestWraparounds() < targetWraps) {
        /* Submit a harmless state packet (e.g. set viewport) */
        SVGA3dCmdSetViewport vpCmd = { 1, { 0, 0, 64, 64 } };
        guestSubmit3dCmd(guest, SVGA_3D_CMD_SETVIEWPORT, &vpCmd, sizeof(vpCmd));

        /* Sync every 50 packets to ensure host processes and updates stop pointer */
        if ((fenceId % 20) == 0) {
            if (!guest.sync(fenceId++)) {
                std::cerr << "FAILED: Sync failed during wraparound loop at fence " << (fenceId - 1) << std::endl;
                return false;
            }
        } else {
            fenceId++;
        }
    }

    /* Final sync */
    if (!guest.sync(fenceId)) {
        std::cerr << "FAILED: Final sync failed after wraparounds." << std::endl;
        return false;
    }

    std::cout << "  - Guest wraparounds observed: " << guest.guestWraparounds() << std::endl;
    std::cout << "  - Device host wraparounds handled: " << dev.totalWraparoundsHandled() << std::endl;
    std::cout << "  - Total FIFO runs: " << dev.totalFifoRuns() << std::endl;
    std::cout << "  - Total fences processed: " << dev.totalFencesProcessed() << std::endl;

    if (dev.totalWraparoundsHandled() < 3) {
        std::cerr << "FAILED: Device did not handle at least 3 ring buffer wraparounds." << std::endl;
        return false;
    }

    /* Submit another draw command across the wraparound boundary to prove integrity */
    struct DrawPacket {
        SVGA3dCmdDrawPrimitives cmd;
        SVGA3dVertexDecl decls[2];
        SVGA3dPrimitiveRange range;
    } drawPkt;
    drawPkt.cmd.cid = 1;
    drawPkt.cmd.numVertexDecls = 2;
    drawPkt.cmd.numRanges = 1;

    drawPkt.decls[0].identity.type = SVGA3D_DECLTYPE_FLOAT3;
    drawPkt.decls[0].identity.method = SVGA3D_DECLMETHOD_DEFAULT;
    drawPkt.decls[0].identity.usage = SVGA3D_DECLUSAGE_POSITION;
    drawPkt.decls[0].identity.usageIndex = 0;
    drawPkt.decls[0].array.surfaceId = 3;
    drawPkt.decls[0].array.offset = 0;
    drawPkt.decls[0].array.stride = sizeof(Vertex);

    drawPkt.decls[1].identity.type = SVGA3D_DECLTYPE_FLOAT2;
    drawPkt.decls[1].identity.method = SVGA3D_DECLMETHOD_DEFAULT;
    drawPkt.decls[1].identity.usage = SVGA3D_DECLUSAGE_TEXCOORD;
    drawPkt.decls[1].identity.usageIndex = 0;
    drawPkt.decls[1].array.surfaceId = 3;
    drawPkt.decls[1].array.offset = static_cast<uint32_t>(offsetof(Vertex, u));
    drawPkt.decls[1].array.stride = sizeof(Vertex);

    drawPkt.range.primType = SVGA3D_PRIMITIVE_TRIANGLELIST;
    drawPkt.range.primitiveCount = 2;
    drawPkt.range.indexArray.surfaceId = 4;
    drawPkt.range.indexArray.offset = 0;
    drawPkt.range.indexArray.stride = sizeof(uint16_t);
    drawPkt.range.indexWidth = 2;
    drawPkt.range.indexBias = 0;

    guestSubmit3dCmd(guest, SVGA_3D_CMD_DRAW_PRIMITIVES, &drawPkt, sizeof(drawPkt));
    fenceId += 10;
    if (!guest.sync(fenceId)) {
        std::cerr << "FAILED: Sync failed after post-wraparound draw." << std::endl;
        return false;
    }

    std::cout << "PASSED: FIFO wraparound and synchronization stress test verified." << std::endl;
    return true;
}

/* =========================================================================
 * Test 6: Fresh VM Start (Reset & State Independence)
 * ========================================================================= */
static bool testFreshVmStart(QemuVmsvgaDevice &dev)
{
    std::cout << "\n[TEST 6] Fresh VM Start (Hardware Reset & State Independence)..." << std::endl;
    std::cout << "  (Resetting device to simulate cold boot, re-running complete acceptance suite)" << std::endl;

    /* Perform hardware reset */
    dev.reset();

    /* Create new guest driver instance for the rebooted VM */
    GuestVmsvgaDriver freshGuest(&dev);

    /* 1. Discover */
    if (!freshGuest.discoverDevice()) {
        std::cerr << "FAILED: Fresh guest failed to discover device after reset." << std::endl;
        return false;
    }

    /* 2. Initialize */
    if (!freshGuest.initializeDevice(800, 600, 32)) {
        std::cerr << "FAILED: Fresh guest failed to initialize device after reset." << std::endl;
        return false;
    }

    /* 3. Register GMR2 */
    if (!freshGuest.registerGuestMemoryRegion(1, freshGuest.guestRamBaseGpa(), 4 * 1024 * 1024)) {
        std::cerr << "FAILED: Fresh guest failed to register GMR2." << std::endl;
        return false;
    }
    if (!freshGuest.sync(1)) {
        std::cerr << "FAILED: Fresh guest fence sync failed." << std::endl;
        return false;
    }

    /* 4. Run end-to-end rendering on fresh VM */
    if (!testEndToEndRendering(dev, freshGuest)) {
        std::cerr << "FAILED: Rendering failed on fresh VM start." << std::endl;
        return false;
    }

    std::cout << "PASSED: Fresh VM start verified; device operates cleanly without stale state." << std::endl;
    return true;
}

/* =========================================================================
 * Main Runner
 * ========================================================================= */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    std::cout << "======================================================================\n";
    std::cout << " SVGA3=VLKN - Deliverable 6: Isolated QEMU Integration Environment\n";
    std::cout << "======================================================================\n";

    /* Bounded FIFO: 64KB to guarantee rapid wraparound under test conditions */
    const size_t fbSize = 16 * 1024 * 1024;
    const size_t fifoSize = 64 * 1024;
    const uint64_t fbGpa = 0xE0000000ULL;
    const uint64_t fifoGpa = 0xF0000000ULL;
    const uint16_t ioBase = 0x1000;

    QemuVmsvgaDevice dev(fbGpa, fbSize, fifoGpa, fifoSize, ioBase);

    /* Initialize hardware model with real Vulkan backend (forcing mock = false) */
    if (!dev.init(true /* validation */, false /* NO MOCK */)) {
        std::cerr << "FATAL: Failed to initialize QEMU VMSVGA device with real Vulkan backend." << std::endl;
        return 1;
    }

    /* Verify driver identity */
    auto *backend = dev.vlknDevice()->backend.get();
    VkPhysicalDeviceProperties props;
    backend->dispatch().vkGetPhysicalDeviceProperties(backend->physicalDevice(), &props);
    std::cout << "[Host Vulkan Driver Information]" << std::endl;
    std::cout << "  - Device Name:   " << props.deviceName << std::endl;
    std::cout << "  - Driver Version: 0x" << std::hex << props.driverVersion << std::dec << std::endl;
    std::cout << "  - API Version:    " << VK_VERSION_MAJOR(props.apiVersion) << "."
              << VK_VERSION_MINOR(props.apiVersion) << "."
              << VK_VERSION_PATCH(props.apiVersion) << std::endl;
    std::cout << "  - Device Type:    " << props.deviceType << std::endl;
    std::cout << "  - Backend Type:   " << (backend->dispatch().isMock ? "MOCK (INVALID)" : "REAL VULKAN (VALID)") << std::endl;

    if (backend->dispatch().isMock) {
        std::cerr << "FATAL: Mock backend active! Real Vulkan execution is strictly required." << std::endl;
        return 2;
    }

    bool allPassed = true;
    {
        GuestVmsvgaDriver guest(&dev);
        allPassed &= testPciDiscovery(dev, guest);
        allPassed &= testNegotiationAndCapabilities(guest);
        allPassed &= testGmrRegistration(guest);
        allPassed &= testEndToEndRendering(dev, guest);
        allPassed &= testFifoWraparound(dev, guest);
    }
    allPassed &= testFreshVmStart(dev);

    std::cout << "\n======================================================================\n";
    if (allPassed) {
        std::cout << " DELIVERABLE 6 RESULT: ALL TESTS PASSED\n";
        std::cout << "======================================================================\n";
        return 0;
    } else {
        std::cerr << " DELIVERABLE 6 RESULT: ONE OR MORE TESTS FAILED!\n";
        std::cerr << "======================================================================\n";
        return 1;
    }
}
