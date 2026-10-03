/* Minimal DX wire dispatch. No DX rendering or capabilities are advertised.
 * IDs follow VMware's svga3d_cmd.h; context payloads follow svga3d_dx.h:
 * https://github.com/torvalds/linux/tree/master/drivers/gpu/drm/vmwgfx/device_include
 * Context IDs have separate bookkeeping from legacy D3D9 contexts.
 * Binding MOBs, resource views, shaders, drawing and presentation are unsupported.
 */
#ifndef SVGA3_DX_H
#define SVGA3_DX_H
#include "svga3_vlkn.h"
#define SVGA_3D_CMD_DX_BASE 1143
#define SVGA_3D_CMD_DX_DEFINE_CONTEXT 1143
#define SVGA_3D_CMD_DX_DESTROY_CONTEXT 1144
// Upper bound covers the current extended protocol, including later DX opcodes.
#define SVGA_3D_CMD_DX_MAX 1304
struct SVGA3dCmdDXDefineContext { uint32_t cid; };
struct SVGA3dCmdDXDestroyContext { uint32_t cid; };
namespace svga3_vlkn {
Svga3VlknStatus svga3_dx_dispatch(::Svga3VlknDevice *dev, uint32_t cmd,
                                const uint8_t *payload, size_t payloadSize,
                                size_t *bytesRead);
}
#endif
