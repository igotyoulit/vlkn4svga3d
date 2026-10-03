/*
 * SVGA3=VLKN - Lightweight SPIR-V Binary Builder
 */

#ifndef ___SVGA3_SPIRV_BUILDER_H___
#define ___SVGA3_SPIRV_BUILDER_H___

#include <cstdint>
#include <vector>
#include <string>
#include <cstring>
#include <map>

namespace svga3_vlkn {

enum SpvOp {
    SpvOpNop = 0,
    SpvOpSource = 3,
    SpvOpName = 5,
    SpvOpMemberName = 6,
    SpvOpExtInstImport = 11,
    SpvOpExtInst = 12,
    SpvOpMemoryModel = 14,
    SpvOpEntryPoint = 15,
    SpvOpExecutionMode = 16,
    SpvOpCapability = 17,
    SpvOpTypeVoid = 19,
    SpvOpTypeBool = 20,
    SpvOpTypeInt = 21,
    SpvOpTypeFloat = 22,
    SpvOpTypeVector = 23,
    SpvOpTypeMatrix = 24,
    SpvOpTypeImage = 25,
    SpvOpTypeSampler = 26,
    SpvOpTypeSampledImage = 27,
    SpvOpTypeArray = 28,
    SpvOpTypeStruct = 30,
    SpvOpTypePointer = 32,
    SpvOpTypeFunction = 33,
    SpvOpConstantTrue = 41,
    SpvOpConstantFalse = 42,
    SpvOpConstant = 43,
    SpvOpConstantComposite = 44,
    SpvOpSpecConstant = 50,
    SpvOpFunction = 54,
    SpvOpFunctionEnd = 56,
    SpvOpVariable = 59,
    SpvOpLoad = 61,
    SpvOpStore = 62,
    SpvOpAccessChain = 65,
    SpvOpDecorate = 71,
    SpvOpMemberDecorate = 72,
    SpvOpVectorShuffle = 79,
    SpvOpCompositeConstruct = 80,
    SpvOpCompositeExtract = 81,
    SpvOpImageSampleImplicitLod = 87,
    SpvOpImageSampleDrefImplicitLod = 89,
    SpvOpCompositeInsert = 82,
    SpvOpFNegate = 127,
    SpvOpIAdd = 128,
    SpvOpFAdd = 129,
    SpvOpISub = 130,
    SpvOpFSub = 131,
    SpvOpFMul = 133,
    SpvOpFDiv = 136,
    SpvOpConvertSToF = 109,
    SpvOpDot = 148,
    SpvOpSelect = 169,
    SpvOpIEqual = 170,
    SpvOpINotEqual = 171,
    SpvOpSLessThan = 177,
    SpvOpFOrdEqual = 180,
    SpvOpFOrdNotEqual = 182,
    SpvOpFOrdLessThan = 184,
    SpvOpFOrdGreaterThan = 186,
    SpvOpFOrdLessThanEqual = 188,
    SpvOpFOrdGreaterThanEqual = 190,
    SpvOpLogicalOr = 166,
    SpvOpLogicalAnd = 167,
    SpvOpLogicalNot = 168,
    SpvOpLoopMerge = 246,
    SpvOpSelectionMerge = 247,
    SpvOpLabel = 248,
    SpvOpBranch = 249,
    SpvOpBranchConditional = 250,
    SpvOpKill = 252,
    SpvOpReturn = 253
};

enum SpvCapability {
    SpvCapabilityShader = 1
};

enum SpvAddressingModel {
    SpvAddressingModelLogical = 0
};

enum SpvMemoryModel {
    SpvMemoryModelGLSL450 = 1
};

enum SpvExecutionModel {
    SpvExecutionModelVertex = 0,
    SpvExecutionModelFragment = 4
};

enum SpvExecutionMode {
    SpvExecutionModeOriginUpperLeft = 7
};

enum SpvStorageClass {
    SpvStorageClassUniformConstant = 0,
    SpvStorageClassInput = 1,
    SpvStorageClassUniform = 2,
    SpvStorageClassOutput = 3,
    SpvStorageClassFunction = 7,
    SpvStorageClassPushConstant = 9
};

enum SpvDecoration {
    SpvDecorationSpecId = 1,
    SpvDecorationBlock = 2,
    SpvDecorationRowMajor = 4,
    SpvDecorationColMajor = 5,
    SpvDecorationArrayStride = 6,
    SpvDecorationMatrixStride = 7,
    SpvDecorationBuiltIn = 11,
    SpvDecorationLocation = 30,
    SpvDecorationBinding = 33,
    SpvDecorationDescriptorSet = 34,
    SpvDecorationOffset = 35
};

enum SpvBuiltIn {
    SpvBuiltInPosition = 0,
    SpvBuiltInPointSize = 1,
    SpvBuiltInClipDistance = 3,
    SpvBuiltInCullDistance = 4,
    SpvBuiltInFragCoord = 15,
    SpvBuiltInFrontFacing = 17
};

enum SpvDim {
    SpvDim2D = 1,
    SpvDimCube = 3
};

enum GLSLstd450 {
    GLSLstd450FAbs = 4,
    GLSLstd450FSign = 6,
    GLSLstd450Floor = 8,
    GLSLstd450Ceil = 9,
    GLSLstd450Fract = 10,
    GLSLstd450Sin = 13,
    GLSLstd450Cos = 14,
    GLSLstd450Pow = 26,
    GLSLstd450Exp = 27,
    GLSLstd450Log = 28,
    GLSLstd450Exp2 = 29,
    GLSLstd450Log2 = 30,
    GLSLstd450Sqrt = 31,
    GLSLstd450InverseSqrt = 32,
    GLSLstd450FMin = 37,
    GLSLstd450FMax = 40,
    GLSLstd450FClamp = 43,
    GLSLstd450FMix = 46,
    GLSLstd450Normalize = 69,
    GLSLstd450Cross = 70
};

class SpirvBuilder {
public:
    SpirvBuilder() : m_nextId(1) {}

    /* Saturate near 2^32: wrapped IDs would duplicate and the header
     * bound would understate IDs in use, producing invalid SPIR-V.
     * Unreachable in practice once the shader-size cap (M2) applies. */
    uint32_t allocId() {
        if (m_nextId >= 0xFFFFFF00u) {
            return 0xFFFFFF00u;
        }
        return m_nextId++;
    }

    void emitWord(std::vector<uint32_t> &stream, uint32_t w) {
        stream.push_back(w);
    }

    void emitInst(std::vector<uint32_t> &stream, SpvOp op, const std::vector<uint32_t> &operands) {
        uint32_t wordCount = (uint32_t)(operands.size() + 1);
        stream.push_back((wordCount << 16) | (uint32_t)op);
        for (uint32_t w : operands) {
            stream.push_back(w);
        }
    }

    void emitString(std::vector<uint32_t> &operands, const std::string &str) {
        size_t len = str.size() + 1;
        size_t wordCount = (len + 3) / 4;
        std::vector<uint8_t> bytes(wordCount * 4, 0);
        for (size_t i = 0; i < str.size(); ++i) {
            bytes[i] = (uint8_t)str[i];
        }
        for (size_t i = 0; i < wordCount; ++i) {
            uint32_t w = (uint32_t)bytes[i*4] |
                         ((uint32_t)bytes[i*4+1] << 8) |
                         ((uint32_t)bytes[i*4+2] << 16) |
                         ((uint32_t)bytes[i*4+3] << 24);
            operands.push_back(w);
        }
    }

    /* Assembles full SPIR-V binary */
    std::vector<uint32_t> assemble(uint32_t bound) {
        std::vector<uint32_t> binary;
        /* SPIR-V Header */
        binary.push_back(0x07230203); /* Magic */
        binary.push_back(0x00010000); /* Version 1.0 */
        binary.push_back(0x12345678); /* Generator Magic */
        binary.push_back(bound);      /* Bound */
        binary.push_back(0);          /* Schema */

        /* Capabilities */
        binary.insert(binary.end(), capabilities.begin(), capabilities.end());
        /* Extensions */
        binary.insert(binary.end(), extensions.begin(), extensions.end());
        /* ExtInstImports */
        binary.insert(binary.end(), extInstImports.begin(), extInstImports.end());
        /* MemoryModel */
        binary.insert(binary.end(), memoryModel.begin(), memoryModel.end());
        /* EntryPoints */
        binary.insert(binary.end(), entryPoints.begin(), entryPoints.end());
        /* ExecutionModes */
        binary.insert(binary.end(), executionModes.begin(), executionModes.end());
        /* Debug/Names */
        binary.insert(binary.end(), debugNames.begin(), debugNames.end());
        /* Annotations/Decorations */
        binary.insert(binary.end(), annotations.begin(), annotations.end());
        /* Types, Constants, Global Variables */
        binary.insert(binary.end(), typesConstantsGlobals.begin(), typesConstantsGlobals.end());
        /* Functions */
        binary.insert(binary.end(), functionDefinitions.begin(), functionDefinitions.end());

        return binary;
    }

    uint32_t getBound() const { return m_nextId; }

    std::vector<uint32_t> capabilities;
    std::vector<uint32_t> extensions;
    std::vector<uint32_t> extInstImports;
    std::vector<uint32_t> memoryModel;
    std::vector<uint32_t> entryPoints;
    std::vector<uint32_t> executionModes;
    std::vector<uint32_t> debugNames;
    std::vector<uint32_t> annotations;
    std::vector<uint32_t> typesConstantsGlobals;
    std::vector<uint32_t> functionDefinitions;

private:
    uint32_t m_nextId;
};

} // namespace svga3_vlkn

#endif /* ___SVGA3_SPIRV_BUILDER_H___ */
