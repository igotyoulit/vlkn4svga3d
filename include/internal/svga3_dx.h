/* SVGA3D DX (D3D10/11-style) command path.
 *
 * This header defines the DX command IDs and structures for the newer
 * SVGA3D command path. It is intentionally separate from the D3D9 path in
 * svga3_fifo.cpp: the D3D9 dispatch is untouched, and all DX handling lives
 * in src/svga3_dx.cpp.
 *
 * This is NOT a full D3D11 implementation. Only the subset listed below is
 * implemented; every other DX command ID fails closed with
 * SVGA3_VLKN_ERROR_UNSUPPORTED_COMMAND.
 *
 * Implemented:
 *   SVGA_3D_CMD_DX_DEFINE_CONTEXT / DESTROY_CONTEXT - DX context lifecycle,
 *       delegated to VlknContextManager.
 *   SVGA_3D_CMD_DX_SET_RENDER_TARGETS - bind surfaces as render targets.
 *   SVGA_3D_CMD_DX_CLEAR_RENDER_TARGET - clear a bound render target.
 *   SVGA_3D_CMD_DX_PRESENT - present (delegates to the existing present path).
 *
 * Not implemented (fail closed):
 *   Everything else in the DX range, including shader/pipeline state,
 *   resource views, and mob-backed (GB) surface backing stores.
 */

#ifndef SVGA3_DX_H
#define SVGA3_DX_H

#include <stdint.h>
#include <stddef.h>
#include "svga3_vlkn.h"

#ifdef __cplusplus
extern "C" {
#endif

/* DX command IDs. Above SVGA_3D_CMD_MAX (1040+42), below SVGA_3D_CMD_FUTURE_MAX. */
#define SVGA_3D_CMD_DX_BASE                 1500
#define SVGA_3D_CMD_DX_DEFINE_CONTEXT       (SVGA_3D_CMD_DX_BASE + 0)
#define SVGA_3D_CMD_DX_DESTROY_CONTEXT      (SVGA_3D_CMD_DX_BASE + 1)
#define SVGA_3D_CMD_DX_SET_RENDER_TARGETS   (SVGA_3D_CMD_DX_BASE + 2)
#define SVGA_3D_CMD_DX_CLEAR_RENDER_TARGET  (SVGA_3D_CMD_DX_BASE + 3)
#define SVGA_3D_CMD_DX_PRESENT              (SVGA_3D_CMD_DX_BASE + 4)
/* IDs 5-15 are reserved; they fail closed. */
#define SVGA_3D_CMD_DX_MAX                  (SVGA_3D_CMD_DX_BASE + 16)

/* Guest-backed (mob-backed) surface flag. Surfaces with this flag need a
 * mob backing store, which is not implemented; the DX path fails closed
 * on them. */
#define SVGA3D_SURFACE_FLAG_GB              (1u << 11)

typedef struct SVGA3dCmdDXDefineContext {
    uint32_t cid;
} SVGA3dCmdDXDefineContext;

typedef struct SVGA3dCmdDXDestroyContext {
    uint32_t cid;
} SVGA3dCmdDXDestroyContext;

typedef struct SVGA3dCmdDXSetRenderTargets {
    uint32_t cid;
    uint32_t numTargets;
    /* Followed by numTargets uint32_t surface IDs. */
} SVGA3dCmdDXSetRenderTargets;

typedef struct SVGA3dCmdDXClearRenderTarget {
    uint32_t cid;
    uint32_t sid;
    float    color[4];
} SVGA3dCmdDXClearRenderTarget;

typedef struct SVGA3dCmdDXPresent {
    uint32_t cid;
    uint32_t sid;
} SVGA3dCmdDXPresent;

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
namespace svga3_vlkn {

/* Dispatch a DX command. Returns SVGA3_VLKN_ERROR_UNSUPPORTED_COMMAND for
 * unknown or unimplemented DX commands (fail closed). Returns
 * SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER when the payload is too short.
 * Sets *bytesRead to the consumed payload size on success. */
Svga3VlknStatus svga3_dx_dispatch(::Svga3VlknDevice *dev,
                                 uint32_t cmd,
                                 const uint8_t *payload,
                                 size_t payloadSize,
                                 size_t *bytesRead);

} // namespace svga3_vlkn
#endif

#endif /* SVGA3_DX_H */
