// ICD-free D3D9->SPIR-V translator tests.
// Calls svga3_translate_shader_d3d9 directly without a Vulkan device.
// Optionally validates output with spirv-val when available.
// Covers issue #9: DCL-declared OUTPUT routing, SM3 implicit o0/o1.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <iostream>
#include "vmsvga/svga3d_shaderdefs.h"
#include "vmsvga/svga3d_reg.h"
#include "internal/svga3_shader_translator.h"

using namespace svga3_vlkn;

#define TEST_CHECK(cond, msg) do { \
    if (cond) { std::cout << "  [PASS] " << msg << std::endl; } \
    else { std::cerr << "  [FAIL] " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; return 1; } \
} while(0)

// Correct D3D9 token macros
#define D3D9_DST(regType, regNum, mask) \
    (0x80000000u | (((regType) & 0x7) << 28) | ((((regType) >> 3) & 0x3) << 11) | (((mask) & 0xF) << 16) | ((regNum) & 0x7FF))
#define D3D9_SRC(regType, regNum, swiz) \
    (0x80000000u | (((regType) & 0x7) << 28) | ((((regType) >> 3) & 0x3) << 11) | (((swiz) & 0xFF) << 16) | ((regNum) & 0x7FF))

// D3DSPR values (from translator)
#define D3DSPR_TEMP_VAL 0
#define D3DSPR_INPUT_VAL 1
#define D3DSPR_OUTPUT_VAL 6
#define D3DSPR_COLOROUT_VAL 8

static bool spirv_val_available() {
    return system("which spirv-val >/dev/null 2>&1") == 0;
}

static bool validate_spirv(const std::vector<uint32_t>& spirv, const char* tag) {
    if (!spirv_val_available()) {
        std::cout << "  [SKIP] spirv-val not available for " << tag << std::endl;
        return true;
    }
    char path[64];
    snprintf(path, sizeof(path), "/tmp/novulkan_%s.spv", tag);
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fwrite(spirv.data(), 4, spirv.size(), f);
    fclose(f);
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "spirv-val %s >/dev/null 2>&1", path);
    return system(cmd) == 0;
}

int main() {
    std::cout << "ICD-free D3D9->SPIR-V translator tests (issue #9)..." << std::endl;

    // Test 1: Mesa-style DCL-declared OUTPUT with TEMP-encoded MOV dst
    {
        const uint32_t vs[] = {
            0xFFFE0300, // vs_3_0
            (31) | (2 << 24), // DCL
            0x80000000 | 0,   // POSITION
            D3D9_DST(D3DSPR_OUTPUT_VAL, 0, 0xF), // o0
            (31) | (2 << 24), // DCL
            0x80000000 | 5,   // TEXCOORD0
            D3D9_DST(D3DSPR_INPUT_VAL, 0, 0xF), // v0
            (1) | (2 << 24),  // MOV
            D3D9_DST(D3DSPR_TEMP_VAL, 0, 0xF), // r0 (Mesa-style misencoding)
            D3D9_SRC(D3DSPR_INPUT_VAL, 0, 0xE4), // v0
            0x0000FFFF
        };
        std::vector<uint32_t> spirv;
        std::string err;
        auto st = svga3_translate_shader_d3d9(SVGA3D_SHADERTYPE_VS, vs,
                                              sizeof(vs)/sizeof(uint32_t), spirv, err);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Mesa-style DCL/TEMP VS translates");
        TEST_CHECK(!spirv.empty(), "Mesa-style VS SPIR-V non-empty");
        TEST_CHECK(validate_spirv(spirv, "mesa_dcl"), "Mesa-style VS passes spirv-val");
    }

    // Test 2: SM3 implicit o0 (no DCL) - should treat r0 as position
    {
        const uint32_t vs[] = {
            0xFFFE0300, // vs_3_0
            // No DCLs - implicit o0=position
            (1) | (2 << 24),  // MOV
            D3D9_DST(D3DSPR_TEMP_VAL, 0, 0xF), // r0 (implicit o0)
            D3D9_SRC(D3DSPR_INPUT_VAL, 0, 0xE4), // v0
            0x0000FFFF
        };
        std::vector<uint32_t> spirv;
        std::string err;
        auto st = svga3_translate_shader_d3d9(SVGA3D_SHADERTYPE_VS, vs,
                                              sizeof(vs)/sizeof(uint32_t), spirv, err);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "SM3 implicit o0 VS translates");
        TEST_CHECK(validate_spirv(spirv, "implicit_o0"), "Implicit o0 VS passes spirv-val");
    }

    // Test 3: Normal OUTPUT (not TEMP) still works
    {
        const uint32_t vs[] = {
            0xFFFE0300,
            (31) | (2 << 24),
            0x80000000 | 0,
            D3D9_DST(D3DSPR_OUTPUT_VAL, 0, 0xF), // o0
            (1) | (2 << 24),  // MOV
            D3D9_DST(D3DSPR_OUTPUT_VAL, 0, 0xF), // o0 (correct encoding)
            D3D9_SRC(D3DSPR_INPUT_VAL, 0, 0xE4),
            0x0000FFFF
        };
        std::vector<uint32_t> spirv;
        std::string err;
        auto st = svga3_translate_shader_d3d9(SVGA3D_SHADERTYPE_VS, vs,
                                              sizeof(vs)/sizeof(uint32_t), spirv, err);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Normal OUTPUT VS translates");
        TEST_CHECK(validate_spirv(spirv, "normal_out"), "Normal OUTPUT VS passes spirv-val");
    }

    std::cout << "All ICD-free translator tests PASSED!" << std::endl;
    return 0;
}
