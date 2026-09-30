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
#include <unordered_map>
#include <set>
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
#define D3DSPR_CONST_VAL 2
#define D3DSPR_RASTOUT_VAL 4
#define D3DSPR_OUTPUT_VAL 6
#define D3DSPR_COLOROUT_VAL 8

static bool spirv_val_available() {
    return system("which spirv-val >/dev/null 2>&1") == 0;
}

/* Returns: 1=pass, 0=fail, -1=skip (spirv-val not available) */
static int validate_spirv(const std::vector<uint32_t>& spirv, const char* tag) {
    if (!spirv_val_available()) {
        std::cout << "  [SKIP] spirv-val not available for " << tag << std::endl;
        return -1;
    }
    char path[64];
    snprintf(path, sizeof(path), "/tmp/novulkan_%s.spv", tag);
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(spirv.data(), 4, spirv.size(), f);
    fclose(f);
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "spirv-val %s >/dev/null 2>&1", path);
    return system(cmd) == 0 ? 1 : 0;
}

#define TEST_CHECK_SPIRV(spirv, tag, msg) do { \
    int vres = validate_spirv(spirv, tag); \
    if (vres == 0) { std::cerr << "  [FAIL] " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; return 1; } \
    else if (vres == 1) { std::cout << "  [PASS] " << msg << std::endl; } \
} while(0)

/* ---------------------------------------------------------------------------
 * Value-flow assertions (replaces decoration-grep routing checks).
 *
 * The old spirv-dis based check only proved a BuiltIn/Location decoration
 * existed somewhere in the module. A misrouted store (e.g. implicit o0 going
 * to a texcoord shadow while the Position output keeps its decoration) passed
 * it. The walker below parses the SPIR-V word stream and proves the value
 * stored to an output is actually derived from a shader input, following
 * OpLoad/OpStore chains through the translator's Function-class shadows and
 * through composite/arithmetic ops (e.g. the position y-flip epilogue).
 * ------------------------------------------------------------------------- */
namespace {

struct SpirvFlow {
    std::unordered_map<uint32_t, uint32_t> defOp;   // result id -> opcode
    std::unordered_map<uint32_t, std::vector<uint32_t>> defArgs; // result id -> id operands
    std::unordered_map<uint32_t, uint32_t> varClass; // var id -> storage class
    std::unordered_map<uint32_t, uint32_t> varType;  // var id -> type id
    std::unordered_map<uint32_t, uint32_t> constU32;  // constant id -> u32 value
    std::unordered_map<uint32_t, uint32_t> pointee;  // pointer type id -> pointee type id
    std::unordered_map<uint64_t, uint32_t> memberBuiltin; // (typeId<<32|member) -> builtin
    std::unordered_map<uint32_t, uint32_t> locDecor; // var id -> Location value
    struct Store { uint32_t ptr, val; };
    std::vector<Store> stores;
    struct Access { uint32_t base, indexId; };
    std::unordered_map<uint32_t, Access> access;     // access-chain result -> base/index

    bool parse(const std::vector<uint32_t>& w) {
        if (w.size() < 5 || w[0] != 0x07230203) return false;
        size_t i = 5; // skip header
        while (i < w.size()) {
            uint32_t word0 = w[i];
            uint32_t wc = word0 >> 16, op = word0 & 0xFFFF;
            if (wc == 0 || i + wc > w.size()) return false;
            auto at = [&](size_t k) -> uint32_t { return w[i + k]; };
            switch (op) {
            case 59: // OpVariable: type, id, storage, [init]
                if (wc >= 4) { varType[at(2)] = at(1); varClass[at(2)] = at(3); }
                break;
            case 71: // OpDecorate: target, decoration, [literals]
                if (wc >= 4 && at(2) == 30) locDecor[at(1)] = at(3); // Location
                break;
            case 72: // OpMemberDecorate: target, member, decoration, [literals]
                if (wc >= 5 && at(3) == 11) // Decoration BuiltIn; literal is the BuiltIn value
                    memberBuiltin[((uint64_t)at(1) << 32) | at(2)] = at(4);
                break;
            case 43: // OpConstant: type, id, value...
                if (wc >= 4) constU32[at(2)] = at(3);
                break;
            case 32: // OpTypePointer: id, storage, pointee type
                if (wc >= 4) pointee[at(1)] = at(3);
                break;
            case 65: // OpAccessChain: type, id, base, idx...
                if (wc >= 5) { access[at(2)] = {at(3), at(4)}; defOp[at(2)] = op; }
                break;
            case 62: // OpStore: ptr, value
                if (wc >= 3) stores.push_back({at(1), at(2)});
                break;
            case 61: // OpLoad: type, id, ptr
                if (wc >= 4) { defOp[at(2)] = op; defArgs[at(2)] = {at(3)}; }
                break;
            case 80: // OpCompositeConstruct
            case 79: // OpVectorShuffle
            case 81: // OpCompositeExtract
            case 127: // OpFNegate
            case 129: // OpFAdd
            case 133: // OpFMul
            case 124: // OpFSub
            case 126: // OpFDiv
            case 148: // OpDot
            case 169: // OpSelect
            case 190: // OpFOrdGreaterThanEqual
                if (wc >= 4) {
                    defOp[at(2)] = op;
                    defArgs[at(2)] = std::vector<uint32_t>(w.begin() + i + 3, w.begin() + i + wc);
                }
                break;
            case 11: // OpExtInstImport: id, name... (no value flow)
                break;
            case 12: // OpExtInst: type, id, set, inst, operands...
                if (wc >= 6) {
                    defOp[at(2)] = op;
                    defArgs[at(2)] = std::vector<uint32_t>(w.begin() + i + 5, w.begin() + i + wc);
                }
                break;
            default:
                break;
            }
            i += wc;
        }
        return true;
    }

    /* Does the value `id` derive from an OpLoad of an Input-class variable? */
    bool flowsFromInput(uint32_t id, int depth, std::set<uint32_t>& seenVars) const {
        if (depth > 64) return false;
        auto it = defOp.find(id);
        if (it == defOp.end()) return false;
        uint32_t op = it->second;
        auto ia = defArgs.find(id);
        if (ia == defArgs.end()) return false;
        const auto& args = ia->second;
        if (op == 61) { // OpLoad
            uint32_t ptr = args[0];
            uint32_t var = ptr;
            auto ac = access.find(ptr);
            if (ac != access.end()) var = ac->second.base;
            auto vc = varClass.find(var);
            if (vc == varClass.end()) return false;
            if (vc->second == 1) return true; // Input
            if (vc->second == 7) { // Function shadow: follow stores into it
                if (!seenVars.insert(var).second) return false;
                for (const auto& s : stores)
                    if (s.ptr == var && flowsFromInput(s.val, depth + 1, seenVars)) return true;
                return false;
            }
            return false;
        }
        // Composite / arithmetic: value flows if any vector operand does.
        for (uint32_t a : args)
            if (flowsFromInput(a, depth + 1, seenVars)) return true;
        return false;
    }

    bool flowsFromInput(uint32_t id) const {
        std::set<uint32_t> seen;
        return flowsFromInput(id, 0, seen);
    }

    /* Access-chain id through which BuiltIn Position is stored, or 0. */
    uint32_t positionStorePtr() const {
        for (const auto& kv : access) {
            auto vt = varType.find(kv.second.base);
            if (vt == varType.end()) continue;
            uint32_t structId = vt->second;
            auto pt = pointee.find(structId); // resolve pointer -> struct
            if (pt != pointee.end()) structId = pt->second;
            auto cv = constU32.find(kv.second.indexId);
            if (cv == constU32.end()) continue;
            uint64_t key = ((uint64_t)structId << 32) | cv->second;
            auto mb = memberBuiltin.find(key);
            if (mb != memberBuiltin.end() && mb->second == 0) { // BuiltIn Position
                auto vc = varClass.find(kv.second.base);
                if (vc != varClass.end() && vc->second == 3) return kv.first;
            }
        }
        return 0;
    }

    /* Output variable id decorated Location N, or 0. */
    uint32_t locationVar(uint32_t loc) const {
        for (const auto& kv : locDecor) {
            if (kv.second != loc) continue;
            auto vc = varClass.find(kv.first);
            if (vc != varClass.end() && vc->second == 3) return kv.first;
        }
        return 0;
    }

    uint32_t storedValue(uint32_t ptr) const {
        for (const auto& s : stores)
            if (s.ptr == ptr) return s.val;
        return 0;
    }
};

/* 1 = value flows from an input, 0 = no flow (misrouted/unwritten), -1 = malformed */
static int check_position_flow(const std::vector<uint32_t>& spirv) {
    SpirvFlow g;
    if (!g.parse(spirv)) return -1;
    uint32_t ptr = g.positionStorePtr();
    if (!ptr) return -1;
    uint32_t val = g.storedValue(ptr);
    if (!val) return 0;
    return g.flowsFromInput(val) ? 1 : 0;
}

static int check_location_flow(const std::vector<uint32_t>& spirv, uint32_t loc) {
    SpirvFlow g;
    if (!g.parse(spirv)) return -1;
    uint32_t var = g.locationVar(loc);
    if (!var) return -1;
    uint32_t val = g.storedValue(var);
    if (!val) return 0;
    return g.flowsFromInput(val) ? 1 : 0;
}

} // namespace

#define TEST_CHECK_VALUE_FLOW(spirv, tag, msg) do { \
    int fres_ = check_position_flow(spirv); \
    if (fres_ != 1) { std::cerr << "  [FAIL] " << msg << " (flow=" << fres_ << ") (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; return 1; } \
    std::cout << "  [PASS] " << msg << std::endl; \
} while(0)

#define TEST_CHECK_LOCATION_FLOW(spirv, tag, loc, msg) do { \
    int fres_ = check_location_flow(spirv, loc); \
    if (fres_ != 1) { std::cerr << "  [FAIL] " << msg << " (flow=" << fres_ << ") (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; return 1; } \
    std::cout << "  [PASS] " << msg << std::endl; \
} while(0)

#define TEST_CHECK_NO_POSITION_FLOW(spirv, tag, msg) do { \
    int fres_ = check_position_flow(spirv); \
    if (fres_ != 0) { std::cerr << "  [FAIL] " << msg << " (flow=" << fres_ << ", expected no flow) (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; return 1; } \
    std::cout << "  [PASS] " << msg << std::endl; \
} while(0)

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
        TEST_CHECK_SPIRV(spirv, "mesa_dcl", "Mesa-style VS passes spirv-val");
        TEST_CHECK_VALUE_FLOW(spirv, "mesa_dcl", "Mesa-style DCL/TEMP VS value flows v0->Position");
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
        TEST_CHECK_SPIRV(spirv, "implicit_o0", "Implicit o0 VS passes spirv-val");
        TEST_CHECK_VALUE_FLOW(spirv, "implicit_o0", "Implicit o0 VS value flows v0->Position");
    }

    // Test 2b: SM3 implicit o1 (no DCL) - should treat r1 as color output
    {
        const uint32_t vs[] = {
            0xFFFE0300, // vs_3_0
            // No DCLs - implicit o1=color
            (1) | (2 << 24),  // MOV
            D3D9_DST(D3DSPR_TEMP_VAL, 1, 0xF), // r1 (implicit o1)
            D3D9_SRC(D3DSPR_INPUT_VAL, 1, 0xE4), // v1
            0x0000FFFF
        };
        std::vector<uint32_t> spirv;
        std::string err;
        auto st = svga3_translate_shader_d3d9(SVGA3D_SHADERTYPE_VS, vs,
                                              sizeof(vs)/sizeof(uint32_t), spirv, err);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "SM3 implicit o1 VS translates");
        TEST_CHECK(!spirv.empty(), "Implicit o1 VS SPIR-V non-empty");
        TEST_CHECK_SPIRV(spirv, "implicit_o1", "Implicit o1 VS passes spirv-val");
        TEST_CHECK_LOCATION_FLOW(spirv, "implicit_o1", 0, "Implicit o1 VS value flows v1->Location 0 (color)");
        /* Negative control: this shader never writes position, so the
         * position-flow assertion must report no flow (proves it is not vacuous). */
        TEST_CHECK_NO_POSITION_FLOW(spirv, "implicit_o1", "Implicit o1 VS has no Position flow (negative control)");
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
        TEST_CHECK_SPIRV(spirv, "normal_out", "Normal OUTPUT VS passes spirv-val");
        TEST_CHECK_VALUE_FLOW(spirv, "normal_out", "Normal OUTPUT VS value flows v0->Position");
    }

    // Test 4: DCL-less SM3 VS writing real OUTPUT registers (o0=position).
    // Same bug class as the TEMP-encoded implicit outputs: without DCLs the
    // translator must still route o0->Position, o1->color.
    {
        const uint32_t vs[] = {
            0xFFFE0300, // vs_3_0, no DCLs
            (1) | (2 << 24),  // MOV
            D3D9_DST(D3DSPR_OUTPUT_VAL, 0, 0xF), // o0 (position, OUTPUT-encoded)
            D3D9_SRC(D3DSPR_INPUT_VAL, 0, 0xE4), // v0
            0x0000FFFF
        };
        std::vector<uint32_t> spirv;
        std::string err;
        auto st = svga3_translate_shader_d3d9(SVGA3D_SHADERTYPE_VS, vs,
                                              sizeof(vs)/sizeof(uint32_t), spirv, err);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "DCL-less OUTPUT-encoded VS translates");
        TEST_CHECK_SPIRV(spirv, "dclless_out", "DCL-less OUTPUT VS passes spirv-val");
        TEST_CHECK_VALUE_FLOW(spirv, "dclless_out", "DCL-less OUTPUT o0 value flows v0->Position");
    }

    /* DCL-less SM3 VS that explicitly writes oPos: oT0 (regtype 6, regNum 0)
     * is a genuine texcoord, NOT the implicit position. This is the
     * test_shader_execution VS1 pattern; an earlier over-broad version of the
     * implicit-output fix rerouted oT0 to Position and broke its rendering. */
    {
        const uint32_t vs[] = {
            0xFFFE0300, // vs_3_0
            (20) | (3 << 24), // M4X4
            D3D9_DST(D3DSPR_RASTOUT_VAL, 0, 0xF), // oPos
            D3D9_SRC(D3DSPR_INPUT_VAL, 0, 0xE4), // v0
            D3D9_SRC(D3DSPR_CONST_VAL, 0, 0xE4), // c0
            (1) | (2 << 24), // MOV
            D3D9_DST(D3DSPR_OUTPUT_VAL, 0, 0xF), // oT0 (regtype 6 == OUTPUT)
            D3D9_SRC(D3DSPR_INPUT_VAL, 1, 0xE4), // v1
            0x0000FFFF
        };
        std::vector<uint32_t> spirv;
        std::string err;
        auto st = svga3_translate_shader_d3d9(SVGA3D_SHADERTYPE_VS, vs,
                                              sizeof(vs)/sizeof(uint32_t), spirv, err);
        TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "oPos+oT0 VS translates");
        TEST_CHECK_SPIRV(spirv, "opos_ot0", "oPos+oT0 VS passes spirv-val");
        TEST_CHECK_VALUE_FLOW(spirv, "opos_ot0", "oPos+oT0 VS value flows v0->Position");
        TEST_CHECK_LOCATION_FLOW(spirv, "opos_ot0", 2, "oPos+oT0 VS value flows v1->Location 2 (texcoord0)");
        int colorFlow = check_location_flow(spirv, 0);
        if (colorFlow != 0) {
            std::cerr << "  [FAIL] oT0 must not flow to color Location 0 (flow=" << colorFlow << ")" << std::endl;
            return 1;
        }
        std::cout << "  [PASS] oT0 does not flow to color Location 0" << std::endl;
    }

    std::cout << "All ICD-free translator tests PASSED!" << std::endl;
    return 0;
}
