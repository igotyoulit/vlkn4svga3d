/* DX context bookkeeping only. Unsupported DX operations cannot touch legacy
 * contexts or treat view IDs as surface IDs. See svga3_dx.h for wire sources. */
#include "svga3_dx.h"
#include "svga3_device.h"
#include <cstring>
#include <new>
namespace svga3_vlkn {
Svga3VlknStatus svga3_dx_dispatch(::Svga3VlknDevice *dev, uint32_t cmd,
                                const uint8_t *payload, size_t payloadSize,
                                size_t *bytesRead) {
    if (!dev || !bytesRead || (!payload && payloadSize))
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    *bytesRead = 0;
    if (cmd != SVGA_3D_CMD_DX_DEFINE_CONTEXT && cmd != SVGA_3D_CMD_DX_DESTROY_CONTEXT)
        return SVGA3_VLKN_ERROR_UNSUPPORTED_COMMAND;
    if (payloadSize != sizeof(uint32_t))
        return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
    uint32_t cid;
    memcpy(&cid, payload, sizeof(cid));
    if (cid == SVGA3D_INVALID_ID) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    if (cmd == SVGA_3D_CMD_DX_DEFINE_CONTEXT) {
        if (dev->dxContexts.count(cid)) return SVGA3_VLKN_ERROR_ALREADY_EXISTS;
        if (dev->dxContexts.size() >= dev->dxContextLimit)
            return SVGA3_VLKN_ERROR_OUT_OF_MEMORY;
        try { dev->dxContexts.insert(cid); }
        catch (const std::bad_alloc &) { return SVGA3_VLKN_ERROR_OUT_OF_MEMORY; }
    } else if (!dev->dxContexts.erase(cid)) {
        return SVGA3_VLKN_ERROR_NOT_FOUND;
    }
    *bytesRead = sizeof(cid);
    return SVGA3_VLKN_SUCCESS;
}
}
