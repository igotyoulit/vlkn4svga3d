#include "svga3_vlkn.h"
#include "svga3_device.h"
#include "vlkn_backend.h"
#include "svga3_shader_translator.h"
#include <iostream>
#include <vector>
#include <cstring>

#define TEST_CHECK(cond, msg) do { \
    if (cond) { \
        std::cout << "  [PASS] " << msg << std::endl; \
    } else { \
        std::cerr << "  [FAIL] " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
        return 1; \
    } \
} while(0)

int main() {
    std::cout << "Testing SVGA3D/D3D9 Shader Bytecode Translation to SPIR-V..." << std::endl;

    /* Initialize real Vulkan backend */
    Svga3VlknConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.appName = "Shader Translator Test";
    cfg.apiVersion = VK_API_VERSION_1_1;
    cfg.forceMockBackend = false;
    cfg.enableValidationLayers = true;

    Svga3VlknDevice *dev = svga3_vlkn_device_create(&cfg);
    if (!dev) {
        std::cerr << "FAIL: Could not create real Vulkan device!" << std::endl;
        return 1;
    }

    svga3_vlkn::VlknBackend *backend = dev->backend.get();

    /* 1. Test D3D9 Vertex Shader with M4x4 transform and color copy */
    /*
        vs_3_0
        dcl_position v0
        dcl_color v1
        m4x4 oPos, v0, c0
        mov oD0, v1
        end
    */
    const uint32_t vsBytecode[] = {
        0xFFFE0300, /* vs_3_0 */
        (31) | (2 << 24), /* DCL */
        0x80000000 | 0, /* Position 0 */
        0x80000000 | (1 << 28) | 0 | (0xF << 16), /* v0 */
        (31) | (2 << 24), /* DCL */
        0x80000000 | 10, /* Color 0 */
        0x80000000 | (1 << 28) | 1 | (0xF << 16), /* v1 */
        (20) | (3 << 24), /* M4x4 */
        0x80000000 | (4 << 28) | 0 | (0xF << 16), /* oPos */
        0x80000000 | (1 << 28) | 0 | (0xE4 << 16), /* v0 */
        0x80000000 | (2 << 28) | 0 | (0xE4 << 16), /* c0 */
        (1) | (2 << 24), /* MOV */
        0x80000000 | (5 << 28) | 0 | (0xF << 16), /* oD0 */
        0x80000000 | (1 << 28) | 1 | (0xE4 << 16), /* v1 */
        0x0000FFFF /* END */
    };

    std::vector<uint32_t> vsSpirv;
    std::string err;
    Svga3VlknStatus st = svga3_vlkn::svga3_translate_shader_d3d9(
        SVGA3D_SHADERTYPE_VS, vsBytecode, sizeof(vsBytecode)/sizeof(uint32_t), vsSpirv, err
    );
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Translate VS bytecode to SPIR-V");
    TEST_CHECK(!vsSpirv.empty(), "VS SPIR-V output is non-empty");

    /* Create real Vulkan Shader Module */
    VkShaderModuleCreateInfo smInfo = {};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vsSpirv.size() * sizeof(uint32_t);
    smInfo.pCode = vsSpirv.data();
    VkShaderModule vsModule = VK_NULL_HANDLE;
    VkResult res = backend->dispatch().vkCreateShaderModule(backend->device(), &smInfo, nullptr, &vsModule);
    TEST_CHECK(res == VK_SUCCESS, "Create VkShaderModule from translated VS SPIR-V on Lavapipe");

    /* 2. Test D3D9 Pixel Shader with texture sampling and constant multiply */
    /*
        ps_3_0
        def c0, 0.5, 0.5, 0.5, 1.0
        dcl_texcoord v0
        dcl_2d s0
        texld r0, v0, s0
        mul oC0, r0, c0
        end
    */
    union { float f; uint32_t u; } def0 = { 0.5f }, def1 = { 1.0f };
    const uint32_t psBytecode[] = {
        0xFFFF0300, /* ps_3_0 */
        (81) | (5 << 24), /* DEF c0 */
        0x80000000 | (2 << 28) | 0 | (0xF << 16), /* c0 */
        def0.u, def0.u, def0.u, def1.u,
        (31) | (2 << 24), /* DCL texcoord */
        0x80000000 | 5, /* Texcoord 0 */
        0x80000000 | (1 << 28) | 0 | (0xF << 16), /* v0 */
        (31) | (2 << 24), /* DCL 2D sampler */
        0x80000000 | (2 << 28), /* 2D */
        0x80000000 | (10 << 28) | 0 | (0xF << 16), /* s0 */
        (66) | (3 << 24), /* TEX / TEXLD */
        0x80000000 | (0 << 28) | 0 | (0xF << 16), /* r0 */
        0x80000000 | (1 << 28) | 0 | (0xE4 << 16), /* v0 */
        0x80000000 | (10 << 28) | 0 | (0xE4 << 16), /* s0 */
        (5) | (3 << 24), /* MUL */
        0x80000000 | (8 << 28) | 0 | (0xF << 16), /* oC0 */
        0x80000000 | (0 << 28) | 0 | (0xE4 << 16), /* r0 */
        0x80000000 | (2 << 28) | 0 | (0xE4 << 16), /* c0 */
        0x0000FFFF /* END */
    };

    std::vector<uint32_t> psSpirv;
    st = svga3_vlkn::svga3_translate_shader_d3d9(
        SVGA3D_SHADERTYPE_PS, psBytecode, sizeof(psBytecode)/sizeof(uint32_t), psSpirv, err
    );
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Translate PS bytecode to SPIR-V");
    TEST_CHECK(!psSpirv.empty(), "PS SPIR-V output is non-empty");

    VkShaderModule psModule = VK_NULL_HANDLE;
    smInfo.codeSize = psSpirv.size() * sizeof(uint32_t);
    smInfo.pCode = psSpirv.data();
    res = backend->dispatch().vkCreateShaderModule(backend->device(), &smInfo, nullptr, &psModule);
    TEST_CHECK(res == VK_SUCCESS, "Create VkShaderModule from translated PS SPIR-V on Lavapipe");

    /* 3. Negative Control: Malformed / Unsupported Opcode */
    const uint32_t badBytecode[] = {
        0xFFFF0300,
        0x00001234, /* Invalid/unsupported opcode */
        0x0000FFFF
    };
    std::vector<uint32_t> badSpirv;
    std::string badErr;
    st = svga3_vlkn::svga3_translate_shader_d3d9(
        SVGA3D_SHADERTYPE_PS, badBytecode, sizeof(badBytecode)/sizeof(uint32_t), badSpirv, badErr
    );
    TEST_CHECK(st == SVGA3_VLKN_ERROR_UNSUPPORTED_SHADER, "Unsupported opcode rejected with error");
    TEST_CHECK(!badErr.empty(), "Error message provided: " + badErr);

    /* Declared instruction length cannot make a missing operand readable. */
    const std::vector<std::vector<uint32_t>> malformedBytecode = {
        {0xffff0300, 0x01000002, 0x800f0000, 0xffff}, // ADD with one operand
        {0xffff0300, 0x02000001, 0x800f0000, 0xffff}, // MOV source replaced by END
        {0xffff0300, 0x7ffffffe, 0xffff},             // truncated COMMENT payload
        {0xffff0300, 0x05000051, 0xa00f0000, 0, 0xffff} // truncated DEF
    };
    for (const auto &code : malformedBytecode) {
        st = svga3_vlkn::svga3_translate_shader_d3d9(SVGA3D_SHADERTYPE_PS,
            code.data(), code.size(), badSpirv, badErr);
        TEST_CHECK(st == SVGA3_VLKN_ERROR_INVALID_PARAM,
                   "Truncated or falsely sized shader instruction rejected");
    }

    /* 4. Negative Control: Missing END token */
    const uint32_t noEndBytecode[] = {
        0xFFFF0300,
        (1) | (2 << 24),
        0x80000000 | (8 << 28) | 0 | (0xF << 16),
        0x80000000 | (1 << 28) | 0 | (0xE4 << 16)
        /* No 0x0000FFFF */
    };
    st = svga3_vlkn::svga3_translate_shader_d3d9(
        SVGA3D_SHADERTYPE_PS, noEndBytecode, sizeof(noEndBytecode)/sizeof(uint32_t), badSpirv, badErr
    );
    TEST_CHECK(st == SVGA3_VLKN_ERROR_INVALID_PARAM, "Missing END token rejected with error");

    /* 5. Test Destination Saturate (_SAT) Modifier */
    /*
        ps_3_0
        dcl_texcoord v0
        mov_sat r0, v0
        mov oC0, r0
        end
    */
    const uint32_t satBytecode[] = {
        0xFFFF0300, /* ps_3_0 */
        (31) | (2 << 24), /* DCL */
        0x80000000 | 5,
        0x80000000 | (1 << 28) | 0 | (0xF << 16), /* v0 */
        (1) | (2 << 24), /* MOV */
        0x80000000 | (0 << 28) | 0 | (0xF << 16) | (1 << 20), /* r0 with dstMod=1 (_SAT) */
        0x80000000 | (1 << 28) | 0 | (0xE4 << 16), /* v0 */
        (1) | (2 << 24), /* MOV */
        0x80000000 | (8 << 28) | 0 | (0xF << 16), /* oC0 */
        0x80000000 | (0 << 28) | 0 | (0xE4 << 16), /* r0 */
        0x0000FFFF /* END */
    };
    std::vector<uint32_t> satSpirv;
    std::string satErr;
    st = svga3_vlkn::svga3_translate_shader_d3d9(
        SVGA3D_SHADERTYPE_PS, satBytecode, sizeof(satBytecode)/sizeof(uint32_t), satSpirv, satErr
    );
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Translate _SAT bytecode to SPIR-V");
    TEST_CHECK(!satSpirv.empty(), "Saturate SPIR-V output is non-empty");

    /* Check that GLSLstd450FClamp (43) is emitted in satSpirv */
    bool foundFClamp = false;
    for (size_t i = 5; i < satSpirv.size(); ) {
        uint32_t word = satSpirv[i];
        uint32_t wordCount = word >> 16;
        uint32_t opcode = word & 0xFFFF;
        if (opcode == 12 /* SpvOpExtInst */ && wordCount >= 5) {
            uint32_t extInstOp = satSpirv[i + 4];
            if (extInstOp == 43 /* GLSLstd450FClamp */) {
                foundFClamp = true;
                break;
            }
        }
        if (wordCount == 0) break;
        i += wordCount;
    }
    TEST_CHECK(foundFClamp, "GLSLstd450FClamp instruction emitted for _SAT modifier");

    VkShaderModule satModule = VK_NULL_HANDLE;
    smInfo.codeSize = satSpirv.size() * sizeof(uint32_t);
    smInfo.pCode = satSpirv.data();
    res = backend->dispatch().vkCreateShaderModule(backend->device(), &smInfo, nullptr, &satModule);
    TEST_CHECK(res == VK_SUCCESS, "Create VkShaderModule from _SAT SPIR-V on Lavapipe");
    backend->dispatch().vkDestroyShaderModule(backend->device(), satModule, nullptr);

    /* Mesa-style D3D9: DCL-declared OUTPUT with TEMP-encoded MOV destination.
     * Mesa's SVGA backend emits MOV dst tokens with regType=TEMP for registers
     * declared as OUTPUT via DCL. The translator must honor the DCL. */
    {
        const uint32_t mesaVsBytecode[] = {
            0xFFFE0300, /* vs_3_0 */
            (31) | (2 << 24), /* DCL */
            0x80000000 | 0,   /* Usage: POSITION 0 */
            0x80000000 | (6 << 28) | 0 | (0xF << 16), /* o0 (OUTPUT) */
            (31) | (2 << 24), /* DCL */
            0x80000000 | 5,   /* Usage: TEXCOORD 0 */
            0x80000000 | (1 << 28) | 0 | (0xF << 16), /* v0 (INPUT) */
            (1) | (2 << 24),  /* MOV */
            0x80000000 | (0 << 28) | 0 | (0xF << 16), /* r0 (TEMP) - Mesa-style! */
            0x80000000 | (1 << 28) | 0 | (0xE4 << 16), /* v0 */
            0x0000FFFF        /* END */
        };
        std::vector<uint32_t> mesaSpirv;
        std::string mesaErr;
        auto mesaSt = svga3_vlkn::svga3_translate_shader_d3d9(
            SVGA3D_SHADERTYPE_VS, mesaVsBytecode,
            sizeof(mesaVsBytecode)/sizeof(uint32_t), mesaSpirv, mesaErr);
        TEST_CHECK(mesaSt == SVGA3_VLKN_SUCCESS,
                   "Mesa-style DCL/TEMP VS translates (DCL output routing)");
    }

    /* Cleanup */
    backend->dispatch().vkDestroyShaderModule(backend->device(), vsModule, nullptr);
    backend->dispatch().vkDestroyShaderModule(backend->device(), psModule, nullptr);
    svga3_vlkn_device_destroy(dev);

    std::cout << "All Shader Translation tests PASSED cleanly on Real Vulkan!" << std::endl;
    return 0;
}
