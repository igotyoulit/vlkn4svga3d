/* Malformed-input regression tests for the security hardening PR.
 * Verifies that hostile guest-controlled values are rejected (fail-closed)
 * instead of causing OOB reads, integer wrap, or unbounded allocation.
 * Uses the mock backend: no real Vulkan device needed.
 */
#include "svga3_vlkn.h"
#include "svga3_device.h"
#include "vlkn_backend.h"
#include "svga3_shader_translator.h"
#include "svga3_surface.h"
#include <iostream>
#include <vector>
#include <cstring>
#include <cstdlib>

#define TEST_CHECK(cond, msg) do { \
    if (cond) { \
        std::cout << "  [PASS] " << msg << std::endl; \
    } else { \
        std::cerr << "  [FAIL] " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
        return 1; \
    } \
} while(0)

int main() {
    std::cout << "Testing malformed-input rejection (security hardening)..." << std::endl;

    /* Mock backend: no GPU needed for input-validation tests. */
    Svga3VlknConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.appName = "Malformed Input Test";
    cfg.apiVersion = VK_API_VERSION_1_1;
    cfg.forceMockBackend = true;

    Svga3VlknDevice *dev = svga3_vlkn_device_create(&cfg);
    if (!dev) {
        std::cerr << "FAIL: Could not create mock Vulkan device!" << std::endl;
        return 1;
    }

    /* H5: ADD declaring 1 operand (needs dest + 2 src = 3).
     * Before the fix, pass-2 read tokens[pc+4] past the validated range. */
    {
        const uint32_t badAdd[] = {
            0xFFFE0300, /* vs_3_0 */
            (5) | (1 << 24), /* ADD, declared length 1 (only dest) */
            0x80000000 | (0 << 28) | 0 | (0xF << 16), /* r0 */
            0x0000FFFF, /* END */
        };
        std::vector<uint32_t> spirv;
        std::string err;
        uint32_t inMask = 0;
        Svga3VlknStatus st = svga3_vlkn::svga3_translate_shader_d3d9(SVGA3D_SHADERTYPE_VS,
            badAdd, sizeof(badAdd)/sizeof(badAdd[0]), spirv, err, &inMask);
        TEST_CHECK(st != SVGA3_VLKN_SUCCESS,
            "Malformed ADD (declared length < consumed operands) rejected");
    }

    /* H5: MOV declaring 0 operands (needs dest + 1 src = 2). */
    {
        const uint32_t badMov[] = {
            0xFFFE0300, /* vs_3_0 */
            (1) | (0 << 24), /* MOV, declared length 0 */
            0x0000FFFF, /* END */
        };
        std::vector<uint32_t> spirv;
        std::string err;
        uint32_t inMask = 0;
        Svga3VlknStatus st = svga3_vlkn::svga3_translate_shader_d3d9(SVGA3D_SHADERTYPE_VS,
            badMov, sizeof(badMov)/sizeof(badMov[0]), spirv, err, &inMask);
        TEST_CHECK(st != SVGA3_VLKN_SUCCESS,
            "Malformed MOV (declared length 0) rejected");
    }

    /* M2: oversized shader rejected at the gate. */
    {
        size_t huge = 65536 + 1 /* translator MAX_SHADER_DWORDS */;
        std::vector<uint32_t> big(huge, 0);
        big[0] = 0xFFFE0300;
        std::vector<uint32_t> spirv;
        std::string err;
        uint32_t inMask = 0;
        Svga3VlknStatus st = svga3_vlkn::svga3_translate_shader_d3d9(SVGA3D_SHADERTYPE_VS,
            big.data(), big.size(), spirv, err, &inMask);
        TEST_CHECK(st != SVGA3_VLKN_SUCCESS,
            "Oversized shader bytecode rejected");
    }

    /* H3: surface with excessive mip levels rejected. */
    {
        SVGA3dSize sizes[1] = {};
        sizes[0].width = 64;
        sizes[0].height = 64;
        sizes[0].depth = 1;
        uint32_t sid = 4242;
        Svga3VlknStatus st = dev->surfaceMgr->defineSurface(sid, 0, SVGA3D_A8R8G8B8,
            sizes, svga3_vlkn::SVGA3_MAX_MIP_LEVELS + 1);
        TEST_CHECK(st != SVGA3_VLKN_SUCCESS,
            "Surface with excessive mip levels rejected");
        TEST_CHECK(!dev->surfaceMgr->getSurface(sid),
            "Rejected surface not registered");
    }

    /* H3: surface with excessive dimensions rejected. */
    {
        SVGA3dSize sizes[1] = {};
        sizes[0].width = svga3_vlkn::SVGA3_MAX_SURFACE_DIM + 1;
        sizes[0].height = 64;
        sizes[0].depth = 1;
        uint32_t sid = 4243;
        Svga3VlknStatus st = dev->surfaceMgr->defineSurface(sid, 0, SVGA3D_A8R8G8B8,
            sizes, 1);
        TEST_CHECK(st != SVGA3_VLKN_SUCCESS,
            "Surface with excessive dimensions rejected");
        TEST_CHECK(!dev->surfaceMgr->getSurface(sid),
            "Rejected surface not registered");
    }

    /* Raw guest SPIR-V must be rejected even if the old environment
     * variable is set. There is no runtime escape hatch in production
     * builds; the gate is compile-time only. */
    {
        setenv("SVGA3_VLKN_ALLOW_GUEST_SPIRV", "1", 1);
        const uint32_t fakeSpirv[] = {
            0x07230203, /* SPIR-V magic */
            0x00010000, 0x00000000, 0x00000001, 0x00000000,
        };
        uint32_t cid = 4343;
        Svga3VlknStatus st = svga3_vlkn_context_create(dev, cid);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Context created for SPIR-V test");
        st = svga3_vlkn_context_define_shader(dev, cid, 1, SVGA3D_SHADERTYPE_VS,
            fakeSpirv, 5);
        TEST_CHECK(st != SVGA3_VLKN_SUCCESS,
            "Raw guest SPIR-V rejected despite env var set");
        unsetenv("SVGA3_VLKN_ALLOW_GUEST_SPIRV");
        svga3_vlkn_context_destroy(dev, cid);
    }

    /* Aggregate surface budget: many per-object-legal surfaces must still
     * hit the device-wide cap, fail cleanly, and release on destroy. */
    {
        auto &budgets = dev->backend->resourceBudgets();
        /* Shrink the cap for the test: 4 surfaces of 4 MiB = 16 MiB cap. */
        budgets.setMaxForTesting(16 * 1024 * 1024, 1024 * 1024, 100);
        uint64_t cap = budgets.maxSurfaceBytes();
        /* 1024x1024 A8R8G8B8 = 4 MiB per surface; 16 MiB cap => 4 fill it. */
        SVGA3dSize sizes[1] = {};
        sizes[0].width = 1024;
        sizes[0].height = 1024;
        sizes[0].depth = 1;
        const uint64_t perSurface = 1024ULL * 1024 * 4;
        const uint32_t fillCount = (uint32_t)(cap / perSurface);
        TEST_CHECK(fillCount > 0 && fillCount < svga3_vlkn::SVGA3_MAX_SURFACES,
            "Surface stress size hits byte budget before count cap");

        uint32_t sidBase = 9000;
        Svga3VlknStatus st = SVGA3_VLKN_SUCCESS;
        uint32_t created = 0;
        for (uint32_t i = 0; i < fillCount; ++i) {
            st = dev->surfaceMgr->defineSurface(sidBase + i, 0, SVGA3D_A8R8G8B8,
                sizes, 1);
            if (st != SVGA3_VLKN_SUCCESS) break;
            ++created;
        }
        TEST_CHECK(created == fillCount && st == SVGA3_VLKN_SUCCESS,
            "Surfaces defined up to the aggregate budget");
        TEST_CHECK(budgets.surfaceBytes() == cap,
            "Budget counter matches the cap exactly");

        st = dev->surfaceMgr->defineSurface(sidBase + fillCount, 0, SVGA3D_A8R8G8B8,
            sizes, 1);
        TEST_CHECK(st == SVGA3_VLKN_ERROR_OUT_OF_MEMORY,
            "Surface past the aggregate budget fails with OUT_OF_MEMORY");
        TEST_CHECK(!dev->surfaceMgr->getSurface(sidBase + fillCount),
            "Over-budget surface not registered");
        TEST_CHECK(budgets.surfaceBytes() == cap,
            "Rejected surface did not move the budget");

        /* Redefine must not double-charge: replacing a surface keeps the
         * budget flat. */
        st = dev->surfaceMgr->defineSurface(sidBase, 0, SVGA3D_A8R8G8B8,
            sizes, 1);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Surface redefine succeeds");
        TEST_CHECK(budgets.surfaceBytes() == cap,
            "Surface redefine did not double-charge the budget");

        /* Destroy half: budget is released and allocation works again. */
        for (uint32_t i = 0; i < fillCount / 2; ++i) {
            st = dev->surfaceMgr->destroySurface(sidBase + i);
            TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Surface destroyed");
        }
        TEST_CHECK(budgets.surfaceBytes() == cap / 2,
            "Destroying surfaces released their budget");
        st = dev->surfaceMgr->defineSurface(sidBase + fillCount, 0, SVGA3D_A8R8G8B8,
            sizes, 1);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS,
            "Allocation succeeds again after budget release");
        TEST_CHECK(budgets.surfaceBytes() == cap / 2 + perSurface,
            "Budget re-charged after re-allocation");

        /* Clean up the rest so later tests start from a zero budget. */
        for (uint32_t i = fillCount / 2; i < fillCount; ++i) {
            dev->surfaceMgr->destroySurface(sidBase + i);
        }
        dev->surfaceMgr->destroySurface(sidBase + fillCount);
        TEST_CHECK(budgets.surfaceBytes() == 0,
            "Surface budget fully released after cleanup");
    }

    /* Aggregate shader budget: bytecode bytes and module count are capped
     * device-wide; destroy releases them. */
    {
        auto &budgets = dev->backend->resourceBudgets();
        uint64_t byteCap = budgets.maxShaderBytecodeBytes();
        /* byteCap is 1 MiB from setMaxForTesting above. */
        uint32_t cid = 9500;
        Svga3VlknStatus st = svga3_vlkn_context_create(dev, cid);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Context created for shader budget test");

        /* Max-size translator input: version + NOPs + END = 65536 dwords
         * = 256 KiB. 4 of them exactly fill the 1 MiB test cap, staying
         * under the per-context and module caps. */
        const uint32_t kDwords = 65536;
        std::vector<uint32_t> big(kDwords, 0);
        big[0] = 0xFFFE0300;      /* vs_3_0 */
        big[kDwords - 1] = 0x0000FFFF; /* END */
        const uint64_t perShader = (uint64_t)kDwords * 4;
        const uint32_t fillCount = (uint32_t)(byteCap / perShader);
        TEST_CHECK(fillCount == 4, "Shader stress size fills the test bytecode cap");

        uint32_t shidBase = 100;
        uint32_t created = 0;
        st = SVGA3_VLKN_SUCCESS;
        for (uint32_t i = 0; i < fillCount; ++i) {
            st = svga3_vlkn_context_define_shader(dev, cid, shidBase + i,
                SVGA3D_SHADERTYPE_VS, big.data(), kDwords);
            if (st != SVGA3_VLKN_SUCCESS) break;
            ++created;
        }
        TEST_CHECK(created == fillCount && st == SVGA3_VLKN_SUCCESS,
            "Shaders defined up to the aggregate bytecode budget");
        TEST_CHECK(budgets.shaderBytecodeBytes() == byteCap,
            "Shader byte counter matches the cap exactly");
        TEST_CHECK(budgets.shaderModuleCount() == fillCount,
            "Shader module counter matches defined count");

        st = svga3_vlkn_context_define_shader(dev, cid, shidBase + fillCount,
            SVGA3D_SHADERTYPE_VS, big.data(), kDwords);
        TEST_CHECK(st == SVGA3_VLKN_ERROR_OUT_OF_MEMORY,
            "Shader past the aggregate budget fails with OUT_OF_MEMORY");
        TEST_CHECK(budgets.shaderBytecodeBytes() == byteCap,
            "Rejected shader did not move the budget");

        /* Redefine must not double-charge. */
        st = svga3_vlkn_context_define_shader(dev, cid, shidBase,
            SVGA3D_SHADERTYPE_VS, big.data(), kDwords);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Shader redefine succeeds");
        TEST_CHECK(budgets.shaderBytecodeBytes() == byteCap,
            "Shader redefine did not double-charge the budget");
        TEST_CHECK(budgets.shaderModuleCount() == fillCount,
            "Shader redefine did not double-charge module count");

        /* Destroy half: budget is released and defining works again. */
        for (uint32_t i = 0; i < fillCount / 2; ++i) {
            st = svga3_vlkn_context_destroy_shader(dev, cid, shidBase + i,
                SVGA3D_SHADERTYPE_VS);
            TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "Shader destroyed");
        }
        TEST_CHECK(budgets.shaderBytecodeBytes() == byteCap / 2,
            "Destroying shaders released their bytecode budget");
        TEST_CHECK(budgets.shaderModuleCount() == fillCount / 2,
            "Destroying shaders released their module budget");
        st = svga3_vlkn_context_define_shader(dev, cid, shidBase + fillCount,
            SVGA3D_SHADERTYPE_VS, big.data(), kDwords);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS,
            "Shader allocation succeeds again after budget release");

        svga3_vlkn_context_destroy(dev, cid);
        TEST_CHECK(budgets.shaderBytecodeBytes() == 0,
            "Shader bytecode budget fully released after context destroy");
        TEST_CHECK(budgets.shaderModuleCount() == 0,
            "Shader module budget fully released after context destroy");
    }

    svga3_vlkn_device_destroy(dev);

    std::cout << "All malformed-input tests PASSED!" << std::endl;
    return 0;
}
