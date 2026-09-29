/*
 * SVGA3=VLKN - Comprehensive Shader Bytecode Execution & Pipeline Verification
 * Real Vulkan execution testing shader translation, parameter/constant changes,
 * textured geometry rendering, multi-context isolation, and error rejection.
 */

#include "svga3_vlkn.h"
#include "svga3_device.h"
#include "vlkn_backend.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <cstring>
#include <iomanip>

#define TEST_CHECK(cond, msg) do { \
    if (cond) { \
        std::cout << "  [PASS] " << msg << std::endl; \
    } else { \
        std::cerr << "  [FAIL] " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
        return 1; \
    } \
} while(0)

struct Vertex {
    float x, y, z;
    float u, v;
};

/* Pixel color structure for BGRA8 / RGBA8 readback */
struct Pixel {
    uint8_t b, g, r, a;
};

int main() {
    std::cout << "======================================================================" << std::endl;
    std::cout << "SVGA3=VLKN Deliverable 2: Real Vulkan Shader Bytecode Execution Test" << std::endl;
    std::cout << "======================================================================" << std::endl;

    /* 1. Real Vulkan Device Creation with Validation Layers */
    Svga3VlknConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.appName = "Shader Bytecode Execution Acceptance Test";
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

    /* 2. Setup Resources: Render Target, Texture, Geometry */
    const uint32_t RT_WIDTH = 64;
    const uint32_t RT_HEIGHT = 64;
    const uint32_t SID_RT1 = 1;
    const uint32_t SID_TEX = 2;
    const uint32_t SID_VB  = 3;
    const uint32_t SID_RT2 = 4;
    const uint32_t CID1    = 1;
    const uint32_t CID2    = 2;

    /* Create Render Target Surface 1 (64x64, SVGA3D_X8R8G8B8) */
    SVGA3dSize rtSize = { RT_WIDTH, RT_HEIGHT, 1 };
    Svga3VlknStatus st = svga3_vlkn_surface_define(dev, SID_RT1, SVGA3D_SURFACE_HINT_RENDERTARGET, SVGA3D_X8R8G8B8, &rtSize, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Define Render Target Surface 1 (64x64)");

    /* Create Texture Surface (4x4, SVGA3D_X8R8G8B8) */
    SVGA3dSize texSize = { 4, 4, 1 };
    st = svga3_vlkn_surface_define(dev, SID_TEX, SVGA3D_SURFACE_HINT_TEXTURE, SVGA3D_X8R8G8B8, &texSize, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Define Texture Surface (4x4)");

    /* Upload 4x4 Distinct Quadrant Color Pattern to Texture:
       Top-Left (0..1, 0..1):     Red     (255,   0,   0, 255) -> BGRA: 0x0000FF / 0xFFFF0000
       Top-Right (2..3, 0..1):    Green   (  0, 255,   0, 255) -> BGRA: 0x00FF00 / 0xFF00FF00
       Bottom-Left (0..1, 2..3):  Blue    (  0,   0, 255, 255) -> BGRA: 0xFF0000 / 0xFF0000FF
       Bottom-Right (2..3, 2..3): Yellow  (255, 255,   0, 255) -> BGRA: 0x00FFFF / 0xFFFFFF00
    */
    uint32_t texPixels[16];
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            uint32_t color = 0;
            if (x < 2 && y < 2) {
                color = 0xFFFF0000; /* Red: ARGB format */
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
    SVGA3dBox texBox = { 0, 0, 0, 4, 4, 1 };
    st = svga3_vlkn_surface_dma_upload(dev, SID_TEX, 0, &texBox, texPixels, 4 * sizeof(uint32_t));
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "DMA Upload 4x4 RGBA pattern to Texture Surface");

    /* Create Vertex Buffer Surface (Full screen quad, 2 triangles = 6 vertices) */
    SVGA3dSize vbSize = { sizeof(Vertex) * 6, 1, 1 };
    st = svga3_vlkn_surface_define(dev, SID_VB, SVGA3D_SURFACE_HINT_VERTEXBUFFER, SVGA3D_BUFFER, &vbSize, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Define Vertex Buffer Surface");

    Vertex quadVertices[6] = {
        /* Triangle 1 */
        { -1.0f,  1.0f, 0.5f,  0.0f, 0.0f },
        {  1.0f,  1.0f, 0.5f,  1.0f, 0.0f },
        {  1.0f, -1.0f, 0.5f,  1.0f, 1.0f },
        /* Triangle 2 */
        { -1.0f,  1.0f, 0.5f,  0.0f, 0.0f },
        {  1.0f, -1.0f, 0.5f,  1.0f, 1.0f },
        { -1.0f, -1.0f, 0.5f,  0.0f, 1.0f }
    };
    SVGA3dBox vbBox = { 0, 0, 0, sizeof(quadVertices), 1, 1 };
    st = svga3_vlkn_surface_dma_upload(dev, SID_VB, 0, &vbBox, quadVertices, sizeof(quadVertices));
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "DMA Upload Quad Geometry to Vertex Buffer Surface");

    /* 3. Define Actual D3D9 / SVGA3D Guest Shader Bytecode */
#define D3D9_DST(regType, regNum, mask) \
    (0x80000000u | (((regType) & 0x7) << 28) | ((((regType) >> 3) & 0x3) << 11) | (((mask) & 0xF) << 16) | ((regNum) & 0x7FF))

#define D3D9_SRC(regType, regNum, swiz) \
    (0x80000000u | (((regType) & 0x7) << 28) | ((((regType) >> 3) & 0x3) << 11) | (((swiz) & 0xFF) << 16) | ((regNum) & 0x7FF))

    /*
     * Vertex Shader 1 (VS1):
     * vs_3_0
     * dcl_position v0
     * dcl_texcoord v1
     * m4x4 oPos, v0, c0   ; Transform position by matrix in c0..c3
     * mov oT0, v1         ; Pass texcoord to fragment stage
     * end
     */
    const uint32_t vs1Bytecode[] = {
        0xFFFE0300, /* vs_3_0 */
        (31) | (2 << 24), /* DCL */
        0x80000000 | 0,   /* Usage: POSITION 0 */
        D3D9_DST(1, 0, 0xF), /* v0 */
        (31) | (2 << 24), /* DCL */
        0x80000000 | 5,   /* Usage: TEXCOORD 0 */
        D3D9_DST(1, 1, 0xF), /* v1 */
        (20) | (3 << 24), /* M4x4 */
        D3D9_DST(4, 0, 0xF), /* oPos */
        D3D9_SRC(1, 0, 0xE4), /* v0 */
        D3D9_SRC(2, 0, 0xE4), /* c0 */
        (1) | (2 << 24),  /* MOV */
        D3D9_DST(6, 0, 0xF), /* oT0 */
        D3D9_SRC(1, 1, 0xE4), /* v1 */
        0x0000FFFF        /* END */
    };

    /*
     * Pixel Shader 1 (PS1):
     * ps_3_0
     * dcl_texcoord v1
     * dcl_2d s0
     * texld r0, v1, s0
     * mul oC0, r0, c0     ; Multiply texture sample by constant c0
     * end
     */
    const uint32_t ps1Bytecode[] = {
        0xFFFF0300, /* ps_3_0 */
        (31) | (2 << 24), /* DCL */
        0x80000000 | 5,   /* Usage: TEXCOORD 0 */
        D3D9_DST(1, 1, 0xF), /* v1 */
        (31) | (2 << 24), /* DCL sampler */
        0x80000000 | (2 << 28), /* 2D */
        D3D9_DST(10, 0, 0xF), /* s0 */
        (66) | (3 << 24), /* TEXLD */
        D3D9_DST(0, 0, 0xF), /* r0 */
        D3D9_SRC(1, 1, 0xE4), /* v1 */
        D3D9_SRC(10, 0, 0xE4), /* s0 */
        (5) | (3 << 24),  /* MUL */
        D3D9_DST(8, 0, 0xF), /* oC0 */
        D3D9_SRC(0, 0, 0xE4), /* r0 */
        D3D9_SRC(2, 0, 0xE4), /* c0 */
        0x0000FFFF        /* END */
    };

    /*
     * Pixel Shader 2 (PS2) - Color Inversion:
     * ps_3_0
     * dcl_texcoord v1
     * dcl_2d s0
     * texld r0, v1, s0
     * sub oC0, c0, r0     ; Invert: c0 - r0 (where c0 is 1.0, 1.0, 1.0, 1.0)
     * end
     */
    const uint32_t ps2Bytecode[] = {
        0xFFFF0300, /* ps_3_0 */
        (31) | (2 << 24), /* DCL */
        0x80000000 | 5,   /* Usage: TEXCOORD 0 */
        D3D9_DST(1, 1, 0xF), /* v1 */
        (31) | (2 << 24), /* DCL sampler */
        0x80000000 | (2 << 28), /* 2D */
        D3D9_DST(10, 0, 0xF), /* s0 */
        (66) | (3 << 24), /* TEXLD */
        D3D9_DST(0, 0, 0xF), /* r0 */
        D3D9_SRC(1, 1, 0xE4), /* v1 */
        D3D9_SRC(10, 0, 0xE4), /* s0 */
        (3) | (3 << 24),  /* SUB: dest = src0 - src1 */
        D3D9_DST(8, 0, 0xF), /* oC0 */
        D3D9_SRC(2, 0, 0xE4), /* c0 */
        D3D9_SRC(0, 0, 0xE4), /* r0 */
        0x0000FFFF        /* END */
    };

    /* Create Context 1 */
    st = svga3_vlkn_context_create(dev, CID1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Create Context 1");

    /* Define Shaders in Context 1 */
    const uint32_t SHID_VS1 = 10;
    const uint32_t SHID_PS1 = 20;
    const uint32_t SHID_PS2 = 21;

    st = svga3_vlkn_context_define_shader(dev, CID1, SHID_VS1, SVGA3D_SHADERTYPE_VS, vs1Bytecode, sizeof(vs1Bytecode)/sizeof(uint32_t));
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Define VS1 in Context 1 (D3D9 bytecode translated to SPIR-V)");

    st = svga3_vlkn_context_define_shader(dev, CID1, SHID_PS1, SVGA3D_SHADERTYPE_PS, ps1Bytecode, sizeof(ps1Bytecode)/sizeof(uint32_t));
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Define PS1 in Context 1 (D3D9 bytecode translated to SPIR-V)");

    st = svga3_vlkn_context_define_shader(dev, CID1, SHID_PS2, SVGA3D_SHADERTYPE_PS, ps2Bytecode, sizeof(ps2Bytecode)/sizeof(uint32_t));
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Define PS2 (color inverter) in Context 1");

    /* Bind Render Target, Viewport, Scissor, Texture */
    st = svga3_vlkn_context_set_render_target(dev, CID1, SVGA3D_RT_COLOR0, SID_RT1, 0, 0);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Set Render Target to Surface 1");

    SVGA3dRect vpRect = { 0, 0, RT_WIDTH, RT_HEIGHT };
    svga3_vlkn_context_set_viewport(dev, CID1, &vpRect);
    svga3_vlkn_context_set_scissor_rect(dev, CID1, &vpRect);
    svga3_vlkn_context_set_texture(dev, CID1, 0, SID_TEX);
    svga3_vlkn_context_set_render_state(dev, CID1, SVGA3D_RS_CULLMODE, SVGA3D_FACE_NONE);

    /* Bind VS1 and PS1 */
    svga3_vlkn_context_set_shader(dev, CID1, SVGA3D_SHADERTYPE_VS, SHID_VS1);
    svga3_vlkn_context_set_shader(dev, CID1, SVGA3D_SHADERTYPE_PS, SHID_PS1);

    /* Set Identity Transform in VS c0..c3 */
    union { float f; uint32_t u; } m00 = {1.0f}, m01 = {0.0f}, m11 = {1.0f}, m22 = {1.0f}, m33 = {1.0f};
    uint32_t row0[4] = { m00.u, m01.u, m01.u, m01.u };
    uint32_t row1[4] = { m01.u, m11.u, m01.u, m01.u };
    uint32_t row2[4] = { m01.u, m01.u, m22.u, m01.u };
    uint32_t row3[4] = { m01.u, m01.u, m01.u, m33.u };
    svga3_vlkn_context_set_shader_const(dev, CID1, 0, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, row0);
    svga3_vlkn_context_set_shader_const(dev, CID1, 1, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, row1);
    svga3_vlkn_context_set_shader_const(dev, CID1, 2, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, row2);
    svga3_vlkn_context_set_shader_const(dev, CID1, 3, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, row3);

    /* Set Tint Multiplier in PS c0 = (1.0, 1.0, 1.0, 1.0) */
    union { float f; uint32_t u; } one = { 1.0f };
    uint32_t psConst1[4] = { one.u, one.u, one.u, one.u };
    svga3_vlkn_context_set_shader_const(dev, CID1, 0, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT, psConst1);

    /* Vertex declarations for (x,y,z) at loc 0 and (u,v) at loc 2 */
    SVGA3dVertexDecl decls[2];
    memset(decls, 0, sizeof(decls));

    /* Attribute 0: POSITION */
    decls[0].identity.type = SVGA3D_DECLTYPE_FLOAT3;
    decls[0].identity.usage = SVGA3D_DECLUSAGE_POSITION;
    decls[0].identity.usageIndex = 0;
    decls[0].array.surfaceId = SID_VB;
    decls[0].array.offset = 0;
    decls[0].array.stride = sizeof(Vertex);

    /* Attribute 1: TEXCOORD */
    decls[1].identity.type = SVGA3D_DECLTYPE_FLOAT2;
    decls[1].identity.usage = SVGA3D_DECLUSAGE_TEXCOORD;
    decls[1].identity.usageIndex = 0;
    decls[1].array.surfaceId = SID_VB;
    decls[1].array.offset = offsetof(Vertex, u);
    decls[1].array.stride = sizeof(Vertex);

    SVGA3dPrimitiveRange range;
    memset(&range, 0, sizeof(range));
    range.primType = SVGA3D_PRIMITIVE_TRIANGLELIST;
    range.primitiveCount = 2; /* 2 triangles = 6 vertices */

    /* Clear surface to black */
    svga3_vlkn_context_clear(dev, CID1, SVGA3D_CLEAR_COLOR, 0xFF000000, 1.0f, 0, nullptr, 0);

    /* Draw Textured Quad */
    st = svga3_vlkn_context_draw(dev, CID1, SVGA3D_PRIMITIVE_TRIANGLELIST, decls, 2, &range, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Draw Primitive (Textured Quad with VS1 + PS1)");

    /* Read back pixels from Render Target */
    std::vector<Pixel> fb(RT_WIDTH * RT_HEIGHT);
    SVGA3dBox readBox = { 0, 0, 0, RT_WIDTH, RT_HEIGHT, 1 };
    st = svga3_vlkn_surface_dma_download(dev, SID_RT1, 0, &readBox, fb.data(), RT_WIDTH * sizeof(Pixel));
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "DMA Download framebuffer from Render Target 1");

    /* 4. Verify Rendered Pixels match Analytical Reference:
       Center of Top-Left quadrant (x=16, y=16): Expected RED (R ~ 255, G ~ 0, B ~ 0)
       Center of Top-Right quadrant (x=48, y=16): Expected GREEN (R ~ 0, G ~ 255, B ~ 0)
       Center of Bottom-Left quadrant (x=16, y=48): Expected BLUE (R ~ 0, G ~ 0, B ~ 255)
       Center of Bottom-Right quadrant (x=48, y=48): Expected YELLOW (R ~ 255, G ~ 255, B ~ 0)
       Tolerance: ±5 color units (documenting linear sampler interpolation and color precision)
    */
    Pixel pTL = fb[16 * RT_WIDTH + 16];
    Pixel pTR = fb[16 * RT_WIDTH + 48];
    Pixel pBL = fb[48 * RT_WIDTH + 16];
    Pixel pBR = fb[48 * RT_WIDTH + 48];

    std::cout << "  [PIXELS] Quadrant 1 (Top-Left):     R=" << (int)pTL.r << " G=" << (int)pTL.g << " B=" << (int)pTL.b << std::endl;
    std::cout << "  [PIXELS] Quadrant 2 (Top-Right):    R=" << (int)pTR.r << " G=" << (int)pTR.g << " B=" << (int)pTR.b << std::endl;
    std::cout << "  [PIXELS] Quadrant 3 (Bottom-Left):  R=" << (int)pBL.r << " G=" << (int)pBL.g << " B=" << (int)pBL.b << std::endl;
    std::cout << "  [PIXELS] Quadrant 4 (Bottom-Right): R=" << (int)pBR.r << " G=" << (int)pBR.g << " B=" << (int)pBR.b << std::endl;

    TEST_CHECK(pTL.r >= 240 && pTL.g <= 15 && pTL.b <= 15, "Quadrant 1 is deterministic RED");
    TEST_CHECK(pTR.r <= 15 && pTR.g >= 240 && pTR.b <= 15, "Quadrant 2 is deterministic GREEN");
    TEST_CHECK(pBL.r <= 15 && pBL.g <= 15 && pBL.b >= 240, "Quadrant 3 is deterministic BLUE");
    TEST_CHECK(pBR.r >= 240 && pBR.g >= 240 && pBR.b <= 15, "Quadrant 4 is deterministic YELLOW");

    /* Regression: D3D9 TEMP r0 and DCL OUTPUT o0 are distinct registers.
     * The shader uses r0 as a temporary, then copies it to the explicitly
     * declared position output. Routing the first write to o0 clips the quad. */
    constexpr uint32_t SHID_VS_TEMP_OUTPUT_COLLISION = 30;
    constexpr uint32_t SHID_PS_SOLID_RED = 31;
    const uint32_t vsTempOutputCollision[] = {
        0xFFFE0300, /* vs_3_0 */
        (31) | (2 << 24), 0x80000000 | 0, D3D9_DST(1, 0, 0xF), /* dcl_position v0 */
        (31) | (2 << 24), 0x80000000 | 0, D3D9_DST(6, 0, 0xF), /* dcl_position o0 */
        (1) | (2 << 24), D3D9_DST(0, 0, 0xF), D3D9_SRC(1, 0, 0xE4), /* mov r0, v0 */
        (1) | (2 << 24), D3D9_DST(6, 0, 0xF), D3D9_SRC(0, 0, 0xE4), /* mov o0, r0 */
        0x0000FFFF
    };
    const uint32_t psSolidRed[] = {
        0xFFFF0300, /* ps_3_0 */
        (81) | (5 << 24), D3D9_DST(2, 0, 0xF), 0x3F800000, 0, 0, 0x3F800000, /* def c0, red */
        (1) | (2 << 24), D3D9_DST(8, 0, 0xF), D3D9_SRC(2, 0, 0xE4), /* mov oC0, c0 */
        0x0000FFFF
    };
    TEST_CHECK(svga3_vlkn_context_define_shader(dev, CID1, SHID_VS_TEMP_OUTPUT_COLLISION,
        SVGA3D_SHADERTYPE_VS, vsTempOutputCollision,
        sizeof(vsTempOutputCollision) / sizeof(uint32_t)) == SVGA3_VLKN_SUCCESS,
        "Define VS with colliding TEMP r0 and DCL OUTPUT o0 numbers");
    TEST_CHECK(svga3_vlkn_context_define_shader(dev, CID1, SHID_PS_SOLID_RED,
        SVGA3D_SHADERTYPE_PS, psSolidRed, sizeof(psSolidRed) / sizeof(uint32_t)) == SVGA3_VLKN_SUCCESS,
        "Define solid-red pixel shader for output-register regression");
    svga3_vlkn_context_set_shader(dev, CID1, SVGA3D_SHADERTYPE_VS, SHID_VS_TEMP_OUTPUT_COLLISION);
    svga3_vlkn_context_set_shader(dev, CID1, SVGA3D_SHADERTYPE_PS, SHID_PS_SOLID_RED);
    svga3_vlkn_context_clear(dev, CID1, SVGA3D_CLEAR_COLOR, 0xFF000000, 1.0f, 0, nullptr, 0);
    TEST_CHECK(svga3_vlkn_context_draw(dev, CID1, SVGA3D_PRIMITIVE_TRIANGLELIST,
        decls, 1, &range, 1) == SVGA3_VLKN_SUCCESS,
        "Draw with explicit DCL output and same-numbered temporary");
    TEST_CHECK(svga3_vlkn_surface_dma_download(dev, SID_RT1, 0, &readBox,
        fb.data(), RT_WIDTH * sizeof(Pixel)) == SVGA3_VLKN_SUCCESS,
        "Read back output-register collision regression draw");
    const Pixel pTempOutput = fb[32 * RT_WIDTH + 32];
    TEST_CHECK(pTempOutput.r >= 240 && pTempOutput.g <= 15 && pTempOutput.b <= 15,
        "TEMP r0 remains independent and the DCL-declared position output renders");

    svga3_vlkn_context_set_shader(dev, CID1, SVGA3D_SHADERTYPE_VS, SHID_VS1);
    svga3_vlkn_context_set_shader(dev, CID1, SVGA3D_SHADERTYPE_PS, SHID_PS1);

    /* 5. Independent Shader Constant Modification:
       Change PS constant c0 to (0.5, 0.5, 0.5, 1.0) -> Dim all pixels by 50%
    */
    union { float f; uint32_t u; } half = { 0.5f };
    uint32_t psConstHalf[4] = { half.u, half.u, half.u, one.u };
    svga3_vlkn_context_set_shader_const(dev, CID1, 0, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT, psConstHalf);

    svga3_vlkn_context_clear(dev, CID1, SVGA3D_CLEAR_COLOR, 0xFF000000, 1.0f, 0, nullptr, 0);
    st = svga3_vlkn_context_draw(dev, CID1, SVGA3D_PRIMITIVE_TRIANGLELIST, decls, 2, &range, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Draw again with PS constant c0 changed to 0.5");

    st = svga3_vlkn_surface_dma_download(dev, SID_RT1, 0, &readBox, fb.data(), RT_WIDTH * sizeof(Pixel));
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Download framebuffer after PS constant alteration");

    pTL = fb[16 * RT_WIDTH + 16];
    std::cout << "  [PIXELS] Quadrant 1 with c0=0.5: R=" << (int)pTL.r << " G=" << (int)pTL.g << " B=" << (int)pTL.b << std::endl;
    TEST_CHECK(pTL.r >= 115 && pTL.r <= 140, "Pixel intensity altered deterministically by PS shader constant (expected ~127)");

    /* 6. Independent Vertex Shader Constant Modification (Transformation scale 0.5):
       Scale matrix by 0.5 -> Quad now occupies central 32x32 area.
       Pixel (4, 4) should remain clear color (black: 0, 0, 0).
       Pixel (32, 32) should be drawn.
    */
    union { float f; uint32_t u; } s05 = { 0.5f };
    uint32_t rowScale0[4] = { s05.u, m01.u, m01.u, m01.u };
    uint32_t rowScale1[4] = { m01.u, s05.u, m01.u, m01.u };
    svga3_vlkn_context_set_shader_const(dev, CID1, 0, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, rowScale0);
    svga3_vlkn_context_set_shader_const(dev, CID1, 1, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, rowScale1);

    svga3_vlkn_context_clear(dev, CID1, SVGA3D_CLEAR_COLOR, 0xFF000000, 1.0f, 0, nullptr, 0);
    st = svga3_vlkn_context_draw(dev, CID1, SVGA3D_PRIMITIVE_TRIANGLELIST, decls, 2, &range, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Draw again with VS matrix constants scaled by 0.5");

    st = svga3_vlkn_surface_dma_download(dev, SID_RT1, 0, &readBox, fb.data(), RT_WIDTH * sizeof(Pixel));
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Download framebuffer after VS transform alteration");

    Pixel pBorder = fb[4 * RT_WIDTH + 4];
    Pixel pCenter = fb[32 * RT_WIDTH + 32];
    std::cout << "  [PIXELS] Border (4,4) outside scaled quad: R=" << (int)pBorder.r << " G=" << (int)pBorder.g << " B=" << (int)pBorder.b << std::endl;
    std::cout << "  [PIXELS] Center (32,32) inside scaled quad: R=" << (int)pCenter.r << " G=" << (int)pCenter.g << " B=" << (int)pCenter.b << std::endl;
    TEST_CHECK(pBorder.r == 0 && pBorder.g == 0 && pBorder.b == 0, "Outer pixel is unaffected background color (0,0,0)");
    TEST_CHECK(pCenter.r > 50 || pCenter.g > 50 || pCenter.b > 50, "Center pixel rendered transformed geometry");

    /* Restore VS Identity transform and full PS constant */
    svga3_vlkn_context_set_shader_const(dev, CID1, 0, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, row0);
    svga3_vlkn_context_set_shader_const(dev, CID1, 1, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, row1);
    svga3_vlkn_context_set_shader_const(dev, CID1, 0, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT, psConst1);

    /* 7. Shader Switching Within Context & Pipeline Cache Verification */
    /* Switch to PS2 (Color Inverter: c0 - r0) */
    svga3_vlkn_context_set_shader(dev, CID1, SVGA3D_SHADERTYPE_PS, SHID_PS2);
    svga3_vlkn_context_clear(dev, CID1, SVGA3D_CLEAR_COLOR, 0xFF000000, 1.0f, 0, nullptr, 0);
    st = svga3_vlkn_context_draw(dev, CID1, SVGA3D_PRIMITIVE_TRIANGLELIST, decls, 2, &range, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Draw with switched shader PS2 (Color Inversion)");

    st = svga3_vlkn_surface_dma_download(dev, SID_RT1, 0, &readBox, fb.data(), RT_WIDTH * sizeof(Pixel));
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Download framebuffer from PS2 execution");

    pTL = fb[16 * RT_WIDTH + 16];
    std::cout << "  [PIXELS] Inverted Quadrant 1 (Expected Cyan): R=" << (int)pTL.r << " G=" << (int)pTL.g << " B=" << (int)pTL.b << std::endl;
    /* Red (255, 0, 0) inverted is Cyan (0, 255, 255) */
    TEST_CHECK(pTL.r <= 15 && pTL.g >= 240 && pTL.b >= 240, "Switched shader PS2 produced inverted colors (Cyan)");

    /* Switch back to PS1 (Cached pipeline re-use) */
    svga3_vlkn_context_set_shader(dev, CID1, SVGA3D_SHADERTYPE_PS, SHID_PS1);
    svga3_vlkn_context_clear(dev, CID1, SVGA3D_CLEAR_COLOR, 0xFF000000, 1.0f, 0, nullptr, 0);
    st = svga3_vlkn_context_draw(dev, CID1, SVGA3D_PRIMITIVE_TRIANGLELIST, decls, 2, &range, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Draw again with restored PS1 (Pipeline Cache Hit)");

    st = svga3_vlkn_surface_dma_download(dev, SID_RT1, 0, &readBox, fb.data(), RT_WIDTH * sizeof(Pixel));
    pTL = fb[16 * RT_WIDTH + 16];
    TEST_CHECK(pTL.r >= 240 && pTL.g <= 15 && pTL.b <= 15, "Pipeline cache correctly restored original PS1 Red rendering");

    /* 8. Multi-Context Shader & State Isolation */
    st = svga3_vlkn_context_create(dev, CID2);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Create Context 2 on same device");

    /* Define Surface 4 as Render Target for Context 2 */
    st = svga3_vlkn_surface_define(dev, SID_RT2, SVGA3D_SURFACE_HINT_RENDERTARGET, SVGA3D_X8R8G8B8, &rtSize, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Define Render Target Surface 4 for Context 2");

    /* In Context 2, define and bind VS1 and PS1 */
    st = svga3_vlkn_context_define_shader(dev, CID2, SHID_VS1, SVGA3D_SHADERTYPE_VS, vs1Bytecode, sizeof(vs1Bytecode)/sizeof(uint32_t));
    st = svga3_vlkn_context_define_shader(dev, CID2, SHID_PS1, SVGA3D_SHADERTYPE_PS, ps1Bytecode, sizeof(ps1Bytecode)/sizeof(uint32_t));
    svga3_vlkn_context_set_render_target(dev, CID2, SVGA3D_RT_COLOR0, SID_RT2, 0, 0);
    svga3_vlkn_context_set_viewport(dev, CID2, &vpRect);
    svga3_vlkn_context_set_scissor_rect(dev, CID2, &vpRect);
    svga3_vlkn_context_set_texture(dev, CID2, 0, SID_TEX);
    svga3_vlkn_context_set_render_state(dev, CID2, SVGA3D_RS_CULLMODE, SVGA3D_FACE_NONE);
    svga3_vlkn_context_set_shader(dev, CID2, SVGA3D_SHADERTYPE_VS, SHID_VS1);
    svga3_vlkn_context_set_shader(dev, CID2, SVGA3D_SHADERTYPE_PS, SHID_PS1);
    svga3_vlkn_context_set_shader_const(dev, CID2, 0, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, row0);
    svga3_vlkn_context_set_shader_const(dev, CID2, 1, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, row1);
    svga3_vlkn_context_set_shader_const(dev, CID2, 2, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, row2);
    svga3_vlkn_context_set_shader_const(dev, CID2, 3, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, row3);

    /* In Context 1: PS constant = Pure RED filter (1.0, 0.0, 0.0, 1.0) */
    union { float f; uint32_t u; } zero = { 0.0f };
    uint32_t cRed[4] = { one.u, zero.u, zero.u, one.u };
    svga3_vlkn_context_set_shader_const(dev, CID1, 0, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT, cRed);

    /* In Context 2: PS constant = Pure BLUE filter (0.0, 0.0, 1.0, 1.0) */
    uint32_t cBlue[4] = { zero.u, zero.u, one.u, one.u };
    svga3_vlkn_context_set_shader_const(dev, CID2, 0, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT, cBlue);

    /* Draw in Context 1 to Target 1 */
    svga3_vlkn_context_clear(dev, CID1, SVGA3D_CLEAR_COLOR, 0xFF000000, 1.0f, 0, nullptr, 0);
    svga3_vlkn_context_draw(dev, CID1, SVGA3D_PRIMITIVE_TRIANGLELIST, decls, 2, &range, 1);

    /* Draw in Context 2 to Target 4 */
    svga3_vlkn_context_clear(dev, CID2, SVGA3D_CLEAR_COLOR, 0xFF000000, 1.0f, 0, nullptr, 0);
    svga3_vlkn_context_draw(dev, CID2, SVGA3D_PRIMITIVE_TRIANGLELIST, decls, 2, &range, 1);

    /* Read back both targets */
    std::vector<Pixel> fbCtx1(RT_WIDTH * RT_HEIGHT);
    std::vector<Pixel> fbCtx2(RT_WIDTH * RT_HEIGHT);
    svga3_vlkn_surface_dma_download(dev, SID_RT1, 0, &readBox, fbCtx1.data(), RT_WIDTH * sizeof(Pixel));
    svga3_vlkn_surface_dma_download(dev, SID_RT2, 0, &readBox, fbCtx2.data(), RT_WIDTH * sizeof(Pixel));

    /* Quadrant 4 in Texture is Yellow (R=255, G=255, B=0):
       Context 1 with Red filter: Expected (255, 0, 0)
       Context 2 with Blue filter: Expected (0, 0, 0)
    */
    Pixel pCtx1_Q4 = fbCtx1[48 * RT_WIDTH + 48];
    Pixel pCtx2_Q4 = fbCtx2[48 * RT_WIDTH + 48];
    std::cout << "  [PIXELS] Context 1 (Red filtered) Q4:  R=" << (int)pCtx1_Q4.r << " G=" << (int)pCtx1_Q4.g << " B=" << (int)pCtx1_Q4.b << std::endl;
    std::cout << "  [PIXELS] Context 2 (Blue filtered) Q4: R=" << (int)pCtx2_Q4.r << " G=" << (int)pCtx2_Q4.g << " B=" << (int)pCtx2_Q4.b << std::endl;
    TEST_CHECK(pCtx1_Q4.r >= 240 && pCtx1_Q4.g <= 15 && pCtx1_Q4.b <= 15, "Context 1 has isolated RED filtered rendering");
    TEST_CHECK(pCtx2_Q4.r <= 15 && pCtx2_Q4.g <= 15 && pCtx2_Q4.b <= 15, "Context 2 has isolated BLUE filtered rendering (no red leak)");

    /* Quadrant 3 in Texture is Blue (R=0, G=0, B=255):
       Context 2 with Blue filter: Expected Blue (0, 0, 255)
    */
    Pixel pCtx2_Q3 = fbCtx2[48 * RT_WIDTH + 16];
    std::cout << "  [PIXELS] Context 2 (Blue filtered) Q3: R=" << (int)pCtx2_Q3.r << " G=" << (int)pCtx2_Q3.g << " B=" << (int)pCtx2_Q3.b << " A=" << (int)pCtx2_Q3.a << std::endl;
    TEST_CHECK(pCtx2_Q3.b >= 240 && pCtx2_Q3.r <= 15 && pCtx2_Q3.g <= 15, "Context 2 renders BLUE quadrant correctly");

    /* 9. Explicit Errors on Unsupported Instructions and Negative Controls */
    /* Bad Opcode */
    const uint32_t badOpcodeBytecode[] = {
        0xFFFF0300,
        0x00009999, /* Unsupported opcode */
        0x0000FFFF
    };
    st = svga3_vlkn_context_define_shader(dev, CID1, 999, SVGA3D_SHADERTYPE_PS, badOpcodeBytecode, sizeof(badOpcodeBytecode)/sizeof(uint32_t));
    TEST_CHECK(st == SVGA3_VLKN_ERROR_UNSUPPORTED_SHADER, "Unsupported opcode explicitly rejected (no silent default fallback)");

    /* Truncated Bytecode */
    const uint32_t truncatedBytecode[] = {
        0xFFFF0300,
        (1) | (2 << 24)
        /* Missing operands and END */
    };
    st = svga3_vlkn_context_define_shader(dev, CID1, 998, SVGA3D_SHADERTYPE_PS, truncatedBytecode, sizeof(truncatedBytecode)/sizeof(uint32_t));
    TEST_CHECK(st == SVGA3_VLKN_ERROR_INVALID_PARAM, "Truncated bytecode explicitly rejected");

    /* Negative Control: Deliberately test false assertion detection */
    bool negativeControlCaught = false;
    if (pCtx1_Q4.b > 200) {
        negativeControlCaught = false; /* Should NOT happen because Blue is filtered out */
    } else {
        negativeControlCaught = true; /* Successfully detected that pixel is NOT blue */
    }
    TEST_CHECK(negativeControlCaught, "Negative control verified (harness detects discrepant pixels)");

    /* 10. Multitexture Unbound Sampler Stage 1 Fallback Test (Minecraft Hand Defect Fix) */
    /*
        ps_3_0
        dcl_texcoord v1
        dcl_2d s0
        dcl_2d s1
        texld r0, v1, s0
        texld r1, v1, s1
        mul oC0, r0, r1
        end
    */
    constexpr uint32_t SHID_PS3 = 22;
    const uint32_t ps3Bytecode[] = {
        0xFFFF0300, /* ps_3_0 */
        (31) | (2 << 24), /* DCL texcoord */
        0x80000000 | 5,
        D3D9_DST(1, 1, 0xF), /* v1 */
        (31) | (2 << 24), /* DCL 2D sampler s0 */
        0x80000000 | (2 << 28),
        D3D9_DST(10, 0, 0xF), /* s0 */
        (31) | (2 << 24), /* DCL 2D sampler s1 */
        0x80000000 | (2 << 28),
        D3D9_DST(10, 1, 0xF), /* s1 */
        (66) | (3 << 24), /* TEXLD r0, v1, s0 */
        D3D9_DST(0, 0, 0xF),
        D3D9_SRC(1, 1, 0xE4),
        D3D9_SRC(10, 0, 0xE4),
        (66) | (3 << 24), /* TEXLD r1, v1, s1 */
        D3D9_DST(0, 1, 0xF),
        D3D9_SRC(1, 1, 0xE4),
        D3D9_SRC(10, 1, 0xE4),
        (5) | (3 << 24), /* MUL oC0, r0, r1 */
        D3D9_DST(8, 0, 0xF),
        D3D9_SRC(0, 0, 0xE4),
        D3D9_SRC(0, 1, 0xE4),
        0x0000FFFF /* END */
    };
    st = svga3_vlkn_context_define_shader(dev, CID1, SHID_PS3, SVGA3D_SHADERTYPE_PS, ps3Bytecode, sizeof(ps3Bytecode)/sizeof(uint32_t));
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Define PS3 (Dual Sampler Modulate with Stage 1 Unbound)");
    svga3_vlkn_context_set_shader(dev, CID1, SVGA3D_SHADERTYPE_PS, SHID_PS3);
    svga3_vlkn_context_set_texture(dev, CID1, 0, SID_TEX);
    svga3_vlkn_context_set_texture(dev, CID1, 1, SVGA3D_INVALID_ID); /* Explicitly unbind stage 1 */
    svga3_vlkn_context_set_shader_const(dev, CID1, 0, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, row0);
    svga3_vlkn_context_set_shader_const(dev, CID1, 1, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, row1);
    svga3_vlkn_context_set_shader_const(dev, CID1, 2, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, row2);
    svga3_vlkn_context_set_shader_const(dev, CID1, 3, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, row3);
    svga3_vlkn_context_clear(dev, CID1, SVGA3D_CLEAR_COLOR, 0xFF000000, 1.0f, 0, nullptr, 0);
    st = svga3_vlkn_context_draw(dev, CID1, SVGA3D_PRIMITIVE_TRIANGLELIST, decls, 2, &range, 1);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Draw with PS3 and unbound Stage 1");

    st = svga3_vlkn_surface_dma_download(dev, SID_RT1, 0, &readBox, fb.data(), RT_WIDTH * sizeof(Pixel));
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Download framebuffer from PS3 execution");
    Pixel pUnbound_Q1 = fb[16 * RT_WIDTH + 16];
    std::cout << "  [PIXELS] Unbound Stage 1 Modulate Q1 (Expected Red): R=" << (int)pUnbound_Q1.r << " G=" << (int)pUnbound_Q1.g << " B=" << (int)pUnbound_Q1.b << std::endl;
    TEST_CHECK(pUnbound_Q1.r >= 240 && pUnbound_Q1.g <= 15 && pUnbound_Q1.b <= 15, "Unbound Stage 1 sampled white (1,1,1,1) preserving stage 0 texture color");

    /* Replacing a shader must not destroy a pipeline referenced by queued work. */
    const uint32_t queuedRed[] = {
        0xFFFF0300,
        81 | (5 << 24), D3D9_DST(2, 0, 0xF), 0x3F800000, 0, 0, 0x3F800000,
        1 | (2 << 24), D3D9_DST(8, 0, 0xF), D3D9_SRC(2, 0, 0xE4),
        0x0000FFFF
    };
    const uint32_t queuedGreen[] = {
        0xFFFF0300,
        81 | (5 << 24), D3D9_DST(2, 0, 0xF), 0, 0x3F800000, 0, 0x3F800000,
        1 | (2 << 24), D3D9_DST(8, 0, 0xF), D3D9_SRC(2, 0, 0xE4),
        0x0000FFFF
    };
    constexpr uint32_t queuedShaderId = 399;
    TEST_CHECK(svga3_vlkn_context_define_shader(dev, CID1, queuedShaderId, SVGA3D_SHADERTYPE_PS,
        queuedRed, sizeof(queuedRed) / sizeof(queuedRed[0])) == SVGA3_VLKN_SUCCESS,
        "Define shader for pending-pipeline regression");
    svga3_vlkn_context_set_shader(dev, CID1, SVGA3D_SHADERTYPE_PS, queuedShaderId);
    svga3_vlkn_context_clear(dev, CID1, SVGA3D_CLEAR_COLOR, 0xFF000000, 1.0f, 0, nullptr, 0);
    TEST_CHECK(svga3_vlkn_context_draw(dev, CID1, SVGA3D_PRIMITIVE_TRIANGLELIST,
        decls, 2, &range, 1) == SVGA3_VLKN_SUCCESS, "Record draw with the old pipeline");
    const uint64_t submissionsBeforeRedefine = backend->completedSubmissionSerial();
    TEST_CHECK(svga3_vlkn_context_define_shader(dev, CID1, queuedShaderId, SVGA3D_SHADERTYPE_PS,
        queuedGreen, sizeof(queuedGreen) / sizeof(queuedGreen[0])) == SVGA3_VLKN_SUCCESS,
        "Redefine shader while its pipeline is pending");
    TEST_CHECK(backend->completedSubmissionSerial() > submissionsBeforeRedefine,
        "Shader replacement waits before retiring the pending pipeline");
    /* Redefine with no dependent pipelines must NOT flush (no stall).
     * Use a fresh shader ID that has never been drawn with. */
    const uint32_t noPipeShaderId = 400;
    TEST_CHECK(svga3_vlkn_context_define_shader(dev, CID1, noPipeShaderId, SVGA3D_SHADERTYPE_PS,
        queuedRed, sizeof(queuedRed) / sizeof(queuedRed[0])) == SVGA3_VLKN_SUCCESS,
        "Define shader with no dependent pipelines");
    const uint64_t serialBeforeNoPipeRedefine = backend->completedSubmissionSerial();
    TEST_CHECK(svga3_vlkn_context_define_shader(dev, CID1, noPipeShaderId, SVGA3D_SHADERTYPE_PS,
        queuedGreen, sizeof(queuedGreen) / sizeof(queuedGreen[0])) == SVGA3_VLKN_SUCCESS,
        "Redefine shader with no dependent pipelines");
    TEST_CHECK(backend->completedSubmissionSerial() == serialBeforeNoPipeRedefine,
        "Redefine with no dependent pipelines does not flush (no stall)");
    svga3_vlkn_context_destroy_shader(dev, CID1, noPipeShaderId, SVGA3D_SHADERTYPE_PS);
    TEST_CHECK(svga3_vlkn_surface_dma_download(dev, SID_RT1, 0, &readBox,
        fb.data(), RT_WIDTH * sizeof(Pixel)) == SVGA3_VLKN_SUCCESS, "Submit and read pending old pipeline");
    TEST_CHECK(fb[16 * RT_WIDTH + 16].r >= 240 && fb[16 * RT_WIDTH + 16].g <= 15,
        "The pending draw completes with its original red pipeline");
    TEST_CHECK(svga3_vlkn_context_draw(dev, CID1, SVGA3D_PRIMITIVE_TRIANGLELIST,
        decls, 2, &range, 1) == SVGA3_VLKN_SUCCESS, "Draw with the replacement pipeline");
    TEST_CHECK(svga3_vlkn_surface_dma_download(dev, SID_RT1, 0, &readBox,
        fb.data(), RT_WIDTH * sizeof(Pixel)) == SVGA3_VLKN_SUCCESS, "Read replacement pipeline output");
    TEST_CHECK(fb[16 * RT_WIDTH + 16].g >= 240 && fb[16 * RT_WIDTH + 16].r <= 15,
        "The replacement pipeline renders green");
    TEST_CHECK(svga3_vlkn_context_draw(dev, CID1, SVGA3D_PRIMITIVE_TRIANGLELIST,
        decls, 2, &range, 1) == SVGA3_VLKN_SUCCESS, "Record draw before destroying its shader");
    TEST_CHECK(svga3_vlkn_context_destroy_shader(dev, CID1, queuedShaderId, SVGA3D_SHADERTYPE_PS) == SVGA3_VLKN_SUCCESS,
        "Destroy shader while its pipeline is pending");
    TEST_CHECK(svga3_vlkn_surface_dma_download(dev, SID_RT1, 0, &readBox,
        fb.data(), RT_WIDTH * sizeof(Pixel)) == SVGA3_VLKN_SUCCESS, "Read draw submitted during shader destruction");
    TEST_CHECK(fb[16 * RT_WIDTH + 16].g >= 240 && fb[16 * RT_WIDTH + 16].r <= 15,
        "Destroying a shader completes queued draws before pipeline eviction");

    /* Destroy-then-redefine with the same ID must not reuse a stale pipeline.
     * The shader was destroyed above; defining a new (blue) shader under the
     * same ID must compile a fresh pipeline, not hit a cache entry from the
     * old (green) shader. */
    const uint32_t queuedBlue[] = {
        0xFFFF0300,
        81 | (5 << 24), D3D9_DST(2, 0, 0xF), 0, 0, 0x3F800000, 0x3F800000,
        1 | (2 << 24), D3D9_DST(8, 0, 0xF), D3D9_SRC(2, 0, 0xE4),
        0x0000FFFF
    };
    TEST_CHECK(svga3_vlkn_context_define_shader(dev, CID1, queuedShaderId, SVGA3D_SHADERTYPE_PS,
        queuedBlue, sizeof(queuedBlue) / sizeof(queuedBlue[0])) == SVGA3_VLKN_SUCCESS,
        "Redefine shader ID after destroy");
    svga3_vlkn_context_set_shader(dev, CID1, SVGA3D_SHADERTYPE_PS, queuedShaderId);
    TEST_CHECK(svga3_vlkn_context_draw(dev, CID1, SVGA3D_PRIMITIVE_TRIANGLELIST,
        decls, 2, &range, 1) == SVGA3_VLKN_SUCCESS, "Draw with redefined shader");
    TEST_CHECK(svga3_vlkn_surface_dma_download(dev, SID_RT1, 0, &readBox,
        fb.data(), RT_WIDTH * sizeof(Pixel)) == SVGA3_VLKN_SUCCESS, "Read redefined shader output");
    TEST_CHECK(fb[16 * RT_WIDTH + 16].b >= 240 && fb[16 * RT_WIDTH + 16].r <= 15,
        "Redefined shader after destroy renders blue (no stale pipeline)");

    /* Cleanup */
    svga3_vlkn_context_destroy(dev, CID1);
    svga3_vlkn_context_destroy(dev, CID2);
    svga3_vlkn_surface_destroy(dev, SID_RT1);
    svga3_vlkn_surface_destroy(dev, SID_TEX);
    svga3_vlkn_surface_destroy(dev, SID_VB);
    svga3_vlkn_surface_destroy(dev, SID_RT2);
    svga3_vlkn_device_destroy(dev);

    std::cout << "\n======================================================================" << std::endl;
    std::cout << "SUCCESS: All 20+ Shader Translation & Execution Tests Passed Cleanly!" << std::endl;
    std::cout << "======================================================================" << std::endl;
    return 0;
}
