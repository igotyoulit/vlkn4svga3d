/* SVGA3D DX (D3D10/11-style) command dispatcher.
 *
 * This file owns the DX command path. The D3D9 dispatch in svga3_fifo.cpp
 * is untouched; DX commands are routed here from the fifo default case.
 *
 * Only the subset declared in include/internal/svga3_dx.h is implemented.
 * Unknown or unimplemented DX commands fail closed with
 * SVGA3_VLKN_ERROR_UNSUPPORTED_COMMAND. This file never claims full D3D11.
 */

#include "svga3_dx.h"
#include "svga3_device.h"
#include "svga3_context.h"
#include "svga3_surface.h"

#include <cstring>

namespace svga3_vlkn {

/* Reject GB (mob-backed) surfaces: no mob backing store exists yet. */
static Svga3VlknStatus checkGbSurface(Svga3VlknDevice *dev, uint32_t sid) {
    VlknSurface *surf = dev->surfaceMgr->getSurface(sid);
    if (!surf) return SVGA3_VLKN_ERROR_NOT_FOUND;
    if (surf->flags() & SVGA3D_SURFACE_FLAG_GB) {
        return SVGA3_VLKN_ERROR_UNSUPPORTED_COMMAND;
    }
    return SVGA3_VLKN_SUCCESS;
}

static Svga3VlknStatus dxDefineContext(Svga3VlknDevice *dev,
                                       const uint8_t *payload,
                                       size_t payloadSize,
                                       size_t *bytesRead) {
    if (payloadSize < sizeof(SVGA3dCmdDXDefineContext)) {
        return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
    }
    const auto *pCmd = reinterpret_cast<const SVGA3dCmdDXDefineContext*>(payload);
    Svga3VlknStatus st = dev->contextMgr->createContext(pCmd->cid);
    *bytesRead = sizeof(SVGA3dCmdDXDefineContext);
    return st;
}

static Svga3VlknStatus dxDestroyContext(Svga3VlknDevice *dev,
                                        const uint8_t *payload,
                                        size_t payloadSize,
                                        size_t *bytesRead) {
    if (payloadSize < sizeof(SVGA3dCmdDXDestroyContext)) {
        return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
    }
    const auto *pCmd = reinterpret_cast<const SVGA3dCmdDXDestroyContext*>(payload);
    Svga3VlknStatus st = dev->contextMgr->destroyContext(pCmd->cid);
    *bytesRead = sizeof(SVGA3dCmdDXDestroyContext);
    return st;
}

static Svga3VlknStatus dxSetRenderTargets(Svga3VlknDevice *dev,
                                          const uint8_t *payload,
                                          size_t payloadSize,
                                          size_t *bytesRead) {
    if (payloadSize < sizeof(SVGA3dCmdDXSetRenderTargets)) {
        return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
    }
    const auto *pCmd = reinterpret_cast<const SVGA3dCmdDXSetRenderTargets*>(payload);
    size_t offset = sizeof(SVGA3dCmdDXSetRenderTargets);
    if (pCmd->numTargets > 8) {
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }
    size_t idsBytes = (size_t)pCmd->numTargets * sizeof(uint32_t);
    if (offset + idsBytes > payloadSize) {
        return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
    }
    VlknContext *ctx = dev->contextMgr->getContext(pCmd->cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;

    const auto *sids = reinterpret_cast<const uint32_t*>(payload + offset);
    for (uint32_t i = 0; i < pCmd->numTargets; ++i) {
        Svga3VlknStatus st = checkGbSurface(dev, sids[i]);
        if (st != SVGA3_VLKN_SUCCESS) return st;
        st = ctx->setRenderTarget(SVGA3D_RT_COLOR0, sids[i], 0, 0);
        if (st != SVGA3_VLKN_SUCCESS) return st;
    }
    *bytesRead = offset + idsBytes;
    return SVGA3_VLKN_SUCCESS;
}

static Svga3VlknStatus dxClearRenderTarget(Svga3VlknDevice *dev,
                                           const uint8_t *payload,
                                           size_t payloadSize,
                                           size_t *bytesRead) {
    if (payloadSize < sizeof(SVGA3dCmdDXClearRenderTarget)) {
        return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
    }
    const auto *pCmd = reinterpret_cast<const SVGA3dCmdDXClearRenderTarget*>(payload);
    Svga3VlknStatus st = checkGbSurface(dev, pCmd->sid);
    if (st != SVGA3_VLKN_SUCCESS) return st;

    dev->contextMgr->endAllRenderPassesExcept(pCmd->cid);
    VlknContext *ctx = dev->contextMgr->getContext(pCmd->cid);
    if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;

    /* Pack float RGBA into the uint32_t color the D3D9 clear path takes. */
    uint32_t color = 0;
    for (int i = 0; i < 4; ++i) {
        float c = pCmd->color[i] < 0.0f ? 0.0f : (pCmd->color[i] > 1.0f ? 1.0f : pCmd->color[i]);
        color |= (uint32_t)(c * 255.0f + 0.5f) << (i * 8);
    }
    st = ctx->clear(SVGA3D_CLEAR_COLOR, color, 1.0f, 0, nullptr, 0);
    *bytesRead = sizeof(SVGA3dCmdDXClearRenderTarget);
    return st;
}

static Svga3VlknStatus dxPresent(Svga3VlknDevice *dev,
                                 const uint8_t *payload,
                                 size_t payloadSize,
                                 size_t *bytesRead) {
    if (payloadSize < sizeof(SVGA3dCmdDXPresent)) {
        return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
    }
    const auto *pCmd = reinterpret_cast<const SVGA3dCmdDXPresent*>(payload);
    Svga3VlknStatus st = checkGbSurface(dev, pCmd->sid);
    if (st != SVGA3_VLKN_SUCCESS) return st;

    if (dev->contextMgr) dev->contextMgr->endAllRenderPasses();
    st = dev->surfaceMgr->present(pCmd->sid, nullptr, 0, dev->guestMem.get());
    *bytesRead = sizeof(SVGA3dCmdDXPresent);
    return st;
}

Svga3VlknStatus svga3_dx_dispatch(::Svga3VlknDevice *dev,
                                 uint32_t cmd,
                                 const uint8_t *payload,
                                 size_t payloadSize,
                                 size_t *bytesRead) {
    if (!dev || !payload || !bytesRead) {
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }
    switch (cmd) {
        case SVGA_3D_CMD_DX_DEFINE_CONTEXT:
            return dxDefineContext(dev, payload, payloadSize, bytesRead);
        case SVGA_3D_CMD_DX_DESTROY_CONTEXT:
            return dxDestroyContext(dev, payload, payloadSize, bytesRead);
        case SVGA_3D_CMD_DX_SET_RENDER_TARGETS:
            return dxSetRenderTargets(dev, payload, payloadSize, bytesRead);
        case SVGA_3D_CMD_DX_CLEAR_RENDER_TARGET:
            return dxClearRenderTarget(dev, payload, payloadSize, bytesRead);
        case SVGA_3D_CMD_DX_PRESENT:
            return dxPresent(dev, payload, payloadSize, bytesRead);
        default:
            /* Fail closed: unknown DX command. */
            *bytesRead = 0;
            return SVGA3_VLKN_ERROR_UNSUPPORTED_COMMAND;
    }
}

} // namespace svga3_vlkn
