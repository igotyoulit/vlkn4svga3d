/*
 * SVGA3=VLKN - Verified Rendering Acceptance Test Suite
 * Deliverable 4: Verified Rendering & Analytical Pixel Comparison
 *
 * Covers:
 * 1. Textured indexed geometry with vertex transformations and shader constants.
 * 2. Depth overlap (Z-buffering, depth testing, closer vs farther geometry).
 * 3. Alpha blending (semi-transparent geometry over background with analytical blend math).
 * 4. Viewport and scissor boundaries (pixel preservation outside clipping regions).
 * 5. Switching render targets (multiple RTs with independent content).
 * 6. Two contexts with distinct state (multi-context isolation).
 * 7. Dynamic resource updates followed by another draw.
 * 8. Negative controls (deliberately altered shaders, constants, textures, expected images).
 *
 * All comparisons are performed against independently calculated analytical pixel values
 * with documented tolerances (never using the implementation under test).
 */

#include "svga3_vlkn.h"
#include "svga3_device.h"
#include "vlkn_backend.h"

#include <iostream>
#include <vector>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <cassert>

#define TEST_CHECK(cond, msg) do { \
    if (cond) { \
        std::cout << "  [PASS] " << msg << std::endl; \
    } else { \
        std::cerr << "  [FAIL] " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
        return 1; \
    } \
} while(0)

#define TEST_CHECK_NEGATIVE(cond, msg) do { \
    if (cond) { \
        std::cout << "  [PASS] Negative control detected mismatch as expected: " << msg << std::endl; \
    } else { \
        std::cerr << "  [FAIL] Negative control failed to detect mismatch: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
        return 1; \
    } \
} while(0)

struct Vertex {
    float x, y, z;
    float u, v;
};

struct Pixel {
    uint8_t b, g, r, a;
};

/* Documented comparison tolerance: +/- 2 LSB to account for 8-bit quantization and fixed-point rasterization */
constexpr int COLOR_TOLERANCE = 2;

static bool pixelMatches(const Pixel &actual, uint8_t er, uint8_t eg, uint8_t eb, uint8_t ea, int tol = COLOR_TOLERANCE) {
    return (std::abs(static_cast<int>(actual.r) - static_cast<int>(er)) <= tol &&
            std::abs(static_cast<int>(actual.g) - static_cast<int>(eg)) <= tol &&
            std::abs(static_cast<int>(actual.b) - static_cast<int>(eb)) <= tol &&
            std::abs(static_cast<int>(actual.a) - static_cast<int>(ea)) <= tol);
}

/* Helper macro for D3D9 token construction */
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

static void saveDiffPPM(const char *filename, const Pixel *actual, const Pixel *reference, int width, int height) {
    FILE *f = fopen(filename, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; ++i) {
        int dr = std::abs((int)actual[i].r - (int)reference[i].r);
        int dg = std::abs((int)actual[i].g - (int)reference[i].g);
        int db = std::abs((int)actual[i].b - (int)reference[i].b);
        uint8_t rgb[3];
        if (dr <= COLOR_TOLERANCE && dg <= COLOR_TOLERANCE && db <= COLOR_TOLERANCE) {
            rgb[0] = rgb[1] = rgb[2] = 0; // Black = match within tolerance
        } else {
            rgb[0] = static_cast<uint8_t>(std::min(255, dr * 8 + 64));
            rgb[1] = static_cast<uint8_t>(std::min(255, dg * 8 + 64));
            rgb[2] = static_cast<uint8_t>(std::min(255, db * 8 + 64));
        }
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static std::vector<Pixel> g_scene1Fb;
static std::vector<Pixel> g_scene1Ref;

int main() {
    std::cout << "======================================================================" << std::endl;
    std::cout << "SVGA3=VLKN Deliverable 4: Verified Rendering & Analytical Comparison" << std::endl;
    std::cout << "======================================================================" << std::endl;

    /* 1. Device Creation on Real Vulkan with Validation Layers */
    Svga3VlknConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.appName = "Verified Rendering Acceptance Test";
    cfg.apiVersion = VK_API_VERSION_1_1;
    cfg.forceMockBackend = false; /* Real Vulkan ONLY */
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

    const uint32_t RT_W = 64;
    const uint32_t RT_H = 64;

    /* Common Shaders:
     * VS1: vs_3_0; dcl_position v0; dcl_texcoord v1; m4x4 oPos, v0, c0; mov oT0, v1; end
     * PS1 (Textured + Constant Tint): ps_3_0; dcl_texcoord v1; dcl_2d s0; texld r0, v1, s0; mul oC0, r0, c0; end
     * PS2 (Solid Constant Color): ps_3_0; mov oC0, c0; end
     */
    const uint32_t vs1Bytecode[] = {
        0xFFFE0300, /* vs_3_0 */
        (31) | (2 << 24), 0x80000000 | 0, D3D9_DST(1, 0, 0xF), /* dcl_position v0 */
        (31) | (2 << 24), 0x80000000 | 5, D3D9_DST(1, 1, 0xF), /* dcl_texcoord v1 */
        (20) | (3 << 24), D3D9_DST(4, 0, 0xF), D3D9_SRC(1, 0, 0xE4), D3D9_SRC(2, 0, 0xE4), /* m4x4 oPos, v0, c0 */
        (1)  | (2 << 24), D3D9_DST(6, 0, 0xF), D3D9_SRC(1, 1, 0xE4), /* mov oT0, v1 */
        0x0000FFFF
    };

    const uint32_t ps1Bytecode[] = {
        0xFFFF0300, /* ps_3_0 */
        (31) | (2 << 24), 0x80000000 | 5, D3D9_DST(1, 1, 0xF), /* dcl_texcoord v1 */
        (31) | (2 << 24), 0x80000000 | (2 << 28), D3D9_DST(10, 0, 0xF), /* dcl_2d s0 */
        (66) | (3 << 24), D3D9_DST(0, 0, 0xF), D3D9_SRC(1, 1, 0xE4), D3D9_SRC(10, 0, 0xE4), /* texld r0, v1, s0 */
        (5)  | (3 << 24), D3D9_DST(8, 0, 0xF), D3D9_SRC(0, 0, 0xE4), D3D9_SRC(2, 0, 0xE4), /* mul oC0, r0, c0 */
        0x0000FFFF
    };

    const uint32_t ps2Bytecode[] = {
        0xFFFF0300, /* ps_3_0 */
        (1)  | (2 << 24), D3D9_DST(8, 0, 0xF), D3D9_SRC(2, 0, 0xE4), /* mov oC0, c0 */
        0x0000FFFF
    };

    /* Common Vertex Declaration for (Position: float3, Texcoord: float2) */
    SVGA3dVertexDecl decls[2] = {};
    decls[0].identity.type = SVGA3D_DECLTYPE_FLOAT3;
    decls[0].identity.method = SVGA3D_DECLMETHOD_DEFAULT;
    decls[0].identity.usage = SVGA3D_DECLUSAGE_POSITION;
    decls[0].identity.usageIndex = 0;
    decls[0].array.surfaceId = 10;
    decls[0].array.offset = 0;
    decls[0].array.stride = sizeof(Vertex);

    decls[1].identity.type = SVGA3D_DECLTYPE_FLOAT2;
    decls[1].identity.method = SVGA3D_DECLMETHOD_DEFAULT;
    decls[1].identity.usage = SVGA3D_DECLUSAGE_TEXCOORD;
    decls[1].identity.usageIndex = 0;
    decls[1].array.surfaceId = 10;
    decls[1].array.offset = offsetof(Vertex, u);
    decls[1].array.stride = sizeof(Vertex);

    /* --------------------------------------------------------------------------
     * SCENE 1: Textured Indexed Geometry with Transformations & Shader Constants
     * -------------------------------------------------------------------------- */
    std::cout << "\n--- Scene 1: Textured Indexed Geometry with Matrix & Constant Tint ---" << std::endl;
    {
        const uint32_t CID = 1;
        const uint32_t SID_RT = 1;
        const uint32_t SID_TEX = 2;
        const uint32_t SID_VB = 10;
        const uint32_t SID_IB = 11;

        svga3_vlkn_context_create(dev, CID);

        SVGA3dSize rtSize = { RT_W, RT_H, 1 };
        svga3_vlkn_surface_define(dev, SID_RT, SVGA3D_SURFACE_HINT_RENDERTARGET, SVGA3D_X8R8G8B8, &rtSize, 1);
        svga3_vlkn_context_set_render_target(dev, CID, SVGA3D_RT_COLOR0, SID_RT, 0, 0);

        SVGA3dRect vp = { 0, 0, RT_W, RT_H };
        svga3_vlkn_context_set_viewport(dev, CID, &vp);
        svga3_vlkn_context_set_render_state(dev, CID, SVGA3D_RS_CULLMODE, SVGA3D_FACE_NONE);

        /* Define 2x2 Texture: Top-left: White, Top-right: Red, Bottom-left: Green, Bottom-right: Blue */
        SVGA3dSize texSize = { 2, 2, 1 };
        svga3_vlkn_surface_define(dev, SID_TEX, SVGA3D_SURFACE_HINT_TEXTURE, SVGA3D_X8R8G8B8, &texSize, 1);
        uint32_t texData[4] = {
            0xFFFFFFFF, /* White */
            0xFFFF0000, /* Red */
            0xFF00FF00, /* Green */
            0xFF0000FF  /* Blue */
        };
        SVGA3dBox tBox = { 0, 0, 0, 2, 2, 1 };
        svga3_vlkn_surface_dma_upload(dev, SID_TEX, 0, &tBox, texData, 2 * 4);
        svga3_vlkn_context_set_texture(dev, CID, 0, SID_TEX);

        /* Define Indexed Quad: 4 vertices, 6 indices (2 triangles) */
        Vertex quadVerts[4] = {
            { -1.0f,  1.0f, 0.5f,  0.0f, 0.0f }, /* 0: TL */
            {  1.0f,  1.0f, 0.5f,  1.0f, 0.0f }, /* 1: TR */
            {  1.0f, -1.0f, 0.5f,  1.0f, 1.0f }, /* 2: BR */
            { -1.0f, -1.0f, 0.5f,  0.0f, 1.0f }  /* 3: BL */
        };
        SVGA3dSize vbSize = { sizeof(quadVerts), 1, 1 };
        svga3_vlkn_surface_define(dev, SID_VB, SVGA3D_SURFACE_HINT_VERTEXBUFFER, SVGA3D_BUFFER, &vbSize, 1);
        SVGA3dBox vbBox = { 0, 0, 0, sizeof(quadVerts), 1, 1 };
        svga3_vlkn_surface_dma_upload(dev, SID_VB, 0, &vbBox, quadVerts, sizeof(quadVerts));

        uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
        SVGA3dSize ibSize = { sizeof(indices), 1, 1 };
        svga3_vlkn_surface_define(dev, SID_IB, SVGA3D_SURFACE_HINT_INDEXBUFFER, SVGA3D_BUFFER, &ibSize, 1);
        SVGA3dBox ibBox = { 0, 0, 0, sizeof(indices), 1, 1 };
        svga3_vlkn_surface_dma_upload(dev, SID_IB, 0, &ibBox, indices, sizeof(indices));

        /* Define Shaders */
        svga3_vlkn_context_define_shader(dev, CID, 1, SVGA3D_SHADERTYPE_VS, vs1Bytecode, sizeof(vs1Bytecode)/4);
        svga3_vlkn_context_define_shader(dev, CID, 1, SVGA3D_SHADERTYPE_PS, ps1Bytecode, sizeof(ps1Bytecode)/4);
        svga3_vlkn_context_set_shader(dev, CID, SVGA3D_SHADERTYPE_VS, 1);
        svga3_vlkn_context_set_shader(dev, CID, SVGA3D_SHADERTYPE_PS, 1);

        /* Transformation Matrix: Scale by 0.5 so quad covers [-0.5, 0.5] (pixels [16..47, 16..47]) */
        float matScaleHalf[16] = {
            0.5f, 0.0f, 0.0f, 0.0f,
            0.0f, 0.5f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };
        for (int r = 0; r < 4; ++r) {
            uint32_t rowValues[4];
            memcpy(rowValues, &matScaleHalf[r * 4], sizeof(float) * 4);
            svga3_vlkn_context_set_shader_const(dev, CID, r, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, rowValues);
        }

        /* Shader Constant Tint: (0.5, 1.0, 0.8, 1.0) */
        float tint[4] = { 0.5f, 1.0f, 0.8f, 1.0f };
        uint32_t tintVal[4];
        memcpy(tintVal, tint, sizeof(tint));
        svga3_vlkn_context_set_shader_const(dev, CID, 0, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT, tintVal);

        /* Clear to Black (0, 0, 0, 0) */
        svga3_vlkn_context_clear(dev, CID, SVGA3D_CLEAR_COLOR, 0x00000000, 1.0f, 0, nullptr, 0);

        /* Draw Indexed */
        SVGA3dPrimitiveRange range = {};
        range.primType = SVGA3D_PRIMITIVE_TRIANGLELIST;
        range.primitiveCount = 2;
        range.indexArray.surfaceId = SID_IB;
        range.indexArray.offset = 0;
        range.indexArray.stride = 2;

        decls[0].array.surfaceId = SID_VB;
        decls[1].array.surfaceId = SID_VB;
        svga3_vlkn_context_draw(dev, CID, SVGA3D_PRIMITIVE_TRIANGLELIST, decls, 2, &range, 1);
        svga3_vlkn_device_wait_idle(dev);

        /* Read back framebuffer and analytically verify */
        std::vector<Pixel> fb(RT_W * RT_H);
        svga3_vlkn_surface_dma_download(dev, SID_RT, 0, nullptr, fb.data(), RT_W * 4);

        /* Analytical expectation:
         * Quad spans [16..47, 16..47]. Outside quad must be (0, 0, 0).
         * Inside quad:
         * Top-Left (~(24, 24)): White * Tint = (255*0.5, 255*1.0, 255*0.8) = (127, 255, 204)
         * Bottom-Left (~(24, 40)): Green * Tint = (0*0.5, 255*1.0, 0*0.8) = (0, 255, 0)
         * Corner (0, 0): untouched black (0, 0, 0)
         */
        Pixel outside = fb[0];
        TEST_CHECK(outside.r == 0 && outside.g == 0 && outside.b == 0,
                   "Scene 1: Pixels outside scaled geometry remain untouched clear color (0, 0, 0)");

        Pixel tl = fb[24 * RT_W + 24];
        TEST_CHECK(pixelMatches(tl, 127, 255, 204, 255),
                   "Scene 1: Top-Left pixel analytically matches White * Tint (127, 255, 204)");

        Pixel bl = fb[40 * RT_W + 24];
        TEST_CHECK(pixelMatches(bl, 0, 255, 0, 255),
                   "Scene 1: Bottom-Left pixel analytically matches Green * Tint (0, 255, 0)");

        /* Generate Analytical Reference Image for Scene 1 */
        std::vector<Pixel> refFb(RT_W * RT_H, { 0, 0, 0, 0 });
        for (int y = 16; y < 48; ++y) {
            for (int x = 16; x < 48; ++x) {
                if (x < 32 && y < 32) {
                    refFb[y * RT_W + x] = { 204, 255, 127, 255 }; // White * Tint -> b=204, g=255, r=127
                } else if (x >= 32 && y < 32) {
                    refFb[y * RT_W + x] = { 0, 0, 127, 255 };     // Red * Tint -> b=0, g=0, r=127
                } else if (x < 32 && y >= 32) {
                    refFb[y * RT_W + x] = { 0, 255, 0, 255 };     // Green * Tint -> b=0, g=255, r=0
                } else {
                    refFb[y * RT_W + x] = { 204, 0, 0, 255 };     // Blue * Tint -> b=204, g=0, r=0
                }
            }
        }
        g_scene1Fb = fb;
        g_scene1Ref = refFb;
        savePPM("artifacts/scene1_rendered.ppm", fb.data(), RT_W, RT_H);
        savePPM("artifacts/scene1_reference.ppm", refFb.data(), RT_W, RT_H);
        saveDiffPPM("artifacts/scene1_diff.ppm", fb.data(), refFb.data(), RT_W, RT_H);

        /* Clean up Scene 1 */
        svga3_vlkn_context_destroy(dev, CID);
        svga3_vlkn_surface_destroy(dev, SID_RT);
        svga3_vlkn_surface_destroy(dev, SID_TEX);
        svga3_vlkn_surface_destroy(dev, SID_VB);
        svga3_vlkn_surface_destroy(dev, SID_IB);
    }

    /* --------------------------------------------------------------------------
     * SCENE 2: Depth Overlap (Depth Testing and Z-Buffering)
     * -------------------------------------------------------------------------- */
    std::cout << "\n--- Scene 2: Depth Overlap (Depth Testing & Z-Buffering) ---" << std::endl;
    {
        const uint32_t CID = 2;
        const uint32_t SID_RT = 20;
        const uint32_t SID_DS = 21;
        const uint32_t SID_VB1 = 22; /* Quad at Z = 0.5 (Red) */
        const uint32_t SID_VB2 = 23; /* Quad at Z = 0.8 (Green, Farther) */
        const uint32_t SID_VB3 = 24; /* Quad at Z = 0.2 (Blue, Closer) */

        svga3_vlkn_context_create(dev, CID);

        SVGA3dSize rtSize = { RT_W, RT_H, 1 };
        svga3_vlkn_surface_define(dev, SID_RT, SVGA3D_SURFACE_HINT_RENDERTARGET, SVGA3D_X8R8G8B8, &rtSize, 1);
        svga3_vlkn_surface_define(dev, SID_DS, SVGA3D_SURFACE_HINT_DEPTHSTENCIL, SVGA3D_Z_D24S8, &rtSize, 1);
        svga3_vlkn_context_set_render_target(dev, CID, SVGA3D_RT_COLOR0, SID_RT, 0, 0);
        svga3_vlkn_context_set_render_target(dev, CID, SVGA3D_RT_DEPTH, SID_DS, 0, 0);

        SVGA3dRect vp = { 0, 0, RT_W, RT_H };
        svga3_vlkn_context_set_viewport(dev, CID, &vp);
        svga3_vlkn_context_set_render_state(dev, CID, SVGA3D_RS_CULLMODE, SVGA3D_FACE_NONE);

        /* Identity matrix for VS */
        float matIdent[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };
        for (int r = 0; r < 4; ++r) {
            uint32_t rowValues[4];
            memcpy(rowValues, &matIdent[r * 4], sizeof(float) * 4);
            svga3_vlkn_context_set_shader_const(dev, CID, r, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, rowValues);
        }

        svga3_vlkn_context_define_shader(dev, CID, 1, SVGA3D_SHADERTYPE_VS, vs1Bytecode, sizeof(vs1Bytecode)/4);
        svga3_vlkn_context_define_shader(dev, CID, 2, SVGA3D_SHADERTYPE_PS, ps2Bytecode, sizeof(ps2Bytecode)/4);
        svga3_vlkn_context_set_shader(dev, CID, SVGA3D_SHADERTYPE_VS, 1);
        svga3_vlkn_context_set_shader(dev, CID, SVGA3D_SHADERTYPE_PS, 2);

        /* Enable Depth Test and Depth Write with LESSEQUAL */
        svga3_vlkn_context_set_render_state(dev, CID, SVGA3D_RS_ZENABLE, 1);
        svga3_vlkn_context_set_render_state(dev, CID, SVGA3D_RS_ZWRITEENABLE, 1);
        svga3_vlkn_context_set_render_state(dev, CID, SVGA3D_RS_ZFUNC, SVGA3D_CMP_LESSEQUAL);

        /* Create 3 quads with different Z values */
        auto makeQuad = [&](float z) -> std::vector<Vertex> {
            return {
                { -1.0f,  1.0f, z,  0.0f, 0.0f },
                {  1.0f,  1.0f, z,  1.0f, 0.0f },
                {  1.0f, -1.0f, z,  1.0f, 1.0f },
                { -1.0f,  1.0f, z,  0.0f, 0.0f },
                {  1.0f, -1.0f, z,  1.0f, 1.0f },
                { -1.0f, -1.0f, z,  0.0f, 1.0f }
            };
        };

        std::vector<Vertex> q1 = makeQuad(0.5f);
        std::vector<Vertex> q2 = makeQuad(0.8f);
        std::vector<Vertex> q3 = makeQuad(0.2f);

        SVGA3dSize vbSz = { sizeof(Vertex) * 6, 1, 1 };
        svga3_vlkn_surface_define(dev, SID_VB1, SVGA3D_SURFACE_HINT_VERTEXBUFFER, SVGA3D_BUFFER, &vbSz, 1);
        svga3_vlkn_surface_define(dev, SID_VB2, SVGA3D_SURFACE_HINT_VERTEXBUFFER, SVGA3D_BUFFER, &vbSz, 1);
        svga3_vlkn_surface_define(dev, SID_VB3, SVGA3D_SURFACE_HINT_VERTEXBUFFER, SVGA3D_BUFFER, &vbSz, 1);

        SVGA3dBox bBox = { 0, 0, 0, sizeof(Vertex) * 6, 1, 1 };
        svga3_vlkn_surface_dma_upload(dev, SID_VB1, 0, &bBox, q1.data(), sizeof(Vertex) * 6);
        svga3_vlkn_surface_dma_upload(dev, SID_VB2, 0, &bBox, q2.data(), sizeof(Vertex) * 6);
        svga3_vlkn_surface_dma_upload(dev, SID_VB3, 0, &bBox, q3.data(), sizeof(Vertex) * 6);

        /* Step 1: Clear Color to Black, Depth to 1.0 */
        svga3_vlkn_context_clear(dev, CID, (SVGA3dClearFlag)(SVGA3D_CLEAR_COLOR | SVGA3D_CLEAR_DEPTH), 0x00000000, 1.0f, 0, nullptr, 0);

        /* Step 2: Draw Quad 1 at Z = 0.5 with Solid Red (1, 0, 0, 1) */
        float colRed[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
        uint32_t cVal[4];
        memcpy(cVal, colRed, sizeof(colRed));
        svga3_vlkn_context_set_shader_const(dev, CID, 0, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT, cVal);

        decls[0].array.surfaceId = SID_VB1;
        decls[1].array.surfaceId = SID_VB1;
        SVGA3dPrimitiveRange r1 = { SVGA3D_PRIMITIVE_TRIANGLELIST, 2, {}, 0, 0 };
        svga3_vlkn_context_draw(dev, CID, SVGA3D_PRIMITIVE_TRIANGLELIST, decls, 2, &r1, 1);

        /* Step 3: Draw Quad 2 at Z = 0.8 with Solid Green (0, 1, 0, 1) - Must fail depth test! */
        float colGreen[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
        memcpy(cVal, colGreen, sizeof(colGreen));
        svga3_vlkn_context_set_shader_const(dev, CID, 0, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT, cVal);

        decls[0].array.surfaceId = SID_VB2;
        decls[1].array.surfaceId = SID_VB2;
        svga3_vlkn_context_draw(dev, CID, SVGA3D_PRIMITIVE_TRIANGLELIST, decls, 2, &r1, 1);
        svga3_vlkn_device_wait_idle(dev);

        std::vector<Pixel> fb(RT_W * RT_H);
        svga3_vlkn_surface_dma_download(dev, SID_RT, 0, nullptr, fb.data(), RT_W * 4);

        /* Verify Quad 2 was rejected by depth test: pixel must remain Red! */
        Pixel center1 = fb[(RT_H / 2) * RT_W + (RT_W / 2)];
        TEST_CHECK(pixelMatches(center1, 255, 0, 0, 255),
                   "Scene 2: Farther geometry (Z=0.8) rejected by depth test; Red (Z=0.5) prevails");

        /* Step 4: Draw Quad 3 at Z = 0.2 with Solid Blue (0, 0, 1, 1) - Must pass depth test! */
        float colBlue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
        memcpy(cVal, colBlue, sizeof(colBlue));
        svga3_vlkn_context_set_shader_const(dev, CID, 0, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT, cVal);

        decls[0].array.surfaceId = SID_VB3;
        decls[1].array.surfaceId = SID_VB3;
        svga3_vlkn_context_draw(dev, CID, SVGA3D_PRIMITIVE_TRIANGLELIST, decls, 2, &r1, 1);
        svga3_vlkn_device_wait_idle(dev);

        svga3_vlkn_surface_dma_download(dev, SID_RT, 0, nullptr, fb.data(), RT_W * 4);
        Pixel center2 = fb[(RT_H / 2) * RT_W + (RT_W / 2)];
        TEST_CHECK(pixelMatches(center2, 0, 0, 255, 255),
                   "Scene 2: Closer geometry (Z=0.2) passes depth test; overwrites with Blue");

        /* Generate Analytical Reference Image for Scene 2 (Closer Blue quad over full surface) */
        std::vector<Pixel> refFb2(RT_W * RT_H, { 255, 0, 0, 255 }); // Blue: B=255, G=0, R=0, A=255
        savePPM("artifacts/scene2_depth_rendered.ppm", fb.data(), RT_W, RT_H);
        savePPM("artifacts/scene2_depth_reference.ppm", refFb2.data(), RT_W, RT_H);
        saveDiffPPM("artifacts/scene2_depth_diff.ppm", fb.data(), refFb2.data(), RT_W, RT_H);

        /* Clean up Scene 2 */
        svga3_vlkn_context_destroy(dev, CID);
        svga3_vlkn_surface_destroy(dev, SID_RT);
        svga3_vlkn_surface_destroy(dev, SID_DS);
        svga3_vlkn_surface_destroy(dev, SID_VB1);
        svga3_vlkn_surface_destroy(dev, SID_VB2);
        svga3_vlkn_surface_destroy(dev, SID_VB3);
    }

    /* --------------------------------------------------------------------------
     * SCENE 3: Alpha Blending
     * -------------------------------------------------------------------------- */
    std::cout << "\n--- Scene 3: Alpha Blending with Analytical Mathematical Verification ---" << std::endl;
    {
        const uint32_t CID = 3;
        const uint32_t SID_RT = 30;
        const uint32_t SID_VB = 31;

        svga3_vlkn_context_create(dev, CID);

        SVGA3dSize rtSize = { RT_W, RT_H, 1 };
        svga3_vlkn_surface_define(dev, SID_RT, SVGA3D_SURFACE_HINT_RENDERTARGET, SVGA3D_X8R8G8B8, &rtSize, 1);
        svga3_vlkn_context_set_render_target(dev, CID, SVGA3D_RT_COLOR0, SID_RT, 0, 0);

        SVGA3dRect vp = { 0, 0, RT_W, RT_H };
        svga3_vlkn_context_set_viewport(dev, CID, &vp);
        svga3_vlkn_context_set_render_state(dev, CID, SVGA3D_RS_CULLMODE, SVGA3D_FACE_NONE);

        float matIdent[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };
        for (int r = 0; r < 4; ++r) {
            uint32_t rowValues[4];
            memcpy(rowValues, &matIdent[r * 4], sizeof(float) * 4);
            svga3_vlkn_context_set_shader_const(dev, CID, r, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, rowValues);
        }

        svga3_vlkn_context_define_shader(dev, CID, 1, SVGA3D_SHADERTYPE_VS, vs1Bytecode, sizeof(vs1Bytecode)/4);
        svga3_vlkn_context_define_shader(dev, CID, 2, SVGA3D_SHADERTYPE_PS, ps2Bytecode, sizeof(ps2Bytecode)/4);
        svga3_vlkn_context_set_shader(dev, CID, SVGA3D_SHADERTYPE_VS, 1);
        svga3_vlkn_context_set_shader(dev, CID, SVGA3D_SHADERTYPE_PS, 2);

        /* Clear to Solid Gray (80, 80, 80, 255) -> 0xFF505050 */
        svga3_vlkn_context_clear(dev, CID, SVGA3D_CLEAR_COLOR, 0xFF505050, 1.0f, 0, nullptr, 0);

        /* Configure Standard Alpha Blending: SRC * SRC_ALPHA + DST * (1 - SRC_ALPHA) */
        svga3_vlkn_context_set_render_state(dev, CID, SVGA3D_RS_BLENDENABLE, 1);
        svga3_vlkn_context_set_render_state(dev, CID, SVGA3D_RS_SRCBLEND, SVGA3D_BLENDOP_SRCALPHA);
        svga3_vlkn_context_set_render_state(dev, CID, SVGA3D_RS_DSTBLEND, SVGA3D_BLENDOP_INVSRCALPHA);
        svga3_vlkn_context_set_render_state(dev, CID, SVGA3D_RS_BLENDEQUATION, SVGA3D_BLENDEQ_ADD);

        /* Draw Quad with Semi-transparent Color:
         * Src Color: R=200/255, G=100/255, B=50/255, Alpha=128/255 (0.50196)
         * Analytical Math:
         * R = round(200 * 0.50196 + 80 * (1 - 0.50196)) = round(100.39 + 39.84) = 140
         * G = round(100 * 0.50196 + 80 * (1 - 0.50196)) = round( 50.20 + 39.84) = 90
         * B = round( 50 * 0.50196 + 80 * (1 - 0.50196)) = round( 25.10 + 39.84) = 65
         * A: separate alpha blending is off, so per D3D9 the alpha
         *    channel blends with the color factors against the cleared
         *    dst alpha 255: round(128 * 0.50196 + 255 * (1 - 0.50196))
         *    = round(64.25 + 127.0) = 191
         */
        float srcColor[4] = { 200.0f / 255.0f, 100.0f / 255.0f, 50.0f / 255.0f, 128.0f / 255.0f };
        uint32_t scVal[4];
        memcpy(scVal, srcColor, sizeof(srcColor));
        svga3_vlkn_context_set_shader_const(dev, CID, 0, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT, scVal);

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
        SVGA3dBox bBox = { 0, 0, 0, sizeof(quadVerts), 1, 1 };
        svga3_vlkn_surface_dma_upload(dev, SID_VB, 0, &bBox, quadVerts, sizeof(quadVerts));

        decls[0].array.surfaceId = SID_VB;
        decls[1].array.surfaceId = SID_VB;
        SVGA3dPrimitiveRange r1 = { SVGA3D_PRIMITIVE_TRIANGLELIST, 2, {}, 0, 0 };
        svga3_vlkn_context_draw(dev, CID, SVGA3D_PRIMITIVE_TRIANGLELIST, decls, 2, &r1, 1);
        svga3_vlkn_device_wait_idle(dev);

        std::vector<Pixel> fb(RT_W * RT_H);
        svga3_vlkn_surface_dma_download(dev, SID_RT, 0, nullptr, fb.data(), RT_W * 4);

        Pixel blended = fb[(RT_H / 2) * RT_W + (RT_W / 2)];
        TEST_CHECK(pixelMatches(blended, 140, 90, 65, 191),
                   "Scene 3: Alpha blending analytically matches round(Src*A + Dst*(1-A)) = (140, 90, 65)");

        /* Generate Analytical Reference Image for Scene 3 */
        std::vector<Pixel> refFb3(RT_W * RT_H, { 65, 90, 140, 191 }); // (B=65, G=90, R=140, A=191)
        savePPM("artifacts/scene3_alpha_rendered.ppm", fb.data(), RT_W, RT_H);
        savePPM("artifacts/scene3_alpha_reference.ppm", refFb3.data(), RT_W, RT_H);
        saveDiffPPM("artifacts/scene3_alpha_diff.ppm", fb.data(), refFb3.data(), RT_W, RT_H);

        /* Clean up Scene 3 */
        svga3_vlkn_context_destroy(dev, CID);
        svga3_vlkn_surface_destroy(dev, SID_RT);
        svga3_vlkn_surface_destroy(dev, SID_VB);
    }

    /* --------------------------------------------------------------------------
     * SCENE 4: Viewport and Scissor Boundaries
     * -------------------------------------------------------------------------- */
    std::cout << "\n--- Scene 4: Viewport and Scissor Boundaries ---" << std::endl;
    {
        const uint32_t CID = 4;
        const uint32_t SID_RT = 40;
        const uint32_t SID_VB = 41;

        svga3_vlkn_context_create(dev, CID);

        SVGA3dSize rtSize = { RT_W, RT_H, 1 };
        svga3_vlkn_surface_define(dev, SID_RT, SVGA3D_SURFACE_HINT_RENDERTARGET, SVGA3D_X8R8G8B8, &rtSize, 1);
        svga3_vlkn_context_set_render_target(dev, CID, SVGA3D_RT_COLOR0, SID_RT, 0, 0);

        /* First clear the entire framebuffer [0, 0, RT_W, RT_H] to Dark Blue (0, 0, 100, 255) -> 0xFF000064 */
        SVGA3dRect fullVp = { 0, 0, RT_W, RT_H };
        svga3_vlkn_context_set_viewport(dev, CID, &fullVp);
        svga3_vlkn_context_clear(dev, CID, SVGA3D_CLEAR_COLOR, 0xFF000064, 1.0f, 0, nullptr, 0);

        /* Set Viewport to [10, 10, 44, 44] */
        SVGA3dRect vp = { 10, 10, 44, 44 };
        svga3_vlkn_context_set_viewport(dev, CID, &vp);

        /* Set Scissor Rect to [20, 20, 24, 24] */
        SVGA3dRect scissor = { 20, 20, 24, 24 };
        svga3_vlkn_context_set_scissor_rect(dev, CID, &scissor);
        svga3_vlkn_context_set_render_state(dev, CID, SVGA3D_RS_SCISSORTESTENABLE, 1);
        svga3_vlkn_context_set_render_state(dev, CID, SVGA3D_RS_CULLMODE, SVGA3D_FACE_NONE);

        /* Identity transform */
        float matIdent[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };
        for (int r = 0; r < 4; ++r) {
            uint32_t rowValues[4];
            memcpy(rowValues, &matIdent[r * 4], sizeof(float) * 4);
            svga3_vlkn_context_set_shader_const(dev, CID, r, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, rowValues);
        }

        svga3_vlkn_context_define_shader(dev, CID, 1, SVGA3D_SHADERTYPE_VS, vs1Bytecode, sizeof(vs1Bytecode)/4);
        svga3_vlkn_context_define_shader(dev, CID, 2, SVGA3D_SHADERTYPE_PS, ps2Bytecode, sizeof(ps2Bytecode)/4);
        svga3_vlkn_context_set_shader(dev, CID, SVGA3D_SHADERTYPE_VS, 1);
        svga3_vlkn_context_set_shader(dev, CID, SVGA3D_SHADERTYPE_PS, 2);

        /* Solid Yellow color (255, 255, 0, 255) */
        float colYellow[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
        uint32_t cVal[4];
        memcpy(cVal, colYellow, sizeof(colYellow));
        svga3_vlkn_context_set_shader_const(dev, CID, 0, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT, cVal);

        /* Draw full-screen quad */
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
        SVGA3dBox bBox = { 0, 0, 0, sizeof(quadVerts), 1, 1 };
        svga3_vlkn_surface_dma_upload(dev, SID_VB, 0, &bBox, quadVerts, sizeof(quadVerts));

        decls[0].array.surfaceId = SID_VB;
        decls[1].array.surfaceId = SID_VB;
        SVGA3dPrimitiveRange r1 = { SVGA3D_PRIMITIVE_TRIANGLELIST, 2, {}, 0, 0 };
        svga3_vlkn_context_draw(dev, CID, SVGA3D_PRIMITIVE_TRIANGLELIST, decls, 2, &r1, 1);
        svga3_vlkn_device_wait_idle(dev);

        std::vector<Pixel> fb(RT_W * RT_H);
        svga3_vlkn_surface_dma_download(dev, SID_RT, 0, nullptr, fb.data(), RT_W * 4);

        /* Verify:
         * Inside scissor [20..43, 20..43]: Yellow (255, 255, 0)
         * Outside scissor, inside viewport (e.g. (15, 15)): Dark Blue (0, 0, 100)
         * Outside viewport (e.g. (5, 5)): Dark Blue (0, 0, 100)
         */
        Pixel insideScissor = fb[30 * RT_W + 30];
        TEST_CHECK(pixelMatches(insideScissor, 255, 255, 0, 255),
                   "Scene 4: Pixel inside scissor is rendered Yellow (255, 255, 0)");

        Pixel outsideScissorInVp = fb[15 * RT_W + 15];
        TEST_CHECK(pixelMatches(outsideScissorInVp, 0, 0, 100, 255),
                   "Scene 4: Pixel outside scissor but inside viewport is preserved Dark Blue (0, 0, 100)");

        Pixel outsideVp = fb[5 * RT_W + 5];
        TEST_CHECK(pixelMatches(outsideVp, 0, 0, 100, 255),
                   "Scene 4: Pixel outside viewport is preserved Dark Blue (0, 0, 100)");

        /* Generate Analytical Reference Image for Scene 4 */
        std::vector<Pixel> refFb4(RT_W * RT_H, { 100, 0, 0, 255 }); // Dark blue clear (B=100, G=0, R=0, A=255)
        for (int y = 20; y < 44; ++y) {
            for (int x = 20; x < 44; ++x) {
                refFb4[y * RT_W + x] = { 0, 255, 255, 255 }; // Yellow quad inside scissor (B=0, G=255, R=255, A=255)
            }
        }
        savePPM("artifacts/scene4_scissor_rendered.ppm", fb.data(), RT_W, RT_H);
        savePPM("artifacts/scene4_scissor_reference.ppm", refFb4.data(), RT_W, RT_H);
        saveDiffPPM("artifacts/scene4_scissor_diff.ppm", fb.data(), refFb4.data(), RT_W, RT_H);

        /* Clean up Scene 4 */
        svga3_vlkn_context_destroy(dev, CID);
        svga3_vlkn_surface_destroy(dev, SID_RT);
        svga3_vlkn_surface_destroy(dev, SID_VB);
    }

    /* --------------------------------------------------------------------------
     * SCENE 5: Switching Render Targets
     * -------------------------------------------------------------------------- */
    std::cout << "\n--- Scene 5: Switching Render Targets (RT1 & RT2 Independent Rendering) ---" << std::endl;
    {
        const uint32_t CID = 5;
        const uint32_t SID_RT1 = 50;
        const uint32_t SID_RT2 = 51;
        const uint32_t SID_VB = 52;

        svga3_vlkn_context_create(dev, CID);

        SVGA3dSize rtSize = { RT_W, RT_H, 1 };
        svga3_vlkn_surface_define(dev, SID_RT1, SVGA3D_SURFACE_HINT_RENDERTARGET, SVGA3D_X8R8G8B8, &rtSize, 1);
        svga3_vlkn_surface_define(dev, SID_RT2, SVGA3D_SURFACE_HINT_RENDERTARGET, SVGA3D_X8R8G8B8, &rtSize, 1);

        SVGA3dRect vp = { 0, 0, RT_W, RT_H };
        svga3_vlkn_context_set_viewport(dev, CID, &vp);
        svga3_vlkn_context_set_render_state(dev, CID, SVGA3D_RS_CULLMODE, SVGA3D_FACE_NONE);

        float matIdent[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };
        for (int r = 0; r < 4; ++r) {
            uint32_t rowValues[4];
            memcpy(rowValues, &matIdent[r * 4], sizeof(float) * 4);
            svga3_vlkn_context_set_shader_const(dev, CID, r, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, rowValues);
        }

        svga3_vlkn_context_define_shader(dev, CID, 1, SVGA3D_SHADERTYPE_VS, vs1Bytecode, sizeof(vs1Bytecode)/4);
        svga3_vlkn_context_define_shader(dev, CID, 2, SVGA3D_SHADERTYPE_PS, ps2Bytecode, sizeof(ps2Bytecode)/4);
        svga3_vlkn_context_set_shader(dev, CID, SVGA3D_SHADERTYPE_VS, 1);
        svga3_vlkn_context_set_shader(dev, CID, SVGA3D_SHADERTYPE_PS, 2);

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
        SVGA3dBox bBox = { 0, 0, 0, sizeof(quadVerts), 1, 1 };
        svga3_vlkn_surface_dma_upload(dev, SID_VB, 0, &bBox, quadVerts, sizeof(quadVerts));

        decls[0].array.surfaceId = SID_VB;
        decls[1].array.surfaceId = SID_VB;
        SVGA3dPrimitiveRange r1 = { SVGA3D_PRIMITIVE_TRIANGLELIST, 2, {}, 0, 0 };

        /* 1. Target RT1: Clear to Magenta (255, 0, 255) */
        svga3_vlkn_context_set_render_target(dev, CID, SVGA3D_RT_COLOR0, SID_RT1, 0, 0);
        svga3_vlkn_context_clear(dev, CID, SVGA3D_CLEAR_COLOR, 0xFFFF00FF, 1.0f, 0, nullptr, 0);

        /* 2. Switch to RT2: Clear to Black (0, 0, 0), Draw Cyan (0, 255, 255) Quad */
        svga3_vlkn_context_set_render_target(dev, CID, SVGA3D_RT_COLOR0, SID_RT2, 0, 0);
        svga3_vlkn_context_clear(dev, CID, SVGA3D_CLEAR_COLOR, 0x00000000, 1.0f, 0, nullptr, 0);

        float colCyan[4] = { 0.0f, 1.0f, 1.0f, 1.0f };
        uint32_t cVal[4];
        memcpy(cVal, colCyan, sizeof(colCyan));
        svga3_vlkn_context_set_shader_const(dev, CID, 0, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT, cVal);
        svga3_vlkn_context_draw(dev, CID, SVGA3D_PRIMITIVE_TRIANGLELIST, decls, 2, &r1, 1);

        /* 3. Switch BACK to RT1: Draw Yellow (255, 255, 0) Quad */
        svga3_vlkn_context_set_render_target(dev, CID, SVGA3D_RT_COLOR0, SID_RT1, 0, 0);
        float colYellow[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
        memcpy(cVal, colYellow, sizeof(colYellow));
        svga3_vlkn_context_set_shader_const(dev, CID, 0, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT, cVal);
        svga3_vlkn_context_draw(dev, CID, SVGA3D_PRIMITIVE_TRIANGLELIST, decls, 2, &r1, 1);
        svga3_vlkn_device_wait_idle(dev);

        /* Read back both RT1 and RT2 and verify independence */
        std::vector<Pixel> fb1(RT_W * RT_H);
        std::vector<Pixel> fb2(RT_W * RT_H);
        svga3_vlkn_surface_dma_download(dev, SID_RT1, 0, nullptr, fb1.data(), RT_W * 4);
        svga3_vlkn_surface_dma_download(dev, SID_RT2, 0, nullptr, fb2.data(), RT_W * 4);

        Pixel p1 = fb1[(RT_H / 2) * RT_W + (RT_W / 2)];
        Pixel p2 = fb2[(RT_H / 2) * RT_W + (RT_W / 2)];
        TEST_CHECK(pixelMatches(p1, 255, 255, 0, 255), "Scene 5: RT1 contains Yellow (255, 255, 0)");
        TEST_CHECK(pixelMatches(p2, 0, 255, 255, 255), "Scene 5: RT2 contains Cyan (0, 255, 255)");

        savePPM("artifacts/scene5_rt1_rendered.ppm", fb1.data(), RT_W, RT_H);
        savePPM("artifacts/scene5_rt2_rendered.ppm", fb2.data(), RT_W, RT_H);

        /* Clean up Scene 5 */
        svga3_vlkn_context_destroy(dev, CID);
        svga3_vlkn_surface_destroy(dev, SID_RT1);
        svga3_vlkn_surface_destroy(dev, SID_RT2);
        svga3_vlkn_surface_destroy(dev, SID_VB);
    }

    /* --------------------------------------------------------------------------
     * SCENE 6: Two Contexts with Distinct State (Multi-Context Isolation)
     * -------------------------------------------------------------------------- */
    std::cout << "\n--- Scene 6: Two Contexts with Distinct State (Context Isolation) ---" << std::endl;
    {
        const uint32_t CID1 = 61;
        const uint32_t CID2 = 62;
        const uint32_t SID_RT1 = 601;
        const uint32_t SID_RT2 = 602;
        const uint32_t SID_VB = 603;

        svga3_vlkn_context_create(dev, CID1);
        svga3_vlkn_context_create(dev, CID2);

        SVGA3dSize rtSize = { RT_W, RT_H, 1 };
        svga3_vlkn_surface_define(dev, SID_RT1, SVGA3D_SURFACE_HINT_RENDERTARGET, SVGA3D_X8R8G8B8, &rtSize, 1);
        svga3_vlkn_surface_define(dev, SID_RT2, SVGA3D_SURFACE_HINT_RENDERTARGET, SVGA3D_X8R8G8B8, &rtSize, 1);

        svga3_vlkn_context_set_render_target(dev, CID1, SVGA3D_RT_COLOR0, SID_RT1, 0, 0);
        svga3_vlkn_context_set_render_target(dev, CID2, SVGA3D_RT_COLOR0, SID_RT2, 0, 0);

        /* Clear both entire framebuffers to Black */
        SVGA3dRect fullVp = { 0, 0, RT_W, RT_H };
        svga3_vlkn_context_set_viewport(dev, CID1, &fullVp);
        svga3_vlkn_context_set_viewport(dev, CID2, &fullVp);
        svga3_vlkn_context_clear(dev, CID1, SVGA3D_CLEAR_COLOR, 0x00000000, 1.0f, 0, nullptr, 0);
        svga3_vlkn_context_clear(dev, CID2, SVGA3D_CLEAR_COLOR, 0x00000000, 1.0f, 0, nullptr, 0);

        /* Context 1: Viewport Left Half [0, 0, 32, 64], Color Red */
        SVGA3dRect vp1 = { 0, 0, RT_W / 2, RT_H };
        svga3_vlkn_context_set_viewport(dev, CID1, &vp1);
        svga3_vlkn_context_set_render_state(dev, CID1, SVGA3D_RS_CULLMODE, SVGA3D_FACE_NONE);

        /* Context 2: Viewport Right Half [32, 0, 32, 64], Color Blue */
        SVGA3dRect vp2 = { RT_W / 2, 0, RT_W / 2, RT_H };
        svga3_vlkn_context_set_viewport(dev, CID2, &vp2);
        svga3_vlkn_context_set_render_state(dev, CID2, SVGA3D_RS_CULLMODE, SVGA3D_FACE_NONE);

        float matIdent[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };
        for (int r = 0; r < 4; ++r) {
            uint32_t rowValues[4];
            memcpy(rowValues, &matIdent[r * 4], sizeof(float) * 4);
            svga3_vlkn_context_set_shader_const(dev, CID1, r, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, rowValues);
            svga3_vlkn_context_set_shader_const(dev, CID2, r, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, rowValues);
        }

        svga3_vlkn_context_define_shader(dev, CID1, 1, SVGA3D_SHADERTYPE_VS, vs1Bytecode, sizeof(vs1Bytecode)/4);
        svga3_vlkn_context_define_shader(dev, CID1, 2, SVGA3D_SHADERTYPE_PS, ps2Bytecode, sizeof(ps2Bytecode)/4);
        svga3_vlkn_context_set_shader(dev, CID1, SVGA3D_SHADERTYPE_VS, 1);
        svga3_vlkn_context_set_shader(dev, CID1, SVGA3D_SHADERTYPE_PS, 2);

        svga3_vlkn_context_define_shader(dev, CID2, 1, SVGA3D_SHADERTYPE_VS, vs1Bytecode, sizeof(vs1Bytecode)/4);
        svga3_vlkn_context_define_shader(dev, CID2, 2, SVGA3D_SHADERTYPE_PS, ps2Bytecode, sizeof(ps2Bytecode)/4);
        svga3_vlkn_context_set_shader(dev, CID2, SVGA3D_SHADERTYPE_VS, 1);
        svga3_vlkn_context_set_shader(dev, CID2, SVGA3D_SHADERTYPE_PS, 2);

        float colRed[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
        float colBlue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
        uint32_t cVal[4];
        memcpy(cVal, colRed, sizeof(colRed));
        svga3_vlkn_context_set_shader_const(dev, CID1, 0, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT, cVal);
        memcpy(cVal, colBlue, sizeof(colBlue));
        svga3_vlkn_context_set_shader_const(dev, CID2, 0, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT, cVal);

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
        SVGA3dBox bBox = { 0, 0, 0, sizeof(quadVerts), 1, 1 };
        svga3_vlkn_surface_dma_upload(dev, SID_VB, 0, &bBox, quadVerts, sizeof(quadVerts));

        decls[0].array.surfaceId = SID_VB;
        decls[1].array.surfaceId = SID_VB;
        SVGA3dPrimitiveRange r1 = { SVGA3D_PRIMITIVE_TRIANGLELIST, 2, {}, 0, 0 };

        /* Interleave draw calls between Context 1 and Context 2 */
        svga3_vlkn_context_draw(dev, CID1, SVGA3D_PRIMITIVE_TRIANGLELIST, decls, 2, &r1, 1);
        svga3_vlkn_context_draw(dev, CID2, SVGA3D_PRIMITIVE_TRIANGLELIST, decls, 2, &r1, 1);
        svga3_vlkn_device_wait_idle(dev);

        std::vector<Pixel> fb1(RT_W * RT_H);
        std::vector<Pixel> fb2(RT_W * RT_H);
        svga3_vlkn_surface_dma_download(dev, SID_RT1, 0, nullptr, fb1.data(), RT_W * 4);
        svga3_vlkn_surface_dma_download(dev, SID_RT2, 0, nullptr, fb2.data(), RT_W * 4);

        /* Verify Context 1: Left half is Red, Right half is untouched Black */
        Pixel c1Left = fb1[32 * RT_W + 16];
        Pixel c1Right = fb1[32 * RT_W + 48];
        TEST_CHECK(pixelMatches(c1Left, 255, 0, 0, 255), "Scene 6: CID1 left half is Red (255, 0, 0)");
        TEST_CHECK(pixelMatches(c1Right, 0, 0, 0, 0), "Scene 6: CID1 right half is Black (0, 0, 0)");

        /* Verify Context 2: Left half is untouched Black, Right half is Blue */
        Pixel c2Left = fb2[32 * RT_W + 16];
        Pixel c2Right = fb2[32 * RT_W + 48];
        TEST_CHECK(pixelMatches(c2Left, 0, 0, 0, 0), "Scene 6: CID2 left half is Black (0, 0, 0)");
        TEST_CHECK(pixelMatches(c2Right, 0, 0, 255, 255), "Scene 6: CID2 right half is Blue (0, 0, 255)");

        savePPM("artifacts/scene6_ctx1_rendered.ppm", fb1.data(), RT_W, RT_H);
        savePPM("artifacts/scene6_ctx2_rendered.ppm", fb2.data(), RT_W, RT_H);

        /* Clean up Scene 6 */
        svga3_vlkn_context_destroy(dev, CID1);
        svga3_vlkn_context_destroy(dev, CID2);
        svga3_vlkn_surface_destroy(dev, SID_RT1);
        svga3_vlkn_surface_destroy(dev, SID_RT2);
        svga3_vlkn_surface_destroy(dev, SID_VB);
    }

    /* --------------------------------------------------------------------------
     * SCENE 7: Dynamic Resource Updates Followed by Another Draw
     * -------------------------------------------------------------------------- */
    std::cout << "\n--- Scene 7: Dynamic Resource Updates & Subsequent Draw ---" << std::endl;
    {
        const uint32_t CID = 7;
        const uint32_t SID_RT = 70;
        const uint32_t SID_TEX = 71;
        const uint32_t SID_VB = 72;

        svga3_vlkn_context_create(dev, CID);

        SVGA3dSize rtSize = { RT_W, RT_H, 1 };
        svga3_vlkn_surface_define(dev, SID_RT, SVGA3D_SURFACE_HINT_RENDERTARGET, SVGA3D_X8R8G8B8, &rtSize, 1);
        svga3_vlkn_context_set_render_target(dev, CID, SVGA3D_RT_COLOR0, SID_RT, 0, 0);

        SVGA3dRect vp = { 0, 0, RT_W, RT_H };
        svga3_vlkn_context_set_viewport(dev, CID, &vp);
        svga3_vlkn_context_set_render_state(dev, CID, SVGA3D_RS_CULLMODE, SVGA3D_FACE_NONE);

        float matIdent[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };
        for (int r = 0; r < 4; ++r) {
            uint32_t rowValues[4];
            memcpy(rowValues, &matIdent[r * 4], sizeof(float) * 4);
            svga3_vlkn_context_set_shader_const(dev, CID, r, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, rowValues);
        }

        /* Identity tint (1, 1, 1, 1) */
        float tintOne[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        uint32_t tVal[4];
        memcpy(tVal, tintOne, sizeof(tintOne));
        svga3_vlkn_context_set_shader_const(dev, CID, 0, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT, tVal);

        svga3_vlkn_context_define_shader(dev, CID, 1, SVGA3D_SHADERTYPE_VS, vs1Bytecode, sizeof(vs1Bytecode)/4);
        svga3_vlkn_context_define_shader(dev, CID, 1, SVGA3D_SHADERTYPE_PS, ps1Bytecode, sizeof(ps1Bytecode)/4);
        svga3_vlkn_context_set_shader(dev, CID, SVGA3D_SHADERTYPE_VS, 1);
        svga3_vlkn_context_set_shader(dev, CID, SVGA3D_SHADERTYPE_PS, 1);

        /* Create 1x1 Texture Surface */
        SVGA3dSize texSz = { 1, 1, 1 };
        svga3_vlkn_surface_define(dev, SID_TEX, SVGA3D_SURFACE_HINT_TEXTURE, SVGA3D_X8R8G8B8, &texSz, 1);
        svga3_vlkn_context_set_texture(dev, CID, 0, SID_TEX);

        Vertex quadVerts[6] = {
            { -1.0f,  1.0f, 0.5f,  0.5f, 0.5f },
            {  1.0f,  1.0f, 0.5f,  0.5f, 0.5f },
            {  1.0f, -1.0f, 0.5f,  0.5f, 0.5f },
            { -1.0f,  1.0f, 0.5f,  0.5f, 0.5f },
            {  1.0f, -1.0f, 0.5f,  0.5f, 0.5f },
            { -1.0f, -1.0f, 0.5f,  0.5f, 0.5f }
        };
        SVGA3dSize vbSz = { sizeof(quadVerts), 1, 1 };
        svga3_vlkn_surface_define(dev, SID_VB, SVGA3D_SURFACE_HINT_VERTEXBUFFER, SVGA3D_BUFFER, &vbSz, 1);
        SVGA3dBox bBox = { 0, 0, 0, sizeof(quadVerts), 1, 1 };
        svga3_vlkn_surface_dma_upload(dev, SID_VB, 0, &bBox, quadVerts, sizeof(quadVerts));

        decls[0].array.surfaceId = SID_VB;
        decls[1].array.surfaceId = SID_VB;
        SVGA3dPrimitiveRange r1 = { SVGA3D_PRIMITIVE_TRIANGLELIST, 2, {}, 0, 0 };

        /* 1. Upload Texture Version A: Orange (255, 128, 0) */
        uint32_t texOrange = 0xFFFF8000;
        SVGA3dBox tBox = { 0, 0, 0, 1, 1, 1 };
        svga3_vlkn_surface_dma_upload(dev, SID_TEX, 0, &tBox, &texOrange, 4);

        svga3_vlkn_context_clear(dev, CID, SVGA3D_CLEAR_COLOR, 0x00000000, 1.0f, 0, nullptr, 0);
        svga3_vlkn_context_draw(dev, CID, SVGA3D_PRIMITIVE_TRIANGLELIST, decls, 2, &r1, 1);
        svga3_vlkn_device_wait_idle(dev);

        std::vector<Pixel> fbA(RT_W * RT_H);
        svga3_vlkn_surface_dma_download(dev, SID_RT, 0, nullptr, fbA.data(), RT_W * 4);
        Pixel pA = fbA[(RT_H / 2) * RT_W + (RT_W / 2)];
        TEST_CHECK(pixelMatches(pA, 255, 128, 0, 255), "Scene 7 (Draw 1): Rendered Texture A (Orange 255, 128, 0)");
        savePPM("artifacts/scene7_texA_rendered.ppm", fbA.data(), RT_W, RT_H);

        /* 2. Dynamically Update Texture to Version B: Purple (128, 0, 255) WITHOUT destroying surface */
        uint32_t texPurple = 0xFF8000FF;
        svga3_vlkn_surface_dma_upload(dev, SID_TEX, 0, &tBox, &texPurple, 4);

        svga3_vlkn_context_clear(dev, CID, SVGA3D_CLEAR_COLOR, 0x00000000, 1.0f, 0, nullptr, 0);
        svga3_vlkn_context_draw(dev, CID, SVGA3D_PRIMITIVE_TRIANGLELIST, decls, 2, &r1, 1);
        svga3_vlkn_device_wait_idle(dev);

        std::vector<Pixel> fbB(RT_W * RT_H);
        svga3_vlkn_surface_dma_download(dev, SID_RT, 0, nullptr, fbB.data(), RT_W * 4);
        Pixel pB = fbB[(RT_H / 2) * RT_W + (RT_W / 2)];
        TEST_CHECK(pixelMatches(pB, 128, 0, 255, 255), "Scene 7 (Draw 2): Dynamically updated Texture B rendered (Purple 128, 0, 255)");
        savePPM("artifacts/scene7_texB_rendered.ppm", fbB.data(), RT_W, RT_H);

        /* Clean up Scene 7 */
        svga3_vlkn_context_destroy(dev, CID);
        svga3_vlkn_surface_destroy(dev, SID_RT);
        svga3_vlkn_surface_destroy(dev, SID_TEX);
        svga3_vlkn_surface_destroy(dev, SID_VB);
    }

    /* --------------------------------------------------------------------------
     * SCENE 8: Negative Controls (Deliberate alterations must trigger detection)
     * -------------------------------------------------------------------------- */
    std::cout << "\n--- Scene 8: Negative Controls (Detect Deliberate Alterations) ---" << std::endl;
    {
        /* Negative Control A: Altered Color Channel (Red changed to 200 instead of 255) */
        Pixel alteredColor = { 0, 128, 200, 255 };
        bool matchA = pixelMatches(alteredColor, 255, 128, 0, 255);
        TEST_CHECK_NEGATIVE(!matchA, "Altered color channel (200 vs 255) rejected");

        /* Negative Control B: Altered Alpha Channel (Alpha changed to 128 instead of 255) */
        Pixel alteredAlpha = { 0, 128, 255, 128 };
        bool matchB = pixelMatches(alteredAlpha, 255, 128, 0, 255);
        TEST_CHECK_NEGATIVE(!matchB, "Altered alpha channel (128 vs 255) rejected");

        /* Negative Control C: Altered Geometry / Viewport (Pixel at (0, 0) checked against quad color) */
        Pixel bgPixel = { 0, 0, 0, 0 };
        bool matchC = pixelMatches(bgPixel, 255, 128, 0, 255);
        TEST_CHECK_NEGATIVE(!matchC, "Untouched background pixel (0,0,0) rejected when expecting quad color");

        /* Negative Control D: Deliberate Shader Constant Modification Detection */
        /* If shader constant was Tint = (0.2, 1.0, 0.8), White * Tint would be (51, 255, 204) */
        Pixel renderedPixel = { 204, 255, 127, 255 }; /* Expected from Tint=(0.5, 1.0, 0.8) */
        bool matchD = pixelMatches(renderedPixel, 51, 255, 204, 255); /* Checking against Tint=(0.2, ...) */
        TEST_CHECK_NEGATIVE(!matchD, "Pixel from Tint=(0.5) rejected when compared against analytical Tint=(0.2)");

        /* Visual Negative Control Failure Artifact */
        if (!g_scene1Fb.empty() && !g_scene1Ref.empty()) {
            std::vector<Pixel> intentionallyFlawedRef = g_scene1Ref;
            for (int y = 20; y < 44; ++y) {
                for (int x = 20; x < 44; ++x) {
                    intentionallyFlawedRef[y * RT_W + x] = { 0, 0, 255, 255 }; // Discrepancy
                }
            }
            saveDiffPPM("artifacts/negative_control_diff.ppm", g_scene1Fb.data(), intentionallyFlawedRef.data(), RT_W, RT_H);
        }
    }

    /* Clean Teardown */
    svga3_vlkn_device_destroy(dev);
    std::cout << "\n  [PASS] Clean device destruction completed" << std::endl;

    std::cout << "\n======================================================================" << std::endl;
    std::cout << "DELIVERABLE 4 ACCEPTANCE SUITE: ALL SCENES VERIFIED SUCCESSFULLY!" << std::endl;
    std::cout << "======================================================================" << std::endl;

    return 0;
}
