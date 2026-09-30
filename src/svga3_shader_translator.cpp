/*
 * SVGA3=VLKN - D3D9 / SVGA3D Guest Shader Bytecode to SPIR-V Translator
 */

#include "svga3_shader_translator.h"
#include "svga3_spirv_builder.h"
#include <map>
#include <unordered_map>
#include <sstream>
#include <iomanip>
#include <iostream>

namespace svga3_vlkn {

/* SVGA3D / D3D9 Opcodes */
enum D3dOpcode {
    D3DSIO_NOP = 0,
    D3DSIO_MOV = 1,
    D3DSIO_ADD = 2,
    D3DSIO_SUB = 3,
    D3DSIO_MAD = 4,
    D3DSIO_MUL = 5,
    D3DSIO_RCP = 6,
    D3DSIO_RSQ = 7,
    D3DSIO_DP3 = 8,
    D3DSIO_DP4 = 9,
    D3DSIO_MIN = 10,
    D3DSIO_MAX = 11,
    D3DSIO_SLT = 12,
    D3DSIO_SGE = 13,
    D3DSIO_EXP = 14,
    D3DSIO_LOG = 15,
    D3DSIO_LIT = 16,
    D3DSIO_DST = 17,
    D3DSIO_LRP = 18,
    D3DSIO_FRC = 19,
    D3DSIO_M4x4 = 20,
    D3DSIO_M4x3 = 21,
    D3DSIO_M3x4 = 22,
    D3DSIO_M3x3 = 23,
    D3DSIO_M3x2 = 24,
    D3DSIO_DCL = 31,
    D3DSIO_POW = 32,
    D3DSIO_CRS = 33,
    D3DSIO_ABS = 35,
    D3DSIO_NRM = 36,
    D3DSIO_SINCOS = 37,
    D3DSIO_TEXCOORD = 64,
    D3DSIO_TEXKILL = 65,
    D3DSIO_TEX = 66,
    D3DSIO_DEF = 81,
    D3DSIO_CMP = 88,
    D3DSIO_DP2ADD = 90,
    D3DSIO_TEXLDD = 93,
    D3DSIO_TEXLDL = 95,
    D3DSIO_COMMENT = 0xFFFE,
    D3DSIO_END = 0xFFFF
};

/* Register Types */
enum D3dRegType {
    D3DSPR_TEMP = 0,
    D3DSPR_INPUT = 1,
    D3DSPR_CONST = 2,
    D3DSPR_ADDR = 3,
    D3DSPR_TEXTURE = 3,
    D3DSPR_RASTOUT = 4,
    D3DSPR_ATTROUT = 5,
    D3DSPR_TEXCRDOUT = 6,
    D3DSPR_OUTPUT = 6,
    D3DSPR_CONSTINT = 7,
    D3DSPR_COLOROUT = 8,
    D3DSPR_DEPTHOUT = 9,
    D3DSPR_SAMPLER = 10,
    D3DSPR_CONSTBOOL = 11
};

struct ParsedDest {
    uint32_t regNum;
    uint32_t regType;
    uint32_t writeMask;
    uint32_t dstMod;
};

struct ParsedSrc {
    uint32_t regNum;
    uint32_t regType;
    uint32_t swizzle[4];
    uint32_t srcMod;
};

struct ShaderDefConst {
    float values[4];
};

static bool parseDest(uint32_t token, ParsedDest &dst) {
    if ((token & 0x80000000) == 0) return false;
    dst.regNum = token & 0x7FF;
    dst.regType = ((token >> 28) & 0x7) | ((token >> 8) & 0x18);
    dst.writeMask = (token >> 16) & 0x0F;
    dst.dstMod = (token >> 20) & 0x0F;
    return true;
}

static bool parseSrc(uint32_t token, ParsedSrc &src) {
    if ((token & 0x80000000) == 0) return false;
    src.regNum = token & 0x7FF;
    src.regType = ((token >> 28) & 0x7) | ((token >> 8) & 0x18);
    uint32_t swiz = (token >> 16) & 0xFF;
    src.swizzle[0] = (swiz >> 0) & 0x3;
    src.swizzle[1] = (swiz >> 2) & 0x3;
    src.swizzle[2] = (swiz >> 4) & 0x3;
    src.swizzle[3] = (swiz >> 6) & 0x3;
    src.srcMod = (token >> 24) & 0x0F;
    return true;
}

/* Detects the SM 1.x dest-only `tex t#` form: zero length field with a
 * texture-register destination, carrying only a destination operand and no
 * coordinate source. */
static bool isDestOnlyTex(const uint32_t *tokens, uint32_t pc, uint32_t numTokens, uint32_t instLen) {
    if (instLen != 0 || pc + 1 >= numTokens) {
        return false;
    }
    ParsedDest dst;
    return parseDest(tokens[pc + 1], dst) && dst.regType == D3DSPR_TEXTURE;
}

Svga3VlknStatus svga3_translate_shader_d3d9(SVGA3dShaderType shaderType,
                                            const uint32_t *tokens,
                                            uint32_t numTokens,
                                            std::vector<uint32_t> &outSpirv,
                                            std::string &outError,
                                            uint32_t *outInputMask,
                                            uint32_t depthSamplerMask)
{
    outSpirv.clear();
    outError.clear();
    if (outInputMask) *outInputMask = 0;

    /* Debug: dump D3D9 input alongside SPIR-V when SVGA3_VLKN_DUMP_SPIRV is set. */
    if (getenv("SVGA3_VLKN_DUMP_SPIRV")) {
        char path[256];
        static int d3d9DumpIdx = 0;
        int idx = __sync_fetch_and_add(&d3d9DumpIdx, 1);
        snprintf(path, sizeof(path), "/tmp/d3d9_dump_%d_%s.bin",
                 idx, shaderType == SVGA3D_SHADERTYPE_VS ? "vs" : "ps");
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(tokens, sizeof(uint32_t), numTokens, f);
            fclose(f);
        }
    }

    if (!tokens || numTokens < 2) {
        outError = "Shader bytecode too short or null";
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }

    /* Cap shader bytecode: without a limit a guest can submit megabytes of
     * bytecode, exhausting host memory in the translator and SPIR-V
     * builder. 64K dwords (256KB) is far above any legitimate shader. */
    static const uint32_t MAX_SHADER_DWORDS = 65536;
    if (numTokens > MAX_SHADER_DWORDS) {
        outError = "Shader bytecode exceeds maximum size";
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }

    uint32_t versionToken = tokens[0];
    uint32_t magic = versionToken >> 16;
    uint32_t major = (versionToken >> 8) & 0xFF;
    uint32_t minor = versionToken & 0xFF;
    (void)major; (void)minor;

    bool isVS = (shaderType == SVGA3D_SHADERTYPE_VS);
    if (isVS) {
        if (magic != 0xFFFE) {
            std::ostringstream ss;
            ss << "Invalid Vertex Shader version magic 0x" << std::hex << magic << " (expected 0xFFFE)";
            outError = ss.str();
            return SVGA3_VLKN_ERROR_INVALID_PARAM;
        }
    } else {
        if (magic != 0xFFFF) {
            std::ostringstream ss;
            ss << "Invalid Pixel Shader version magic 0x" << std::hex << magic << " (expected 0xFFFF)";
            outError = ss.str();
            return SVGA3_VLKN_ERROR_INVALID_PARAM;
        }
    }

    /* First pass: validate opcodes and extract DEF constants */
    std::map<uint32_t, ShaderDefConst> defConstants;
    std::vector<uint32_t> dclPositions; /* true DCL instruction boundaries */
    std::unordered_map<uint32_t, bool> explicitVsOutputRegs;
    /* DCL-less SM 3.0 VS: o0/oT0 (regtype 6) is ambiguous — it is the implicit
     * position output only when the shader does NOT explicitly write oPos
     * (RASTOUT). If oPos is written, regtype-6 outputs are real texcoords and
     * TEMP r0/r1 are genuine temporaries. Computed in the pre-pass below. */
    bool writesExplicitPosition = false;
    uint32_t pc = 1;
    bool foundEnd = false;

    while (pc < numTokens) {
        uint32_t instToken = tokens[pc];
        uint32_t op = instToken & 0xFFFF;

        if (op == D3DSIO_END) {
            foundEnd = true;
            break;
        }

        if (op == D3DSIO_COMMENT) {
            uint32_t count = (instToken >> 16) & 0x7FFF;
            if (1 + count > numTokens - pc) {
                outError = "Truncated D3DSIO_COMMENT instruction";
                return SVGA3_VLKN_ERROR_INVALID_PARAM;
            }
            pc += 1 + count;
            continue;
        }

        uint32_t instLen = (instToken >> 24) & 0x0F;
        if (op == D3DSIO_DEF) {
            if (pc + 5 >= numTokens) {
                outError = "Truncated D3DSIO_DEF instruction";
                return SVGA3_VLKN_ERROR_INVALID_PARAM;
            }
            ParsedDest dst;
            if (!parseDest(tokens[pc + 1], dst)) {
                outError = "Malformed D3DSIO_DEF dest parameter";
                return SVGA3_VLKN_ERROR_INVALID_PARAM;
            }
            if (dst.regType != D3DSPR_CONST || dst.regNum >= 256 ||
                (major >= 2 && instLen != 5)) {
                outError = "Invalid D3DSIO_DEF destination or operand count";
                return SVGA3_VLKN_ERROR_INVALID_PARAM;
            }
            ShaderDefConst def;
            memcpy(def.values, &tokens[pc + 2], sizeof(float) * 4);
            defConstants[dst.regNum] = def;
            pc += 6;
            continue;
        }

        /* Reject malformed lengths before either pass can read parameters.
         * D3D9 token lengths count every following parameter token. */
        uint32_t requiredParams = UINT32_MAX;
        switch (op) {
            case D3DSIO_NOP: requiredParams = 0; break;
            case D3DSIO_MOV: case D3DSIO_RCP: case D3DSIO_RSQ: case D3DSIO_ABS:
            case D3DSIO_FRC: case D3DSIO_DCL:
                requiredParams = 2; break;
            case D3DSIO_ADD: case D3DSIO_SUB: case D3DSIO_MUL: case D3DSIO_DP3:
            case D3DSIO_DP4: case D3DSIO_MIN: case D3DSIO_MAX: case D3DSIO_M4x4:
            case D3DSIO_TEX: case D3DSIO_POW:
                requiredParams = 3; break;
            case D3DSIO_MAD: case D3DSIO_LRP: case D3DSIO_CMP:
                requiredParams = 4; break;
            default: break; /* The opcode allowlist below rejects these. */
        }
        if (requiredParams != UINT32_MAX) {
            if (instToken & (1u << 28)) {
                outError = "Predicated shader instructions are not supported";
                return SVGA3_VLKN_ERROR_UNSUPPORTED_SHADER;
            }
            if ((major >= 2 && instLen != requiredParams) ||
                requiredParams >= numTokens - pc) {
                outError = "Invalid shader instruction operand count";
                return SVGA3_VLKN_ERROR_INVALID_PARAM;
            }
            for (uint32_t i = 1; i <= requiredParams; ++i) {
                if (!(tokens[pc + i] & 0x80000000u)) {
                    outError = "Invalid shader parameter token";
                    return SVGA3_VLKN_ERROR_INVALID_PARAM;
                }
                if (i > 1 && (tokens[pc + i] & (1u << 13))) {
                    outError = "Relative register addressing is not supported";
                    return SVGA3_VLKN_ERROR_UNSUPPORTED_SHADER;
                }
            }
        }

        /* Check for unsupported instructions */
        switch (op) {
            case D3DSIO_NOP:
            case D3DSIO_MOV:
            case D3DSIO_ADD:
            case D3DSIO_SUB:
            case D3DSIO_MUL:
            case D3DSIO_MAD:
            case D3DSIO_DP3:
            case D3DSIO_DP4:
            case D3DSIO_M4x4:
            case D3DSIO_RCP:
            case D3DSIO_RSQ:
            case D3DSIO_MIN:
            case D3DSIO_MAX:
            case D3DSIO_ABS:
            case D3DSIO_LRP:
            case D3DSIO_FRC:
            case D3DSIO_CMP:
            case D3DSIO_TEX:
            case D3DSIO_DCL:
                break;
            default: {
                std::ostringstream ss;
                ss << "Unsupported D3D9/SVGA3D shader opcode: " << op;
                outError = ss.str();
                return SVGA3_VLKN_ERROR_UNSUPPORTED_SHADER;
            }
        }

        /* Record true DCL instruction boundaries for the semantic scan. */
        if (op == D3DSIO_DCL) {
            dclPositions.push_back(pc);
        }

        /* Advance PC, verifying the parameter tokens exist first. */
        uint32_t advance;
        if (instLen > 0) {
            advance = 1 + instLen;
        } else {
            /* Fallback parameter count for SM 1.x where length field was 0 */
            switch (op) {
                case D3DSIO_NOP: advance = 1; break;
                case D3DSIO_MOV:
                case D3DSIO_RCP:
                case D3DSIO_RSQ:
                case D3DSIO_ABS:
                case D3DSIO_FRC: advance = 3; break;
                case D3DSIO_ADD:
                case D3DSIO_SUB:
                case D3DSIO_MUL:
                case D3DSIO_DP3:
                case D3DSIO_DP4:
                case D3DSIO_MIN:
                case D3DSIO_MAX:
                case D3DSIO_M4x4: advance = 4; break;
                case D3DSIO_TEX:
                    /* SM 1.x dest-only `tex t#` carries just the destination. */
                    advance = isDestOnlyTex(tokens, pc, numTokens, instLen) ? 2 : 4;
                    break;
                case D3DSIO_MAD:
                case D3DSIO_LRP:
                case D3DSIO_CMP: advance = 5; break;
                case D3DSIO_DCL: advance = 3; break;
                default: advance = 1; break;
            }
        }
        if (advance > numTokens - pc) {
            outError = "Truncated shader instruction: parameter tokens exceed bytecode length";
            return SVGA3_VLKN_ERROR_INVALID_PARAM;
        }
        /* D3D9 TEMP and OUTPUT registers are separate namespaces. Remember
         * explicit OUTPUT writes so the Mesa TEMP-output compatibility path
         * below does not steal same-numbered temporaries used for calculations. */
        if (isVS && major >= 3 && op != D3DSIO_DCL && advance > 1) {
            ParsedDest dst;
            if (parseDest(tokens[pc + 1], dst)) {
                if (dst.regType == D3DSPR_OUTPUT) {
                    explicitVsOutputRegs[dst.regNum] = true;
                }
                if (dst.regType == D3DSPR_RASTOUT) {
                    writesExplicitPosition = true;
                }
            }
        }
        pc += advance;
    }

    if (!foundEnd) {
        outError = "Shader bytecode missing D3DSIO_END terminator";
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }

    /* Second pass: SPIR-V generation */
    SpirvBuilder b;

    /* Capabilities */
    b.emitInst(b.capabilities, SpvOpCapability, { SpvCapabilityShader });

    /* ExtInstImport GLSL.std.450 */
    uint32_t glslSetId = b.allocId();
    std::vector<uint32_t> extOps;
    extOps.push_back(glslSetId);
    b.emitString(extOps, "GLSL.std.450");
    b.emitInst(b.extInstImports, SpvOpExtInstImport, extOps);

    /* MemoryModel */
    b.emitInst(b.memoryModel, SpvOpMemoryModel, { SpvAddressingModelLogical, SpvMemoryModelGLSL450 });

    /* Forward allocate IDs for entry point interface and common types */
    uint32_t mainFuncId = b.allocId();
    uint32_t typeVoid = b.allocId();
    uint32_t typeFunc = b.allocId();
    uint32_t typeBool = b.allocId();
    uint32_t typeV4Bool = b.allocId();
    uint32_t typeFloat = b.allocId();
    uint32_t typeV2Float = b.allocId();
    uint32_t typeV3Float = b.allocId();
    uint32_t typeV4Float = b.allocId();
    uint32_t typeInt = b.allocId();
    uint32_t typeUInt = b.allocId();

    /* Common Constants */
    uint32_t const0_f = b.allocId();
    uint32_t const1_f = b.allocId();
    uint32_t constHalf_f = b.allocId();
    uint32_t const2_f = b.allocId();
    uint32_t const0_v4 = b.allocId();
    uint32_t const1_v4 = b.allocId();

    /* Pointer Types */
    uint32_t ptrFunctionV4Float = b.allocId();
    uint32_t ptrInputV4Float = b.allocId();
    uint32_t ptrOutputV4Float = b.allocId();

    /* Collect DCL semantic declarations */
    struct SemanticInfo {
        uint32_t usage;
        uint32_t usageIndex;
    };
    std::unordered_map<uint32_t, SemanticInfo> inputRegToSemantic;
    std::unordered_map<uint32_t, SemanticInfo> outputRegToSemantic;
    /* Only honor DCL instructions at true instruction boundaries, as recorded
     * during pass 1. Scanning raw tokens would misread parameter words or DEF
     * literals whose low 16 bits happen to equal D3DSIO_DCL. */
    for (uint32_t dclPc : dclPositions) {
        if (dclPc + 2 >= numTokens) {
            continue; /* Cannot happen: pass 1 validated the bounds. */
        }
        uint32_t semToken = tokens[dclPc + 1];
        uint32_t regToken = tokens[dclPc + 2];
        uint32_t usage = semToken & 0x1F;
        uint32_t usageIndex = (semToken >> 16) & 0x0F;
        uint32_t regNum = regToken & 0x7FF;
        uint32_t regType = ((regToken >> 28) & 0x7) | (((regToken >> 8) & 0x18));
        if (regType == D3DSPR_INPUT || regType == D3DSPR_TEXTURE) {
            inputRegToSemantic[regNum] = { usage, usageIndex };
        } else if (regType == 6) { /* D3DSPR_OUTPUT in SM 3.0 */
            outputRegToSemantic[regNum] = { usage, usageIndex };
        }
    }

    auto getVsInputLocation = [](uint32_t usage, uint32_t usageIndex) -> uint32_t {
        if (usage == 0 || usage == 9) { /* POSITION or POSITIONT */
            return 0;
        } else if (usage == 10) { /* COLOR */
            return (usageIndex == 0) ? 1 : 7;
        } else if (usage == 5) { /* TEXCOORD */
            return 2 + usageIndex;
        } else if (usage == 3) { /* NORMAL */
            return 6;
        }
        return 8 + usageIndex;
    };

    /* Interface Variables */
    std::vector<uint32_t> entryInterface;

    struct VsInputInfo {
        uint32_t varId;
        uint32_t location;
    };
    std::unordered_map<uint32_t, VsInputInfo> vsInRegs;

    uint32_t vsGlPerVertex = 0;
    uint32_t vsOutColor[2] = { 0 };
    uint32_t vsOutTexCoords[8] = { 0 };

    /* PS Interface */
    uint32_t psInColor[2] = { 0 };
    uint32_t psInTexCoords[8] = { 0 };
    uint32_t psOutColor = 0;

    /* Samplers. depthSamplerMask bit N means stage N is a depth texture. */
    uint32_t samplerVars[8] = { 0 };
    uint32_t typeSampledImage2D = 0;
    uint32_t typeSampledDepth2D = 0;

    /* Uniform Buffer for constants c[256] */
    uint32_t uboBlockVar = b.allocId();

    uint32_t inLocMask = 0;

    if (isVS) {
        vsGlPerVertex = b.allocId();
        entryInterface.push_back(vsGlPerVertex);

        for (int c = 0; c < 2; ++c) {
            vsOutColor[c] = b.allocId();
            entryInterface.push_back(vsOutColor[c]);
        }
        for (int t = 0; t < 8; ++t) {
            vsOutTexCoords[t] = b.allocId();
            entryInterface.push_back(vsOutTexCoords[t]);
        }

        if (inputRegToSemantic.empty()) {
            uint32_t defRegs[] = { 0, 1, 2, 3 };
            uint32_t defUsages[] = { 0, 10, 5, 5 };
            uint32_t defIndices[] = { 0, 0, 0, 1 };
            for (int k = 0; k < 4; ++k) {
                uint32_t vid = b.allocId();
                uint32_t loc = getVsInputLocation(defUsages[k], defIndices[k]);
                vsInRegs[defRegs[k]] = { vid, loc };
                entryInterface.push_back(vid);
                inLocMask |= (1u << loc);
            }
        } else {
            for (const auto &pair : inputRegToSemantic) {
                uint32_t vid = b.allocId();
                uint32_t loc = getVsInputLocation(pair.second.usage, pair.second.usageIndex);
                vsInRegs[pair.first] = { vid, loc };
                entryInterface.push_back(vid);
                inLocMask |= (1u << loc);
            }
        }
        if (outInputMask) *outInputMask = inLocMask;
    } else {
        for (int c = 0; c < 2; ++c) {
            psInColor[c] = b.allocId();
            entryInterface.push_back(psInColor[c]);
        }
        for (int t = 0; t < 8; ++t) {
            psInTexCoords[t] = b.allocId();
            entryInterface.push_back(psInTexCoords[t]);
        }
        psOutColor = b.allocId();
        entryInterface.push_back(psOutColor);

        for (int i = 0; i < 8; ++i) {
            samplerVars[i] = b.allocId();
        }
    }

    /* EntryPoint */
    std::vector<uint32_t> entryOps;
    entryOps.push_back(isVS ? SpvExecutionModelVertex : SpvExecutionModelFragment);
    entryOps.push_back(mainFuncId);
    b.emitString(entryOps, "main");
    for (uint32_t ifId : entryInterface) {
        entryOps.push_back(ifId);
    }
    b.emitInst(b.entryPoints, SpvOpEntryPoint, entryOps);

    /* ExecutionMode for Fragment: OriginUpperLeft */
    if (!isVS) {
        b.emitInst(b.executionModes, SpvOpExecutionMode, { mainFuncId, SpvExecutionModeOriginUpperLeft });
    }

    /* Annotations */
    uint32_t perVertexStructType = 0;
    if (isVS) {
        for (const auto &pair : vsInRegs) {
            b.emitInst(b.annotations, SpvOpDecorate, { pair.second.varId, SpvDecorationLocation, pair.second.location });
        }

        for (int c = 0; c < 2; ++c) {
            b.emitInst(b.annotations, SpvOpDecorate, { vsOutColor[c], SpvDecorationLocation, (uint32_t)c });
        }
        for (int t = 0; t < 8; ++t) {
            b.emitInst(b.annotations, SpvOpDecorate, { vsOutTexCoords[t], SpvDecorationLocation, (uint32_t)(2 + t) });
        }

        /* Decorate gl_PerVertex struct */
        perVertexStructType = b.allocId();
        b.emitInst(b.annotations, SpvOpMemberDecorate, { perVertexStructType, 0, SpvDecorationBuiltIn, SpvBuiltInPosition });
        b.emitInst(b.annotations, SpvOpDecorate, { perVertexStructType, SpvDecorationBlock });
    } else {
        for (int c = 0; c < 2; ++c) {
            b.emitInst(b.annotations, SpvOpDecorate, { psInColor[c], SpvDecorationLocation, (uint32_t)c });
        }
        for (int t = 0; t < 8; ++t) {
            b.emitInst(b.annotations, SpvOpDecorate, { psInTexCoords[t], SpvDecorationLocation, (uint32_t)(2 + t) });
        }
        b.emitInst(b.annotations, SpvOpDecorate, { psOutColor, SpvDecorationLocation, 0 });

        for (int i = 0; i < 8; ++i) {
            b.emitInst(b.annotations, SpvOpDecorate, { samplerVars[i], SpvDecorationDescriptorSet, 0 });
            b.emitInst(b.annotations, SpvOpDecorate, { samplerVars[i], SpvDecorationBinding, (uint32_t)(2 + i) });
        }
    }

    /* UBO Block Decoration */
    uint32_t uboStructType = b.allocId();
    b.emitInst(b.annotations, SpvOpMemberDecorate, { uboStructType, 0, SpvDecorationOffset, 0 });
    b.emitInst(b.annotations, SpvOpDecorate, { uboStructType, SpvDecorationBlock });
    b.emitInst(b.annotations, SpvOpDecorate, { uboBlockVar, SpvDecorationDescriptorSet, 0 });
    b.emitInst(b.annotations, SpvOpDecorate, { uboBlockVar, SpvDecorationBinding, isVS ? 0u : 1u });

    /* Types, Constants & Globals */
    b.emitInst(b.typesConstantsGlobals, SpvOpTypeVoid, { typeVoid });
    b.emitInst(b.typesConstantsGlobals, SpvOpTypeFunction, { typeFunc, typeVoid });
    b.emitInst(b.typesConstantsGlobals, SpvOpTypeBool, { typeBool });
    b.emitInst(b.typesConstantsGlobals, SpvOpTypeVector, { typeV4Bool, typeBool, 4 });
    b.emitInst(b.typesConstantsGlobals, SpvOpTypeFloat, { typeFloat, 32 });
    b.emitInst(b.typesConstantsGlobals, SpvOpTypeVector, { typeV2Float, typeFloat, 2 });
    b.emitInst(b.typesConstantsGlobals, SpvOpTypeVector, { typeV3Float, typeFloat, 3 });
    b.emitInst(b.typesConstantsGlobals, SpvOpTypeVector, { typeV4Float, typeFloat, 4 });
    b.emitInst(b.typesConstantsGlobals, SpvOpTypeInt, { typeInt, 32, 1 });
    b.emitInst(b.typesConstantsGlobals, SpvOpTypeInt, { typeUInt, 32, 0 });

    /* Pointer Types */
    b.emitInst(b.typesConstantsGlobals, SpvOpTypePointer, { ptrFunctionV4Float, SpvStorageClassFunction, typeV4Float });
    b.emitInst(b.typesConstantsGlobals, SpvOpTypePointer, { ptrInputV4Float, SpvStorageClassInput, typeV4Float });
    b.emitInst(b.typesConstantsGlobals, SpvOpTypePointer, { ptrOutputV4Float, SpvStorageClassOutput, typeV4Float });

    /* Scalar & Vector Float Constants */
    union { float f; uint32_t u; } f2u;
    f2u.f = 0.0f; b.emitInst(b.typesConstantsGlobals, SpvOpConstant, { typeFloat, const0_f, f2u.u });
    f2u.f = 1.0f; b.emitInst(b.typesConstantsGlobals, SpvOpConstant, { typeFloat, const1_f, f2u.u });
    f2u.f = 0.5f; b.emitInst(b.typesConstantsGlobals, SpvOpConstant, { typeFloat, constHalf_f, f2u.u });
    f2u.f = 2.0f; b.emitInst(b.typesConstantsGlobals, SpvOpConstant, { typeFloat, const2_f, f2u.u });

    b.emitInst(b.typesConstantsGlobals, SpvOpConstantComposite, { typeV4Float, const0_v4, const0_f, const0_f, const0_f, const0_f });
    b.emitInst(b.typesConstantsGlobals, SpvOpConstantComposite, { typeV4Float, const1_v4, const1_f, const1_f, const1_f, const1_f });

    /* Integer constants 0, 1, 2, 3 for vector extraction */
    uint32_t intConsts[4];
    for (uint32_t i = 0; i < 4; ++i) {
        intConsts[i] = b.allocId();
        b.emitInst(b.typesConstantsGlobals, SpvOpConstant, { typeInt, intConsts[i], i });
    }

    /* UBO Type: vec4 c[256] */
    uint32_t const256_u = b.allocId();
    b.emitInst(b.typesConstantsGlobals, SpvOpConstant, { typeUInt, const256_u, 256 });
    uint32_t array256V4Float = b.allocId();
    b.emitInst(b.typesConstantsGlobals, SpvOpTypeArray, { array256V4Float, typeV4Float, const256_u });
    b.emitInst(b.annotations, SpvOpDecorate, { array256V4Float, SpvDecorationArrayStride, 16 });

    /* UBO struct type */
    b.emitInst(b.typesConstantsGlobals, SpvOpTypeStruct, { uboStructType, array256V4Float });
    uint32_t ptrUniformUbo = b.allocId();
    b.emitInst(b.typesConstantsGlobals, SpvOpTypePointer, { ptrUniformUbo, SpvStorageClassUniform, uboStructType });
    uint32_t ptrUniformV4Float = b.allocId();
    b.emitInst(b.typesConstantsGlobals, SpvOpTypePointer, { ptrUniformV4Float, SpvStorageClassUniform, typeV4Float });

    /* UBO Variable */
    b.emitInst(b.typesConstantsGlobals, SpvOpVariable, { ptrUniformUbo, uboBlockVar, SpvStorageClassUniform });

    /* Interface Variables Definition */
    uint32_t ptrOutputPerVertex = 0;
    if (isVS) {
        for (const auto &pair : vsInRegs) {
            b.emitInst(b.typesConstantsGlobals, SpvOpVariable, { ptrInputV4Float, pair.second.varId, SpvStorageClassInput });
        }

        /* gl_PerVertex struct { vec4 gl_Position; } */
        b.emitInst(b.typesConstantsGlobals, SpvOpTypeStruct, { perVertexStructType, typeV4Float });
        ptrOutputPerVertex = b.allocId();
        b.emitInst(b.typesConstantsGlobals, SpvOpTypePointer, { ptrOutputPerVertex, SpvStorageClassOutput, perVertexStructType });
        b.emitInst(b.typesConstantsGlobals, SpvOpVariable, { ptrOutputPerVertex, vsGlPerVertex, SpvStorageClassOutput });

        for (int c = 0; c < 2; ++c) {
            b.emitInst(b.typesConstantsGlobals, SpvOpVariable, { ptrOutputV4Float, vsOutColor[c], SpvStorageClassOutput });
        }
        for (int t = 0; t < 8; ++t) {
            b.emitInst(b.typesConstantsGlobals, SpvOpVariable, { ptrOutputV4Float, vsOutTexCoords[t], SpvStorageClassOutput });
        }
    } else {
        for (int c = 0; c < 2; ++c) {
            b.emitInst(b.typesConstantsGlobals, SpvOpVariable, { ptrInputV4Float, psInColor[c], SpvStorageClassInput });
        }
        for (int t = 0; t < 8; ++t) {
            b.emitInst(b.typesConstantsGlobals, SpvOpVariable, { ptrInputV4Float, psInTexCoords[t], SpvStorageClassInput });
        }
        b.emitInst(b.typesConstantsGlobals, SpvOpVariable, { ptrOutputV4Float, psOutColor, SpvStorageClassOutput });

        /* Sampler Types: OpTypeImage, OpTypeSampledImage.
         * Depth images must be declared with Depth=1. Sampling a depth image
         * through an Unknown/color image type is illegal and faults the driver. */
        uint32_t typeImage2D = b.allocId();
        b.emitInst(b.typesConstantsGlobals, SpvOpTypeImage, { typeImage2D, typeFloat, SpvDim2D, 0, 0, 0, 1, 0 });
        typeSampledImage2D = b.allocId();
        b.emitInst(b.typesConstantsGlobals, SpvOpTypeSampledImage, { typeSampledImage2D, typeImage2D });
        uint32_t ptrColorSampler = b.allocId();
        b.emitInst(b.typesConstantsGlobals, SpvOpTypePointer, { ptrColorSampler, SpvStorageClassUniformConstant, typeSampledImage2D });

        uint32_t typeDepthImage = b.allocId();
        b.emitInst(b.typesConstantsGlobals, SpvOpTypeImage, { typeDepthImage, typeFloat, SpvDim2D, 1, 0, 0, 1, 0 });
        typeSampledDepth2D = b.allocId();
        b.emitInst(b.typesConstantsGlobals, SpvOpTypeSampledImage, { typeSampledDepth2D, typeDepthImage });
        uint32_t ptrDepthSampler = b.allocId();
        b.emitInst(b.typesConstantsGlobals, SpvOpTypePointer, { ptrDepthSampler, SpvStorageClassUniformConstant, typeSampledDepth2D });

        for (int i = 0; i < 8; ++i) {
            uint32_t ptrType = (depthSamplerMask & (1u << i)) ? ptrDepthSampler : ptrColorSampler;
            b.emitInst(b.typesConstantsGlobals, SpvOpVariable, { ptrType, samplerVars[i], SpvStorageClassUniformConstant });
        }
    }

    /* Function Definition */
    b.emitInst(b.functionDefinitions, SpvOpFunction, { typeVoid, mainFuncId, 0, typeFunc });
    uint32_t labelEntry = b.allocId();
    b.emitInst(b.functionDefinitions, SpvOpLabel, { labelEntry });

    /* Temporary Register Allocation: r0..r15 as Function local vec4 pointers */
    uint32_t rVars[16];
    for (int i = 0; i < 16; ++i) {
        rVars[i] = b.allocId();
        b.emitInst(b.functionDefinitions, SpvOpVariable, { ptrFunctionV4Float, rVars[i], SpvStorageClassFunction, const0_v4 });
    }

    /* Output Registers Function locals */
    uint32_t outPosVar = b.allocId();
    uint32_t outColorVar[2] = { b.allocId(), b.allocId() };
    uint32_t outTexCoordVar[8];
    for (int t = 0; t < 8; ++t) outTexCoordVar[t] = b.allocId();

    b.emitInst(b.functionDefinitions, SpvOpVariable, { ptrFunctionV4Float, outPosVar, SpvStorageClassFunction, const0_v4 });
    b.emitInst(b.functionDefinitions, SpvOpVariable, { ptrFunctionV4Float, outColorVar[0], SpvStorageClassFunction, const1_v4 });
    b.emitInst(b.functionDefinitions, SpvOpVariable, { ptrFunctionV4Float, outColorVar[1], SpvStorageClassFunction, const1_v4 });
    for (int t = 0; t < 8; ++t) {
        b.emitInst(b.functionDefinitions, SpvOpVariable, { ptrFunctionV4Float, outTexCoordVar[t], SpvStorageClassFunction, const0_v4 });
    }

    /* Fail-closed operand validation: the emit helpers below set transError
     * (and outError) on out-of-range indices, unsupported register classes,
     * or unimplemented source modifiers. The translation loop checks the
     * flag once per instruction and aborts. */
    bool transError = false;
    Svga3VlknStatus transErrorCode = SVGA3_VLKN_ERROR_INVALID_PARAM;

    /* Helper: load source register into vec4 value with swizzle and modifier */
    auto emitLoadSrc = [&](const ParsedSrc &src) -> uint32_t {
        /* Only None / Negate / Abs / AbsNeg source modifiers are implemented. */
        if (src.srcMod != 0 && src.srcMod != 1 && src.srcMod != 11 && src.srcMod != 12) {
            std::ostringstream ss;
            ss << "Unsupported D3D9 source modifier: " << src.srcMod;
            outError = ss.str();
            transError = true;
            transErrorCode = SVGA3_VLKN_ERROR_UNSUPPORTED_SHADER;
            return const0_v4;
        }

        uint32_t baseVal = 0;

        if (src.regType == D3DSPR_TEMP) {
            if (src.regNum >= 16) {
                outError = "Temporary register index out of range (r0-r15 supported)";
                transError = true;
                transErrorCode = SVGA3_VLKN_ERROR_INVALID_PARAM;
                return const0_v4;
            }
            baseVal = b.allocId();
            b.emitInst(b.functionDefinitions, SpvOpLoad, { typeV4Float, baseVal, rVars[src.regNum] });
        } else if (src.regType == D3DSPR_INPUT || src.regType == D3DSPR_TEXTURE) {
            baseVal = b.allocId();
            if (isVS) {
                auto it = vsInRegs.find(src.regNum);
                if (it != vsInRegs.end()) {
                    b.emitInst(b.functionDefinitions, SpvOpLoad, { typeV4Float, baseVal, it->second.varId });
                } else {
                    baseVal = const0_v4;
                }
            } else {
                uint32_t inVar = 0;
                auto it = inputRegToSemantic.find(src.regNum);
                if (it != inputRegToSemantic.end()) {
                    if (it->second.usage == 10) {
                        inVar = psInColor[it->second.usageIndex < 2 ? it->second.usageIndex : 0];
                    } else if (it->second.usage == 5) {
                        inVar = psInTexCoords[it->second.usageIndex < 8 ? it->second.usageIndex : 0];
                    } else {
                        inVar = psInColor[0];
                    }
                } else {
                    if (src.regType == D3DSPR_INPUT) {
                        inVar = psInColor[src.regNum < 2 ? src.regNum : 0];
                    } else if (src.regType == D3DSPR_TEXTURE) {
                        inVar = psInTexCoords[src.regNum < 8 ? src.regNum : 0];
                    } else {
                        inVar = psInColor[0];
                    }
                }
                if (inVar != 0) {
                    b.emitInst(b.functionDefinitions, SpvOpLoad, { typeV4Float, baseVal, inVar });
                } else {
                    baseVal = const0_v4;
                }
            }
        } else if (src.regType == D3DSPR_CONST) {
            if (src.regNum >= 256) {
                outError = "Constant register index out of range (c0-c255 supported)";
                transError = true;
                transErrorCode = SVGA3_VLKN_ERROR_INVALID_PARAM;
                return const0_v4;
            }
            /* Check if defined by DEF */
            auto it = defConstants.find(src.regNum);
            if (it != defConstants.end()) {
                /* Create constant composite */
                uint32_t cf[4];
                for (int c = 0; c < 4; ++c) {
                    cf[c] = b.allocId();
                    union { float f; uint32_t u; } cv;
                    cv.f = it->second.values[c];
                    b.emitInst(b.typesConstantsGlobals, SpvOpConstant, { typeFloat, cf[c], cv.u });
                }
                baseVal = b.allocId();
                b.emitInst(b.typesConstantsGlobals, SpvOpConstantComposite, { typeV4Float, baseVal, cf[0], cf[1], cf[2], cf[3] });
            } else {
                /* Load from UBO */
                uint32_t regConstId = b.allocId();
                b.emitInst(b.typesConstantsGlobals, SpvOpConstant, { typeInt, regConstId, src.regNum });
                uint32_t elemPtr = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpAccessChain, { ptrUniformV4Float, elemPtr, uboBlockVar, intConsts[0], regConstId });
                baseVal = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpLoad, { typeV4Float, baseVal, elemPtr });
            }
        } else {
            /* ADDR-as-source is handled by the TEXTURE branch above; anything
             * reaching here (integer/bool constants, sampler-as-source, loop
             * counters, output registers) has no representation. */
            std::ostringstream ss;
            ss << "Unsupported D3D9 source register type: " << src.regType;
            outError = ss.str();
            transError = true;
            transErrorCode = SVGA3_VLKN_ERROR_UNSUPPORTED_SHADER;
            return const0_v4;
        }

        /* Swizzle */
        uint32_t swizVal = baseVal;
        if (src.swizzle[0] != 0 || src.swizzle[1] != 1 || src.swizzle[2] != 2 || src.swizzle[3] != 3) {
            swizVal = b.allocId();
            b.emitInst(b.functionDefinitions, SpvOpVectorShuffle, {
                typeV4Float, swizVal, baseVal, baseVal,
                src.swizzle[0], src.swizzle[1], src.swizzle[2], src.swizzle[3]
            });
        }

        /* Source Modifier */
        uint32_t modVal = swizVal;
        if (src.srcMod == 1) { /* Negate */
            modVal = b.allocId();
            b.emitInst(b.functionDefinitions, SpvOpFNegate, { typeV4Float, modVal, swizVal });
        } else if (src.srcMod == 11) { /* Abs */
            modVal = b.allocId();
            b.emitInst(b.functionDefinitions, SpvOpExtInst, { typeV4Float, modVal, glslSetId, GLSLstd450FAbs, swizVal });
        } else if (src.srcMod == 12) { /* AbsNeg */
            uint32_t absVal = b.allocId();
            b.emitInst(b.functionDefinitions, SpvOpExtInst, { typeV4Float, absVal, glslSetId, GLSLstd450FAbs, swizVal });
            modVal = b.allocId();
            b.emitInst(b.functionDefinitions, SpvOpFNegate, { typeV4Float, modVal, absVal });
        }

        return modVal;
    };

    /* Helper: store vec4 value into destination register with write mask */
    auto emitStoreDest = [&](const ParsedDest &dst, uint32_t val) {
        uint32_t dstVar = 0;
        /* Mesa's SVGA backend can encode a DCL-declared output destination as
         * TEMP. Only apply this compatibility fallback if the shader never
         * explicitly writes that OUTPUT register: TEMP and OUTPUT register
         * numbers are independent, and real shaders commonly use both r0 and o0. */
        bool isDclOutput = (isVS && major >= 3 && dst.regType == D3DSPR_TEMP &&
                            outputRegToSemantic.find(dst.regNum) != outputRegToSemantic.end() &&
                            explicitVsOutputRegs.find(dst.regNum) == explicitVsOutputRegs.end());
        /* SM 3.0 VS without output DCLs: implicit outputs o0=position, o1=color.
         * Mesa encodes these as TEMP in MOV dst tokens. Treat reg 0 as
         * position, reg 1 as color — but only when the shader does not write
         * oPos explicitly (otherwise r0/r1 are genuine temporaries and o0 is
         * a real texcoord; see writesExplicitPosition). */
        bool isImplicitVsOutput = (isVS && major >= 3 && outputRegToSemantic.empty() &&
                                   !writesExplicitPosition &&
                                   dst.regType == D3DSPR_TEMP &&
                                   (dst.regNum == 0 || dst.regNum == 1) &&
                                   explicitVsOutputRegs.find(dst.regNum) == explicitVsOutputRegs.end());
        uint32_t effectiveRegType = (isDclOutput || isImplicitVsOutput) ? 6 : dst.regType;
        if (effectiveRegType == D3DSPR_TEMP) {
            if (dst.regNum >= 16) {
                outError = "Temporary register index out of range (r0-r15 supported)";
                transError = true;
                transErrorCode = SVGA3_VLKN_ERROR_INVALID_PARAM;
                return;
            }
            dstVar = rVars[dst.regNum];
        } else if (effectiveRegType == D3DSPR_RASTOUT) {
            dstVar = outPosVar;
        } else if (effectiveRegType == D3DSPR_ATTROUT || effectiveRegType == D3DSPR_COLOROUT) {
            dstVar = outColorVar[dst.regNum < 2 ? dst.regNum : 0];
        } else if (effectiveRegType == 6) { /* D3DSPR_TEXCRDOUT (SM 1/2) or D3DSPR_OUTPUT (SM 3) */
            if (isVS) {
                if (major >= 3 && !outputRegToSemantic.empty()) {
                    auto it = outputRegToSemantic.find(dst.regNum);
                    if (it != outputRegToSemantic.end()) {
                        if (it->second.usage == 0 || it->second.usage == 9) { /* POSITION or POSITIONT */
                            dstVar = outPosVar;
                        } else if (it->second.usage == 10) { /* COLOR */
                            dstVar = outColorVar[it->second.usageIndex < 2 ? it->second.usageIndex : 0];
                        } else if (it->second.usage == 5) { /* TEXCOORD */
                            dstVar = outTexCoordVar[it->second.usageIndex < 8 ? it->second.usageIndex : 0];
                        } else {
                            dstVar = outTexCoordVar[0];
                        }
                    } else {
                        dstVar = (dst.regNum == 0) ? outPosVar : (dst.regNum == 1 ? outColorVar[0] : outTexCoordVar[(dst.regNum >= 2 && dst.regNum - 2 < 8) ? (dst.regNum - 2) : 0]);
                    }
                } else if (isVS && major >= 3 && !writesExplicitPosition && dst.regNum <= 1) {
                    /* DCL-less SM 3.0 VS without an explicit oPos write:
                     * o0=position, o1=color (Mesa convention), whether encoded
                     * as TEMP (implicit outputs) or OUTPUT. If oPos IS written
                     * elsewhere, regtype-6 outputs are real texcoords (e.g.
                     * oT0) and keep the texcoord routing below.
                     * The old code routed these to texcoord shadows, so the
                     * Position/color outputs kept only their decorations while
                     * the actual values went to Location 2/3. */
                    dstVar = (dst.regNum == 0) ? outPosVar : outColorVar[0];
                } else {
                    dstVar = outTexCoordVar[dst.regNum < 8 ? dst.regNum : 0];
                }
            } else {
                dstVar = outColorVar[0];
            }
        }

        if (!dstVar) return;

        uint32_t finalVal = val;
        if ((dst.dstMod & 1) != 0) { /* _SAT (saturate: clamp float values to [0.0, 1.0]) */
            uint32_t satVal = b.allocId();
            b.emitInst(b.functionDefinitions, SpvOpExtInst, { typeV4Float, satVal, glslSetId, GLSLstd450FClamp, val, const0_v4, const1_v4 });
            finalVal = satVal;
        }

        if (dst.writeMask != 0x0F) {
            /* Masked write: shuffle old value and new value */
            uint32_t oldVal = b.allocId();
            b.emitInst(b.functionDefinitions, SpvOpLoad, { typeV4Float, oldVal, dstVar });

            uint32_t comps[4];
            for (int i = 0; i < 4; ++i) {
                if (dst.writeMask & (1 << i)) {
                    comps[i] = 4 + i; /* from finalVal */
                } else {
                    comps[i] = i;     /* from oldVal */
                }
            }
            uint32_t maskedVal = b.allocId();
            b.emitInst(b.functionDefinitions, SpvOpVectorShuffle, {
                typeV4Float, maskedVal, oldVal, finalVal, comps[0], comps[1], comps[2], comps[3]
            });
            finalVal = maskedVal;
        }

        /* Direct3D 9 rasterizer clamps vertex shader color outputs (oD0/oD1) to [0.0, 1.0] */
        if (isVS && (dstVar == outColorVar[0] || dstVar == outColorVar[1])) {
            uint32_t satVal = b.allocId();
            b.emitInst(b.functionDefinitions, SpvOpExtInst, { typeV4Float, satVal, glslSetId, GLSLstd450FClamp, finalVal, const0_v4, const1_v4 });
            finalVal = satVal;
        }

        b.emitInst(b.functionDefinitions, SpvOpStore, { dstVar, finalVal });
    };

    /* Instruction Translation Loop */
    pc = 1;
    while (pc < numTokens) {
        uint32_t instToken = tokens[pc];
        uint32_t op = instToken & 0xFFFF;

        if (op == D3DSIO_END) break;

        if (op == D3DSIO_COMMENT) {
            uint32_t count = (instToken >> 16) & 0x7FFF;
            if (1 + count > numTokens - pc) {
                outError = "Truncated D3DSIO_COMMENT instruction";
                return SVGA3_VLKN_ERROR_INVALID_PARAM;
            }
            pc += 1 + count;
            continue;
        }

        if (op == D3DSIO_DEF) {
            pc += 6;
            continue;
        }

        if (op == D3DSIO_DCL) {
            pc += 3;
            continue;
        }

        uint32_t instLen = (instToken >> 24) & 0x0F;

        /* Fail-closed instruction sizing: determine how many parameter tokens
         * this instruction reads and verify they are all present before the
         * translation switch touches them. */
        uint32_t paramCount = instLen;
        bool destOnlyTex = (op == D3DSIO_TEX) && isDestOnlyTex(tokens, pc, numTokens, instLen);
        if (destOnlyTex) {
            paramCount = 1; /* SM 1.x dest-only `tex t#`: destination only */
        } else if (op == D3DSIO_TEX) {
            if (instLen == 0) {
                paramCount = 3; /* dest + coord + sampler */
            } else if (instLen == 1) {
                paramCount = 2; /* dest + coord, sampler implied by dest */
            } else {
                paramCount = 3; /* dest + coord + sampler */
            }
        } else if (paramCount == 0) {
            /* SM 1.x fallback parameter counts (length field is 0). */
            switch (op) {
                case D3DSIO_NOP: paramCount = 0; break;
                case D3DSIO_MOV:
                case D3DSIO_RCP:
                case D3DSIO_RSQ:
                case D3DSIO_ABS:
                case D3DSIO_FRC: paramCount = 2; break;
                case D3DSIO_ADD:
                case D3DSIO_SUB:
                case D3DSIO_MUL:
                case D3DSIO_DP3:
                case D3DSIO_DP4:
                case D3DSIO_MIN:
                case D3DSIO_MAX:
                case D3DSIO_M4x4: paramCount = 3; break;
                case D3DSIO_MAD:
                case D3DSIO_LRP:
                case D3DSIO_CMP: paramCount = 4; break;
                default: paramCount = 0; break;
            }
        }
        if (paramCount >= numTokens - pc) {
            outError = "Truncated shader instruction: parameter tokens exceed bytecode length";
            return SVGA3_VLKN_ERROR_INVALID_PARAM;
        }
        /* Declared-length vs consumed-operand-count mismatch: the switch
         * below consumes the opcode's NATURAL operand count, but paramCount
         * above is the DECLARED length. If declared < natural (e.g. ADD
         * with instLen=1), the switch would read past the validated range.
         * Validate the max of both. */
        uint32_t naturalCount = 0;
        switch (op) {
            case D3DSIO_NOP: naturalCount = 0; break;
            case D3DSIO_MOV:
            case D3DSIO_RCP:
            case D3DSIO_RSQ:
            case D3DSIO_ABS:
            case D3DSIO_FRC: naturalCount = 2; break;
            case D3DSIO_ADD:
            case D3DSIO_SUB:
            case D3DSIO_MUL:
            case D3DSIO_DP3:
            case D3DSIO_DP4:
            case D3DSIO_MIN:
            case D3DSIO_MAX:
            case D3DSIO_M4x4: naturalCount = 3; break;
            case D3DSIO_MAD:
            case D3DSIO_LRP:
            case D3DSIO_CMP: naturalCount = 4; break;
            case D3DSIO_TEX: naturalCount = paramCount; break;
            default: naturalCount = paramCount; break;
        }
        uint32_t checkCount = (naturalCount > paramCount) ? naturalCount : paramCount;
        if (checkCount >= numTokens - pc) {
            outError = "Truncated shader instruction: parameter tokens exceed bytecode length";
            return SVGA3_VLKN_ERROR_INVALID_PARAM;
        }
        /* Operand tokens must carry the parameter-token high bit; otherwise
         * parseDest/parseSrc would leave the parsed struct uninitialized. */
        for (uint32_t k = 1; k <= checkCount; ++k) {
            if ((tokens[pc + k] & 0x80000000) == 0) {
                outError = "Malformed shader operand token";
                return SVGA3_VLKN_ERROR_INVALID_PARAM;
            }
        }

        switch (op) {
            case D3DSIO_MOV: {
                ParsedDest dst;
                ParsedSrc src;
                parseDest(tokens[pc + 1], dst);
                parseSrc(tokens[pc + 2], src);
                uint32_t val = emitLoadSrc(src);
                emitStoreDest(dst, val);
                break;
            }
            case D3DSIO_ADD: {
                ParsedDest dst;
                ParsedSrc s0, s1;
                parseDest(tokens[pc + 1], dst);
                parseSrc(tokens[pc + 2], s0);
                parseSrc(tokens[pc + 3], s1);
                uint32_t v0 = emitLoadSrc(s0);
                uint32_t v1 = emitLoadSrc(s1);
                uint32_t res = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpFAdd, { typeV4Float, res, v0, v1 });
                emitStoreDest(dst, res);
                break;
            }
            case D3DSIO_SUB: {
                ParsedDest dst;
                ParsedSrc s0, s1;
                parseDest(tokens[pc + 1], dst);
                parseSrc(tokens[pc + 2], s0);
                parseSrc(tokens[pc + 3], s1);
                uint32_t v0 = emitLoadSrc(s0);
                uint32_t v1 = emitLoadSrc(s1);
                uint32_t res = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpFSub, { typeV4Float, res, v0, v1 });
                emitStoreDest(dst, res);
                break;
            }
            case D3DSIO_MUL: {
                ParsedDest dst;
                ParsedSrc s0, s1;
                parseDest(tokens[pc + 1], dst);
                parseSrc(tokens[pc + 2], s0);
                parseSrc(tokens[pc + 3], s1);
                uint32_t v0 = emitLoadSrc(s0);
                uint32_t v1 = emitLoadSrc(s1);
                uint32_t res = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpFMul, { typeV4Float, res, v0, v1 });
                emitStoreDest(dst, res);
                break;
            }
            case D3DSIO_MAD: {
                ParsedDest dst;
                ParsedSrc s0, s1, s2;
                parseDest(tokens[pc + 1], dst);
                parseSrc(tokens[pc + 2], s0);
                parseSrc(tokens[pc + 3], s1);
                parseSrc(tokens[pc + 4], s2);
                uint32_t v0 = emitLoadSrc(s0);
                uint32_t v1 = emitLoadSrc(s1);
                uint32_t v2 = emitLoadSrc(s2);
                uint32_t mul = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpFMul, { typeV4Float, mul, v0, v1 });
                uint32_t res = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpFAdd, { typeV4Float, res, mul, v2 });
                emitStoreDest(dst, res);
                break;
            }
            case D3DSIO_DP3: {
                ParsedDest dst;
                ParsedSrc s0, s1;
                parseDest(tokens[pc + 1], dst);
                parseSrc(tokens[pc + 2], s0);
                parseSrc(tokens[pc + 3], s1);
                uint32_t v0 = emitLoadSrc(s0);
                uint32_t v1 = emitLoadSrc(s1);
                uint32_t v0_xyz = b.allocId();
                uint32_t v1_xyz = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpVectorShuffle, { typeV3Float, v0_xyz, v0, v0, 0, 1, 2 });
                b.emitInst(b.functionDefinitions, SpvOpVectorShuffle, { typeV3Float, v1_xyz, v1, v1, 0, 1, 2 });
                uint32_t dot = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpDot, { typeFloat, dot, v0_xyz, v1_xyz });
                uint32_t res = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpCompositeConstruct, { typeV4Float, res, dot, dot, dot, dot });
                emitStoreDest(dst, res);
                break;
            }
            case D3DSIO_DP4: {
                ParsedDest dst;
                ParsedSrc s0, s1;
                parseDest(tokens[pc + 1], dst);
                parseSrc(tokens[pc + 2], s0);
                parseSrc(tokens[pc + 3], s1);
                uint32_t v0 = emitLoadSrc(s0);
                uint32_t v1 = emitLoadSrc(s1);
                uint32_t dot = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpDot, { typeFloat, dot, v0, v1 });
                uint32_t res = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpCompositeConstruct, { typeV4Float, res, dot, dot, dot, dot });
                emitStoreDest(dst, res);
                break;
            }
            case D3DSIO_M4x4: {
                ParsedDest dst;
                ParsedSrc s0, s1;
                parseDest(tokens[pc + 1], dst);
                parseSrc(tokens[pc + 2], s0);
                parseSrc(tokens[pc + 3], s1);
                uint32_t vec = emitLoadSrc(s0);

                uint32_t dots[4];
                for (uint32_t r = 0; r < 4; ++r) {
                    ParsedSrc rowSrc = s1;
                    rowSrc.regNum = s1.regNum + r;
                    uint32_t rowVal = emitLoadSrc(rowSrc);
                    dots[r] = b.allocId();
                    b.emitInst(b.functionDefinitions, SpvOpDot, { typeFloat, dots[r], vec, rowVal });
                }
                uint32_t res = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpCompositeConstruct, { typeV4Float, res, dots[0], dots[1], dots[2], dots[3] });
                emitStoreDest(dst, res);
                break;
            }
            case D3DSIO_MIN: {
                ParsedDest dst;
                ParsedSrc s0, s1;
                parseDest(tokens[pc + 1], dst);
                parseSrc(tokens[pc + 2], s0);
                parseSrc(tokens[pc + 3], s1);
                uint32_t v0 = emitLoadSrc(s0);
                uint32_t v1 = emitLoadSrc(s1);
                uint32_t res = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpExtInst, { typeV4Float, res, glslSetId, GLSLstd450FMin, v0, v1 });
                emitStoreDest(dst, res);
                break;
            }
            case D3DSIO_MAX: {
                ParsedDest dst;
                ParsedSrc s0, s1;
                parseDest(tokens[pc + 1], dst);
                parseSrc(tokens[pc + 2], s0);
                parseSrc(tokens[pc + 3], s1);
                uint32_t v0 = emitLoadSrc(s0);
                uint32_t v1 = emitLoadSrc(s1);
                uint32_t res = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpExtInst, { typeV4Float, res, glslSetId, GLSLstd450FMax, v0, v1 });
                emitStoreDest(dst, res);
                break;
            }
            case D3DSIO_RCP: {
                ParsedDest dst;
                ParsedSrc s0;
                parseDest(tokens[pc + 1], dst);
                parseSrc(tokens[pc + 2], s0);
                uint32_t v0 = emitLoadSrc(s0);
                uint32_t x0 = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpCompositeExtract, { typeFloat, x0, v0, 0 });
                uint32_t rcp = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpFDiv, { typeFloat, rcp, const1_f, x0 });
                uint32_t res = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpCompositeConstruct, { typeV4Float, res, rcp, rcp, rcp, rcp });
                emitStoreDest(dst, res);
                break;
            }
            case D3DSIO_RSQ: {
                ParsedDest dst;
                ParsedSrc s0;
                parseDest(tokens[pc + 1], dst);
                parseSrc(tokens[pc + 2], s0);
                uint32_t v0 = emitLoadSrc(s0);
                uint32_t x0 = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpCompositeExtract, { typeFloat, x0, v0, 0 });
                uint32_t rsq = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpExtInst, { typeFloat, rsq, glslSetId, GLSLstd450InverseSqrt, x0 });
                uint32_t res = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpCompositeConstruct, { typeV4Float, res, rsq, rsq, rsq, rsq });
                emitStoreDest(dst, res);
                break;
            }
            case D3DSIO_ABS: {
                ParsedDest dst;
                ParsedSrc s0;
                parseDest(tokens[pc + 1], dst);
                parseSrc(tokens[pc + 2], s0);
                uint32_t v0 = emitLoadSrc(s0);
                uint32_t res = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpExtInst, { typeV4Float, res, glslSetId, GLSLstd450FAbs, v0 });
                emitStoreDest(dst, res);
                break;
            }
            case D3DSIO_LRP: {
                ParsedDest dst;
                ParsedSrc s0, s1, s2;
                parseDest(tokens[pc + 1], dst);
                parseSrc(tokens[pc + 2], s0);
                parseSrc(tokens[pc + 3], s1);
                parseSrc(tokens[pc + 4], s2);
                uint32_t v0 = emitLoadSrc(s0);
                uint32_t v1 = emitLoadSrc(s1);
                uint32_t v2 = emitLoadSrc(s2);
                /* D3D LRP: dest = src0 * (src1 - src2) + src2 == mix(src2, src1, src0) */
                uint32_t res = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpExtInst, { typeV4Float, res, glslSetId, GLSLstd450FMix, v2, v1, v0 });
                emitStoreDest(dst, res);
                break;
            }
            case D3DSIO_FRC: {
                ParsedDest dst;
                ParsedSrc s0;
                parseDest(tokens[pc + 1], dst);
                parseSrc(tokens[pc + 2], s0);
                uint32_t v0 = emitLoadSrc(s0);
                uint32_t res = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpExtInst, { typeV4Float, res, glslSetId, GLSLstd450Fract, v0 });
                emitStoreDest(dst, res);
                break;
            }
            case D3DSIO_CMP: {
                ParsedDest dst;
                ParsedSrc s0, s1, s2;
                parseDest(tokens[pc + 1], dst);
                parseSrc(tokens[pc + 2], s0);
                parseSrc(tokens[pc + 3], s1);
                parseSrc(tokens[pc + 4], s2);
                uint32_t v0 = emitLoadSrc(s0);
                uint32_t v1 = emitLoadSrc(s1);
                uint32_t v2 = emitLoadSrc(s2);
                /* dest = (src0 >= 0.0) ? src1 : src2 */
                uint32_t cmp = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpFOrdGreaterThanEqual, { typeV4Bool, cmp, v0, const0_v4 });
                uint32_t res = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpSelect, { typeV4Float, res, cmp, v1, v2 });
                emitStoreDest(dst, res);
                break;
            }
            case D3DSIO_TEX: {
                ParsedDest dst;
                parseDest(tokens[pc + 1], dst);

                uint32_t coord;
                uint32_t samplerIdx;
                if (destOnlyTex) {
                    /* SM 1.x `tex t#`: sample the stage with the interpolator
                     * texture coordinates; there is no source operand. */
                    if (isVS) {
                        outError = "Dest-only tex is not supported in vertex shaders";
                        return SVGA3_VLKN_ERROR_UNSUPPORTED_SHADER;
                    }
                    samplerIdx = (dst.regNum < 8) ? dst.regNum : 0;
                    coord = b.allocId();
                    b.emitInst(b.functionDefinitions, SpvOpLoad, { typeV4Float, coord, psInTexCoords[samplerIdx] });
                } else {
                    ParsedSrc s0;
                    parseSrc(tokens[pc + 2], s0);
                    coord = emitLoadSrc(s0);
                    if (instLen >= 2) {
                        ParsedSrc s1;
                        parseSrc(tokens[pc + 3], s1);
                        samplerIdx = (s1.regNum < 8) ? s1.regNum : 0;
                    } else {
                        samplerIdx = (dst.regNum < 8) ? dst.regNum : 0;
                    }
                }
                uint32_t uv = b.allocId();
                b.emitInst(b.functionDefinitions, SpvOpVectorShuffle, { typeV2Float, uv, coord, coord, 0, 1 });

                uint32_t sampledImage = b.allocId();
                bool depthStage = (depthSamplerMask & (1u << samplerIdx)) != 0;
                b.emitInst(b.functionDefinitions, SpvOpLoad,
                           { depthStage ? typeSampledDepth2D : typeSampledImage2D, sampledImage, samplerVars[samplerIdx] });

                uint32_t sampled = b.allocId();
                if (depthStage) {
                    /* D3D tex of a depth map returns the fetched depth in every channel. */
                    uint32_t drefResult = b.allocId();
                    b.emitInst(b.functionDefinitions, SpvOpImageSampleImplicitLod,
                               { typeFloat, drefResult, sampledImage, uv });
                    b.emitInst(b.functionDefinitions, SpvOpCompositeConstruct,
                               { typeV4Float, sampled, drefResult, drefResult, drefResult, const1_f });
                } else {
                    b.emitInst(b.functionDefinitions, SpvOpImageSampleImplicitLod, { typeV4Float, sampled, sampledImage, uv });
                }
                emitStoreDest(dst, sampled);
                break;
            }
            default:
                break;
        }

        /* Fail-closed: operand validation errors abort translation. */
        if (transError) {
            return transErrorCode;
        }

        /* Advance PC exactly as validated in pass 1. */
        uint32_t advance;
        if (instLen > 0) {
            advance = 1 + instLen;
        } else {
            switch (op) {
                case D3DSIO_NOP: advance = 1; break;
                case D3DSIO_MOV:
                case D3DSIO_RCP:
                case D3DSIO_RSQ:
                case D3DSIO_ABS:
                case D3DSIO_FRC: advance = 3; break;
                case D3DSIO_ADD:
                case D3DSIO_SUB:
                case D3DSIO_MUL:
                case D3DSIO_DP3:
                case D3DSIO_DP4:
                case D3DSIO_MIN:
                case D3DSIO_MAX:
                case D3DSIO_M4x4: advance = 4; break;
                case D3DSIO_TEX:
                    advance = destOnlyTex ? 2 : 4;
                    break;
                case D3DSIO_MAD:
                case D3DSIO_LRP:
                case D3DSIO_CMP: advance = 5; break;
                default: advance = 1; break;
            }
        }
        if (advance > numTokens - pc) {
            outError = "Truncated shader instruction: parameter tokens exceed bytecode length";
            return SVGA3_VLKN_ERROR_INVALID_PARAM;
        }
        pc += advance;
    }

    /* Epilogue: copy outputs */
    if (isVS) {
        /* Store gl_Position = outPos */
        uint32_t posVal = b.allocId();
        b.emitInst(b.functionDefinitions, SpvOpLoad, { typeV4Float, posVal, outPosVar });

        /* Y-flip for Vulkan coordinate space: pos.y = -pos.y */
        uint32_t negPos = b.allocId();
        b.emitInst(b.functionDefinitions, SpvOpVectorShuffle, { typeV4Float, negPos, posVal, posVal, 0, 1, 2, 3 });
        uint32_t yVal = b.allocId();
        b.emitInst(b.functionDefinitions, SpvOpCompositeExtract, { typeFloat, yVal, posVal, 1 });
        uint32_t negY = b.allocId();
        b.emitInst(b.functionDefinitions, SpvOpFNegate, { typeFloat, negY, yVal });

        uint32_t xVal = b.allocId();
        uint32_t zVal = b.allocId();
        uint32_t wVal = b.allocId();
        b.emitInst(b.functionDefinitions, SpvOpCompositeExtract, { typeFloat, xVal, posVal, 0 });
        b.emitInst(b.functionDefinitions, SpvOpCompositeExtract, { typeFloat, zVal, posVal, 2 });
        b.emitInst(b.functionDefinitions, SpvOpCompositeExtract, { typeFloat, wVal, posVal, 3 });

        uint32_t flippedPos = b.allocId();
        b.emitInst(b.functionDefinitions, SpvOpCompositeConstruct, { typeV4Float, flippedPos, xVal, negY, zVal, wVal });

        uint32_t posPtr = b.allocId();
        b.emitInst(b.functionDefinitions, SpvOpAccessChain, { ptrOutputV4Float, posPtr, vsGlPerVertex, intConsts[0] });
        b.emitInst(b.functionDefinitions, SpvOpStore, { posPtr, flippedPos });

        /* Store out_color[0..1] */
        for (int c = 0; c < 2; ++c) {
            uint32_t colVal = b.allocId();
            b.emitInst(b.functionDefinitions, SpvOpLoad, { typeV4Float, colVal, outColorVar[c] });
            b.emitInst(b.functionDefinitions, SpvOpStore, { vsOutColor[c], colVal });
        }

        /* Store out_texcoord[0..7] */
        for (int t = 0; t < 8; ++t) {
            uint32_t texVal = b.allocId();
            b.emitInst(b.functionDefinitions, SpvOpLoad, { typeV4Float, texVal, outTexCoordVar[t] });
            b.emitInst(b.functionDefinitions, SpvOpStore, { vsOutTexCoords[t], texVal });
        }
    } else {
        /* Store out_color */
        uint32_t colVal = b.allocId();
        b.emitInst(b.functionDefinitions, SpvOpLoad, { typeV4Float, colVal, outColorVar[0] });
        b.emitInst(b.functionDefinitions, SpvOpStore, { psOutColor, colVal });
    }

    b.emitInst(b.functionDefinitions, SpvOpReturn, {});
    b.emitInst(b.functionDefinitions, SpvOpFunctionEnd, {});

    outSpirv = b.assemble(b.getBound());
    /* Debug: dump SPIR-V to /tmp if SVGA3_VLKN_DUMP_SPIRV is set.
     * Useful for validating with spirv-val/spirv-dis. */
    if (getenv("SVGA3_VLKN_DUMP_SPIRV")) {
        char path[256];
        static int dumpIdx = 0;
        int idx = __sync_fetch_and_add(&dumpIdx, 1);
        snprintf(path, sizeof(path), "/tmp/spirv_dump_%d_%s.spv",
                 idx, isVS ? "vs" : "ps");
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(outSpirv.data(), sizeof(uint32_t), outSpirv.size(), f);
            fclose(f);
        }
    }
    return SVGA3_VLKN_SUCCESS;
}

} // namespace svga3_vlkn
