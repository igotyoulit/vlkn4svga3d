/*
 * SVGA3=VLKN - D3D9 / SVGA3D Guest Shader Bytecode to SPIR-V Translator
 */

#ifndef ___SVGA3_SHADER_TRANSLATOR_H___
#define ___SVGA3_SHADER_TRANSLATOR_H___

#include "svga3_vlkn.h"
#include <cstdint>
#include <vector>
#include <string>

namespace svga3_vlkn {

/*
 * Translates guest D3D9/SVGA3D shader bytecode to standard Vulkan GLSL450 SPIR-V binary.
 *
 * If the bytecode contains unsupported opcodes or is malformed, returns an error status
 * (e.g. SVGA3_VLKN_ERROR_NOT_SUPPORTED or SVGA3_VLKN_ERROR_INVALID_PARAM) and details in outError.
 * NEVER silently substitutes default dummy shaders.
 */
Svga3VlknStatus svga3_translate_shader_d3d9(SVGA3dShaderType type,
                                            const uint32_t *tokens,
                                            uint32_t numTokens,
                                            std::vector<uint32_t> &outSpirv,
                                            std::string &outError,
                                            uint32_t *outInputMask = nullptr,
                                            uint32_t depthSamplerMask = 0);

} // namespace svga3_vlkn

#endif /* ___SVGA3_SHADER_TRANSLATOR_H___ */
