/*
 * SVGA3=VLKN - Comprehensive Multi-Thousand Test Suite and Oracle Verification Harness
 *
 * Validates the SVGA3D-to-Vulkan translation engine against the Oracle fixtures
 * extracted from DevVGA-SVGA3d-win.cpp.
 */

#include "svga3_vlkn.h"
#include "svga3_device.h"
#include "svga3d_reference.h"
#include "svga3d_tables.h"
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cmath>
#include <cassert>
#include <chrono>

#define ANSI_GREEN  "\033[1;32m"
#define ANSI_RED    "\033[1;31m"
#define ANSI_YELLOW "\033[1;33m"
#define ANSI_CYAN   "\033[1;36m"
#define ANSI_RESET  "\033[0m"

static int g_testsPassed = 0;
static int g_testsFailed = 0;
static PFN_vkQueueSubmit g_originalQueueSubmit = nullptr;
static uint32_t g_queueSubmitCount = 0;

static VkResult VKAPI_CALL countQueueSubmit(VkQueue queue,
                                           uint32_t submitCount,
                                           const VkSubmitInfo *submits,
                                           VkFence fence) {
    ++g_queueSubmitCount;
    return g_originalQueueSubmit(queue, submitCount, submits, fence);
}

#define TEST_CHECK(cond, desc) do { \
    if (cond) { \
        g_testsPassed++; \
    } else { \
        g_testsFailed++; \
        std::cerr << ANSI_RED << "  [FAIL] " << desc << " (" << __FILE__ << ":" << __LINE__ << ")" << ANSI_RESET << std::endl; \
    } \
} while(0)

static void TestRenderPassEndingBatchesCommands() {
    Svga3VlknConfig cfg{};
    cfg.forceMockBackend = true;
    auto *dev = svga3_vlkn_device_create(&cfg);
    TEST_CHECK(dev != nullptr, "Create batching regression device");
    if (!dev) return;

    SVGA3dSize size = {64, 64, 1};
    TEST_CHECK(svga3_vlkn_surface_define(dev, 9100, SVGA3D_SURFACE_HINT_RENDERTARGET,
                                         SVGA3D_A8R8G8B8, &size, 1) == SVGA3_VLKN_SUCCESS,
               "Create render target for batching regression");
    TEST_CHECK(svga3_vlkn_context_create(dev, 9100) == SVGA3_VLKN_SUCCESS,
               "Create context for batching regression");
    TEST_CHECK(svga3_vlkn_context_set_render_target(dev, 9100, SVGA3D_RT_COLOR0, 9100, 0, 0) ==
                   SVGA3_VLKN_SUCCESS,
               "Bind batching regression render target");

    auto &dispatch = dev->backend->dispatch();
    g_originalQueueSubmit = dispatch.vkQueueSubmit;
    g_queueSubmitCount = 0;
    dispatch.vkQueueSubmit = countQueueSubmit;
    TEST_CHECK(svga3_vlkn_context_clear(dev, 9100, SVGA3D_CLEAR_COLOR, 0xff336699, 1.0f, 0,
                                       nullptr, 0) == SVGA3_VLKN_SUCCESS,
               "Record clear in active render pass");
    TEST_CHECK(svga3_vlkn_context_set_render_target(dev, 9100, SVGA3D_RT_COLOR0, 9100, 0, 0) ==
                   SVGA3_VLKN_SUCCESS,
               "End render pass without submitting the command buffer");
    TEST_CHECK(g_queueSubmitCount == 0, "Render pass end does not submit or wait for the queue");

    dev->backend->flushCommandBuffer();
    TEST_CHECK(g_queueSubmitCount == 1, "Explicit flush submits the batched render work");
    dispatch.vkQueueSubmit = g_originalQueueSubmit;
    svga3_vlkn_device_destroy(dev);
}

/* --------------------------------------------------------------------------
 * Test 1: Device Lifecycle & Oracle Capability Verification
 * -------------------------------------------------------------------------- */
static void TestDeviceLifecycleAndCaps() {
    std::cout << ANSI_CYAN << "[TEST 1] Device Lifecycle & Oracle Capability Verification..." << ANSI_RESET << std::endl;

    Svga3VlknConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.appName = "SVGA3=VLKN Test Harness";
    cfg.apiVersion = VK_API_VERSION_1_0;
    cfg.forceMockBackend = true;
    cfg.stagingBufferSize = 16 * 1024 * 1024;

    Svga3VlknDevice *dev = svga3_vlkn_device_create(&cfg);
    TEST_CHECK(dev != nullptr, "Device creation with headless mock backend");
    if (!dev) return;

    /* Verify essential capabilities */
    uint32_t val = 0;
    uint32_t sup = svga3_vlkn_query_cap(dev, SVGA3D_DEVCAP_3D, &val);
    TEST_CHECK(sup == 1 && val == 1, "SVGA3D_DEVCAP_3D is enabled");

    sup = svga3_vlkn_query_cap(dev, SVGA3D_DEVCAP_MAX_TEXTURES, &val);
    TEST_CHECK(sup == 1 && val == 8, "SVGA3D_DEVCAP_MAX_TEXTURES == 8");

    sup = svga3_vlkn_query_cap(dev, SVGA3D_DEVCAP_MAX_CLIP_PLANES, &val);
    TEST_CHECK(sup == 1 && val == 6, "SVGA3D_DEVCAP_MAX_CLIP_PLANES == 6");

    sup = svga3_vlkn_query_cap(dev, SVGA3D_DEVCAP_VERTEX_SHADER_VERSION, &val);
    TEST_CHECK(sup == 1 && val == 0x300, "SVGA3D_DEVCAP_VERTEX_SHADER_VERSION == VS 3.0 (768)");

    sup = svga3_vlkn_query_cap(dev, SVGA3D_DEVCAP_FRAGMENT_SHADER_VERSION, &val);
    TEST_CHECK(sup == 1 && val == 0x300, "SVGA3D_DEVCAP_FRAGMENT_SHADER_VERSION == PS 3.0 (768)");

    sup = svga3_vlkn_query_cap(dev, SVGA3D_DEVCAP_MAX_RENDER_TARGETS, &val);
    TEST_CHECK(sup == 1 && val == 4, "SVGA3D_DEVCAP_MAX_RENDER_TARGETS == 4");

    sup = svga3_vlkn_query_cap(dev, SVGA3D_DEVCAP_MAX_TEXTURE_WIDTH, &val);
    TEST_CHECK(sup == 1 && val == 8192, "SVGA3D_DEVCAP_MAX_TEXTURE_WIDTH == 8192");

    sup = svga3_vlkn_query_cap(dev, SVGA3D_DEVCAP_MAX_TEXTURE_HEIGHT, &val);
    TEST_CHECK(sup == 1 && val == 8192, "SVGA3D_DEVCAP_MAX_TEXTURE_HEIGHT == 8192");

    sup = svga3_vlkn_query_cap(dev, SVGA3D_DEVCAP_MAX_VOLUME_EXTENT, &val);
    TEST_CHECK(sup == 1 && val == 2048, "SVGA3D_DEVCAP_MAX_VOLUME_EXTENT == 2048");

    sup = svga3_vlkn_query_cap(dev, SVGA3D_DEVCAP_MAX_TEXTURE_ANISOTROPY, &val);
    TEST_CHECK(sup == 1 && val == 16, "SVGA3D_DEVCAP_MAX_TEXTURE_ANISOTROPY == 16");

    sup = svga3_vlkn_query_cap(dev, SVGA3D_DEVCAP_ALPHATOCOVERAGE, &val);
    TEST_CHECK(sup == 1 && val == 1, "SVGA3D_DEVCAP_ALPHATOCOVERAGE == 1");

    /* Verify rejected capabilities match Oracle */
    sup = svga3_vlkn_query_cap(dev, SVGA3D_DEVCAP_S23E8_TEXTURES, &val);
    TEST_CHECK(sup == 0, "SVGA3D_DEVCAP_S23E8_TEXTURES rejected (as in Oracle D3D9)");

    sup = svga3_vlkn_query_cap(dev, SVGA3D_DEVCAP_S10E5_TEXTURES, &val);
    TEST_CHECK(sup == 0, "SVGA3D_DEVCAP_S10E5_TEXTURES rejected (as in Oracle D3D9)");

    /* Verify all 84 caps in g_DevCaps */
    size_t devCapsCount = sizeof(g_DevCaps) / sizeof(g_DevCaps[0]);
    for (size_t i = 0; i < devCapsCount; ++i) {
        const DevCapInfo &dc = g_DevCaps[i];
        uint32_t capVal = 0;
        uint32_t supported = svga3_vlkn_query_cap(dev, dc.id, &capVal);
        if (dc.expectedRc == 0) {
            TEST_CHECK(supported == 1, "Supported cap " + std::string(dc.name));
            TEST_CHECK(capVal == dc.expectedValue, "Cap value " + std::string(dc.name));
        } else {
            TEST_CHECK(supported == 0, "Unsupported cap " + std::string(dc.name));
        }
    }

    /* Verify reset & wait idle */
    Svga3VlknStatus st = svga3_vlkn_device_reset(dev);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Device reset");

    st = svga3_vlkn_device_wait_idle(dev);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Device wait idle");

    svga3_vlkn_device_destroy(dev);
    std::cout << ANSI_GREEN << "  Test 1 passed successfully." << ANSI_RESET << std::endl;
}

/* --------------------------------------------------------------------------
 * Test 2: Surface Format Mapping Verification against Oracle Table
 * -------------------------------------------------------------------------- */
static void TestSurfaceFormatMappings() {
    std::cout << ANSI_CYAN << "[TEST 2] Surface Format Verification against Oracle Fixtures..." << ANSI_RESET << std::endl;

    size_t fmtCount = sizeof(kSvga3dFormatTable) / sizeof(kSvga3dFormatTable[0]);
    size_t matchedCount = 0;

    for (size_t i = 0; i < fmtCount; ++i) {
        const auto &ref = kSvga3dFormatTable[i];
        VkFormat engineVkFmt = svga3_format_to_vk((SVGA3dSurfaceFormat)ref.svgaFormat);
        uint32_t expectedVkFmt = ref.vkFormat;

        bool ok = (engineVkFmt == (VkFormat)expectedVkFmt);
        TEST_CHECK(ok, "Format conversion: " + std::string(ref.svgaName) + " -> " + ref.vkName);
        if (ok) matchedCount++;
    }

    /* Verify depth-stencil recognition */
    TEST_CHECK(svga3_format_to_vk(SVGA3D_Z_D24S8) == VK_FORMAT_D24_UNORM_S8_UINT, "SVGA3D_Z_D24S8 format");
    TEST_CHECK(svga3_format_to_vk(SVGA3D_Z_D16) == VK_FORMAT_D16_UNORM, "SVGA3D_Z_D16 format");
    TEST_CHECK(svga3_format_to_vk(SVGA3D_X8R8G8B8) == VK_FORMAT_B8G8R8A8_UNORM, "SVGA3D_X8R8G8B8 format");
    TEST_CHECK(svga3_format_to_vk(SVGA3D_R5G6B5) == VK_FORMAT_B5G6R5_UNORM_PACK16, "SVGA3D_R5G6B5 format");

    std::cout << ANSI_GREEN << "  Verified " << matchedCount << "/" << fmtCount << " surface formats matching Oracle." << ANSI_RESET << std::endl;
}

/* --------------------------------------------------------------------------
 * Test 3: Primitive Topology Mapping against Oracle Table
 * -------------------------------------------------------------------------- */
static void TestTopologyMappings() {
    std::cout << ANSI_CYAN << "[TEST 3] Primitive Topology Verification against Oracle Table..." << ANSI_RESET << std::endl;

    size_t topCount = sizeof(kSvga3dTopologyTable) / sizeof(kSvga3dTopologyTable[0]);
    size_t matchedCount = 0;

    for (size_t i = 0; i < topCount; ++i) {
        const auto &ref = kSvga3dTopologyTable[i];
        VkPrimitiveTopology engineTop = svga3_primitive_to_vk((SVGA3dPrimitiveType)ref.svgaTopology);
        bool ok = (engineTop == (VkPrimitiveTopology)ref.vkTopology);
        TEST_CHECK(ok, "Topology conversion: " + std::string(ref.svgaName) + " -> " + ref.vkName);
        if (ok) matchedCount++;
    }

    std::cout << ANSI_GREEN << "  Verified " << matchedCount << "/" << topCount << " primitive topologies." << ANSI_RESET << std::endl;
}

/* --------------------------------------------------------------------------
 * Test 4: Compare, Blend & Stencil State Conversions
 * -------------------------------------------------------------------------- */
static void TestStateConverters() {
    std::cout << ANSI_CYAN << "[TEST 4] State Conversion Utilities..." << ANSI_RESET << std::endl;

    TEST_CHECK(svga3_cmp_func_to_vk(SVGA3D_CMP_NEVER) == VK_COMPARE_OP_NEVER, "SVGA3D_CMP_NEVER");
    TEST_CHECK(svga3_cmp_func_to_vk(SVGA3D_CMP_LESS) == VK_COMPARE_OP_LESS, "SVGA3D_CMP_LESS");
    TEST_CHECK(svga3_cmp_func_to_vk(SVGA3D_CMP_EQUAL) == VK_COMPARE_OP_EQUAL, "SVGA3D_CMP_EQUAL");
    TEST_CHECK(svga3_cmp_func_to_vk(SVGA3D_CMP_LESSEQUAL) == VK_COMPARE_OP_LESS_OR_EQUAL, "SVGA3D_CMP_LESSEQUAL");
    TEST_CHECK(svga3_cmp_func_to_vk(SVGA3D_CMP_GREATER) == VK_COMPARE_OP_GREATER, "SVGA3D_CMP_GREATER");
    TEST_CHECK(svga3_cmp_func_to_vk(SVGA3D_CMP_NOTEQUAL) == VK_COMPARE_OP_NOT_EQUAL, "SVGA3D_CMP_NOTEQUAL");
    TEST_CHECK(svga3_cmp_func_to_vk(SVGA3D_CMP_GREATEREQUAL) == VK_COMPARE_OP_GREATER_OR_EQUAL, "SVGA3D_CMP_GREATEREQUAL");
    TEST_CHECK(svga3_cmp_func_to_vk(SVGA3D_CMP_ALWAYS) == VK_COMPARE_OP_ALWAYS, "SVGA3D_CMP_ALWAYS");

    TEST_CHECK(svga3_blend_eq_to_vk(SVGA3D_BLENDEQ_ADD) == VK_BLEND_OP_ADD, "SVGA3D_BLENDEQ_ADD");
    TEST_CHECK(svga3_blend_eq_to_vk(SVGA3D_BLENDEQ_SUBTRACT) == VK_BLEND_OP_SUBTRACT, "SVGA3D_BLENDEQ_SUBTRACT");
    TEST_CHECK(svga3_blend_eq_to_vk(SVGA3D_BLENDEQ_REVSUBTRACT) == VK_BLEND_OP_REVERSE_SUBTRACT, "SVGA3D_BLENDEQ_REVSUBTRACT");
    TEST_CHECK(svga3_blend_eq_to_vk(SVGA3D_BLENDEQ_MINIMUM) == VK_BLEND_OP_MIN, "SVGA3D_BLENDEQ_MINIMUM");
    TEST_CHECK(svga3_blend_eq_to_vk(SVGA3D_BLENDEQ_MAXIMUM) == VK_BLEND_OP_MAX, "SVGA3D_BLENDEQ_MAXIMUM");

    TEST_CHECK(svga3_blend_factor_to_vk(SVGA3D_BLENDOP_ZERO) == VK_BLEND_FACTOR_ZERO, "SVGA3D_BLENDOP_ZERO");
    TEST_CHECK(svga3_blend_factor_to_vk(SVGA3D_BLENDOP_ONE) == VK_BLEND_FACTOR_ONE, "SVGA3D_BLENDOP_ONE");
    TEST_CHECK(svga3_blend_factor_to_vk(SVGA3D_BLENDOP_SRCCOLOR) == VK_BLEND_FACTOR_SRC_COLOR, "SVGA3D_BLENDOP_SRCCOLOR");
    TEST_CHECK(svga3_blend_factor_to_vk(SVGA3D_BLENDOP_INVSRCCOLOR) == VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, "SVGA3D_BLENDOP_INVSRCCOLOR");
    TEST_CHECK(svga3_blend_factor_to_vk(SVGA3D_BLENDOP_SRCALPHA) == VK_BLEND_FACTOR_SRC_ALPHA, "SVGA3D_BLENDOP_SRCALPHA");
    TEST_CHECK(svga3_blend_factor_to_vk(SVGA3D_BLENDOP_INVSRCALPHA) == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, "SVGA3D_BLENDOP_INVSRCALPHA");
    TEST_CHECK(svga3_blend_factor_to_vk(SVGA3D_BLENDOP_DESTALPHA) == VK_BLEND_FACTOR_DST_ALPHA, "SVGA3D_BLENDOP_DESTALPHA");
    TEST_CHECK(svga3_blend_factor_to_vk(SVGA3D_BLENDOP_INVDESTALPHA) == VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, "SVGA3D_BLENDOP_INVDESTALPHA");
    TEST_CHECK(svga3_blend_factor_to_vk(SVGA3D_BLENDOP_DESTCOLOR) == VK_BLEND_FACTOR_DST_COLOR, "SVGA3D_BLENDOP_DESTCOLOR");
    TEST_CHECK(svga3_blend_factor_to_vk(SVGA3D_BLENDOP_INVDESTCOLOR) == VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, "SVGA3D_BLENDOP_INVDESTCOLOR");
    TEST_CHECK(svga3_blend_factor_to_vk(SVGA3D_BLENDOP_SRCALPHASAT) == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, "SVGA3D_BLENDOP_SRCALPHASAT");
    TEST_CHECK(svga3_blend_factor_to_vk(SVGA3D_BLENDOP_BLENDFACTOR) == VK_BLEND_FACTOR_CONSTANT_COLOR, "SVGA3D_BLENDOP_BLENDFACTOR");
    TEST_CHECK(svga3_blend_factor_to_vk(SVGA3D_BLENDOP_INVBLENDFACTOR) == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, "SVGA3D_BLENDOP_INVBLENDFACTOR");

    TEST_CHECK(svga3_stencil_op_to_vk(SVGA3D_STENCILOP_KEEP) == VK_STENCIL_OP_KEEP, "SVGA3D_STENCILOP_KEEP");
    TEST_CHECK(svga3_stencil_op_to_vk(SVGA3D_STENCILOP_ZERO) == VK_STENCIL_OP_ZERO, "SVGA3D_STENCILOP_ZERO");
    TEST_CHECK(svga3_stencil_op_to_vk(SVGA3D_STENCILOP_REPLACE) == VK_STENCIL_OP_REPLACE, "SVGA3D_STENCILOP_REPLACE");
    TEST_CHECK(svga3_stencil_op_to_vk(SVGA3D_STENCILOP_INCRSAT) == VK_STENCIL_OP_INCREMENT_AND_CLAMP, "SVGA3D_STENCILOP_INCRSAT");
    TEST_CHECK(svga3_stencil_op_to_vk(SVGA3D_STENCILOP_DECRSAT) == VK_STENCIL_OP_DECREMENT_AND_CLAMP, "SVGA3D_STENCILOP_DECRSAT");
    TEST_CHECK(svga3_stencil_op_to_vk(SVGA3D_STENCILOP_INVERT) == VK_STENCIL_OP_INVERT, "SVGA3D_STENCILOP_INVERT");
    TEST_CHECK(svga3_stencil_op_to_vk(SVGA3D_STENCILOP_INCR) == VK_STENCIL_OP_INCREMENT_AND_WRAP, "SVGA3D_STENCILOP_INCR");
    TEST_CHECK(svga3_stencil_op_to_vk(SVGA3D_STENCILOP_DECR) == VK_STENCIL_OP_DECREMENT_AND_WRAP, "SVGA3D_STENCILOP_DECR");

    TEST_CHECK(svga3_texture_address_to_vk(SVGA3D_TEX_ADDRESS_WRAP) == VK_SAMPLER_ADDRESS_MODE_REPEAT, "SVGA3D_TEX_ADDRESS_WRAP");
    TEST_CHECK(svga3_texture_address_to_vk(SVGA3D_TEX_ADDRESS_MIRROR) == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, "SVGA3D_TEX_ADDRESS_MIRROR");
    TEST_CHECK(svga3_texture_address_to_vk(SVGA3D_TEX_ADDRESS_CLAMP) == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, "SVGA3D_TEX_ADDRESS_CLAMP");
    TEST_CHECK(svga3_texture_address_to_vk(SVGA3D_TEX_ADDRESS_BORDER) == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, "SVGA3D_TEX_ADDRESS_BORDER");

    std::cout << ANSI_GREEN << "  State converters verified successfully." << ANSI_RESET << std::endl;
}

/* --------------------------------------------------------------------------
 * Test 5: Surface Lifecycle (2D, Cubemap, Depth/Stencil, Volume)
 * -------------------------------------------------------------------------- */
static void TestSurfaceLifecycle(Svga3VlknDevice *dev) {
    std::cout << ANSI_CYAN << "[TEST 5] Surface Lifecycle & Resource Allocation..." << ANSI_RESET << std::endl;

    /* 1. Define standard 2D color surface (64x64, 1 mip) */
    SVGA3dSize size2d = { 64, 64, 1 };
    Svga3VlknStatus st = svga3_vlkn_surface_define(dev, 1, 0, SVGA3D_X8R8G8B8, &size2d, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Define 2D surface (sid=1)");
    TEST_CHECK(svga3_vlkn_surface_exists(dev, 1), "Surface exists (sid=1)");

    /* 2. Define Cubemap surface (128x128, 6 faces, 2 mip levels = 12 sizes) */
    std::vector<SVGA3dSize> cubeSizes;
    for (int face = 0; face < 6; ++face) {
        cubeSizes.push_back({ 128, 128, 1 });
        cubeSizes.push_back({ 64, 64, 1 });
    }
    st = svga3_vlkn_surface_define(dev, 2, SVGA3D_SURFACE_CUBEMAP, SVGA3D_A8R8G8B8, cubeSizes.data(), (uint32_t)cubeSizes.size());
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Define Cubemap surface (sid=2)");
    TEST_CHECK(svga3_vlkn_surface_exists(dev, 2), "Surface exists (sid=2)");

    /* 3. Define Depth/Stencil surface (1024x768) */
    SVGA3dSize depthSize = { 1024, 768, 1 };
    st = svga3_vlkn_surface_define(dev, 3, 0, SVGA3D_Z_D24S8, &depthSize, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Define Depth/Stencil surface (sid=3)");
    TEST_CHECK(svga3_vlkn_surface_exists(dev, 3), "Surface exists (sid=3)");

    /* 4. Define 3D Volume texture (64x64x16) */
    SVGA3dSize volSize = { 64, 64, 16 };
    st = svga3_vlkn_surface_define(dev, 4, 0, SVGA3D_A8R8G8B8, &volSize, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Define 3D Volume surface (sid=4)");
    TEST_CHECK(svga3_vlkn_surface_exists(dev, 4), "Surface exists (sid=4)");

    /* 5. Define V2 surface with MSAA and autogen filter */
    SVGA3dSize msaaSize = { 128, 128, 1 };
    st = svga3_vlkn_surface_define_v2(dev, 5, 0, SVGA3D_A8R8G8B8, 4, SVGA3D_TEX_FILTER_LINEAR, &msaaSize, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Define MSAA surface v2 (sid=5)");
    TEST_CHECK(svga3_vlkn_surface_exists(dev, 5), "Surface exists (sid=5)");

    /* 6. Active state toggle */
    st = svga3_vlkn_surface_set_active(dev, 1, false);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Set surface 1 inactive");
    st = svga3_vlkn_surface_set_active(dev, 1, true);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Set surface 1 active");

    /* 7. Define duplicate SID -> replaces surface smoothly */
    st = svga3_vlkn_surface_define(dev, 1, 0, SVGA3D_X8R8G8B8, &size2d, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Duplicate SID replaced");

    /* 8. Destroy surfaces */
    st = svga3_vlkn_surface_destroy(dev, 2);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Destroy Cubemap surface (sid=2)");
    TEST_CHECK(!svga3_vlkn_surface_exists(dev, 2), "Surface destroyed (sid=2)");

    st = svga3_vlkn_surface_destroy(dev, 4);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Destroy Volume surface (sid=4)");
    TEST_CHECK(!svga3_vlkn_surface_exists(dev, 4), "Surface destroyed (sid=4)");

    st = svga3_vlkn_surface_destroy(dev, 5);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Destroy MSAA surface (sid=5)");
    TEST_CHECK(!svga3_vlkn_surface_exists(dev, 5), "Surface destroyed (sid=5)");

    /* Destroying nonexistent surface -> NOT_FOUND */
    st = svga3_vlkn_surface_destroy(dev, 999);
    TEST_CHECK(st == SVGA3_VLKN_ERROR_NOT_FOUND, "Destroy non-existent SID rejected");

    std::cout << ANSI_GREEN << "  Surface lifecycle verified successfully." << ANSI_RESET << std::endl;
}

/* --------------------------------------------------------------------------
 * Test 6: Surface DMA Upload, Download & Bit-Exact Readback Across Formats
 * -------------------------------------------------------------------------- */
static void TestSurfaceDMA(Svga3VlknDevice *dev) {
    std::cout << ANSI_CYAN << "[TEST 6] Surface DMA Upload, Download & Bit-Exact Readback Across Formats..." << ANSI_RESET << std::endl;

    /* Test 6.1: 16x16 RGBA8 (SID 10) */
    uint32_t sid = 10;
    SVGA3dSize size = { 16, 16, 1 };
    Svga3VlknStatus st = svga3_vlkn_surface_define(dev, sid, 0, SVGA3D_A8R8G8B8, &size, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Define DMA test surface (sid=10)");

    std::vector<uint32_t> uploadPixels(16 * 16);
    for (size_t y = 0; y < 16; ++y) {
        for (size_t x = 0; x < 16; ++x) {
            uint8_t a = 0xff;
            uint8_t r = (uint8_t)(x * 16);
            uint8_t g = (uint8_t)(y * 16);
            uint8_t b = (uint8_t)((x + y) * 8);
            uploadPixels[y * 16 + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }

    SVGA3dBox box = { 0, 0, 0, 16, 16, 1 };
    st = svga3_vlkn_surface_dma_upload(dev, sid, 0, &box, uploadPixels.data(), 16 * sizeof(uint32_t));
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Surface DMA upload (RGBA8)");

    std::vector<uint32_t> downloadPixels(16 * 16, 0x12345678);
    st = svga3_vlkn_surface_dma_download(dev, sid, 0, &box, downloadPixels.data(), 16 * sizeof(uint32_t));
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Surface DMA download (RGBA8)");

    int mismatches = 0;
    for (size_t i = 0; i < 16 * 16; ++i) {
        if (uploadPixels[i] != downloadPixels[i]) mismatches++;
    }
    TEST_CHECK(mismatches == 0, "RGBA8 DMA upload and download are bit-for-bit identical");

    /* Test 6.2: 32x32 R5G6B5 (SID 11) */
    uint32_t sid16 = 11;
    SVGA3dSize size16 = { 32, 32, 1 };
    st = svga3_vlkn_surface_define(dev, sid16, 0, SVGA3D_R5G6B5, &size16, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Define DMA test surface (sid=11, 16-bit)");

    std::vector<uint16_t> upload16(32 * 32);
    for (size_t i = 0; i < 32 * 32; ++i) upload16[i] = (uint16_t)(i ^ 0xbeef);
    SVGA3dBox box16 = { 0, 0, 0, 32, 32, 1 };
    st = svga3_vlkn_surface_dma_upload(dev, sid16, 0, &box16, upload16.data(), 32 * sizeof(uint16_t));
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Surface DMA upload (R5G6B5)");

    std::vector<uint16_t> download16(32 * 32, 0);
    st = svga3_vlkn_surface_dma_download(dev, sid16, 0, &box16, download16.data(), 32 * sizeof(uint16_t));
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Surface DMA download (R5G6B5)");

    bool match16 = (memcmp(upload16.data(), download16.data(), 32 * 32 * sizeof(uint16_t)) == 0);
    TEST_CHECK(match16, "R5G6B5 DMA upload and download are bit-for-bit identical");

    /* Check device DMA transfer statistics */
    Svga3VlknStats stats;
    svga3_vlkn_get_stats(dev, &stats);
    TEST_CHECK(stats.dmaBytesTransferred >= (16 * 16 * 4 * 2 + 32 * 32 * 2 * 2), "DMA byte statistics updated");

    std::cout << ANSI_GREEN << "  Surface DMA verified successfully." << ANSI_RESET << std::endl;
}

/* --------------------------------------------------------------------------
 * Test 7: Surface Copy (Blit) Operations
 * -------------------------------------------------------------------------- */
static void TestSurfaceCopy(Svga3VlknDevice *dev) {
    std::cout << ANSI_CYAN << "[TEST 7] Surface Copy (Blit) Operations..." << ANSI_RESET << std::endl;

    uint32_t srcSid = 20;
    uint32_t dstSid = 21;
    SVGA3dSize size = { 32, 32, 1 };

    svga3_vlkn_surface_define(dev, srcSid, 0, SVGA3D_X8R8G8B8, &size, 1);
    svga3_vlkn_surface_define(dev, dstSid, 0, SVGA3D_X8R8G8B8, &size, 1);

    std::vector<uint32_t> srcData(32 * 32, 0x00aabbcc);
    SVGA3dBox box = { 0, 0, 0, 32, 32, 1 };
    svga3_vlkn_surface_dma_upload(dev, srcSid, 0, &box, srcData.data(), 32 * sizeof(uint32_t));

    SVGA3dCopyBox copyBox;
    copyBox.x = 0; copyBox.y = 0; copyBox.z = 0;
    copyBox.w = 32; copyBox.h = 32; copyBox.d = 1;
    copyBox.srcx = 0; copyBox.srcy = 0; copyBox.srcz = 0;

    Svga3VlknStatus st = svga3_vlkn_surface_copy(dev, srcSid, dstSid, &copyBox, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Surface copy (32x32 blit)");

    std::vector<uint32_t> dstData(32 * 32, 0);
    svga3_vlkn_surface_dma_download(dev, dstSid, 0, &box, dstData.data(), 32 * sizeof(uint32_t));

    bool match = (memcmp(srcData.data(), dstData.data(), srcData.size() * sizeof(uint32_t)) == 0);
    TEST_CHECK(match, "Copied surface contents match source exactly");

    /* Subrectangle Copy */
    SVGA3dCopyBox subBox;
    subBox.x = 8; subBox.y = 8; subBox.z = 0;
    subBox.w = 16; subBox.h = 16; subBox.d = 1;
    subBox.srcx = 0; subBox.srcy = 0; subBox.srcz = 0;
    st = svga3_vlkn_surface_copy(dev, srcSid, dstSid, &subBox, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Surface copy subrectangle");

    std::cout << ANSI_GREEN << "  Surface copy verified successfully." << ANSI_RESET << std::endl;
}

/* --------------------------------------------------------------------------
 * Test 8: Context Lifecycle & Render States
 * -------------------------------------------------------------------------- */
static void TestContextAndRenderStates(Svga3VlknDevice *dev) {
    std::cout << ANSI_CYAN << "[TEST 8] Context Lifecycle & Render State Tracking..." << ANSI_RESET << std::endl;

    uint32_t cid = 100;
    Svga3VlknStatus st = svga3_vlkn_context_create(dev, cid);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Context create (cid=100)");
    TEST_CHECK(svga3_vlkn_context_exists(dev, cid), "Context exists (cid=100)");

    st = svga3_vlkn_context_create(dev, cid);
    TEST_CHECK(st == SVGA3_VLKN_ERROR_ALREADY_EXISTS, "Duplicate context rejected");

    /* Verify default render states */
    uint32_t val = 0;
    st = svga3_vlkn_context_get_render_state(dev, cid, SVGA3D_RS_ZENABLE, &val);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS && val == 1, "Default SVGA3D_RS_ZENABLE == 1");

    st = svga3_vlkn_context_get_render_state(dev, cid, SVGA3D_RS_FILLMODE, &val);
    SVGA3dFillMode defaultFm;
    defaultFm.uintValue = val;
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS && defaultFm.s.mode == SVGA3D_FILLMODE_FILL, "Default SVGA3D_RS_FILLMODE == FILL");

    st = svga3_vlkn_context_get_render_state(dev, cid, SVGA3D_RS_CULLMODE, &val);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS && val == SVGA3D_FACE_BACK, "Default SVGA3D_RS_CULLMODE == BACK");

    /* Modify render states */
    svga3_vlkn_context_set_render_state(dev, cid, SVGA3D_RS_ZENABLE, 0);
    svga3_vlkn_context_get_render_state(dev, cid, SVGA3D_RS_ZENABLE, &val);
    TEST_CHECK(val == 0, "Set SVGA3D_RS_ZENABLE to 0");

    svga3_vlkn_context_set_render_state(dev, cid, SVGA3D_RS_BLENDENABLE, 1);
    svga3_vlkn_context_set_render_state(dev, cid, SVGA3D_RS_SRCBLEND, SVGA3D_BLENDOP_SRCALPHA);
    svga3_vlkn_context_set_render_state(dev, cid, SVGA3D_RS_DSTBLEND, SVGA3D_BLENDOP_INVSRCALPHA);

    svga3_vlkn_context_get_render_state(dev, cid, SVGA3D_RS_BLENDENABLE, &val);
    TEST_CHECK(val == 1, "Set SVGA3D_RS_BLENDENABLE to 1");

    svga3_vlkn_context_get_render_state(dev, cid, SVGA3D_RS_SRCBLEND, &val);
    TEST_CHECK(val == SVGA3D_BLENDOP_SRCALPHA, "Set SVGA3D_RS_SRCBLEND to SRCALPHA");

    svga3_vlkn_context_get_render_state(dev, cid, SVGA3D_RS_DSTBLEND, &val);
    TEST_CHECK(val == SVGA3D_BLENDOP_INVSRCALPHA, "Set SVGA3D_RS_DSTBLEND to INVSRCALPHA");

    std::cout << ANSI_GREEN << "  Context render states verified successfully." << ANSI_RESET << std::endl;
}

/* --------------------------------------------------------------------------
 * Test 9: Render Target, Viewport & Scissor Configuration
 * -------------------------------------------------------------------------- */
static void TestRenderTargetAndViewport(Svga3VlknDevice *dev) {
    std::cout << ANSI_CYAN << "[TEST 9] Render Target, Viewport & Scissor Configuration..." << ANSI_RESET << std::endl;

    uint32_t cid = 100;
    uint32_t colorSid = 1;
    uint32_t depthSid = 3;

    /* Bind Color Render Targets 0..3 */
    for (uint32_t rt = 0; rt < 4; ++rt) {
        Svga3VlknStatus st = svga3_vlkn_context_set_render_target(
            dev, cid, (SVGA3dRenderTargetType)(SVGA3D_RT_COLOR0 + rt), colorSid, 0, 0
        );
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Set color render target RT" + std::to_string(rt));
    }

    /* Bind Depth Render Target */
    Svga3VlknStatus st = svga3_vlkn_context_set_render_target(
        dev, cid, SVGA3D_RT_DEPTH, depthSid, 0, 0
    );
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Set depth render target");

    /* Viewport Sweeps */
    SVGA3dRect vpRects[] = {
        { 0, 0, 64, 64 },
        { 10, 10, 50, 50 },
        { 0, 0, 1024, 768 },
        { 100, 100, 800, 600 }
    };
    for (size_t i = 0; i < sizeof(vpRects) / sizeof(vpRects[0]); ++i) {
        st = svga3_vlkn_context_set_viewport(dev, cid, &vpRects[i]);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Set viewport rect " + std::to_string(i));
    }

    /* Scissor Rect Sweeps */
    SVGA3dRect scRects[] = {
        { 0, 0, 64, 64 },
        { 5, 5, 55, 55 },
        { 20, 20, 40, 40 }
    };
    for (size_t i = 0; i < sizeof(scRects) / sizeof(scRects[0]); ++i) {
        st = svga3_vlkn_context_set_scissor_rect(dev, cid, &scRects[i]);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Set scissor rect " + std::to_string(i));
    }

    std::cout << ANSI_GREEN << "  Render target and viewport verified successfully." << ANSI_RESET << std::endl;
}

/* --------------------------------------------------------------------------
 * Test 10: Clear Operations & Render Pass Activation
 * -------------------------------------------------------------------------- */
static void TestClearOperations(Svga3VlknDevice *dev) {
    std::cout << ANSI_CYAN << "[TEST 10] Clear Operations & Render Pass Execution..." << ANSI_RESET << std::endl;

    uint32_t cid = 100;
    SVGA3dRect clearRect = { 0, 0, 64, 64 };
    uint32_t colors[] = { 0x6495edff, 0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff, 0x00000000 };

    for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i) {
        Svga3VlknStatus st = svga3_vlkn_context_clear(
            dev, cid,
            (SVGA3dClearFlag)(SVGA3D_CLEAR_COLOR | SVGA3D_CLEAR_DEPTH | SVGA3D_CLEAR_STENCIL),
            colors[i], 1.0f, 0, &clearRect, 1
        );
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Clear color variation " + std::to_string(i));
    }

    /* Test Clear Depth Only and Stencil Only */
    Svga3VlknStatus st = svga3_vlkn_context_clear(dev, cid, SVGA3D_CLEAR_DEPTH, 0, 0.5f, 0, &clearRect, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Clear depth only");

    st = svga3_vlkn_context_clear(dev, cid, SVGA3D_CLEAR_STENCIL, 0, 1.0f, 128, &clearRect, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Clear stencil only");

    /* Multi-rectangle clear */
    SVGA3dRect multiRects[2] = { { 0, 0, 32, 32 }, { 32, 32, 32, 32 } };
    st = svga3_vlkn_context_clear(dev, cid, SVGA3D_CLEAR_COLOR, 0xff00ffff, 1.0f, 0, multiRects, 2);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Clear multi-rectangles");

    std::cout << ANSI_GREEN << "  Clear operations verified successfully." << ANSI_RESET << std::endl;
}

/* --------------------------------------------------------------------------
 * Test 11: Draw Call Dispatch & All 6 Primitive Topologies
 * -------------------------------------------------------------------------- */
static void TestDrawPrimitives(Svga3VlknDevice *dev) {
    std::cout << ANSI_CYAN << "[TEST 11] Draw Call Dispatch & All 6 Primitive Topologies..." << ANSI_RESET << std::endl;

    uint32_t cid = 100;

    /* Vertex declarations */
    SVGA3dVertexDecl decls[2];
    memset(decls, 0, sizeof(decls));
    decls[0].identity.usage = SVGA3D_DECLUSAGE_POSITION;
    decls[0].identity.type = SVGA3D_DECLTYPE_FLOAT3;
    decls[0].array.stride = 24;
    decls[0].array.offset = 0;

    decls[1].identity.usage = SVGA3D_DECLUSAGE_COLOR;
    decls[1].identity.type = SVGA3D_DECLTYPE_D3DCOLOR;
    decls[1].array.stride = 24;
    decls[1].array.offset = 12;

    /* Sweep all 6 SVGA3D primitive topologies */
    SVGA3dPrimitiveType topologies[] = {
        SVGA3D_PRIMITIVE_TRIANGLELIST,
        SVGA3D_PRIMITIVE_TRIANGLESTRIP,
        SVGA3D_PRIMITIVE_TRIANGLEFAN,
        SVGA3D_PRIMITIVE_POINTLIST,
        SVGA3D_PRIMITIVE_LINELIST,
        SVGA3D_PRIMITIVE_LINESTRIP
    };

    for (size_t t = 0; t < sizeof(topologies) / sizeof(topologies[0]); ++t) {
        SVGA3dPrimitiveRange range;
        memset(&range, 0, sizeof(range));
        range.primType = topologies[t];
        range.primitiveCount = 4;
        range.indexArray.stride = 0;

        Svga3VlknStatus st = svga3_vlkn_context_draw(
            dev, cid, topologies[t], decls, 2, &range, 1
        );
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Draw topology " + std::to_string(topologies[t]));

        /* Second draw call to test cached pipeline lookup */
        st = svga3_vlkn_context_draw(
            dev, cid, topologies[t], decls, 2, &range, 1
        );
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Draw cached pipeline " + std::to_string(topologies[t]));
    }

    /* A fixed-function transform update gets a new ring slice and must not
     * submit/wait for the command buffer that contains earlier draws. */
    auto &dispatch = dev->backend->dispatch();
    g_originalQueueSubmit = dispatch.vkQueueSubmit;
    g_queueSubmitCount = 0;
    dispatch.vkQueueSubmit = countQueueSubmit;
    const float movedTransform[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0.25f, 0, 0, 1
    };
    Svga3VlknStatus st = svga3_vlkn_context_set_transform(dev, cid, SVGA3D_TRANSFORM_WORLD,
                                                         movedTransform);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Update fixed-function transform for ring test");
    SVGA3dPrimitiveRange ringRange{};
    ringRange.primType = topologies[5];
    ringRange.primitiveCount = 4;
    st = svga3_vlkn_context_draw(dev, cid, topologies[5], decls, 2, &ringRange, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Draw with changed constants from a new ring slice");
    TEST_CHECK(g_queueSubmitCount == 0, "Dynamic constant update does not submit or wait for prior draws");
    dev->backend->flushCommandBuffer();
    TEST_CHECK(g_queueSubmitCount == 1, "Explicit flush submits draws using the constant ring");
    dispatch.vkQueueSubmit = g_originalQueueSubmit;

    std::cout << ANSI_GREEN << "  Draw calls across all topologies verified successfully." << ANSI_RESET << std::endl;
}

/* --------------------------------------------------------------------------
 * Test 12: Binary FIFO Command Stream Decoding & Execution
 * -------------------------------------------------------------------------- */
static void TestFifoExecution(Svga3VlknDevice *dev) {
    std::cout << ANSI_CYAN << "[TEST 12] Binary FIFO Command Stream Decoding & Execution..." << ANSI_RESET << std::endl;

    std::vector<uint8_t> fifoStream;

    auto appendCmd = [&](uint32_t cmdId, const void *payload, size_t size) {
        size_t cur = fifoStream.size();
        SVGA3dCmdHeader hdr;
        hdr.size = (uint32_t)size;

        fifoStream.resize(cur + sizeof(uint32_t) + sizeof(SVGA3dCmdHeader) + size);
        memcpy(fifoStream.data() + cur, &cmdId, sizeof(uint32_t));
        cur += sizeof(uint32_t);
        memcpy(fifoStream.data() + cur, &hdr, sizeof(SVGA3dCmdHeader));
        cur += sizeof(SVGA3dCmdHeader);
        if (size > 0 && payload) {
            memcpy(fifoStream.data() + cur, payload, size);
        }
    };

    /* 1. CMD_SURFACE_DEFINE (SID 200, 32x32, RGBA) */
    SVGA3dCmdDefineSurface defSurf;
    memset(&defSurf, 0, sizeof(defSurf));
    defSurf.sid = 200;
    defSurf.surfaceFlags = (SVGA3dSurfaceFlags)0;
    defSurf.format = SVGA3D_A8R8G8B8;
    defSurf.face[0].numMipLevels = 1;
    SVGA3dSize surfSize = { 32, 32, 1 };

    std::vector<uint8_t> surfPacket(sizeof(SVGA3dCmdDefineSurface) + sizeof(SVGA3dSize));
    memcpy(surfPacket.data(), &defSurf, sizeof(SVGA3dCmdDefineSurface));
    memcpy(surfPacket.data() + sizeof(SVGA3dCmdDefineSurface), &surfSize, sizeof(SVGA3dSize));
    appendCmd(SVGA_3D_CMD_SURFACE_DEFINE, surfPacket.data(), surfPacket.size());

    /* 2. CMD_CONTEXT_DEFINE (CID 200) */
    SVGA3dCmdDefineContext defCtx;
    defCtx.cid = 200;
    appendCmd(SVGA_3D_CMD_CONTEXT_DEFINE, &defCtx, sizeof(defCtx));

    /* 3. CMD_SETRENDERSTATE (CID 200, ZENABLE = 1, FILLMODE = SOLID) */
    SVGA3dCmdSetRenderState setRS;
    setRS.cid = 200;
    SVGA3dRenderState rsArray[2];
    rsArray[0].state = SVGA3D_RS_ZENABLE;
    rsArray[0].uintValue = 1;
    rsArray[1].state = SVGA3D_RS_FILLMODE;
    rsArray[1].uintValue = SVGA3D_FILLMODE_FILL;

    std::vector<uint8_t> rsPacket(sizeof(SVGA3dCmdSetRenderState) + sizeof(rsArray));
    memcpy(rsPacket.data(), &setRS, sizeof(SVGA3dCmdSetRenderState));
    memcpy(rsPacket.data() + sizeof(SVGA3dCmdSetRenderState), rsArray, sizeof(rsArray));
    appendCmd(SVGA_3D_CMD_SETRENDERSTATE, rsPacket.data(), rsPacket.size());

    /* 4. CMD_SETRENDERTARGET (CID 200, RT0 = SID 200) */
    SVGA3dCmdSetRenderTarget setRT;
    setRT.cid = 200;
    setRT.type = SVGA3D_RT_COLOR0;
    setRT.target.sid = 200;
    setRT.target.face = 0;
    setRT.target.mipmap = 0;
    appendCmd(SVGA_3D_CMD_SETRENDERTARGET, &setRT, sizeof(setRT));

    /* 5. CMD_SETVIEWPORT */
    SVGA3dCmdSetViewport setVp;
    setVp.cid = 200;
    setVp.rect.x = 0; setVp.rect.y = 0;
    setVp.rect.w = 32; setVp.rect.h = 32;
    appendCmd(SVGA_3D_CMD_SETVIEWPORT, &setVp, sizeof(setVp));

    /* 6. CMD_CLEAR */
    SVGA3dCmdClear clearCmd;
    clearCmd.cid = 200;
    clearCmd.clearFlag = SVGA3D_CLEAR_COLOR;
    clearCmd.color = 0xff0000ff;
    clearCmd.depth = 1.0f;
    clearCmd.stencil = 0;
    SVGA3dRect clRect = { 0, 0, 32, 32 };
    std::vector<uint8_t> clearPacket(sizeof(SVGA3dCmdClear) + sizeof(SVGA3dRect));
    memcpy(clearPacket.data(), &clearCmd, sizeof(SVGA3dCmdClear));
    memcpy(clearPacket.data() + sizeof(SVGA3dCmdClear), &clRect, sizeof(SVGA3dRect));
    appendCmd(SVGA_3D_CMD_CLEAR, clearPacket.data(), clearPacket.size());

    /* 7. CMD_DRAW_PRIMITIVES */
    SVGA3dCmdDrawPrimitives drawCmd;
    drawCmd.cid = 200;
    drawCmd.numVertexDecls = 1;
    drawCmd.numRanges = 1;
    SVGA3dVertexDecl decl;
    memset(&decl, 0, sizeof(decl));
    decl.identity.usage = SVGA3D_DECLUSAGE_POSITION;
    decl.identity.type = SVGA3D_DECLTYPE_FLOAT3;
    decl.array.stride = 12;
    SVGA3dPrimitiveRange range;
    memset(&range, 0, sizeof(range));
    range.primType = SVGA3D_PRIMITIVE_TRIANGLELIST;
    range.primitiveCount = 1;

    std::vector<uint8_t> drawPacket(sizeof(SVGA3dCmdDrawPrimitives) + sizeof(SVGA3dVertexDecl) + sizeof(SVGA3dPrimitiveRange));
    size_t off = 0;
    memcpy(drawPacket.data() + off, &drawCmd, sizeof(drawCmd)); off += sizeof(drawCmd);
    memcpy(drawPacket.data() + off, &decl, sizeof(decl)); off += sizeof(decl);
    memcpy(drawPacket.data() + off, &range, sizeof(range));
    appendCmd(SVGA_3D_CMD_DRAW_PRIMITIVES, drawPacket.data(), drawPacket.size());

    /* 8. CMD_SETTRANSFORM (World Matrix) */
    SVGA3dCmdSetTransform transCmd;
    transCmd.cid = 200;
    transCmd.type = SVGA3D_TRANSFORM_WORLD;
    for (int i = 0; i < 16; ++i) transCmd.matrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    appendCmd(SVGA_3D_CMD_SETTRANSFORM, &transCmd, sizeof(transCmd));

    /* 9. CMD_SETZRANGE */
    SVGA3dCmdSetZRange zCmd;
    zCmd.cid = 200;
    zCmd.zRange.min = 0.0f;
    zCmd.zRange.max = 1.0f;
    appendCmd(SVGA_3D_CMD_SETZRANGE, &zCmd, sizeof(zCmd));

    /* 10. CMD_SETSCISSORRECT */
    SVGA3dCmdSetScissorRect scCmd;
    scCmd.cid = 200;
    scCmd.rect.x = 0; scCmd.rect.y = 0; scCmd.rect.w = 32; scCmd.rect.h = 32;
    appendCmd(SVGA_3D_CMD_SETSCISSORRECT, &scCmd, sizeof(scCmd));

    /* 11. CMD_SETCLIPPLANE */
    SVGA3dCmdSetClipPlane clipCmd;
    clipCmd.cid = 200;
    clipCmd.index = 0;
    clipCmd.plane[0] = 1.0f; clipCmd.plane[1] = 0.0f; clipCmd.plane[2] = 0.0f; clipCmd.plane[3] = -10.0f;
    appendCmd(SVGA_3D_CMD_SETCLIPPLANE, &clipCmd, sizeof(clipCmd));

    /* 12. CMD_SETMATERIAL */
    SVGA3dCmdSetMaterial matCmd;
    matCmd.cid = 200;
    matCmd.face = SVGA3D_FACE_FRONT_BACK;
    memset(&matCmd.material, 0, sizeof(matCmd.material));
    matCmd.material.diffuse[0] = 1.0f; matCmd.material.diffuse[1] = 1.0f; matCmd.material.diffuse[2] = 1.0f; matCmd.material.diffuse[3] = 1.0f;
    appendCmd(SVGA_3D_CMD_SETMATERIAL, &matCmd, sizeof(matCmd));

    /* 13. CMD_SETLIGHTDATA & CMD_SETLIGHTENABLED */
    SVGA3dCmdSetLightData lightDataCmd;
    lightDataCmd.cid = 200;
    lightDataCmd.index = 0;
    memset(&lightDataCmd.data, 0, sizeof(lightDataCmd.data));
    lightDataCmd.data.type = SVGA3D_LIGHTTYPE_POINT;
    lightDataCmd.data.diffuse[0] = 1.0f; lightDataCmd.data.diffuse[1] = 1.0f; lightDataCmd.data.diffuse[2] = 1.0f; lightDataCmd.data.diffuse[3] = 1.0f;
    appendCmd(SVGA_3D_CMD_SETLIGHTDATA, &lightDataCmd, sizeof(lightDataCmd));

    SVGA3dCmdSetLightEnabled lightEnCmd;
    lightEnCmd.cid = 200;
    lightEnCmd.index = 0;
    lightEnCmd.enabled = 1;
    appendCmd(SVGA_3D_CMD_SETLIGHTENABLED, &lightEnCmd, sizeof(lightEnCmd));

    /* 14. CMD_SETTEXTURESTATE */
    SVGA3dCmdSetTextureState tsCmd;
    tsCmd.cid = 200;
    SVGA3dTextureState tsArray[2];
    tsArray[0].stage = 0; tsArray[0].name = SVGA3D_TS_COLOROP; tsArray[0].value = SVGA3D_TC_MODULATE;
    tsArray[1].stage = 0; tsArray[1].name = SVGA3D_TS_ADDRESSU; tsArray[1].value = SVGA3D_TEX_ADDRESS_CLAMP;
    std::vector<uint8_t> tsPacket(sizeof(SVGA3dCmdSetTextureState) + sizeof(tsArray));
    memcpy(tsPacket.data(), &tsCmd, sizeof(SVGA3dCmdSetTextureState));
    memcpy(tsPacket.data() + sizeof(SVGA3dCmdSetTextureState), tsArray, sizeof(tsArray));
    appendCmd(SVGA_3D_CMD_SETTEXTURESTATE, tsPacket.data(), tsPacket.size());

    /* 15. CMD_SHADER_DEFINE, CMD_SET_SHADER, CMD_SET_SHADER_CONST, CMD_SHADER_DESTROY */
    SVGA3dCmdDefineShader defSh;
    defSh.cid = 200; defSh.shid = 1; defSh.type = SVGA3D_SHADERTYPE_VS;
    uint32_t vsTokens[] = { 0xfffe0300, 0x0000ffff };
    std::vector<uint8_t> shPacket(sizeof(SVGA3dCmdDefineShader) + sizeof(vsTokens));
    memcpy(shPacket.data(), &defSh, sizeof(SVGA3dCmdDefineShader));
    memcpy(shPacket.data() + sizeof(SVGA3dCmdDefineShader), vsTokens, sizeof(vsTokens));
    appendCmd(SVGA_3D_CMD_SHADER_DEFINE, shPacket.data(), shPacket.size());

    SVGA3dCmdSetShader setSh;
    setSh.cid = 200; setSh.type = SVGA3D_SHADERTYPE_VS; setSh.shid = 1;
    appendCmd(SVGA_3D_CMD_SET_SHADER, &setSh, sizeof(setSh));

    SVGA3dCmdSetShaderConst setConst;
    setConst.cid = 200; setConst.reg = 0; setConst.type = SVGA3D_SHADERTYPE_VS; setConst.ctype = SVGA3D_CONST_TYPE_FLOAT;
    setConst.values[0] = 0x3f800000; setConst.values[1] = 0; setConst.values[2] = 0; setConst.values[3] = 0x3f800000;
    appendCmd(SVGA_3D_CMD_SET_SHADER_CONST, &setConst, sizeof(setConst));

    SVGA3dCmdDestroyShader destSh;
    destSh.cid = 200; destSh.shid = 1; destSh.type = SVGA3D_SHADERTYPE_VS;
    appendCmd(SVGA_3D_CMD_SHADER_DESTROY, &destSh, sizeof(destSh));

    /* 16. CMD_BEGIN_QUERY, CMD_END_QUERY, CMD_WAIT_FOR_QUERY */
    SVGA3dCmdBeginQuery bq;
    bq.cid = 200; bq.type = SVGA3D_QUERYTYPE_OCCLUSION;
    appendCmd(SVGA_3D_CMD_BEGIN_QUERY, &bq, sizeof(bq));

    SVGA3dCmdEndQuery eq;
    eq.cid = 200; eq.type = SVGA3D_QUERYTYPE_OCCLUSION;
    appendCmd(SVGA_3D_CMD_END_QUERY, &eq, sizeof(eq));

    SVGA3dCmdWaitForQuery wq;
    wq.cid = 200; wq.type = SVGA3D_QUERYTYPE_OCCLUSION;
    wq.guestResult.offset = 0;
    appendCmd(SVGA_3D_CMD_WAIT_FOR_QUERY, &wq, sizeof(wq));

    /* 17. Secondary Surface (SID 201) & CMD_SURFACE_COPY, CMD_SURFACE_STRETCHBLT */
    SVGA3dCmdDefineSurface defSurf2;
    memset(&defSurf2, 0, sizeof(defSurf2));
    defSurf2.sid = 201;
    defSurf2.surfaceFlags = (SVGA3dSurfaceFlags)0;
    defSurf2.format = SVGA3D_A8R8G8B8;
    defSurf2.face[0].numMipLevels = 1;
    std::vector<uint8_t> surfPacket2(sizeof(SVGA3dCmdDefineSurface) + sizeof(SVGA3dSize));
    memcpy(surfPacket2.data(), &defSurf2, sizeof(SVGA3dCmdDefineSurface));
    memcpy(surfPacket2.data() + sizeof(SVGA3dCmdDefineSurface), &surfSize, sizeof(SVGA3dSize));
    appendCmd(SVGA_3D_CMD_SURFACE_DEFINE, surfPacket2.data(), surfPacket2.size());

    SVGA3dCmdSurfaceCopy copyCmd;
    copyCmd.src.sid = 200; copyCmd.src.face = 0; copyCmd.src.mipmap = 0;
    copyCmd.dest.sid = 201; copyCmd.dest.face = 0; copyCmd.dest.mipmap = 0;
    SVGA3dCopyBox cBox;
    cBox.x = 0; cBox.y = 0; cBox.z = 0; cBox.w = 32; cBox.h = 32; cBox.d = 1;
    cBox.srcx = 0; cBox.srcy = 0; cBox.srcz = 0;
    std::vector<uint8_t> copyPacket(sizeof(SVGA3dCmdSurfaceCopy) + sizeof(SVGA3dCopyBox));
    memcpy(copyPacket.data(), &copyCmd, sizeof(SVGA3dCmdSurfaceCopy));
    memcpy(copyPacket.data() + sizeof(SVGA3dCmdSurfaceCopy), &cBox, sizeof(SVGA3dCopyBox));
    appendCmd(SVGA_3D_CMD_SURFACE_COPY, copyPacket.data(), copyPacket.size());

    SVGA3dCmdSurfaceStretchBlt stretchCmd;
    stretchCmd.src.sid = 200; stretchCmd.src.face = 0; stretchCmd.src.mipmap = 0;
    stretchCmd.dest.sid = 201; stretchCmd.dest.face = 0; stretchCmd.dest.mipmap = 0;
    stretchCmd.boxSrc = { 0, 0, 0, 32, 32, 1 };
    stretchCmd.boxDest = { 0, 0, 0, 32, 32, 1 };
    stretchCmd.mode = SVGA3D_STRETCH_BLT_LINEAR;
    appendCmd(SVGA_3D_CMD_SURFACE_STRETCHBLT, &stretchCmd, sizeof(stretchCmd));

    /* 18. CMD_GENERATE_MIPMAPS */
    SVGA3dCmdGenerateMipmaps gmCmd;
    gmCmd.sid = 200; gmCmd.filter = SVGA3D_TEX_FILTER_LINEAR;
    appendCmd(SVGA_3D_CMD_GENERATE_MIPMAPS, &gmCmd, sizeof(gmCmd));

    /* 19. CMD_BLIT_SURFACE_TO_SCREEN */
    SVGA3dCmdBlitSurfaceToScreen blitCmd;
    blitCmd.srcImage.sid = 200; blitCmd.srcImage.face = 0; blitCmd.srcImage.mipmap = 0;
    blitCmd.srcRect = { 0, 0, 32, 32 };
    blitCmd.destScreenId = 0;
    blitCmd.destRect = { 0, 0, 32, 32 };
    SVGASignedRect clip = { 0, 0, 32, 32 };
    std::vector<uint8_t> blitPacket(sizeof(SVGA3dCmdBlitSurfaceToScreen) + sizeof(SVGASignedRect));
    memcpy(blitPacket.data(), &blitCmd, sizeof(SVGA3dCmdBlitSurfaceToScreen));
    memcpy(blitPacket.data() + sizeof(SVGA3dCmdBlitSurfaceToScreen), &clip, sizeof(SVGASignedRect));
    appendCmd(SVGA_3D_CMD_BLIT_SURFACE_TO_SCREEN, blitPacket.data(), blitPacket.size());

    /* 20. CMD_PRESENT */
    SVGA3dCmdPresent prCmd;
    prCmd.sid = 200;
    SVGA3dCopyRect prR = { 0, 0, 0, 0, 32, 32 };
    std::vector<uint8_t> prPacket(sizeof(SVGA3dCmdPresent) + sizeof(SVGA3dCopyRect));
    memcpy(prPacket.data(), &prCmd, sizeof(SVGA3dCmdPresent));
    memcpy(prPacket.data() + sizeof(SVGA3dCmdPresent), &prR, sizeof(SVGA3dCopyRect));
    appendCmd(SVGA_3D_CMD_PRESENT, prPacket.data(), prPacket.size());

    /* 21. CMD_SURFACE_DEFINE_V2, CMD_ACTIVATE_SURFACE, CMD_DEACTIVATE_SURFACE */
    SVGA3dCmdDefineSurface_v2 v2Cmd;
    memset(&v2Cmd, 0, sizeof(v2Cmd));
    v2Cmd.sid = 202;
    v2Cmd.format = SVGA3D_A8R8G8B8;
    v2Cmd.multisampleCount = 1;
    v2Cmd.autogenFilter = SVGA3D_TEX_FILTER_LINEAR;
    v2Cmd.face[0].numMipLevels = 1;
    std::vector<uint8_t> v2Packet(sizeof(SVGA3dCmdDefineSurface_v2) + sizeof(SVGA3dSize));
    memcpy(v2Packet.data(), &v2Cmd, sizeof(SVGA3dCmdDefineSurface_v2));
    memcpy(v2Packet.data() + sizeof(SVGA3dCmdDefineSurface_v2), &surfSize, sizeof(SVGA3dSize));
    appendCmd(SVGA_3D_CMD_SURFACE_DEFINE_V2, v2Packet.data(), v2Packet.size());

    uint32_t actSid = 202;
    appendCmd(SVGA_3D_CMD_ACTIVATE_SURFACE, &actSid, sizeof(actSid));
    appendCmd(SVGA_3D_CMD_DEACTIVATE_SURFACE, &actSid, sizeof(actSid));

    /* Destroy surfaces 201 & 202 */
    SVGA3dCmdDestroySurface destSurf2; destSurf2.sid = 201;
    appendCmd(SVGA_3D_CMD_SURFACE_DESTROY, &destSurf2, sizeof(destSurf2));
    SVGA3dCmdDestroySurface destSurf3; destSurf3.sid = 202;
    appendCmd(SVGA_3D_CMD_SURFACE_DESTROY, &destSurf3, sizeof(destSurf3));

    /* 22. CMD_CONTEXT_DESTROY & CMD_SURFACE_DESTROY */
    SVGA3dCmdDestroyContext destCtx;
    destCtx.cid = 200;
    appendCmd(SVGA_3D_CMD_CONTEXT_DESTROY, &destCtx, sizeof(destCtx));

    SVGA3dCmdDestroySurface destSurf;
    destSurf.sid = 200;
    appendCmd(SVGA_3D_CMD_SURFACE_DESTROY, &destSurf, sizeof(destSurf));

    /* Execute the generated binary FIFO command packet */
    size_t bytesConsumed = 0;
    Svga3VlknStatus st = svga3_vlkn_fifo_execute(dev, fifoStream.data(), fifoStream.size(), &bytesConsumed);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "FIFO stream execution");
    TEST_CHECK(bytesConsumed == fifoStream.size(), "Exact bytes consumed matches buffer size");

    /* Verify destroyed resources */
    TEST_CHECK(!svga3_vlkn_context_exists(dev, 200), "FIFO destroyed context 200");
    TEST_CHECK(!svga3_vlkn_surface_exists(dev, 200), "FIFO destroyed surface 200");

    std::cout << ANSI_GREEN << "  FIFO command stream verified successfully." << ANSI_RESET << std::endl;
}

/* --------------------------------------------------------------------------
 * Test 13: Exhaustive Transforms and Z-Range
 * -------------------------------------------------------------------------- */
static void TestTransformsAndZRange(Svga3VlknDevice *dev) {
    std::cout << ANSI_CYAN << "[TEST 13] Exhaustive Transforms & Z-Range (World, View, Proj, Tex 0..7)..." << ANSI_RESET << std::endl;

    uint32_t cid = 100;
    SVGA3dTransformType types[] = {
        SVGA3D_TRANSFORM_WORLD,
        SVGA3D_TRANSFORM_VIEW,
        SVGA3D_TRANSFORM_PROJECTION,
        SVGA3D_TRANSFORM_TEXTURE0,
        SVGA3D_TRANSFORM_TEXTURE1,
        SVGA3D_TRANSFORM_TEXTURE2,
        SVGA3D_TRANSFORM_TEXTURE3,
        SVGA3D_TRANSFORM_TEXTURE4,
        SVGA3D_TRANSFORM_TEXTURE5,
        SVGA3D_TRANSFORM_TEXTURE6,
        SVGA3D_TRANSFORM_TEXTURE7
    };

    for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); ++t) {
        SVGA3dTransformType type = types[t];
        for (int variation = 0; variation < 10; ++variation) {
            float matrixIn[16];
            for (int i = 0; i < 16; ++i) {
                matrixIn[i] = (i % 5 == 0) ? 1.0f : ((float)(t + 1) * 0.1f * (float)(i + variation + 1));
            }

            Svga3VlknStatus st = svga3_vlkn_context_set_transform(dev, cid, type, matrixIn);
            TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Set transform type " + std::to_string(type) + " v" + std::to_string(variation));

            float matrixOut[16]{};
            st = svga3_vlkn_context_get_transform(dev, cid, type, matrixOut);
            TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Get transform type " + std::to_string(type) + " v" + std::to_string(variation));

            bool match = (memcmp(matrixIn, matrixOut, sizeof(matrixIn)) == 0);
            TEST_CHECK(match, "Transform matrix values match exactly");
        }
    }

    /* Test Z-Range sweeps */
    float zTests[][2] = {
        { 0.0f, 1.0f },
        { 0.1f, 0.9f },
        { 0.25f, 0.75f },
        { 0.0f, 0.5f },
        { 0.5f, 1.0f },
        { 0.15f, 0.85f },
        { 0.33f, 0.66f },
        { 0.05f, 0.95f },
        { 0.4f, 0.6f },
        { 0.0f, 0.25f }
    };

    for (size_t i = 0; i < sizeof(zTests) / sizeof(zTests[0]); ++i) {
        SVGA3dZRange zr = { zTests[i][0], zTests[i][1] };
        Svga3VlknStatus st = svga3_vlkn_context_set_zrange(dev, cid, &zr);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Set Z-Range sweep " + std::to_string(i));

        SVGA3dZRange zOut = { 0.0f, 0.0f };
        st = svga3_vlkn_context_get_zrange(dev, cid, &zOut);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Get Z-Range sweep " + std::to_string(i));
        TEST_CHECK(std::fabs(zOut.min - zTests[i][0]) < 1e-4f, "Z-Range min match");
        TEST_CHECK(std::fabs(zOut.max - zTests[i][1]) < 1e-4f, "Z-Range max match");
    }
}

/* --------------------------------------------------------------------------
 * Test 14: Exhaustive Materials and Lights
 * -------------------------------------------------------------------------- */
static void TestMaterialsAndLights(Svga3VlknDevice *dev) {
    std::cout << ANSI_CYAN << "[TEST 14] Exhaustive Materials & Hardware Lights..." << ANSI_RESET << std::endl;

    uint32_t cid = 100;

    /* Sweep materials across all faces */
    SVGA3dFace faces[] = { SVGA3D_FACE_NONE, SVGA3D_FACE_FRONT, SVGA3D_FACE_BACK, SVGA3D_FACE_FRONT_BACK };
    for (size_t f = 0; f < sizeof(faces) / sizeof(faces[0]); ++f) {
        for (int m = 0; m < 5; ++m) {
            SVGA3dMaterial mat;
            mat.diffuse[0] = 0.2f * (float)(m + 1); mat.diffuse[1] = 0.7f; mat.diffuse[2] = 0.6f; mat.diffuse[3] = 1.0f;
            mat.ambient[0] = 0.1f * (float)(m + 1); mat.ambient[1] = 0.2f; mat.ambient[2] = 0.2f; mat.ambient[3] = 1.0f;
            mat.specular[0] = 1.0f; mat.specular[1] = 1.0f; mat.specular[2] = 1.0f; mat.specular[3] = 1.0f;
            mat.emissive[0] = 0.05f * (float)m; mat.emissive[1] = 0.0f; mat.emissive[2] = 0.0f; mat.emissive[3] = 1.0f;
            mat.shininess = 16.0f * (float)(m + 1);

            Svga3VlknStatus st = svga3_vlkn_context_set_material(dev, cid, faces[f], &mat);
            TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Set material face " + std::to_string(faces[f]) + " mat " + std::to_string(m));
        }
    }

    /* Sweep 8 lights with Point, Directional, and Spot types */
    for (uint32_t lightIdx = 0; lightIdx < 8; ++lightIdx) {
        SVGA3dLightData light;
        memset(&light, 0, sizeof(light));
        light.type = SVGA3D_LIGHTTYPE_POINT;
        light.diffuse[0] = 1.0f; light.diffuse[1] = 0.9f; light.diffuse[2] = 0.8f; light.diffuse[3] = 1.0f;
        light.position[0] = 10.0f * (float)(lightIdx + 1);
        light.position[1] = 20.0f;
        light.position[2] = 30.0f;
        light.range = 100.0f;
        light.attenuation0 = 1.0f;

        Svga3VlknStatus st = svga3_vlkn_context_set_light_data(dev, cid, lightIdx, &light);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Set light (Point) " + std::to_string(lightIdx));

        st = svga3_vlkn_context_set_light_enabled(dev, cid, lightIdx, 1);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Enable light " + std::to_string(lightIdx));

        light.type = SVGA3D_LIGHTTYPE_DIRECTIONAL;
        light.direction[0] = 0.0f; light.direction[1] = -1.0f; light.direction[2] = 0.0f;
        st = svga3_vlkn_context_set_light_data(dev, cid, lightIdx, &light);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Set light (Directional) " + std::to_string(lightIdx));

        light.type = SVGA3D_LIGHTTYPE_SPOT1;
        light.theta = 0.5f; light.phi = 1.0f; light.falloff = 1.0f;
        st = svga3_vlkn_context_set_light_data(dev, cid, lightIdx, &light);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Set light (Spot) " + std::to_string(lightIdx));

        st = svga3_vlkn_context_set_light_enabled(dev, cid, lightIdx, 0);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Disable light " + std::to_string(lightIdx));
    }
}

/* --------------------------------------------------------------------------
 * Test 15: Exhaustive Clip Planes
 * -------------------------------------------------------------------------- */
static void TestClipPlanes(Svga3VlknDevice *dev) {
    std::cout << ANSI_CYAN << "[TEST 15] Exhaustive User Clip Planes (0..5)..." << ANSI_RESET << std::endl;

    uint32_t cid = 100;
    float planes[][4] = {
        { 1.0f, 0.0f, 0.0f, -5.0f },
        { 0.0f, 1.0f, 0.0f, -10.0f },
        { 0.0f, 0.0f, 1.0f, -15.0f },
        { 0.7071f, 0.7071f, 0.0f, -20.0f },
        { 0.0f, 0.7071f, 0.7071f, -25.0f },
        { 0.5773f, 0.5773f, 0.5773f, -50.0f },
        { -1.0f, 0.0f, 0.0f, 5.0f },
        { 0.0f, -1.0f, 0.0f, 10.0f },
        { 0.0f, 0.0f, -1.0f, 15.0f },
        { 0.5f, 0.5f, 0.7071f, -30.0f }
    };

    for (uint32_t idx = 0; idx < 6; ++idx) {
        for (size_t p = 0; p < sizeof(planes) / sizeof(planes[0]); ++p) {
            Svga3VlknStatus st = svga3_vlkn_context_set_clip_plane(dev, cid, idx, planes[p]);
            TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Set clip plane " + std::to_string(idx) + " p" + std::to_string(p));
        }
    }
}

/* --------------------------------------------------------------------------
 * Test 16: Exhaustive Texture Stage States (8 Stages)
 * -------------------------------------------------------------------------- */
static void TestTextureStageStates(Svga3VlknDevice *dev) {
    std::cout << ANSI_CYAN << "[TEST 16] Exhaustive Texture Stage States & Combiners (8 Stages)..." << ANSI_RESET << std::endl;

    uint32_t cid = 100;

    for (uint32_t stage = 0; stage < 8; ++stage) {
        /* Sweep all 26 texture combiners on COLOROP */
        size_t tcCount = sizeof(g_TextureCombiners) / sizeof(g_TextureCombiners[0]);
        for (size_t i = 0; i < tcCount; ++i) {
            const TextureCombinerInfo &tc = g_TextureCombiners[i];
            Svga3VlknStatus st = svga3_vlkn_context_set_texture_stage_state(
                dev, cid, stage, SVGA3D_TS_COLOROP, tc.id
            );
            TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Stage " + std::to_string(stage) + " COLOROP: " + tc.name);
        }

        /* ALPHAOP */
        Svga3VlknStatus st = svga3_vlkn_context_set_texture_stage_state(
            dev, cid, stage, SVGA3D_TS_ALPHAOP, SVGA3D_TC_SELECTARG1
        );
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Stage " + std::to_string(stage) + " ALPHAOP");

        /* COLORARG1 / COLORARG2 */
        st = svga3_vlkn_context_set_texture_stage_state(dev, cid, stage, SVGA3D_TS_COLORARG1, D3DTA_TEXTURE);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Stage " + std::to_string(stage) + " COLORARG1");
        st = svga3_vlkn_context_set_texture_stage_state(dev, cid, stage, SVGA3D_TS_COLORARG2, D3DTA_DIFFUSE);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Stage " + std::to_string(stage) + " COLORARG2");

        /* ADDRESSU / ADDRESSV / ADDRESSW */
        st = svga3_vlkn_context_set_texture_stage_state(dev, cid, stage, SVGA3D_TS_ADDRESSU, SVGA3D_TEX_ADDRESS_CLAMP);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Stage " + std::to_string(stage) + " ADDRESSU");
        st = svga3_vlkn_context_set_texture_stage_state(dev, cid, stage, SVGA3D_TS_ADDRESSV, SVGA3D_TEX_ADDRESS_WRAP);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Stage " + std::to_string(stage) + " ADDRESSV");
        st = svga3_vlkn_context_set_texture_stage_state(dev, cid, stage, SVGA3D_TS_ADDRESSW, SVGA3D_TEX_ADDRESS_MIRROR);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Stage " + std::to_string(stage) + " ADDRESSW");

        /* FILTERS */
        st = svga3_vlkn_context_set_texture_stage_state(dev, cid, stage, SVGA3D_TS_MAGFILTER, SVGA3D_TEX_FILTER_LINEAR);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Stage " + std::to_string(stage) + " MAGFILTER");
        st = svga3_vlkn_context_set_texture_stage_state(dev, cid, stage, SVGA3D_TS_MINFILTER, SVGA3D_TEX_FILTER_LINEAR);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Stage " + std::to_string(stage) + " MINFILTER");
        st = svga3_vlkn_context_set_texture_stage_state(dev, cid, stage, SVGA3D_TS_MIPFILTER, SVGA3D_TEX_FILTER_NEAREST);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Stage " + std::to_string(stage) + " MIPFILTER");
    }
}

/* --------------------------------------------------------------------------
 * Test 17: Exhaustive Render State Sweeps
 * -------------------------------------------------------------------------- */
static void TestRenderStatesExhaustive(Svga3VlknDevice *dev) {
    std::cout << ANSI_CYAN << "[TEST 17] Exhaustive Render State Value Sweeps (82 States)..." << ANSI_RESET << std::endl;

    uint32_t cid = 100;
    size_t count = sizeof(g_RenderStates) / sizeof(g_RenderStates[0]);

    for (size_t i = 0; i < count; ++i) {
        const RenderStateInfo &rsi = g_RenderStates[i];

        /* Sweep 4 distinct values */
        for (uint32_t sweep = 0; sweep < 4; ++sweep) {
            uint32_t testVal = sweep;
            if (rsi.id == SVGA3D_RS_FILLMODE) testVal = (sweep % 2 == 0) ? SVGA3D_FILLMODE_FILL : SVGA3D_FILLMODE_LINE;
            else if (rsi.id == SVGA3D_RS_SHADEMODE) testVal = (sweep % 2 == 0) ? SVGA3D_SHADEMODE_SMOOTH : SVGA3D_SHADEMODE_FLAT;
            else if (rsi.id == SVGA3D_RS_CULLMODE) testVal = (sweep % 2 == 0) ? SVGA3D_FACE_BACK : SVGA3D_FACE_FRONT;
            else if (rsi.id == SVGA3D_RS_SRCBLEND || rsi.id == SVGA3D_RS_DSTBLEND) testVal = (sweep % 2 == 0) ? SVGA3D_BLENDOP_SRCALPHA : SVGA3D_BLENDOP_INVSRCALPHA;
            else if (rsi.id == SVGA3D_RS_BLENDEQUATION) testVal = (sweep % 2 == 0) ? SVGA3D_BLENDEQ_ADD : SVGA3D_BLENDEQ_SUBTRACT;
            else if (rsi.id == SVGA3D_RS_ZFUNC) testVal = (sweep % 2 == 0) ? SVGA3D_CMP_LESSEQUAL : SVGA3D_CMP_ALWAYS;

            Svga3VlknStatus st = svga3_vlkn_context_set_render_state(dev, cid, (SVGA3dRenderStateName)rsi.id, testVal);
            TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Set RS " + std::string(rsi.name) + " s" + std::to_string(sweep));

            uint32_t readBack = 0;
            st = svga3_vlkn_context_get_render_state(dev, cid, (SVGA3dRenderStateName)rsi.id, &readBack);
            TEST_CHECK(st == SVGA3_VLKN_SUCCESS && readBack == testVal, "Get RS " + std::string(rsi.name) + " s" + std::to_string(sweep));
        }
    }
}

/* --------------------------------------------------------------------------
 * Test 18: Shaders and Full Constant Register Banks
 * -------------------------------------------------------------------------- */
static void TestShadersAndConstants(Svga3VlknDevice *dev) {
    std::cout << ANSI_CYAN << "[TEST 18] Shader Lifecycle & Constant Banks (Float 0..255, Int 0..15, Bool 0..15)..." << ANSI_RESET << std::endl;

    uint32_t cid = 100;
    uint32_t vsTokens[] = { 0xfffe0300, 0x0000ffff };
    Svga3VlknStatus st = svga3_vlkn_context_define_shader(dev, cid, 1, SVGA3D_SHADERTYPE_VS, vsTokens, 2);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Define vertex shader 1");

    st = svga3_vlkn_context_set_shader(dev, cid, SVGA3D_SHADERTYPE_VS, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Bind vertex shader 1");

    uint32_t psTokens[] = { 0xffff0300, 0x0000ffff };
    st = svga3_vlkn_context_define_shader(dev, cid, 1, SVGA3D_SHADERTYPE_PS, psTokens, 2);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Define pixel shader 1");

    st = svga3_vlkn_context_set_shader(dev, cid, SVGA3D_SHADERTYPE_PS, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Bind pixel shader 1");

    /* Full Float Register Bank Sweep 0..255 for VS and PS */
    for (uint32_t reg = 0; reg < 256; ++reg) {
        float fVals[4] = { (float)reg, (float)reg + 0.25f, (float)reg + 0.5f, (float)reg + 0.75f };
        const uint32_t *uVals = reinterpret_cast<const uint32_t*>(fVals);

        st = svga3_vlkn_context_set_shader_const(dev, cid, reg, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, uVals);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Set VS Float Const reg " + std::to_string(reg));

        uint32_t readBack[4]{};
        st = svga3_vlkn_context_get_shader_const(dev, cid, reg, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, readBack);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS && memcmp(uVals, readBack, 4 * sizeof(uint32_t)) == 0, "Get VS Float Const reg " + std::to_string(reg));

        st = svga3_vlkn_context_set_shader_const(dev, cid, reg, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT, uVals);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Set PS Float Const reg " + std::to_string(reg));

        st = svga3_vlkn_context_get_shader_const(dev, cid, reg, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT, readBack);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS && memcmp(uVals, readBack, 4 * sizeof(uint32_t)) == 0, "Get PS Float Const reg " + std::to_string(reg));
    }

    /* Int Register Bank Sweep 0..15 for VS and PS */
    for (uint32_t reg = 0; reg < 16; ++reg) {
        int32_t iVals[4] = { (int32_t)reg, (int32_t)reg * 10, (int32_t)reg * 100, 1 };
        const uint32_t *uVals = reinterpret_cast<const uint32_t*>(iVals);

        st = svga3_vlkn_context_set_shader_const(dev, cid, reg, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_INT, uVals);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Set VS Int Const reg " + std::to_string(reg));

        uint32_t readBack[4]{};
        st = svga3_vlkn_context_get_shader_const(dev, cid, reg, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_INT, readBack);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS && memcmp(uVals, readBack, 4 * sizeof(uint32_t)) == 0, "Get VS Int Const reg " + std::to_string(reg));

        st = svga3_vlkn_context_set_shader_const(dev, cid, reg, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_INT, uVals);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Set PS Int Const reg " + std::to_string(reg));

        st = svga3_vlkn_context_get_shader_const(dev, cid, reg, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_INT, readBack);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS && memcmp(uVals, readBack, 4 * sizeof(uint32_t)) == 0, "Get PS Int Const reg " + std::to_string(reg));
    }

    /* Bool Register Bank Sweep 0..15 for VS and PS */
    for (uint32_t reg = 0; reg < 16; ++reg) {
        uint32_t bVal = (reg % 2 == 0) ? 1 : 0;
        uint32_t bArray[4] = { bVal, 0, 0, 0 };

        st = svga3_vlkn_context_set_shader_const(dev, cid, reg, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_BOOL, bArray);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Set VS Bool Const reg " + std::to_string(reg));

        uint32_t readBack[4]{};
        st = svga3_vlkn_context_get_shader_const(dev, cid, reg, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_BOOL, readBack);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS && readBack[0] == bVal, "Get VS Bool Const reg " + std::to_string(reg));

        st = svga3_vlkn_context_set_shader_const(dev, cid, reg, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_BOOL, bArray);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Set PS Bool Const reg " + std::to_string(reg));

        st = svga3_vlkn_context_get_shader_const(dev, cid, reg, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_BOOL, readBack);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS && readBack[0] == bVal, "Get PS Bool Const reg " + std::to_string(reg));
    }

    /* Destroy Shaders */
    st = svga3_vlkn_context_destroy_shader(dev, cid, 1, SVGA3D_SHADERTYPE_VS);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Destroy vertex shader 1");

    st = svga3_vlkn_context_destroy_shader(dev, cid, 1, SVGA3D_SHADERTYPE_PS);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Destroy pixel shader 1");
}

/* --------------------------------------------------------------------------
 * Test 19: Hardware Occlusion Queries
 * -------------------------------------------------------------------------- */
static void TestHardwareQueries(Svga3VlknDevice *dev) {
    std::cout << ANSI_CYAN << "[TEST 19] Hardware Occlusion Queries (Begin, End, Wait)..." << ANSI_RESET << std::endl;

    uint32_t cid = 100;

    for (uint32_t cycle = 1; cycle <= 16; ++cycle) {
        Svga3VlknStatus st = svga3_vlkn_context_begin_query(dev, cid, SVGA3D_QUERYTYPE_OCCLUSION);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Begin query cycle " + std::to_string(cycle));

        st = svga3_vlkn_context_end_query(dev, cid, SVGA3D_QUERYTYPE_OCCLUSION);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "End query cycle " + std::to_string(cycle));

        uint32_t result = 0;
        st = svga3_vlkn_context_wait_for_query(dev, cid, SVGA3D_QUERYTYPE_OCCLUSION, &result);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Wait for query cycle " + std::to_string(cycle));
    }
}

/* --------------------------------------------------------------------------
 * Test 20: Advanced Surface Operations (StretchBlt, Mipmaps, BlitToScreen, Present)
 * -------------------------------------------------------------------------- */
static void TestAdvancedSurfaceOperations(Svga3VlknDevice *dev) {
    std::cout << ANSI_CYAN << "[TEST 20] Advanced Surface Operations (StretchBlt, Mipmaps, BlitToScreen, Present)..." << ANSI_RESET << std::endl;

    /* Stretch Blit between SID 10 and SID 1 */
    SVGA3dBox srcBox = { 0, 0, 0, 16, 16, 1 };
    SVGA3dBox dstBox = { 0, 0, 0, 64, 64, 1 };
    Svga3VlknStatus st = svga3_vlkn_surface_stretch_blt(dev, 10, 1, &srcBox, &dstBox, SVGA3D_STRETCH_BLT_LINEAR);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Surface stretch blt (Linear)");

    st = svga3_vlkn_surface_stretch_blt(dev, 10, 1, &srcBox, &dstBox, SVGA3D_STRETCH_BLT_POINT);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Surface stretch blt (Point)");

    /* Mipmap Generation */
    st = svga3_vlkn_surface_generate_mipmaps(dev, 1, SVGA3D_TEX_FILTER_LINEAR);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Generate mipmaps (Linear)");

    st = svga3_vlkn_surface_generate_mipmaps(dev, 1, SVGA3D_TEX_FILTER_NEAREST);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Generate mipmaps (Nearest)");

    /* Surface Blit to Screen */
    SVGA3dSurfaceImageId srcImage = { 1, 0, 0 };
    SVGASignedRect destRect = { 0, 0, 64, 64 };
    SVGASignedRect srcRect = { 0, 0, 64, 64 };
    SVGASignedRect clipRect = { 0, 0, 64, 64 };
    st = svga3_vlkn_surface_blit_to_screen(dev, &srcImage, &srcRect, 0, &destRect, &clipRect, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Surface blit to screen");

    /* Surface Present */
    SVGA3dCopyRect prRect = { 0, 0, 0, 0, 64, 64 };
    st = svga3_vlkn_surface_present(dev, 1, &prRect, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Surface present");
}

/* --------------------------------------------------------------------------
 * Test 21: Multi-Context State Isolation
 * -------------------------------------------------------------------------- */
static void TestMultiContextIsolation(Svga3VlknDevice *dev) {
    std::cout << ANSI_CYAN << "[TEST 21] Multi-Context State Isolation (8 Contexts)..." << ANSI_RESET << std::endl;

    for (uint32_t c = 301; c <= 308; ++c) {
        svga3_vlkn_context_create(dev, c);
    }

    /* Assign distinct render states and transforms to each context */
    for (uint32_t c = 301; c <= 308; ++c) {
        svga3_vlkn_context_set_render_state(dev, c, SVGA3D_RS_ZENABLE, (c % 2));
        svga3_vlkn_context_set_render_state(dev, c, SVGA3D_RS_STENCILREF, c * 10);

        float matrix[16];
        for (int i = 0; i < 16; ++i) matrix[i] = (float)c + (float)i * 0.1f;
        svga3_vlkn_context_set_transform(dev, c, SVGA3D_TRANSFORM_WORLD, matrix);
    }

    /* Verify each context preserved its unique state */
    for (uint32_t c = 301; c <= 308; ++c) {
        uint32_t zVal = 0, stVal = 0;
        svga3_vlkn_context_get_render_state(dev, c, SVGA3D_RS_ZENABLE, &zVal);
        TEST_CHECK(zVal == (c % 2), "Context " + std::to_string(c) + " ZEnable preserved");

        svga3_vlkn_context_get_render_state(dev, c, SVGA3D_RS_STENCILREF, &stVal);
        TEST_CHECK(stVal == c * 10, "Context " + std::to_string(c) + " StencilRef preserved");

        float matrixOut[16]{};
        svga3_vlkn_context_get_transform(dev, c, SVGA3D_TRANSFORM_WORLD, matrixOut);
        TEST_CHECK(matrixOut[0] == (float)c, "Context " + std::to_string(c) + " Matrix [0][0] preserved");
    }

    for (uint32_t c = 301; c <= 308; ++c) {
        svga3_vlkn_context_destroy(dev, c);
    }
}

/* --------------------------------------------------------------------------
 * Test 22: Stress & High-Load Execution
 * -------------------------------------------------------------------------- */
static void TestStressExecution(Svga3VlknDevice *dev) {
    std::cout << ANSI_CYAN << "[TEST 22] Stress & High-Load Execution..." << ANSI_RESET << std::endl;

    auto tStart = std::chrono::high_resolution_clock::now();

    /* Allocate and free 250 surfaces */
    for (uint32_t i = 1000; i < 1250; ++i) {
        SVGA3dSize sz = { 16, 16, 1 };
        Svga3VlknStatus st = svga3_vlkn_surface_define(dev, i, 0, SVGA3D_X8R8G8B8, &sz, 1);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Surface stress allocation " + std::to_string(i));
    }

    for (uint32_t i = 1000; i < 1250; ++i) {
        Svga3VlknStatus st = svga3_vlkn_surface_destroy(dev, i);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Surface stress destruction " + std::to_string(i));
    }

    /* Create 30 contexts */
    for (uint32_t c = 500; c < 530; ++c) {
        Svga3VlknStatus st = svga3_vlkn_context_create(dev, c);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Context stress create " + std::to_string(c));
        svga3_vlkn_context_set_render_state(dev, c, SVGA3D_RS_ZENABLE, 1);
        svga3_vlkn_context_set_render_state(dev, c, SVGA3D_RS_BLENDENABLE, 1);
    }

    for (uint32_t c = 500; c < 530; ++c) {
        Svga3VlknStatus st = svga3_vlkn_context_destroy(dev, c);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Context stress destroy " + std::to_string(c));
    }

    auto tEnd = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

    std::cout << ANSI_GREEN << "  Stress test completed resource cycles in " << ms << " ms." << ANSI_RESET << std::endl;
}

/* Exercise the fence helper used by the preload hook, including offscreen work. */
static PFN_vkQueueSubmit savedQueueSubmit;
static PFN_vkQueueWaitIdle savedQueueWaitIdle;
static unsigned fenceSubmits, fenceWaits;
static VkResult VKAPI_CALL countFenceSubmit(VkQueue queue, uint32_t count,
                                           const VkSubmitInfo *info, VkFence fence) {
    ++fenceSubmits;
    return savedQueueSubmit(queue, count, info, fence);
}
static VkResult VKAPI_CALL countFenceWait(VkQueue queue) {
    ++fenceWaits;
    return savedQueueWaitIdle(queue);
}

static void TestFenceWithoutWindow() {
    Svga3VlknConfig cfg{};
    cfg.forceMockBackend = true;
    auto *dev = svga3_vlkn_device_create(&cfg);
    TEST_CHECK(dev != nullptr, "Create fence regression device");
    if (!dev) return;
    auto &backend = *dev->backend;
    auto &dispatch = backend.dispatch();
    savedQueueSubmit = dispatch.vkQueueSubmit;
    savedQueueWaitIdle = dispatch.vkQueueWaitIdle;
    dispatch.vkQueueSubmit = countFenceSubmit;
    dispatch.vkQueueWaitIdle = countFenceWait;

    for (bool offscreen : {false, true}) {
        if (offscreen) {
            SVGA3dSize size = {64, 64, 1};
            TEST_CHECK(svga3_vlkn_surface_define(dev, 1, 0, SVGA3D_Z_D16, &size, 1) == SVGA3_VLKN_SUCCESS,
                       "Create offscreen depth target");
            TEST_CHECK(svga3_vlkn_context_create(dev, 1) == SVGA3_VLKN_SUCCESS, "Create offscreen context");
            auto *ctx = dev->contextMgr->getContext(1);
            ctx->setRenderTarget(SVGA3D_RT_DEPTH, 1, 0, 0);
            TEST_CHECK(ctx->clear(SVGA3D_CLEAR_DEPTH, 0, 1.0f, 0, nullptr, 0) == SVGA3_VLKN_SUCCESS,
                       "Record depth-only clear");
        } else {
            backend.getActiveCommandBuffer();
        }
        fenceSubmits = fenceWaits = 0;
        svga3_vlkn::svga3_vlkn_present_client_surfaces(dev, "fence-regression");
        TEST_CHECK(fenceSubmits == 1 && fenceWaits == 1,
                   "Fence submits and waits even with no pending window");
        backend.flushCommandBuffer();
        TEST_CHECK(fenceSubmits == 1 && fenceWaits == 1, "Fence leaves no unsubmitted commands");
    }
    dispatch.vkQueueSubmit = savedQueueSubmit;
    dispatch.vkQueueWaitIdle = savedQueueWaitIdle;
    svga3_vlkn_device_destroy(dev);
}

static VkResult g_injectedWaitResult = VK_SUCCESS;
static VkResult g_injectedDeviceWaitResult = VK_SUCCESS;
static PFN_vkQueueWaitIdle g_savedErrorQueueWait = nullptr;
static PFN_vkDeviceWaitIdle g_savedErrorDeviceWait = nullptr;
static VkResult VKAPI_CALL injectQueueWaitResult(VkQueue queue) {
    return g_injectedWaitResult == VK_SUCCESS ? g_savedErrorQueueWait(queue) : g_injectedWaitResult;
}
static VkResult VKAPI_CALL injectDeviceWaitResult(VkDevice device) {
    return g_injectedDeviceWaitResult == VK_SUCCESS ? g_savedErrorDeviceWait(device) : g_injectedDeviceWaitResult;
}

static void TestFlushPropagatesWaitFailures() {
    Svga3VlknConfig cfg{};
    cfg.forceMockBackend = true;
    auto *dev = svga3_vlkn_device_create(&cfg);
    TEST_CHECK(dev != nullptr, "Create flush error regression device");
    if (!dev) return;
    auto &backend = *dev->backend;
    auto &dispatch = backend.dispatch();
    g_savedErrorQueueWait = dispatch.vkQueueWaitIdle;
    g_savedErrorDeviceWait = dispatch.vkDeviceWaitIdle;
    dispatch.vkQueueWaitIdle = injectQueueWaitResult;
    dispatch.vkDeviceWaitIdle = injectDeviceWaitResult;

    const uint64_t serial = backend.completedSubmissionSerial();
    backend.getActiveCommandBuffer();
    g_injectedWaitResult = VK_ERROR_DEVICE_LOST;
    TEST_CHECK(backend.flushCommandBuffer() == SVGA3_VLKN_ERROR_DEVICE_LOST,
               "Queue wait failure is returned from flush");
    TEST_CHECK(backend.completedSubmissionSerial() == serial,
               "Failed queue wait does not report submission complete");

    g_injectedWaitResult = VK_SUCCESS;
    g_injectedDeviceWaitResult = VK_ERROR_DEVICE_LOST;
    TEST_CHECK(backend.waitIdle() == SVGA3_VLKN_ERROR_DEVICE_LOST,
               "Device wait failure is returned to caller");
    TEST_CHECK(backend.completedSubmissionSerial() == serial,
               "Failed device wait does not advance completion serial");

    g_injectedDeviceWaitResult = VK_SUCCESS;
    TEST_CHECK(backend.waitIdle() == SVGA3_VLKN_SUCCESS,
               "Successful device wait recovers pending submission");
    TEST_CHECK(backend.completedSubmissionSerial() == serial + 1,
               "Completion serial advances once after confirmed completion");

    dispatch.vkQueueWaitIdle = g_savedErrorQueueWait;
    dispatch.vkDeviceWaitIdle = g_savedErrorDeviceWait;
    svga3_vlkn_device_destroy(dev);
}

static VkResult VKAPI_CALL failAllSubmits(VkQueue queue, uint32_t count,
                                         const VkSubmitInfo *info, VkFence fence) {
    (void)queue; (void)count; (void)info; (void)fence;
    return VK_ERROR_DEVICE_LOST;
}

/* Failure-injection (issue #12): a failed flush must propagate out of the
 * surface transfer paths instead of being silently discarded as SUCCESS. */
static void TestSurfaceTransfersPropagateFlushFailures() {
    Svga3VlknConfig cfg{};
    cfg.forceMockBackend = true;
    auto *dev = svga3_vlkn_device_create(&cfg);
    TEST_CHECK(dev != nullptr, "Create flush-propagation regression device");
    if (!dev) return;

    SVGA3dSize size = {64, 64, 1};
    TEST_CHECK(svga3_vlkn_surface_define(dev, 9201, 0, SVGA3D_X8R8G8B8, &size, 1) ==
                   SVGA3_VLKN_SUCCESS,
               "Define flush-propagation surface A");
    TEST_CHECK(svga3_vlkn_surface_define(dev, 9202, 0, SVGA3D_X8R8G8B8, &size, 1) ==
                   SVGA3_VLKN_SUCCESS,
               "Define flush-propagation surface B");

    auto &dispatch = dev->backend->dispatch();
    PFN_vkQueueSubmit saved = dispatch.vkQueueSubmit;
    dispatch.vkQueueSubmit = failAllSubmits;

    std::vector<uint32_t> pixels(64 * 64, 0x11223344u);
    SVGA3dBox box = { 0, 0, 0, 64, 64, 1 };
    TEST_CHECK(svga3_vlkn_surface_dma_upload(dev, 9201, 0, &box, pixels.data(),
                                            64 * sizeof(uint32_t)) != SVGA3_VLKN_SUCCESS,
               "dma_upload propagates flush/submit failure");

    SVGA3dCopyBox cbox;
    cbox.x = 0; cbox.y = 0; cbox.z = 0;
    cbox.w = 64; cbox.h = 64; cbox.d = 1;
    cbox.srcx = 0; cbox.srcy = 0; cbox.srcz = 0;
    TEST_CHECK(svga3_vlkn_surface_copy(dev, 9201, 9202, &cbox, 1) != SVGA3_VLKN_SUCCESS,
               "surface_copy propagates flush/submit failure");

    dispatch.vkQueueSubmit = saved;
    svga3_vlkn_device_destroy(dev);
}

static VkResult VKAPI_CALL failCreateFramebuffer(VkDevice device,
                                                 const VkFramebufferCreateInfo *info,
                                                 const VkAllocationCallbacks *alloc,
                                                 VkFramebuffer *framebuffer) {
    (void)device; (void)info; (void)alloc; (void)framebuffer;
    return VK_ERROR_DEVICE_LOST;
}

/* Review findings (PR #16): clear()/draw() discarded the
 * ensureRenderPassActive() status and recorded commands with no render
 * pass active when framebuffer creation failed, and ensureBufferSize()
 * leaked its growth budget reservation when the pre-growth flush failed.
 * Both failures must propagate and leave no residue. */
static void TestRenderPassAndGrowthFlushFailuresPropagate() {
    Svga3VlknConfig cfg{};
    cfg.forceMockBackend = true;
    auto *dev = svga3_vlkn_device_create(&cfg);
    TEST_CHECK(dev != nullptr, "Create review-findings regression device");
    if (!dev) return;

    SVGA3dSize size = {64, 64, 1};
    TEST_CHECK(svga3_vlkn_surface_define(dev, 9301, SVGA3D_SURFACE_HINT_RENDERTARGET,
                                         SVGA3D_A8R8G8B8, &size, 1) == SVGA3_VLKN_SUCCESS,
               "Define review-findings render target");
    TEST_CHECK(svga3_vlkn_context_create(dev, 9301) == SVGA3_VLKN_SUCCESS,
               "Create review-findings context");
    TEST_CHECK(svga3_vlkn_context_set_render_target(dev, 9301, SVGA3D_RT_COLOR0, 9301, 0, 0) ==
                   SVGA3_VLKN_SUCCESS,
               "Bind review-findings render target");

    auto &dispatch = dev->backend->dispatch();
    PFN_vkCreateFramebuffer savedCreateFb = dispatch.vkCreateFramebuffer;
    dispatch.vkCreateFramebuffer = failCreateFramebuffer;

    TEST_CHECK(svga3_vlkn_context_clear(dev, 9301, SVGA3D_CLEAR_COLOR, 0xff336699, 1.0f, 0,
                                        nullptr, 0) != SVGA3_VLKN_SUCCESS,
               "clear propagates render-pass activation failure");

    SVGA3dVertexDecl decls[2];
    memset(decls, 0, sizeof(decls));
    decls[0].identity.usage = SVGA3D_DECLUSAGE_POSITION;
    decls[0].identity.type = SVGA3D_DECLTYPE_FLOAT3;
    decls[0].array.stride = 24;
    decls[1].identity.usage = SVGA3D_DECLUSAGE_COLOR;
    decls[1].identity.type = SVGA3D_DECLTYPE_D3DCOLOR;
    decls[1].array.stride = 24;
    decls[1].array.offset = 12;
    SVGA3dPrimitiveRange range;
    memset(&range, 0, sizeof(range));
    range.primType = SVGA3D_PRIMITIVE_TRIANGLELIST;
    range.primitiveCount = 1;
    TEST_CHECK(svga3_vlkn_context_draw(dev, 9301, SVGA3D_PRIMITIVE_TRIANGLELIST,
                                       decls, 2, &range, 1) != SVGA3_VLKN_SUCCESS,
               "draw propagates render-pass activation failure");

    dispatch.vkCreateFramebuffer = savedCreateFb;
    TEST_CHECK(svga3_vlkn_context_clear(dev, 9301, SVGA3D_CLEAR_COLOR, 0xff336699, 1.0f, 0,
                                        nullptr, 0) == SVGA3_VLKN_SUCCESS,
               "Clear recovers once framebuffer creation succeeds again");

    /* ensureBufferSize: a failed pre-growth flush must release the
     * growth budget reservation it took before flushing. */
    TEST_CHECK(svga3_vlkn_surface_define(dev, 9302, 0, SVGA3D_X8R8G8B8, &size, 1) ==
                   SVGA3_VLKN_SUCCESS,
               "Define growth-budget surface");
    svga3_vlkn::VlknSurface *surf = dev->surfaceMgr->getSurface(9302);
    TEST_CHECK(surf != nullptr, "Growth-budget surface resolves");
    if (surf) {
        uint64_t budgetBefore = dev->backend->resourceBudgets().surfaceBytes();
        dev->backend->getActiveCommandBuffer(); /* make the flush submit */
        PFN_vkQueueSubmit savedSubmit = dispatch.vkQueueSubmit;
        dispatch.vkQueueSubmit = failAllSubmits;
        TEST_CHECK(surf->ensureBufferSize(1u << 20) != SVGA3_VLKN_SUCCESS,
                   "Buffer growth propagates flush failure");
        TEST_CHECK(dev->backend->resourceBudgets().surfaceBytes() == budgetBefore,
                   "Failed growth flush releases the reserved growth budget");
        dispatch.vkQueueSubmit = savedSubmit;
        TEST_CHECK(surf->ensureBufferSize(1u << 20) == SVGA3_VLKN_SUCCESS,
                   "Buffer growth succeeds once the flush recovers");
    }

    svga3_vlkn_device_destroy(dev);
}

static void TestPendingWindowPresentsOnlyWhenDirty() {
    Svga3VlknConfig cfg{};
    cfg.forceMockBackend = true;
    auto *dev = svga3_vlkn_device_create(&cfg);
    TEST_CHECK(dev != nullptr, "Create window-present regression device");
    if (!dev) return;

    SVGA3dSize size = {800, 600, 1};
    TEST_CHECK(svga3_vlkn_surface_define(dev, 9200, SVGA3D_SURFACE_HINT_RENDERTARGET,
                                         SVGA3D_A8R8G8B8, &size, 1) == SVGA3_VLKN_SUCCESS,
               "Create window surface for present tracking");
    TEST_CHECK(svga3_vlkn_context_create(dev, 9200) == SVGA3_VLKN_SUCCESS,
               "Create context for present tracking");
    auto *ctx = dev->contextMgr->getContext(9200);
    TEST_CHECK(ctx != nullptr, "Find context for present tracking");
    if (ctx) ctx->markWindowDrawn(9200);

    auto first = dev->contextMgr->collectPendingWindowPresents();
    TEST_CHECK(first.size() == 1 && first[0].first == 9200 && first[0].second == 9200,
               "Collect a newly drawn window surface once");
    auto second = dev->contextMgr->collectPendingWindowPresents();
    TEST_CHECK(second.empty(), "Do not re-present an unchanged window surface");

    svga3_vlkn_device_destroy(dev);
}

static unsigned presentUpdateCount;
static void countDisplayUpdate(void *, int32_t, int32_t, int32_t, int32_t) {
    ++presentUpdateCount;
}

static void TestPresentAlwaysCopies() {
    /* PRESENT is a command to copy now: the framebuffer is shared state
     * the driver does not own (the display layer may reset or redraw it
     * at the same address), so re-presenting an unchanged surface must
     * still copy and notify. A duplicate-present suppression here once
     * dropped partial presents after a framebuffer reset
     * (test_presentation Test 2). */
    Svga3VlknConfig cfg{};
    cfg.forceMockBackend = true;
    auto *dev = svga3_vlkn_device_create(&cfg);
    TEST_CHECK(dev != nullptr, "Create present-contract regression device");
    if (!dev) return;

    static std::vector<uint32_t> fb(128 * 128, 0);
    TEST_CHECK(svga3_vlkn_device_set_framebuffer(dev, fb.data(), 0xE0000000ULL,
                                                 fb.size() * sizeof(uint32_t),
                                                 128, 128, 128 * 4, 4) == SVGA3_VLKN_SUCCESS,
               "Register framebuffer for present contract");
    TEST_CHECK(svga3_vlkn_device_set_display_callback(dev, nullptr, countDisplayUpdate) == SVGA3_VLKN_SUCCESS,
               "Register display update counter");
    SVGA3dSize size = {128, 128, 1};
    TEST_CHECK(svga3_vlkn_surface_define(dev, 42, 0, SVGA3D_A8R8G8B8, &size, 1) == SVGA3_VLKN_SUCCESS,
               "Define presentable surface");

    std::vector<uint8_t> pixels(128 * 128 * 4, 0x7F);
    TEST_CHECK(svga3_vlkn_surface_dma_upload(dev, 42, 0, nullptr, pixels.data(), 128 * 4) == SVGA3_VLKN_SUCCESS,
               "Upload surface content");

    presentUpdateCount = 0;
    TEST_CHECK(svga3_vlkn_surface_present(dev, 42, nullptr, 0) == SVGA3_VLKN_SUCCESS,
               "First present succeeds");
    TEST_CHECK(presentUpdateCount == 1, "First present notifies display once");
    TEST_CHECK(fb[0] == 0x7F7F7F7Fu, "First present copies surface into framebuffer");

    /* Simulate the display layer resetting the framebuffer in place
     * (same address, same size): the surface is unchanged, but the
     * framebuffer no longer holds the frame. */
    fb[0] = 0;
    fb[64 * 128 + 64] = 0;
    TEST_CHECK(svga3_vlkn_surface_present(dev, 42, nullptr, 0) == SVGA3_VLKN_SUCCESS,
               "Re-present of unchanged surface succeeds");
    TEST_CHECK(presentUpdateCount == 2, "Re-present of unchanged surface still notifies");
    TEST_CHECK(fb[0] == 0x7F7F7F7Fu && fb[64 * 128 + 64] == 0x7F7F7F7Fu,
               "Re-present restores framebuffer contents after external reset");

    /* A partial present after a full present must copy its rects too. */
    SVGA3dCopyRect rect = {10, 10, 20, 20, 0, 0};
    fb[10 * 128 + 10] = 0;
    TEST_CHECK(svga3_vlkn_surface_present(dev, 42, &rect, 1) == SVGA3_VLKN_SUCCESS,
               "Partial present succeeds");
    TEST_CHECK(fb[10 * 128 + 10] == 0x7F7F7F7Fu,
               "Partial present copies its rectangle after a full present");

    svga3_vlkn_device_destroy(dev);
}

static PFN_vkCreateShaderModule savedCreateShaderModule;
static PFN_vkDestroyShaderModule savedDestroyShaderModule;
static VkShaderModule trackedShader;
static unsigned trackedShaderDestroys, variantFailures;
static VkResult VKAPI_CALL captureShader(VkDevice device, const VkShaderModuleCreateInfo *info,
                                        const VkAllocationCallbacks *allocator, VkShaderModule *module) {
    VkResult result = savedCreateShaderModule(device, info, allocator, module);
    if (result == VK_SUCCESS) trackedShader = *module;
    return result;
}
static VkResult VKAPI_CALL failDepthVariant(VkDevice, const VkShaderModuleCreateInfo *,
                                          const VkAllocationCallbacks *, VkShaderModule *module) {
    ++variantFailures;
    *module = VK_NULL_HANDLE;
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void VKAPI_CALL countShaderDestroy(VkDevice device, VkShaderModule module,
                                         const VkAllocationCallbacks *allocator) {
    if (module == trackedShader) ++trackedShaderDestroys;
    savedDestroyShaderModule(device, module, allocator);
}

static void TestDepthVariantCleanup() {
    /* Multiple failed masks alias the base module. Test all three cleanup paths. */
    for (int cleanup = 0; cleanup < 3; ++cleanup) {
        Svga3VlknConfig cfg{};
        cfg.forceMockBackend = true;
        auto *dev = svga3_vlkn_device_create(&cfg);
        TEST_CHECK(dev != nullptr, "Create depth variant regression device");
        if (!dev) return;
        TEST_CHECK(svga3_vlkn_context_create(dev, 1) == SVGA3_VLKN_SUCCESS, "Create variant context");
        auto *ctx = dev->contextMgr->getContext(1);
        SVGA3dSize size = {64, 64, 1};
        TEST_CHECK(svga3_vlkn_surface_define(dev, 1, 0, SVGA3D_A8R8G8B8, &size, 1) == SVGA3_VLKN_SUCCESS,
                   "Create variant render target");
        TEST_CHECK(svga3_vlkn_surface_define(dev, 2, 0, SVGA3D_Z_D16, &size, 1) == SVGA3_VLKN_SUCCESS,
                   "Create sampled depth surface");
        ctx->setRenderTarget(SVGA3D_RT_COLOR0, 1, 0, 0);
        auto &dispatch = dev->backend->dispatch();
        savedCreateShaderModule = dispatch.vkCreateShaderModule;
        savedDestroyShaderModule = dispatch.vkDestroyShaderModule;
        dispatch.vkCreateShaderModule = captureShader;
        const uint32_t tokens[] = {0xFFFF0300, 0x0000FFFF};
        TEST_CHECK(ctx->defineShader(1, SVGA3D_SHADERTYPE_PS, tokens, 2) == SVGA3_VLKN_SUCCESS,
                   "Define base pixel shader");
        ctx->setShader(SVGA3D_SHADERTYPE_PS, 1);
        trackedShaderDestroys = variantFailures = 0;
        dispatch.vkDestroyShaderModule = countShaderDestroy;
        dispatch.vkCreateShaderModule = failDepthVariant;
        SVGA3dPrimitiveRange range{};
        range.primType = SVGA3D_PRIMITIVE_TRIANGLELIST;
        range.primitiveCount = 1;
        for (uint32_t stage = 0; stage < 2; ++stage) {
            ctx->setTexture(stage, 2);
            TEST_CHECK(ctx->draw(range.primType, nullptr, 0, &range, 1) == SVGA3_VLKN_SUCCESS,
                       "Draw with failed depth variant");
        }
        TEST_CHECK(variantFailures == 2, "Two distinct depth variants failed creation");
        ctx->endRenderPassIfActive();
        dispatch.vkCreateShaderModule = savedCreateShaderModule;
        if (cleanup == 0) {
            ctx->destroyShader(1, SVGA3D_SHADERTYPE_PS);
        } else if (cleanup == 1) {
            TEST_CHECK(ctx->defineShader(1, SVGA3D_SHADERTYPE_PS, tokens, 2) == SVGA3_VLKN_SUCCESS,
                       "Redefine shader after variant failure");
        } else {
            svga3_vlkn_context_destroy(dev, 1);
        }
        TEST_CHECK(trackedShaderDestroys == 1, "Base shader destroyed exactly once after variant failures");
        dispatch.vkDestroyShaderModule = savedDestroyShaderModule;
        svga3_vlkn_device_destroy(dev);
    }
}

static PFN_vkCreateGraphicsPipelines savedCreatePipelines;
static uint32_t capturedBlendAttachmentCount;
static unsigned pipelineCreateCalls;
static VkResult VKAPI_CALL capturePipelineCreate(VkDevice device, VkPipelineCache cache,
                                                 uint32_t createInfoCount,
                                                 const VkGraphicsPipelineCreateInfo *infos,
                                                 const VkAllocationCallbacks *allocator,
                                                 VkPipeline *pipelines) {
    ++pipelineCreateCalls;
    if (createInfoCount > 0 && infos && infos[0].pColorBlendState) {
        capturedBlendAttachmentCount = infos[0].pColorBlendState->attachmentCount;
    }
    return savedCreatePipelines(device, cache, createInfoCount, infos, allocator, pipelines);
}

static void TestDepthOnlyPipelineBlendState() {
    Svga3VlknConfig cfg{};
    cfg.forceMockBackend = true;
    auto *dev = svga3_vlkn_device_create(&cfg);
    TEST_CHECK(dev != nullptr, "Create depth-only pipeline regression device");
    if (!dev) return;
    TEST_CHECK(svga3_vlkn_context_create(dev, 1) == SVGA3_VLKN_SUCCESS, "Create depth-only pipeline context");
    auto *ctx = dev->contextMgr->getContext(1);
    TEST_CHECK(ctx != nullptr, "Find depth-only pipeline context");
    if (!ctx) { svga3_vlkn_device_destroy(dev); return; }
    SVGA3dSize size = {64, 64, 1};
    TEST_CHECK(svga3_vlkn_surface_define(dev, 1, 0, SVGA3D_A8R8G8B8, &size, 1) == SVGA3_VLKN_SUCCESS,
               "Create color target for pipeline test");
    TEST_CHECK(svga3_vlkn_surface_define(dev, 2, 0, SVGA3D_Z_D16, &size, 1) == SVGA3_VLKN_SUCCESS,
               "Create depth target for pipeline test");
    auto &dispatch = dev->backend->dispatch();
    savedCreatePipelines = dispatch.vkCreateGraphicsPipelines;
    dispatch.vkCreateGraphicsPipelines = capturePipelineCreate;

    SVGA3dPrimitiveRange range{};
    range.primType = SVGA3D_PRIMITIVE_TRIANGLELIST;
    range.primitiveCount = 1;

    /* Color + depth bound: the pipeline must declare one blend attachment. */
    ctx->setRenderTarget(SVGA3D_RT_COLOR0, 1, 0, 0);
    ctx->setRenderTarget(SVGA3D_RT_DEPTH, 2, 0, 0);
    pipelineCreateCalls = 0;
    capturedBlendAttachmentCount = 0xFFFFFFFFu;
    TEST_CHECK(ctx->draw(range.primType, nullptr, 0, &range, 1) == SVGA3_VLKN_SUCCESS,
               "Draw with color and depth targets");
    TEST_CHECK(pipelineCreateCalls >= 1, "Pipeline created for color+depth draw");
    TEST_CHECK(capturedBlendAttachmentCount == 1, "Color pipeline declares one blend attachment");

    /* Depth only: the render pass has no color attachment, so the pipeline
     * must declare zero blend attachments or creation is invalid. */
    ctx->setRenderTarget(SVGA3D_RT_COLOR0, SVGA3D_INVALID_ID, 0, 0);
    pipelineCreateCalls = 0;
    capturedBlendAttachmentCount = 0xFFFFFFFFu;
    TEST_CHECK(ctx->draw(range.primType, nullptr, 0, &range, 1) == SVGA3_VLKN_SUCCESS,
               "Draw with depth target only");
    TEST_CHECK(pipelineCreateCalls >= 1, "Pipeline created for depth-only draw");
    TEST_CHECK(capturedBlendAttachmentCount == 0, "Depth-only pipeline declares zero blend attachments");

    dispatch.vkCreateGraphicsPipelines = savedCreatePipelines;
    svga3_vlkn_device_destroy(dev);
}

/* --------------------------------------------------------------------------
 * Rendering-correctness regressions for the glmark2 validation gate.
 * Each test pins one proven root cause:
 *  - raster grid: guest D3D positions assume evaluation half a pixel off
 *    Vulkan's grid; the draw viewport must carry the (+0.5,+0.5) shift
 *    while the stored guest viewport stays unshifted.
 *  - blend: D3D9 alpha selection — with SEPARATEALPHABLENDENABLE off,
 *    alpha blends with the COLOR factors/equation and the *ALPHA states
 *    are ignored; with it on, the *ALPHA states rule. Guests driving
 *    non-separate blending only emit the *ALPHA states in separate
 *    mode, so honoring them unconditionally blended alpha with stale
 *    ONE/ZERO defaults (glmark2 desktop blur/shadow pixel failures).
 *  - mip render target: the framebuffer/render area must use the bound
 *    mip level's dimensions, not the base level's.
 *  - mip copy: surface copies must honor the command's mip/face selection.
 *  - constant ring: every draw uploads to a private ring slot; a draw
 *    binding the previous draw's slot was rewritten by the next epoch.
 *  - mip sampling: a render-generated mip chain must be reachable -- the
 *    sampled view expands to all levels and the sampler maxLod follows
 *    the image's level count, not the view's initial single level.
 * -------------------------------------------------------------------------- */
static PFN_vkCmdSetViewport savedCmdSetViewport;
static VkViewport capturedDrawViewport;
static unsigned setViewportCalls;
static void VKAPI_CALL captureCmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t count,
                                             const VkViewport *viewports) {
    if (count > 0 && viewports) { capturedDrawViewport = viewports[0]; ++setViewportCalls; }
    savedCmdSetViewport(cb, first, count, viewports);
}

static void TestRasterViewportCompensation() {
    Svga3VlknConfig cfg{};
    cfg.forceMockBackend = true;
    auto *dev = svga3_vlkn_device_create(&cfg);
    TEST_CHECK(dev != nullptr, "Create raster viewport regression device");
    if (!dev) return;
    TEST_CHECK(svga3_vlkn_context_create(dev, 1) == SVGA3_VLKN_SUCCESS, "Create raster viewport context");
    auto *ctx = dev->contextMgr->getContext(1);
    SVGA3dSize size = {64, 64, 1};
    TEST_CHECK(svga3_vlkn_surface_define(dev, 1, 0, SVGA3D_A8R8G8B8, &size, 1) == SVGA3_VLKN_SUCCESS,
               "Create raster viewport render target");
    ctx->setRenderTarget(SVGA3D_RT_COLOR0, 1, 0, 0);
    SVGA3dRect rect = {4, 8, 32, 16};
    TEST_CHECK(ctx->setViewport(&rect) == SVGA3_VLKN_SUCCESS, "Set guest viewport");

    /* The stored viewport keeps guest values (scissor/clear math depends
     * on them); only the rasterization viewport is shifted. */
    TEST_CHECK(ctx->getViewport().x == 4.0f && ctx->getViewport().y == 8.0f,
               "Stored viewport keeps guest coordinates");
    VkViewport rv = ctx->rasterViewport();
    TEST_CHECK(rv.x == 4.5f && rv.y == 8.5f && rv.width == 32.0f && rv.height == 16.0f,
               "Raster viewport shifted by (+0.5,+0.5)");

    auto &dispatch = dev->backend->dispatch();
    savedCmdSetViewport = dispatch.vkCmdSetViewport;
    dispatch.vkCmdSetViewport = captureCmdSetViewport;
    setViewportCalls = 0;
    SVGA3dPrimitiveRange range{};
    range.primType = SVGA3D_PRIMITIVE_TRIANGLELIST;
    range.primitiveCount = 1;
    TEST_CHECK(ctx->draw(range.primType, nullptr, 0, &range, 1) == SVGA3_VLKN_SUCCESS,
               "Draw for raster viewport capture");
    TEST_CHECK(setViewportCalls >= 1, "Draw sets a viewport");
    TEST_CHECK(capturedDrawViewport.x == 4.5f && capturedDrawViewport.y == 8.5f,
               "Draw binds the shifted raster viewport");
    dispatch.vkCmdSetViewport = savedCmdSetViewport;
    svga3_vlkn_device_destroy(dev);
}

static PFN_vkCreateGraphicsPipelines savedCreatePipelinesBlend;
static VkPipelineColorBlendAttachmentState capturedBlendAttach;
static bool capturedBlendValid;
static VkResult VKAPI_CALL capturePipelineBlend(VkDevice device, VkPipelineCache cache,
                                                uint32_t createInfoCount,
                                                const VkGraphicsPipelineCreateInfo *infos,
                                                const VkAllocationCallbacks *allocator,
                                                VkPipeline *pipelines) {
    if (createInfoCount > 0 && infos && infos[0].pColorBlendState &&
        infos[0].pColorBlendState->attachmentCount > 0 && infos[0].pColorBlendState->pAttachments) {
        capturedBlendAttach = infos[0].pColorBlendState->pAttachments[0];
        capturedBlendValid = true;
    }
    return savedCreatePipelinesBlend(device, cache, createInfoCount, infos, allocator, pipelines);
}

static void TestSeparateAlphaBlendFactors() {
    Svga3VlknConfig cfg{};
    cfg.forceMockBackend = true;
    auto *dev = svga3_vlkn_device_create(&cfg);
    TEST_CHECK(dev != nullptr, "Create blend regression device");
    if (!dev) return;
    TEST_CHECK(svga3_vlkn_context_create(dev, 1) == SVGA3_VLKN_SUCCESS, "Create blend context");
    auto *ctx = dev->contextMgr->getContext(1);
    SVGA3dSize size = {64, 64, 1};
    TEST_CHECK(svga3_vlkn_surface_define(dev, 1, 0, SVGA3D_A8R8G8B8, &size, 1) == SVGA3_VLKN_SUCCESS,
               "Create blend render target");
    ctx->setRenderTarget(SVGA3D_RT_COLOR0, 1, 0, 0);
    ctx->setRenderState(SVGA3D_RS_BLENDENABLE, 1);
    ctx->setRenderState(SVGA3D_RS_SRCBLEND, SVGA3D_BLENDOP_SRCALPHA);
    ctx->setRenderState(SVGA3D_RS_DSTBLEND, SVGA3D_BLENDOP_INVSRCALPHA);
    ctx->setRenderState(SVGA3D_RS_BLENDEQUATION, SVGA3D_BLENDEQ_ADD);
    /* Distinct alpha states, deliberately different from the color
     * states: with separate alpha blending disabled, D3D9 selects the
     * COLOR factors/equation for the alpha channel and the *ALPHA
     * states are ignored. */
    ctx->setRenderState(SVGA3D_RS_SRCBLENDALPHA, SVGA3D_BLENDOP_ONE);
    ctx->setRenderState(SVGA3D_RS_DSTBLENDALPHA, SVGA3D_BLENDOP_ZERO);
    ctx->setRenderState(SVGA3D_RS_BLENDEQUATIONALPHA, SVGA3D_BLENDEQ_SUBTRACT);
    ctx->setRenderState(SVGA3D_RS_SEPARATEALPHABLENDENABLE, 0);

    auto &dispatch = dev->backend->dispatch();
    savedCreatePipelinesBlend = dispatch.vkCreateGraphicsPipelines;
    dispatch.vkCreateGraphicsPipelines = capturePipelineBlend;
    SVGA3dPrimitiveRange range{};
    range.primType = SVGA3D_PRIMITIVE_TRIANGLELIST;
    range.primitiveCount = 1;

    capturedBlendValid = false;
    TEST_CHECK(ctx->draw(range.primType, nullptr, 0, &range, 1) == SVGA3_VLKN_SUCCESS,
               "Draw with separate alpha disabled");
    TEST_CHECK(capturedBlendValid, "Pipeline captured for separate-alpha-off draw");
    TEST_CHECK(capturedBlendAttach.srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA &&
               capturedBlendAttach.dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA &&
               capturedBlendAttach.colorBlendOp == VK_BLEND_OP_ADD,
               "Color blend factors as set");
    TEST_CHECK(capturedBlendAttach.srcAlphaBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA &&
               capturedBlendAttach.dstAlphaBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA &&
               capturedBlendAttach.alphaBlendOp == VK_BLEND_OP_ADD,
               "Alpha mirrors the color factors when separate alpha is disabled");

    /* Enable separate alpha blending: now the *ALPHA states rule, so
     * the pipeline key changes and a new pipeline is created. */
    ctx->setRenderState(SVGA3D_RS_SRCBLENDALPHA, SVGA3D_BLENDOP_ONE);
    ctx->setRenderState(SVGA3D_RS_DSTBLENDALPHA, SVGA3D_BLENDOP_ZERO);
    ctx->setRenderState(SVGA3D_RS_BLENDEQUATIONALPHA, SVGA3D_BLENDEQ_ADD);
    ctx->setRenderState(SVGA3D_RS_SEPARATEALPHABLENDENABLE, 1);
    capturedBlendValid = false;
    TEST_CHECK(ctx->draw(range.primType, nullptr, 0, &range, 1) == SVGA3_VLKN_SUCCESS,
               "Draw with separate alpha enabled");
    TEST_CHECK(capturedBlendValid, "Pipeline captured for separate-alpha-on draw");
    TEST_CHECK(capturedBlendAttach.srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE &&
               capturedBlendAttach.dstAlphaBlendFactor == VK_BLEND_FACTOR_ZERO &&
               capturedBlendAttach.alphaBlendOp == VK_BLEND_OP_ADD,
               "Alpha uses its own factors when separate alpha is enabled");
    dispatch.vkCreateGraphicsPipelines = savedCreatePipelinesBlend;
    svga3_vlkn_device_destroy(dev);
}

static PFN_vkCmdBeginRenderPass savedCmdBeginRenderPass;
static VkRect2D capturedRenderArea;
static unsigned beginRenderPassCalls;
static void VKAPI_CALL captureCmdBeginRenderPass(VkCommandBuffer cb,
                                                 const VkRenderPassBeginInfo *info,
                                                 VkSubpassContents contents) {
    if (info) { capturedRenderArea = info->renderArea; ++beginRenderPassCalls; }
    savedCmdBeginRenderPass(cb, info, contents);
}

static void TestNonzeroMipRenderTarget() {
    Svga3VlknConfig cfg{};
    cfg.forceMockBackend = true;
    auto *dev = svga3_vlkn_device_create(&cfg);
    TEST_CHECK(dev != nullptr, "Create mip render target regression device");
    if (!dev) return;
    TEST_CHECK(svga3_vlkn_context_create(dev, 1) == SVGA3_VLKN_SUCCESS, "Create mip RT context");
    auto *ctx = dev->contextMgr->getContext(1);
    SVGA3dSize sizes[2] = {{64, 64, 1}, {32, 32, 1}};
    TEST_CHECK(svga3_vlkn_surface_define(dev, 7, 0, SVGA3D_A8R8G8B8, sizes, 2) == SVGA3_VLKN_SUCCESS,
               "Define two-level surface");
    auto &dispatch = dev->backend->dispatch();
    savedCmdBeginRenderPass = dispatch.vkCmdBeginRenderPass;
    dispatch.vkCmdBeginRenderPass = captureCmdBeginRenderPass;
    SVGA3dPrimitiveRange range{};
    range.primType = SVGA3D_PRIMITIVE_TRIANGLELIST;
    range.primitiveCount = 1;

    ctx->setRenderTarget(SVGA3D_RT_COLOR0, 7, 0, 0);
    beginRenderPassCalls = 0;
    TEST_CHECK(ctx->draw(range.primType, nullptr, 0, &range, 1) == SVGA3_VLKN_SUCCESS,
               "Draw to mip level 0");
    TEST_CHECK(beginRenderPassCalls >= 1 &&
               capturedRenderArea.extent.width == 64 && capturedRenderArea.extent.height == 64,
               "Mip 0 render area is 64x64");
    ctx->endRenderPassIfActive();

    ctx->setRenderTarget(SVGA3D_RT_COLOR0, 7, 0, 1);
    beginRenderPassCalls = 0;
    TEST_CHECK(ctx->draw(range.primType, nullptr, 0, &range, 1) == SVGA3_VLKN_SUCCESS,
               "Draw to mip level 1");
    TEST_CHECK(beginRenderPassCalls >= 1 &&
               capturedRenderArea.extent.width == 32 && capturedRenderArea.extent.height == 32,
               "Mip 1 render area is 32x32, not the base level size");
    dispatch.vkCmdBeginRenderPass = savedCmdBeginRenderPass;
    svga3_vlkn_device_destroy(dev);
}

static PFN_vkCmdCopyImage savedCmdCopyImage;
static uint32_t capturedCopySrcMip, capturedCopyDstMip;
static unsigned copyImageCalls;
static void VKAPI_CALL captureCmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout srcLayout,
                                           VkImage dst, VkImageLayout dstLayout,
                                           uint32_t regionCount, const VkImageCopy *regions) {
    if (regionCount > 0 && regions) {
        capturedCopySrcMip = regions[0].srcSubresource.mipLevel;
        capturedCopyDstMip = regions[0].dstSubresource.mipLevel;
        ++copyImageCalls;
    }
    savedCmdCopyImage(cb, src, srcLayout, dst, dstLayout, regionCount, regions);
}

static void TestMipLevelCopy() {
    Svga3VlknConfig cfg{};
    cfg.forceMockBackend = true;
    auto *dev = svga3_vlkn_device_create(&cfg);
    TEST_CHECK(dev != nullptr, "Create mip copy regression device");
    if (!dev) return;
    SVGA3dSize sizes[4] = {{64, 64, 1}, {32, 32, 1}, {16, 16, 1}, {8, 8, 1}};
    TEST_CHECK(svga3_vlkn_surface_define(dev, 10, 0, SVGA3D_A8R8G8B8, sizes, 4) == SVGA3_VLKN_SUCCESS,
               "Define four-level copy source");
    TEST_CHECK(svga3_vlkn_surface_define(dev, 11, 0, SVGA3D_A8R8G8B8, sizes, 4) == SVGA3_VLKN_SUCCESS,
               "Define four-level copy destination");
    auto &dispatch = dev->backend->dispatch();
    savedCmdCopyImage = dispatch.vkCmdCopyImage;
    dispatch.vkCmdCopyImage = captureCmdCopyImage;
    SVGA3dCopyBox box{};
    box.x = 0; box.y = 0; box.z = 0; box.w = 8; box.h = 8; box.d = 1;
    box.srcx = 0; box.srcy = 0; box.srcz = 0;

    copyImageCalls = 0;
    TEST_CHECK(dev->surfaceMgr->copy(10, 11, &box, 1, 2, 0, 3, 0) == SVGA3_VLKN_SUCCESS,
               "Copy mip 2 to mip 3 succeeds");
    TEST_CHECK(copyImageCalls >= 1 && capturedCopySrcMip == 2 && capturedCopyDstMip == 3,
               "Copy honors the command's mip levels");

    copyImageCalls = 0;
    TEST_CHECK(dev->surfaceMgr->copy(10, 11, &box, 1, 0, 0, 0, 0) == SVGA3_VLKN_SUCCESS,
               "Copy mip 0 to mip 0 succeeds");
    TEST_CHECK(copyImageCalls >= 1 && capturedCopySrcMip == 0 && capturedCopyDstMip == 0,
               "Base-level copy still targets level 0");
    dispatch.vkCmdCopyImage = savedCmdCopyImage;
    svga3_vlkn_device_destroy(dev);
}

static PFN_vkCmdBindDescriptorSets savedCmdBindDescriptorSets;
static uint32_t capturedDynOffsets[2];
static unsigned bindDescriptorCalls;
static void VKAPI_CALL captureCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bindPoint,
                                                    VkPipelineLayout layout, uint32_t firstSet,
                                                    uint32_t setCount, const VkDescriptorSet *sets,
                                                    uint32_t dynamicOffsetCount,
                                                    const uint32_t *dynamicOffsets) {
    if (dynamicOffsetCount >= 2 && dynamicOffsets) {
        capturedDynOffsets[0] = dynamicOffsets[0];
        capturedDynOffsets[1] = dynamicOffsets[1];
        ++bindDescriptorCalls;
    }
    savedCmdBindDescriptorSets(cb, bindPoint, layout, firstSet, setCount, sets,
                               dynamicOffsetCount, dynamicOffsets);
}

static void TestConstantRingPerDrawSlot() {
    Svga3VlknConfig cfg{};
    cfg.forceMockBackend = true;
    auto *dev = svga3_vlkn_device_create(&cfg);
    TEST_CHECK(dev != nullptr, "Create constant ring regression device");
    if (!dev) return;
    TEST_CHECK(svga3_vlkn_context_create(dev, 1) == SVGA3_VLKN_SUCCESS, "Create constant ring context");
    auto *ctx = dev->contextMgr->getContext(1);
    SVGA3dSize size = {64, 64, 1};
    TEST_CHECK(svga3_vlkn_surface_define(dev, 1, 0, SVGA3D_A8R8G8B8, &size, 1) == SVGA3_VLKN_SUCCESS,
               "Create constant ring render target");
    ctx->setRenderTarget(SVGA3D_RT_COLOR0, 1, 0, 0);
    auto &dispatch = dev->backend->dispatch();
    savedCmdBindDescriptorSets = dispatch.vkCmdBindDescriptorSets;
    dispatch.vkCmdBindDescriptorSets = captureCmdBindDescriptorSets;
    SVGA3dPrimitiveRange range{};
    range.primType = SVGA3D_PRIMITIVE_TRIANGLELIST;
    range.primitiveCount = 1;

    /* Constants never change and nothing is submitted between the draws:
     * the exact pattern that aliased when upload was dirty-only. Every
     * draw must still bind its own ring slot. */
    uint32_t vsOffsets[3] = {0, 0, 0};
    uint32_t psOffsets[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        bindDescriptorCalls = 0;
        TEST_CHECK(ctx->draw(range.primType, nullptr, 0, &range, 1) == SVGA3_VLKN_SUCCESS,
                   "Draw for constant ring slot capture");
        TEST_CHECK(bindDescriptorCalls >= 1, "Draw binds descriptor set with dynamic offsets");
        vsOffsets[i] = capturedDynOffsets[0];
        psOffsets[i] = capturedDynOffsets[1];
    }
    TEST_CHECK(vsOffsets[0] != vsOffsets[1] && vsOffsets[1] != vsOffsets[2] &&
               vsOffsets[0] != vsOffsets[2],
               "Each draw gets a private VS constant ring slot");
    TEST_CHECK(psOffsets[0] != psOffsets[1] && psOffsets[1] != psOffsets[2] &&
               psOffsets[0] != psOffsets[2],
               "Each draw gets a private PS constant ring slot");
    TEST_CHECK(vsOffsets[1] > vsOffsets[0] && vsOffsets[2] > vsOffsets[1],
               "Ring slots advance monotonically within an epoch");
    dispatch.vkCmdBindDescriptorSets = savedCmdBindDescriptorSets;
    svga3_vlkn_device_destroy(dev);
}

static PFN_vkCreateSampler savedCreateSampler;
static float capturedSamplerMaxLod;
static unsigned createSamplerCalls;
static VkResult VKAPI_CALL captureCreateSampler(VkDevice device, const VkSamplerCreateInfo *info,
                                                const VkAllocationCallbacks *allocator,
                                                VkSampler *sampler) {
    if (info) { capturedSamplerMaxLod = info->maxLod; ++createSamplerCalls; }
    return savedCreateSampler(device, info, allocator, sampler);
}
static PFN_vkCreateImageView savedCreateImageView;
static uint32_t capturedMaxViewLevels;
static VkResult VKAPI_CALL captureCreateImageView(VkDevice device, const VkImageViewCreateInfo *info,
                                                  const VkAllocationCallbacks *allocator,
                                                  VkImageView *view) {
    if (info && info->subresourceRange.levelCount > capturedMaxViewLevels) {
        capturedMaxViewLevels = info->subresourceRange.levelCount;
    }
    return savedCreateImageView(device, info, allocator, view);
}

static void TestMipSampledViewAndSamplerLod() {
    Svga3VlknConfig cfg{};
    cfg.forceMockBackend = true;
    auto *dev = svga3_vlkn_device_create(&cfg);
    TEST_CHECK(dev != nullptr, "Create mip sampling regression device");
    if (!dev) return;
    TEST_CHECK(svga3_vlkn_context_create(dev, 1) == SVGA3_VLKN_SUCCESS, "Create mip sampling context");
    auto *ctx = dev->contextMgr->getContext(1);
    SVGA3dSize rtSize = {64, 64, 1};
    TEST_CHECK(svga3_vlkn_surface_define(dev, 1, 0, SVGA3D_A8R8G8B8, &rtSize, 1) == SVGA3_VLKN_SUCCESS,
               "Create mip sampling render target");
    SVGA3dSize sizes[4] = {{64, 64, 1}, {32, 32, 1}, {16, 16, 1}, {8, 8, 1}};
    TEST_CHECK(svga3_vlkn_surface_define(dev, 5, 0, SVGA3D_A8R8G8B8, sizes, 4) == SVGA3_VLKN_SUCCESS,
               "Define four-level texture");
    auto *texSurf = dev->surfaceMgr->getSurface(5);
    TEST_CHECK(texSurf && texSurf->mipLevels() == 4, "Texture reports four mip levels");
    /* Only level 0 was ever uploaded; the chain is render-generated, so
     * the sampled view starts at a single level (the bug condition). */
    TEST_CHECK(texSurf && texSurf->viewMipLevels() == 1,
               "Sampled view starts at one level before expansion");
    ctx->setRenderTarget(SVGA3D_RT_COLOR0, 1, 0, 0);
    ctx->setTexture(0, 5);
    ctx->setTextureStageState(0, SVGA3D_TS_MINFILTER, SVGA3D_TEX_FILTER_LINEAR);
    ctx->setTextureStageState(0, SVGA3D_TS_MAGFILTER, SVGA3D_TEX_FILTER_LINEAR);
    ctx->setTextureStageState(0, SVGA3D_TS_MIPFILTER, SVGA3D_TEX_FILTER_LINEAR);

    auto &dispatch = dev->backend->dispatch();
    savedCreateSampler = dispatch.vkCreateSampler;
    savedCreateImageView = dispatch.vkCreateImageView;
    dispatch.vkCreateSampler = captureCreateSampler;
    dispatch.vkCreateImageView = captureCreateImageView;
    createSamplerCalls = 0;
    capturedSamplerMaxLod = -1.0f;
    capturedMaxViewLevels = 0;
    SVGA3dPrimitiveRange range{};
    range.primType = SVGA3D_PRIMITIVE_TRIANGLELIST;
    range.primitiveCount = 1;
    TEST_CHECK(ctx->draw(range.primType, nullptr, 0, &range, 1) == SVGA3_VLKN_SUCCESS,
               "Draw sampling the mipmapped texture");
    TEST_CHECK(createSamplerCalls >= 1, "Sampler created for the sampling draw");
    TEST_CHECK(capturedSamplerMaxLod == 3.0f,
               "Sampler maxLod covers all four levels despite the single-level start");
    TEST_CHECK(capturedMaxViewLevels == 4,
               "Sampled view expanded to all four levels for the draw");
    TEST_CHECK(texSurf->viewMipLevels() == 4, "Surface view level count updated by expansion");
    dispatch.vkCreateSampler = savedCreateSampler;
    dispatch.vkCreateImageView = savedCreateImageView;
    svga3_vlkn_device_destroy(dev);
}

static VkResult VKAPI_CALL failCreateImageView(VkDevice, const VkImageViewCreateInfo *,
                                               const VkAllocationCallbacks *, VkImageView *view) {
    if (view) *view = VK_NULL_HANDLE;
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

/* Review follow-up: view expansion used to destroy the old view before
 * creating the wider one and ignored the creation result, so a failed
 * vkCreateImageView left the surface with a dead view and a level count
 * claiming levels it did not cover. Expansion must now fail atomically:
 * status reported, previous view and level count preserved. */
static void TestMipViewExpansionFailureKeepsOldView() {
    Svga3VlknConfig cfg{};
    cfg.forceMockBackend = true;
    auto *dev = svga3_vlkn_device_create(&cfg);
    TEST_CHECK(dev != nullptr, "Create mip expansion failure device");
    if (!dev) return;
    SVGA3dSize sizes[4] = {{64, 64, 1}, {32, 32, 1}, {16, 16, 1}, {8, 8, 1}};
    TEST_CHECK(svga3_vlkn_surface_define(dev, 5, 0, SVGA3D_A8R8G8B8, sizes, 4) == SVGA3_VLKN_SUCCESS,
               "Define four-level texture for expansion failure");
    auto *texSurf = dev->surfaceMgr->getSurface(5);
    TEST_CHECK(texSurf && texSurf->viewMipLevels() == 1, "View starts at one level");
    VkImageView originalView = texSurf ? texSurf->imageView() : VK_NULL_HANDLE;
    TEST_CHECK(originalView != VK_NULL_HANDLE, "Original sampled view exists");

    auto &dispatch = dev->backend->dispatch();
    savedCreateImageView = dispatch.vkCreateImageView;
    dispatch.vkCreateImageView = failCreateImageView;
    bool expanded = texSurf->ensureViewMipLevels(4);
    dispatch.vkCreateImageView = savedCreateImageView;
    TEST_CHECK(!expanded, "Failed view creation reports expansion failure");
    TEST_CHECK(texSurf->imageView() == originalView,
               "Failed expansion keeps the previous view");
    TEST_CHECK(texSurf->viewMipLevels() == 1,
               "Failed expansion keeps the previous level count");
    TEST_CHECK(texSurf->ensureViewMipLevels(4), "Expansion succeeds once creation recovers");
    TEST_CHECK(texSurf->viewMipLevels() == 4, "Recovered expansion reaches all four levels");
    svga3_vlkn_device_destroy(dev);
}

/* --------------------------------------------------------------------------
 * Main Test Runner
 * -------------------------------------------------------------------------- */
int main() {
    std::cout << ANSI_YELLOW << "==================================================================" << ANSI_RESET << std::endl;
    std::cout << ANSI_YELLOW << "       SVGA3=VLKN - SVGA3D to Vulkan Engine Test Harness          " << ANSI_RESET << std::endl;
    std::cout << ANSI_YELLOW << "==================================================================" << ANSI_RESET << std::endl;

    TestFenceWithoutWindow();
    TestFlushPropagatesWaitFailures();
    TestSurfaceTransfersPropagateFlushFailures();
    TestRenderPassAndGrowthFlushFailuresPropagate();
    TestPendingWindowPresentsOnlyWhenDirty();
    TestPresentAlwaysCopies();
    TestDepthVariantCleanup();
    TestDepthOnlyPipelineBlendState();
    TestRasterViewportCompensation();
    TestSeparateAlphaBlendFactors();
    TestNonzeroMipRenderTarget();
    TestMipLevelCopy();
    TestConstantRingPerDrawSlot();
    TestMipSampledViewAndSamplerLod();
    TestMipViewExpansionFailureKeepsOldView();
    TestRenderPassEndingBatchesCommands();
    TestSurfaceFormatMappings();
    TestTopologyMappings();
    TestStateConverters();
    TestDeviceLifecycleAndCaps();

    /* Create persistent device for comprehensive pipeline tests */
    Svga3VlknConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.appName = "SVGA3=VLKN Suite";
    cfg.forceMockBackend = true;
    cfg.stagingBufferSize = 16 * 1024 * 1024;
    Svga3VlknDevice *dev = svga3_vlkn_device_create(&cfg);
    assert(dev != nullptr);

    TestSurfaceLifecycle(dev);
    TestSurfaceDMA(dev);
    TestSurfaceCopy(dev);
    TestContextAndRenderStates(dev);
    TestRenderTargetAndViewport(dev);
    TestClearOperations(dev);
    TestDrawPrimitives(dev);
    TestFifoExecution(dev);
    TestTransformsAndZRange(dev);
    TestMaterialsAndLights(dev);
    TestClipPlanes(dev);
    TestTextureStageStates(dev);
    TestRenderStatesExhaustive(dev);
    TestShadersAndConstants(dev);
    TestHardwareQueries(dev);
    TestAdvancedSurfaceOperations(dev);
    TestMultiContextIsolation(dev);
    TestStressExecution(dev);

    /* Print final execution statistics */
    Svga3VlknStats stats;
    svga3_vlkn_get_stats(dev, &stats);
    std::cout << ANSI_CYAN << "\nEngine Execution Statistics Summary:" << ANSI_RESET << std::endl;
    std::cout << "  - Total Commands Processed: " << stats.totalCommandsProcessed << std::endl;
    std::cout << "  - Draw Calls Submitted:     " << stats.drawCallsSubmitted << std::endl;
    std::cout << "  - Primitives Rendered:      " << stats.primitivesRendered << std::endl;
    std::cout << "  - Vertices Rendered:        " << stats.verticesRendered << std::endl;
    std::cout << "  - Surfaces Created:         " << stats.surfacesCreated << std::endl;
    std::cout << "  - Surfaces Destroyed:       " << stats.surfacesDestroyed << std::endl;
    std::cout << "  - Render Passes Executed:   " << stats.renderPassesExecuted << std::endl;
    std::cout << "  - DMA Bytes Transferred:    " << stats.dmaBytesTransferred << " bytes" << std::endl;

    svga3_vlkn_device_destroy(dev);

    std::cout << ANSI_YELLOW << "\n==================================================================" << ANSI_RESET << std::endl;
    if (g_testsFailed == 0) {
        std::cout << ANSI_GREEN << " ALL " << g_testsPassed << " TESTS PASSED SUCCESSFULLY! (100% PASS RATE)" << ANSI_RESET << std::endl;
        std::cout << ANSI_YELLOW << "==================================================================" << ANSI_RESET << std::endl;
        return 0;
    } else {
        std::cout << ANSI_RED << " " << g_testsFailed << " TESTS FAILED out of " << (g_testsPassed + g_testsFailed) << ANSI_RESET << std::endl;
        std::cout << ANSI_YELLOW << "==================================================================" << ANSI_RESET << std::endl;
        return 1;
    }
}
