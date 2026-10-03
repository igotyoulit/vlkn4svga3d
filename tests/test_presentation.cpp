/*
 * SVGA3=VLKN - Deliverable 5: Actual Presentation Pipeline Acceptance Test Suite
 *
 * Verifies:
 * - Real Vulkan execution (software Vulkan / llvmpipe accepted, NO mock fallback)
 * - Validation layers active with zero errors
 * - Emulated QEMU display surface framebuffer (BAR1 mapping)
 * - Guest PRESENT command processing via FIFO (full surface & partial sub-rectangles)
 * - Guest BLIT_SURFACE_TO_SCREEN command processing via FIFO
 * - Screen boundary clipping for partial & out-of-bounds blits
 * - Explicit SVGASignedRect clipping: only pixels inside clip rects are transferred
 * - Strict preservation of untouched background and neighboring pixels
 * - Display update notifications (QEMU dpy_gfx_update hook)
 * - Analytical pixel verification and negative controls
 */

#include "svga3_vlkn.h"
#include "svga3_device.h"
#include "vlkn_backend.h"
#include "svga3_shader_translator.h"
#include "fake_host_adapter.h"

#include <iostream>
#include <vector>
#include <set>
#include <cstring>
#include <cmath>
#include <cassert>
#include <algorithm>

#define TEST_CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "  [FAIL] " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
            exit(1); \
        } else { \
            std::cout << "  [PASS] " << msg << std::endl; \
        } \
    } while(0)

#define TEST_CHECK_ERR(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "  [FAIL] " << msg << " (Expected error was not raised!)" << std::endl; \
            exit(1); \
        } else { \
            std::cout << "  [PASS] Negative control detected mismatch as expected: " << msg << std::endl; \
        } \
    } while(0)

#pragma pack(push, 1)
struct Pixel {
    uint8_t b, g, r, a;
};
#pragma pack(pop)

static inline bool pixelMatches(const Pixel &p, uint8_t r, uint8_t g, uint8_t b, uint8_t a, int tol = 2) {
    return std::abs((int)p.r - (int)r) <= tol &&
           std::abs((int)p.g - (int)g) <= tol &&
           std::abs((int)p.b - (int)b) <= tol &&
           std::abs((int)p.a - (int)a) <= tol;
}

static inline bool pixelsIdentical(const Pixel &p1, const Pixel &p2) {
    return p1.r == p2.r && p1.g == p2.g && p1.b == p2.b && p1.a == p2.a;
}

struct DirtyRect {
    int32_t x, y, w, h;
};

static std::vector<DirtyRect> g_dirtyRects;

static void testDisplayUpdateCallback(void *opaque, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)opaque;
    g_dirtyRects.push_back({x, y, w, h});
}

/* Vertex layout */
struct Vertex {
    float x, y, z;
    float u, v;
};

/* D3D9 Bytecode helper macros */
#define D3D9_DST(regType, regNum, mask) \
    (0x80000000u | (((regType) & 0x7) << 28) | ((((regType) >> 3) & 0x3) << 11) | (((mask) & 0xF) << 16) | ((regNum) & 0x7FF))

#define D3D9_SRC(regType, regNum, swiz) \
    (0x80000000u | (((regType) & 0x7) << 28) | ((((regType) >> 3) & 0x3) << 11) | (((swiz) & 0xFF) << 16) | ((regNum) & 0x7FF))

static void savePPM(const char *filename, const Pixel *pixels, int width, int height) {
    FILE *f = fopen(filename, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; ++i) {
        uint8_t rgb[3] = { pixels[i].r, pixels[i].g, pixels[i].b };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main() {
    std::cout << "======================================================================\n";
    std::cout << "SVGA3=VLKN Deliverable 5: Actual Presentation Acceptance Suite\n";
    std::cout << "======================================================================\n";

    /* Step 1: Initialize Real Vulkan Device (NO mock fallback) */
    Svga3VlknConfig cfg = {};
    cfg.appName = "Deliverable 5: Presentation Acceptance Test";
    cfg.forceMockBackend = false;
    cfg.enableValidationLayers = true;
    cfg.stagingBufferSize = 64 * 1024;

    Svga3VlknDevice *dev = svga3_vlkn_device_create(&cfg);
    TEST_CHECK(dev != nullptr, "Real Vulkan device creation succeeded");
    svga3_vlkn::VlknBackend *backend = dev->backend.get();
    TEST_CHECK(!backend->dispatch().isMock, "Execution is running on real Vulkan driver (NOT mock)");

    VkPhysicalDeviceProperties props;
    backend->dispatch().vkGetPhysicalDeviceProperties(backend->physicalDevice(), &props);
    std::cout << "  [INFO] Real Vulkan Driver: " << props.deviceName
              << " (Driver version: 0x" << std::hex << props.driverVersion << std::dec
              << ", Vulkan API: " << VK_VERSION_MAJOR(props.apiVersion) << "."
              << VK_VERSION_MINOR(props.apiVersion) << "." << VK_VERSION_PATCH(props.apiVersion) << ")\n";

    /* Step 2: Configure Emulated QEMU Display Surface (BAR1 Framebuffer) */
    const uint32_t FB_W = 160;
    const uint32_t FB_H = 120;
    const uint32_t FB_BPP = 4;
    std::vector<Pixel> qemuFb(FB_W * FB_H);

    /* Initialize baseline background pattern: Dark Navy Blue (0xFF051025) */
    const Pixel BASELINE_PIXEL = { 0x25, 0x10, 0x05, 0xFF };
    auto resetFramebuffer = [&]() {
        for (size_t i = 0; i < qemuFb.size(); ++i) {
            qemuFb[i] = BASELINE_PIXEL;
        }
        g_dirtyRects.clear();
    };
    resetFramebuffer();

    Svga3VlknStatus status = svga3_vlkn_device_set_framebuffer(dev, qemuFb.data(), 0xE0000000ULL, qemuFb.size() * sizeof(Pixel),
                                                              FB_W, FB_H, FB_W * FB_BPP, FB_BPP);
    TEST_CHECK(status == SVGA3_VLKN_SUCCESS, "Registered emulated QEMU display framebuffer (BAR1)");

    Svga3HostAdapter adapter = fake_host_adapter(testDisplayUpdateCallback, nullptr);
    status = svga3_vlkn_device_set_host_adapter(dev, &adapter);
    TEST_CHECK(status == SVGA3_VLKN_SUCCESS, "Registered display surface update callback via host adapter");

    /* Step 3: Render a realistic 3D scene to an SVGA3D Render Target Surface */
    const uint32_t CID = 501;
    const uint32_t SID_RT = 502;
    const uint32_t SID_TEX = 503;
    const uint32_t SID_VB = 504;
    const uint32_t RT_W = 64;
    const uint32_t RT_H = 64;

    svga3_vlkn_context_create(dev, CID);

    SVGA3dSize rtSize = { RT_W, RT_H, 1 };
    svga3_vlkn_surface_define(dev, SID_RT, SVGA3D_SURFACE_HINT_RENDERTARGET, SVGA3D_X8R8G8B8, &rtSize, 1);
    svga3_vlkn_context_set_render_target(dev, CID, SVGA3D_RT_COLOR0, SID_RT, 0, 0);

    SVGA3dRect fullVp = { 0, 0, RT_W, RT_H };
    svga3_vlkn_context_set_viewport(dev, CID, &fullVp);
    svga3_vlkn_context_set_render_state(dev, CID, SVGA3D_RS_CULLMODE, SVGA3D_FACE_NONE);
    svga3_vlkn_context_clear(dev, CID, SVGA3D_CLEAR_COLOR, 0x00000000, 1.0f, 0, nullptr, 0);

    /* 4x4 Texture with 4 Quadrants: Top-Left=Red, Top-Right=Green, Bottom-Left=Blue, Bottom-Right=Yellow */
    SVGA3dSize texSize = { 4, 4, 1 };
    svga3_vlkn_surface_define(dev, SID_TEX, SVGA3D_SURFACE_HINT_TEXTURE, SVGA3D_X8R8G8B8, &texSize, 1);
    uint32_t texPixels[16];
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            uint32_t color = 0;
            if (x < 2 && y < 2) {
                color = 0xFFFF0000; /* Red */
            } else if (x >= 2 && y < 2) {
                color = 0xFF00FF00; /* Green */
            } else if (x < 2 && y >= 2) {
                color = 0xFF0000FF; /* Blue */
            } else {
                color = 0xFFFFFF00; /* Yellow */
            }
            texPixels[y * 4 + x] = color;
        }
    }
    SVGA3dBox tBox = { 0, 0, 0, 4, 4, 1 };
    svga3_vlkn_surface_dma_upload(dev, SID_TEX, 0, &tBox, texPixels, 4 * sizeof(uint32_t));
    svga3_vlkn_context_set_texture(dev, CID, 0, SID_TEX);

    /* Real D3D9 Bytecode Shaders */
    const uint32_t vsBytecode[] = {
        0xFFFE0300, /* vs_3_0 */
        (31) | (2 << 24), 0x80000000 | 0, D3D9_DST(1, 0, 0xF), /* dcl_position v0 */
        (31) | (2 << 24), 0x80000000 | 5, D3D9_DST(1, 1, 0xF), /* dcl_texcoord v1 */
        (20) | (3 << 24), D3D9_DST(4, 0, 0xF), D3D9_SRC(1, 0, 0xE4), D3D9_SRC(2, 0, 0xE4), /* m4x4 oPos, v0, c0 */
        (1)  | (2 << 24), D3D9_DST(6, 0, 0xF), D3D9_SRC(1, 1, 0xE4), /* mov oT0, v1 */
        0x0000FFFF
    };
    const uint32_t psBytecode[] = {
        0xFFFF0300, /* ps_3_0 */
        (31) | (2 << 24), 0x80000000 | 5, D3D9_DST(1, 1, 0xF), /* dcl_texcoord v1 */
        (31) | (2 << 24), 0x80000000 | (2 << 28), D3D9_DST(10, 0, 0xF), /* dcl_2d s0 */
        (66) | (3 << 24), D3D9_DST(0, 0, 0xF), D3D9_SRC(1, 1, 0xE4), D3D9_SRC(10, 0, 0xE4), /* texld r0, v1, s0 */
        (5)  | (3 << 24), D3D9_DST(8, 0, 0xF), D3D9_SRC(0, 0, 0xE4), D3D9_SRC(2, 0, 0xE4), /* mul oC0, r0, c0 */
        0x0000FFFF
    };

    status = svga3_vlkn_context_define_shader(dev, CID, 1, SVGA3D_SHADERTYPE_VS, vsBytecode, sizeof(vsBytecode)/sizeof(uint32_t));
    TEST_CHECK(status == SVGA3_VLKN_SUCCESS, "Define VS in context");
    status = svga3_vlkn_context_define_shader(dev, CID, 2, SVGA3D_SHADERTYPE_PS, psBytecode, sizeof(psBytecode)/sizeof(uint32_t));
    TEST_CHECK(status == SVGA3_VLKN_SUCCESS, "Define PS in context");
    svga3_vlkn_context_set_shader(dev, CID, SVGA3D_SHADERTYPE_VS, 1);
    svga3_vlkn_context_set_shader(dev, CID, SVGA3D_SHADERTYPE_PS, 2);

    /* VS Matrix: Identity */
    float matIdentity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    for (int r = 0; r < 4; ++r) {
        uint32_t rowValues[4];
        memcpy(rowValues, &matIdentity[r * 4], sizeof(float) * 4);
        svga3_vlkn_context_set_shader_const(dev, CID, r, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, rowValues);
    }

    /* PS Constant c0 = (1.0, 1.0, 1.0, 1.0) */
    float psConst[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    uint32_t cVal[4];
    memcpy(cVal, psConst, sizeof(psConst));
    svga3_vlkn_context_set_shader_const(dev, CID, 0, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT, cVal);

    /* Quad covering the entire 64x64 render target */
    Vertex quadVerts[6] = {
        { -1.0f,  1.0f, 0.5f,  0.0f, 0.0f },
        {  1.0f,  1.0f, 0.5f,  1.0f, 0.0f },
        {  1.0f, -1.0f, 0.5f,  1.0f, 1.0f },
        { -1.0f,  1.0f, 0.5f,  0.0f, 0.0f },
        {  1.0f, -1.0f, 0.5f,  1.0f, 1.0f },
        { -1.0f, -1.0f, 0.5f,  0.0f, 1.0f }
    };
    SVGA3dSize vbSz = { sizeof(quadVerts), 1, 1 };
    svga3_vlkn_surface_define(dev, SID_VB, SVGA3D_SURFACE_HINT_VERTEXBUFFER, SVGA3D_BUFFER, &vbSz, 1);
    SVGA3dBox vbBox = { 0, 0, 0, sizeof(quadVerts), 1, 1 };
    svga3_vlkn_surface_dma_upload(dev, SID_VB, 0, &vbBox, quadVerts, sizeof(quadVerts));

    SVGA3dVertexDecl decls[2] = {};
    decls[0].identity.type = SVGA3D_DECLTYPE_FLOAT3;
    decls[0].identity.method = SVGA3D_DECLMETHOD_DEFAULT;
    decls[0].identity.usage = SVGA3D_DECLUSAGE_POSITION;
    decls[0].array.surfaceId = SID_VB;
    decls[0].array.stride = sizeof(Vertex);
    decls[0].array.offset = offsetof(Vertex, x);

    decls[1].identity.type = SVGA3D_DECLTYPE_FLOAT2;
    decls[1].identity.method = SVGA3D_DECLMETHOD_DEFAULT;
    decls[1].identity.usage = SVGA3D_DECLUSAGE_TEXCOORD;
    decls[1].array.surfaceId = SID_VB;
    decls[1].array.stride = sizeof(Vertex);
    decls[1].array.offset = offsetof(Vertex, u);

    SVGA3dPrimitiveRange r1 = { SVGA3D_PRIMITIVE_TRIANGLELIST, 2, {}, 0, 0 };
    svga3_vlkn_context_draw(dev, CID, SVGA3D_PRIMITIVE_TRIANGLELIST, decls, 2, &r1, 1);
    svga3_vlkn_device_wait_idle(dev);

    /* Read back 3D surface to analytical reference vector */
    std::vector<Pixel> renderedSurface(RT_W * RT_H);
    svga3_vlkn_surface_dma_download(dev, SID_RT, 0, nullptr, renderedSurface.data(), RT_W * sizeof(Pixel));

    /* Verify rendered surface corners */
    Pixel tl = renderedSurface[0];
    Pixel tr = renderedSurface[RT_W - 1];
    Pixel bl = renderedSurface[(RT_H - 1) * RT_W];
    Pixel br = renderedSurface[(RT_H - 1) * RT_W + (RT_W - 1)];
    TEST_CHECK(pixelMatches(tl, 255, 0, 0, 255), "3D Rendered Surface TL is Red (255, 0, 0)");
    TEST_CHECK(pixelMatches(tr, 0, 255, 0, 255), "3D Rendered Surface TR is Green (0, 255, 0)");
    TEST_CHECK(pixelMatches(bl, 0, 0, 255, 255), "3D Rendered Surface BL is Blue (0, 0, 255)");
    TEST_CHECK(pixelMatches(br, 255, 255, 0, 255), "3D Rendered Surface BR is Yellow (255, 255, 0)");

    /* Helper lambda to execute FIFO stream */
    auto executeFifo = [&](const std::vector<uint8_t> &stream) -> Svga3VlknStatus {
        size_t consumed = 0;
        return svga3_vlkn_fifo_execute(dev, stream.data(), stream.size(), &consumed);
    };

    /* =========================================================================
     * TEST 1: Full Surface PRESENT via FIFO Command (numRects = 0)
     * ========================================================================= */
    std::cout << "\n--- Test 1: Full Surface PRESENT via FIFO (numRects = 0) ---" << std::endl;
    resetFramebuffer();

    /* Build FIFO packet for SVGA_3D_CMD_PRESENT */
    std::vector<uint8_t> fStream1;
    uint32_t cmdPresent = SVGA_3D_CMD_PRESENT;
    SVGA3dCmdHeader hdrPresent = { sizeof(SVGA3dCmdPresent) };
    SVGA3dCmdPresent p1 = {};
    p1.sid = SID_RT;
    auto append1 = [&](const void *data, size_t sz) {
        const uint8_t *b = reinterpret_cast<const uint8_t*>(data);
        fStream1.insert(fStream1.end(), b, b + sz);
    };
    append1(&cmdPresent, sizeof(cmdPresent));
    append1(&hdrPresent, sizeof(hdrPresent));
    append1(&p1, sizeof(p1));

    status = executeFifo(fStream1);
    TEST_CHECK(status == SVGA3_VLKN_SUCCESS, "Processed SVGA_3D_CMD_PRESENT via FIFO");

    /* Verify:
     * 1. Display surface [0..63, 0..63] contains the rendered 3D surface exactly.
     * 2. Display surface pixels outside [0..63, 0..63] retain BASELINE_PIXEL.
     */
    bool fullPresentMatches = true;
    for (uint32_t y = 0; y < RT_H; ++y) {
        for (uint32_t x = 0; x < RT_W; ++x) {
            const Pixel &dispP = qemuFb[y * FB_W + x];
            const Pixel &surfP = renderedSurface[y * RT_W + x];
            if (!pixelsIdentical(dispP, surfP)) {
                fullPresentMatches = false;
                break;
            }
        }
    }
    TEST_CHECK(fullPresentMatches, "Test 1: Full surface PRESENT exactly matches rendered 3D surface [0..63, 0..63]");

    /* Check untouched areas */
    bool rightUntouched = true;
    for (uint32_t y = 0; y < FB_H; ++y) {
        for (uint32_t x = RT_W; x < FB_W; ++x) {
            if (!pixelsIdentical(qemuFb[y * FB_W + x], BASELINE_PIXEL)) {
                rightUntouched = false;
                break;
            }
        }
    }
    TEST_CHECK(rightUntouched, "Test 1: Pixels to the right [64..159, 0..119] are strictly preserved untouched");

    bool bottomUntouched = true;
    for (uint32_t y = RT_H; y < FB_H; ++y) {
        for (uint32_t x = 0; x < RT_W; ++x) {
            if (!pixelsIdentical(qemuFb[y * FB_W + x], BASELINE_PIXEL)) {
                bottomUntouched = false;
                break;
            }
        }
    }
    TEST_CHECK(bottomUntouched, "Test 1: Pixels below [0..63, 64..119] are strictly preserved untouched");

    /* Check dirty rect notification */
    TEST_CHECK(!g_dirtyRects.empty(), "Test 1: Display update callback received dirty notifications");
    TEST_CHECK(g_dirtyRects[0].x == 0 && g_dirtyRects[0].y == 0 &&
               g_dirtyRects[0].w == (int32_t)RT_W && g_dirtyRects[0].h == (int32_t)RT_H,
               "Test 1: Dirty rectangle notified as (0, 0, 64, 64)");

    savePPM("artifacts/presentation_test1_full.ppm", qemuFb.data(), FB_W, FB_H);

    /* =========================================================================
     * TEST 2: Partial PRESENT with Explicit Sub-Rectangles via FIFO
     * ========================================================================= */
    std::cout << "\n--- Test 2: Partial PRESENT with Sub-Rectangles via FIFO ---" << std::endl;
    resetFramebuffer();

    /* Copy two disjoint sub-rectangles:
     * Rect 1: x=10, y=10, w=20, h=20, srcx=0, srcy=0 (copies Red TL corner to (10, 10))
     * Rect 2: x=80, y=50, w=25, h=25, srcx=39, srcy=39 (copies Yellow BR corner to (80, 50))
     */
    std::vector<uint8_t> fStream2;
    auto append2 = [&](const void *data, size_t sz) {
        const uint8_t *b = reinterpret_cast<const uint8_t*>(data);
        fStream2.insert(fStream2.end(), b, b + sz);
    };
    SVGA3dCmdHeader hdrPresent2 = { sizeof(SVGA3dCmdPresent) + 2 * sizeof(SVGA3dCopyRect) };
    SVGA3dCmdPresent p2 = {};
    p2.sid = SID_RT;
    SVGA3dCopyRect copyRects[2] = {
        { 10, 10, 20, 20, 0, 0 },
        { 80, 50, 25, 25, 39, 39 }
    };
    append2(&cmdPresent, sizeof(cmdPresent));
    append2(&hdrPresent2, sizeof(hdrPresent2));
    append2(&p2, sizeof(p2));
    append2(copyRects, sizeof(copyRects));

    status = executeFifo(fStream2);
    TEST_CHECK(status == SVGA3_VLKN_SUCCESS, "Processed partial SVGA_3D_CMD_PRESENT via FIFO");

    /* Verify Rect 1: [10..29, 10..29] matches [0..19, 0..19] of source */
    bool rect1Matches = true;
    for (uint32_t y = 0; y < 20; ++y) {
        for (uint32_t x = 0; x < 20; ++x) {
            const Pixel &dispP = qemuFb[(10 + y) * FB_W + (10 + x)];
            const Pixel &surfP = renderedSurface[(0 + y) * RT_W + (0 + x)];
            if (!pixelsIdentical(dispP, surfP)) {
                rect1Matches = false;
                break;
            }
        }
    }
    TEST_CHECK(rect1Matches, "Test 2: Sub-rectangle 1 [10..29, 10..29] matches source surface [0..19, 0..19]");

    /* Verify Rect 2: [80..104, 50..74] matches [39..63, 39..63] of source */
    bool rect2Matches = true;
    for (uint32_t y = 0; y < 25; ++y) {
        for (uint32_t x = 0; x < 25; ++x) {
            const Pixel &dispP = qemuFb[(50 + y) * FB_W + (80 + x)];
            const Pixel &surfP = renderedSurface[(39 + y) * RT_W + (39 + x)];
            if (!pixelsIdentical(dispP, surfP)) {
                rect2Matches = false;
                break;
            }
        }
    }
    TEST_CHECK(rect2Matches, "Test 2: Sub-rectangle 2 [80..104, 50..74] matches source surface [39..63, 39..63]");

    /* Verify boundary isolation: pixels immediately surrounding Rect 1 are UNTOUCHED */
    TEST_CHECK(pixelsIdentical(qemuFb[9 * FB_W + 10], BASELINE_PIXEL), "Test 2: Pixel (10, 9) above Rect 1 preserved");
    TEST_CHECK(pixelsIdentical(qemuFb[30 * FB_W + 10], BASELINE_PIXEL), "Test 2: Pixel (10, 30) below Rect 1 preserved");
    TEST_CHECK(pixelsIdentical(qemuFb[10 * FB_W + 9], BASELINE_PIXEL), "Test 2: Pixel (9, 10) left of Rect 1 preserved");
    TEST_CHECK(pixelsIdentical(qemuFb[10 * FB_W + 30], BASELINE_PIXEL), "Test 2: Pixel (30, 10) right of Rect 1 preserved");

    /* Verify notifications for both rects */
    TEST_CHECK(g_dirtyRects.size() == 2, "Test 2: Received exactly 2 dirty rectangle notifications");
    TEST_CHECK(g_dirtyRects[0].x == 10 && g_dirtyRects[0].y == 10 && g_dirtyRects[0].w == 20 && g_dirtyRects[0].h == 20,
               "Test 2: Dirty rect 1 notified as (10, 10, 20, 20)");
    TEST_CHECK(g_dirtyRects[1].x == 80 && g_dirtyRects[1].y == 50 && g_dirtyRects[1].w == 25 && g_dirtyRects[1].h == 25,
               "Test 2: Dirty rect 2 notified as (80, 50, 25, 25)");

    savePPM("artifacts/presentation_test2_partial.ppm", qemuFb.data(), FB_W, FB_H);

    /* =========================================================================
     * TEST 3: PRESENT Out-of-Bounds Screen Boundary Clipping
     * ========================================================================= */
    std::cout << "\n--- Test 3: PRESENT Screen Boundary Clipping ---" << std::endl;
    resetFramebuffer();

    /* Rect extending past the right and bottom edges of the 160x120 screen:
     * x=140, y=100, w=40, h=40, srcx=0, srcy=0
     * Screen bounds allow only 20x20 visible: [140..159, 100..119]
     */
    std::vector<uint8_t> fStream3;
    auto append3 = [&](const void *data, size_t sz) {
        const uint8_t *b = reinterpret_cast<const uint8_t*>(data);
        fStream3.insert(fStream3.end(), b, b + sz);
    };
    SVGA3dCmdHeader hdrPresent3 = { sizeof(SVGA3dCmdPresent) + sizeof(SVGA3dCopyRect) };
    SVGA3dCmdPresent p3 = {};
    p3.sid = SID_RT;
    SVGA3dCopyRect oobRect = { 140, 100, 40, 40, 0, 0 };
    append3(&cmdPresent, sizeof(cmdPresent));
    append3(&hdrPresent3, sizeof(hdrPresent3));
    append3(&p3, sizeof(p3));
    append3(&oobRect, sizeof(oobRect));

    status = executeFifo(fStream3);
    TEST_CHECK(status == SVGA3_VLKN_SUCCESS, "Processed clipped SVGA_3D_CMD_PRESENT without error");

    /* Verify visible portion was updated without crashing */
    bool clippedMatches = true;
    for (uint32_t y = 0; y < 20; ++y) {
        for (uint32_t x = 0; x < 20; ++x) {
            const Pixel &dispP = qemuFb[(100 + y) * FB_W + (140 + x)];
            const Pixel &surfP = renderedSurface[(0 + y) * RT_W + (0 + x)];
            if (!pixelsIdentical(dispP, surfP)) {
                clippedMatches = false;
                break;
            }
        }
    }
    TEST_CHECK(clippedMatches, "Test 3: Visible 20x20 intersection [140..159, 100..119] updated correctly");

    /* Untouched pixels before x=140 and above y=100 remain baseline */
    TEST_CHECK(pixelsIdentical(qemuFb[100 * FB_W + 139], BASELINE_PIXEL), "Test 3: Pixel (139, 100) outside clipped rect preserved");
    TEST_CHECK(pixelsIdentical(qemuFb[99 * FB_W + 140], BASELINE_PIXEL), "Test 3: Pixel (140, 99) outside clipped rect preserved");

    /* =========================================================================
     * TEST 4: BLIT_SURFACE_TO_SCREEN via FIFO (No Clip Rects)
     * ========================================================================= */
    std::cout << "\n--- Test 4: BLIT_SURFACE_TO_SCREEN via FIFO (No Clip Rects) ---" << std::endl;
    resetFramebuffer();

    /* Blit from source surface [16, 16, 48, 48] to screen [30, 20, 62, 52] */
    std::vector<uint8_t> fStream4;
    auto append4 = [&](const void *data, size_t sz) {
        const uint8_t *b = reinterpret_cast<const uint8_t*>(data);
        fStream4.insert(fStream4.end(), b, b + sz);
    };
    uint32_t cmdBlit = SVGA_3D_CMD_BLIT_SURFACE_TO_SCREEN;
    SVGA3dCmdHeader hdrBlit4 = { sizeof(SVGA3dCmdBlitSurfaceToScreen) };
    SVGA3dCmdBlitSurfaceToScreen p4 = {};
    p4.srcImage.sid = SID_RT;
    p4.srcImage.face = 0;
    p4.srcImage.mipmap = 0;
    p4.srcRect = { 16, 16, 48, 48 };
    p4.destScreenId = 0;
    p4.destRect = { 30, 20, 62, 52 };
    append4(&cmdBlit, sizeof(cmdBlit));
    append4(&hdrBlit4, sizeof(hdrBlit4));
    append4(&p4, sizeof(p4));

    status = executeFifo(fStream4);
    TEST_CHECK(status == SVGA3_VLKN_SUCCESS, "Processed SVGA_3D_CMD_BLIT_SURFACE_TO_SCREEN via FIFO");

    /* Verify blitted region [30..61, 20..51] matches source [16..47, 16..47] */
    bool blitMatches = true;
    for (uint32_t y = 0; y < 32; ++y) {
        for (uint32_t x = 0; x < 32; ++x) {
            const Pixel &dispP = qemuFb[(20 + y) * FB_W + (30 + x)];
            const Pixel &surfP = renderedSurface[(16 + y) * RT_W + (16 + x)];
            if (!pixelsIdentical(dispP, surfP)) {
                blitMatches = false;
                break;
            }
        }
    }
    TEST_CHECK(blitMatches, "Test 4: BLIT_SURFACE_TO_SCREEN region [30..61, 20..51] matches source [16..47, 16..47]");

    /* Verify surrounding border pixels remain untouched */
    TEST_CHECK(pixelsIdentical(qemuFb[19 * FB_W + 30], BASELINE_PIXEL), "Test 4: Pixel (30, 19) above blit preserved");
    TEST_CHECK(pixelsIdentical(qemuFb[52 * FB_W + 30], BASELINE_PIXEL), "Test 4: Pixel (30, 52) below blit preserved");
    TEST_CHECK(pixelsIdentical(qemuFb[20 * FB_W + 29], BASELINE_PIXEL), "Test 4: Pixel (29, 20) left of blit preserved");
    TEST_CHECK(pixelsIdentical(qemuFb[20 * FB_W + 62], BASELINE_PIXEL), "Test 4: Pixel (62, 20) right of blit preserved");

    savePPM("artifacts/presentation_test4_blit.ppm", qemuFb.data(), FB_W, FB_H);

    /* =========================================================================
     * TEST 5: BLIT_SURFACE_TO_SCREEN with Explicit SVGASignedRect Clipping
     * ========================================================================= */
    std::cout << "\n--- Test 5: BLIT_SURFACE_TO_SCREEN with Explicit SVGASignedRect Clipping ---" << std::endl;
    resetFramebuffer();

    /* Dest rect: [10, 10, 70, 70] (60x60 pixels)
     * Provide 2 clip rects:
     *   Clip 1: [15, 15, 35, 35] (20x20 pixels)
     *   Clip 2: [45, 45, 65, 65] (20x20 pixels)
     * Pixels in destination rect outside these clip rects MUST remain untouched!
     */
    std::vector<uint8_t> fStream5;
    auto append5 = [&](const void *data, size_t sz) {
        const uint8_t *b = reinterpret_cast<const uint8_t*>(data);
        fStream5.insert(fStream5.end(), b, b + sz);
    };
    SVGA3dCmdHeader hdrBlit5 = { sizeof(SVGA3dCmdBlitSurfaceToScreen) + 2 * sizeof(SVGASignedRect) };
    SVGA3dCmdBlitSurfaceToScreen p5 = {};
    p5.srcImage.sid = SID_RT;
    p5.srcImage.face = 0;
    p5.srcImage.mipmap = 0;
    p5.srcRect = { 0, 0, 60, 60 };
    p5.destScreenId = 0;
    p5.destRect = { 10, 10, 70, 70 };
    SVGASignedRect blitClips[2] = {
        { 5, 5, 25, 25 },
        { 35, 35, 55, 55 }
    };
    append5(&cmdBlit, sizeof(cmdBlit));
    append5(&hdrBlit5, sizeof(hdrBlit5));
    append5(&p5, sizeof(p5));
    append5(blitClips, sizeof(blitClips));

    status = executeFifo(fStream5);
    TEST_CHECK(status == SVGA3_VLKN_SUCCESS, "Processed clipped SVGA_3D_CMD_BLIT_SURFACE_TO_SCREEN via FIFO");

    /* Verify Clip 1: [15..34, 15..34] received surface pixels from [(15-10)..(34-10)] = [5..24] */
    bool clip1Matches = true;
    for (int32_t y = 15; y < 35; ++y) {
        for (int32_t x = 15; x < 35; ++x) {
            int32_t sx = x - 10;
            int32_t sy = y - 10;
            if (!pixelsIdentical(qemuFb[y * FB_W + x], renderedSurface[sy * RT_W + sx])) {
                clip1Matches = false;
                break;
            }
        }
    }
    TEST_CHECK(clip1Matches, "Test 5: Pixels inside Clip Rect 1 [15..34, 15..34] correctly blitted");

    /* Verify Clip 2: [45..64, 45..64] received surface pixels from [(45-10)..(64-10)] = [35..54] */
    bool clip2Matches = true;
    for (int32_t y = 45; y < 65; ++y) {
        for (int32_t x = 45; x < 65; ++x) {
            int32_t sx = x - 10;
            int32_t sy = y - 10;
            if (!pixelsIdentical(qemuFb[y * FB_W + x], renderedSurface[sy * RT_W + sx])) {
                clip2Matches = false;
                break;
            }
        }
    }
    TEST_CHECK(clip2Matches, "Test 5: Pixels inside Clip Rect 2 [45..64, 45..64] correctly blitted");

    /* Verify: A pixel inside destRect [10..69, 10..69] but OUTSIDE clip rects (e.g. (40, 40), (20, 50), (50, 20))
     * MUST remain UNTOUCHED BASELINE!
     */
    TEST_CHECK(pixelsIdentical(qemuFb[40 * FB_W + 40], BASELINE_PIXEL),
               "Test 5: Pixel (40, 40) inside destRect but between clip rects remains strictly preserved");
    TEST_CHECK(pixelsIdentical(qemuFb[50 * FB_W + 20], BASELINE_PIXEL),
               "Test 5: Pixel (20, 50) inside destRect but outside clip rects remains strictly preserved");
    TEST_CHECK(pixelsIdentical(qemuFb[20 * FB_W + 50], BASELINE_PIXEL),
               "Test 5: Pixel (50, 20) inside destRect but outside clip rects remains strictly preserved");
    TEST_CHECK(pixelsIdentical(qemuFb[12 * FB_W + 12], BASELINE_PIXEL),
               "Test 5: Pixel (12, 12) inside destRect before Clip 1 remains strictly preserved");

    savePPM("artifacts/presentation_test5_clipped.ppm", qemuFb.data(), FB_W, FB_H);

    /* =========================================================================
     * TEST 6: BLIT_SURFACE_TO_SCREEN with Partially Off-Screen Destination
     * ========================================================================= */
    std::cout << "\n--- Test 6: BLIT_SURFACE_TO_SCREEN Partially Off-Screen Dest ---" << std::endl;
    resetFramebuffer();

    /* Dest rect with negative left and top coordinates:
     * src: [0, 0, 40, 40]
     * dest: [-10, -10, 30, 30]
     * Visible destination on screen is [0..29, 0..29], corresponding to src [10..39, 10..39]
     */
    std::vector<uint8_t> fStream6;
    auto append6 = [&](const void *data, size_t sz) {
        const uint8_t *b = reinterpret_cast<const uint8_t*>(data);
        fStream6.insert(fStream6.end(), b, b + sz);
    };
    SVGA3dCmdHeader hdrBlit6 = { sizeof(SVGA3dCmdBlitSurfaceToScreen) };
    SVGA3dCmdBlitSurfaceToScreen p6 = {};
    p6.srcImage.sid = SID_RT;
    p6.srcImage.face = 0;
    p6.srcImage.mipmap = 0;
    p6.srcRect = { 0, 0, 40, 40 };
    p6.destScreenId = 0;
    p6.destRect = { -10, -10, 30, 30 };
    append6(&cmdBlit, sizeof(cmdBlit));
    append6(&hdrBlit6, sizeof(hdrBlit6));
    append6(&p6, sizeof(p6));

    status = executeFifo(fStream6);
    TEST_CHECK(status == SVGA3_VLKN_SUCCESS, "Processed negative-coordinate SVGA_3D_CMD_BLIT_SURFACE_TO_SCREEN");

    bool offscreenMatches = true;
    for (int32_t dy = 0; dy < 30; ++dy) {
        for (int32_t dx = 0; dx < 30; ++dx) {
            int32_t sx = dx - (-10);
            int32_t sy = dy - (-10);
            if (!pixelsIdentical(qemuFb[dy * FB_W + dx], renderedSurface[sy * RT_W + sx])) {
                offscreenMatches = false;
                break;
            }
        }
    }
    TEST_CHECK(offscreenMatches, "Test 6: Visible [0..29, 0..29] correctly matches clipped source [10..39, 10..39]");
    TEST_CHECK(pixelsIdentical(qemuFb[30 * FB_W + 30], BASELINE_PIXEL), "Test 6: Pixel (30, 30) outside destination preserved");

    /* A small visible area of a large image must fit in the staging buffer.
     * The entire 256x256 image exceeds the deliberately small 64 KiB pool. */
    const uint32_t SID_LARGE = 505;
    SVGA3dSize largeSize = {256, 256, 1};
    status = svga3_vlkn_surface_define(dev, SID_LARGE, SVGA3D_SURFACE_HINT_TEXTURE,
        SVGA3D_X8R8G8B8, &largeSize, 1);
    TEST_CHECK(status == SVGA3_VLKN_SUCCESS, "Define image larger than readback staging capacity");
    std::vector<Pixel> tile(8 * 8);
    for (size_t i = 0; i < tile.size(); ++i)
        tile[i] = Pixel{static_cast<uint8_t>(i), static_cast<uint8_t>(i + 32), 210, 255};
    SVGA3dBox tileBox = {200, 201, 0, 8, 8, 1};
    status = svga3_vlkn_surface_dma_upload(dev, SID_LARGE, 0, &tileBox, tile.data(), 8 * sizeof(Pixel));
    TEST_CHECK(status == SVGA3_VLKN_SUCCESS, "Upload offset tile for partial presentation");

    auto executeBlit = [&](const SVGA3dCmdBlitSurfaceToScreen &command,
                           const std::vector<SVGASignedRect> &clips) {
        const size_t payloadBytes = sizeof(command) + clips.size() * sizeof(SVGASignedRect);
        std::vector<uint8_t> packet(sizeof(cmdBlit) + sizeof(SVGA3dCmdHeader) + payloadBytes);
        SVGA3dCmdHeader header = {static_cast<uint32_t>(payloadBytes)};
        memcpy(packet.data(), &cmdBlit, sizeof(cmdBlit));
        memcpy(packet.data() + sizeof(cmdBlit), &header, sizeof(header));
        memcpy(packet.data() + sizeof(cmdBlit) + sizeof(header), &command, sizeof(command));
        if (!clips.empty()) memcpy(packet.data() + sizeof(cmdBlit) + sizeof(header) + sizeof(command),
                                   clips.data(), clips.size() * sizeof(SVGASignedRect));
        return executeFifo(packet);
    };
    resetFramebuffer();
    SVGA3dCmdBlitSurfaceToScreen partialBlit = {{SID_LARGE, 0, 0}, {202, 203, 206, 207},
        0, {17, 19, 21, 23}};
    status = executeBlit(partialBlit, {{0, 0, 2, 4}, {1, 1, 4, 3}});
    TEST_CHECK(status == SVGA3_VLKN_SUCCESS, "Read back only visible tile with overlapping relative clip rectangles");
    bool partialMatches = true;
    for (uint32_t y = 0; y < FB_H; ++y) {
        for (uint32_t x = 0; x < FB_W; ++x) {
            const bool visible = (x >= 17 && x < 19 && y >= 19 && y < 23) ||
                                 (x >= 18 && x < 21 && y >= 20 && y < 22);
            const Pixel expected = visible ? tile[(y - 17) * 8 + (x - 15)] : BASELINE_PIXEL;
            partialMatches &= pixelsIdentical(qemuFb[y * FB_W + x], expected);
        }
    }
    TEST_CHECK(partialMatches, "Partial readback preserves source offsets, overlapping clips and all surrounding pixels");
    svga3_vlkn_surface_destroy(dev, SID_LARGE);

    resetFramebuffer();
    partialBlit = {{SID_RT, 0, 0}, {-4, -5, 12, 11}, 0, {8, 9, 24, 25}};
    status = executeBlit(partialBlit, {});
    bool sourceClipMatches = status == SVGA3_VLKN_SUCCESS;
    for (uint32_t y = 0; y < FB_H; ++y) {
        for (uint32_t x = 0; x < FB_W; ++x) {
            const bool visible = x >= 12 && x < 24 && y >= 14 && y < 25;
            const Pixel expected = visible ? renderedSurface[(y - 14) * RT_W + x - 12] : BASELINE_PIXEL;
            sourceClipMatches &= pixelsIdentical(qemuFb[y * FB_W + x], expected);
        }
    }
    TEST_CHECK(sourceClipMatches, "Negative source origins clip and advance destination without changing surrounding pixels");

    resetFramebuffer();
    partialBlit = {{SID_RT, 0, 0}, {0, 0, 64, 64},
        0, {INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX}};
    status = executeBlit(partialBlit, {});
    TEST_CHECK(status == SVGA3_VLKN_SUCCESS &&
        std::all_of(qemuFb.begin(), qemuFb.end(), [&](const Pixel &p) {return pixelsIdentical(p, BASELINE_PIXEL);}),
        "Extreme signed endpoints safely produce an offscreen blit");

    /* =========================================================================
     * TEST 7: Negative Controls
     * ========================================================================= */
    std::cout << "\n--- Test 7: Negative Controls (Deliberate Discrepancies) ---" << std::endl;
    {
        /* Negative control 1: Modified expected color value must fail comparison */
        Pixel goodP = qemuFb[15 * FB_W + 15];
        Pixel badP = goodP;
        badP.r = (badP.r == 255) ? 0 : 255;
        TEST_CHECK_ERR(!pixelMatches(goodP, badP.r, badP.g, badP.b, badP.a, 0),
                       "Altered color component triggers mismatch in pixel comparison");

        /* Negative control 2: Untouched baseline pixel must not match rendered 3D surface pixel */
        Pixel untouchedP = qemuFb[100 * FB_W + 100];
        Pixel activeSurfP = renderedSurface[0];
        TEST_CHECK_ERR(!pixelMatches(untouchedP, activeSurfP.r, activeSurfP.g, activeSurfP.b, activeSurfP.a, 0),
                       "Untouched baseline pixel does not match rendered surface pixel");

        /* Negative control 3: Invalid surface ID for PRESENT returns error */
        std::vector<uint8_t> fStreamBad;
        auto appendBad = [&](const void *data, size_t sz) {
            const uint8_t *b = reinterpret_cast<const uint8_t*>(data);
            fStreamBad.insert(fStreamBad.end(), b, b + sz);
        };
        SVGA3dCmdHeader hdrBad = { sizeof(SVGA3dCmdPresent) };
        SVGA3dCmdPresent badPresent = {};
        badPresent.sid = 999999; /* Nonexistent surface */
        appendBad(&cmdPresent, sizeof(cmdPresent));
        appendBad(&hdrBad, sizeof(hdrBad));
        appendBad(&badPresent, sizeof(badPresent));

        Svga3VlknStatus invSt = executeFifo(fStreamBad);
        TEST_CHECK_ERR(invSt != SVGA3_VLKN_SUCCESS,
                       "Presenting nonexistent surface ID fails safely with error code");
    }

    /* A legitimate all-black image must replace the previous framebuffer contents. */
    std::vector<Pixel> blackPixels(RT_W * RT_H, Pixel{0, 0, 0, 255});
    SVGA3dBox fullRt = {0, 0, 0, RT_W, RT_H, 1};
    status = svga3_vlkn_surface_dma_upload(dev, SID_RT, 0, &fullRt, blackPixels.data(), RT_W * sizeof(Pixel));
    if (status == SVGA3_VLKN_SUCCESS) status = svga3_vlkn_surface_present(dev, SID_RT, nullptr, 0);
    TEST_CHECK_ERR(status == SVGA3_VLKN_SUCCESS && qemuFb[0].r == 0 && qemuFb[0].g == 0 && qemuFb[0].b == 0,
                   "Presenting a black frame clears old framebuffer pixels");

    /* Clean up */
    svga3_vlkn_context_destroy(dev, CID);
    svga3_vlkn_surface_destroy(dev, SID_RT);
    svga3_vlkn_surface_destroy(dev, SID_TEX);
    svga3_vlkn_surface_destroy(dev, SID_VB);
    dev->contextMgr->clear();
    dev->surfaceMgr->clear();
    Svga3VlknStatus idleStatus = backend->waitIdle();
    backend->shutdown();
    const uint32_t validationErrors = backend->validationErrors();
    const uint32_t validationWarnings = backend->validationWarnings();
    svga3_vlkn_device_destroy(dev);
    TEST_CHECK(idleStatus == SVGA3_VLKN_SUCCESS && validationErrors == 0 && validationWarnings == 0,
        "Presentation and teardown are validation-clean");

    std::cout << "\n======================================================================\n";
    std::cout << "DELIVERABLE 5 ACCEPTANCE SUITE: ALL PRESENTATION TESTS PASSED!\n";
    std::cout << "======================================================================\n";
    return 0;
}
