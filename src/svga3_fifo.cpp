/*
 * SVGA3=VLKN - SVGA3D FIFO Command Stream Processor Implementation
 */

#include "svga3_device.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <mutex>

extern "C" void log_msg(const char *fmt, ...);

namespace svga3_vlkn {

static void blitClientSurfaceToFramebuffer(Svga3VlknDevice *dev, uint32_t cid, uint32_t sid, const char *reason) {
    if (!dev || !dev->guestMem || !dev->surfaceMgr || sid == 0 || sid == SVGA3D_INVALID_ID) return;
    VlknSurface *surf = dev->surfaceMgr->getSurface(sid);
    /* Cursors are 64 or smaller. A window can be as small as the OpenGL test. */
    if (!surf || surf->width() < 128 || surf->height() < 128 || surf->isDepthStencil()) return;

    if (dev->contextMgr) dev->contextMgr->endAllRenderPasses();

    const auto &fb = dev->guestMem->getFramebuffer();
    if (!fb.hva || fb.width == 0 || fb.height == 0) return;

    const void *mapped = nullptr;
    size_t rowPitch = 0;
    std::unique_lock<std::mutex> lock;
    bool reused = false;
    if (surf->hasReadback(surf->width(), surf->height())) {
        mapped = surf->readbackData();
        rowPitch = surf->readbackPitch();
        reused = true;
    } else {
        Svga3VlknStatus stDown = surf->dmaDownloadToStaging(0, nullptr, &mapped, &rowPitch, lock);
        if (stDown != SVGA3_VLKN_SUCCESS || !mapped) return;
    }

    uint32_t surfW = surf->width();
    uint32_t surfH = surf->height();
    uint32_t dstX = (fb.width > surfW) ? (fb.width - surfW) / 2 : 0;
    uint32_t dstY = (fb.height > surfH) ? (fb.height - surfH) / 2 : 0;
    uint32_t copyW = std::min(surfW, fb.width - dstX);
    uint32_t copyH = std::min(surfH, fb.height - dstY);
    uint32_t dstBpp = fb.bpp ? fb.bpp : 4;
    uint32_t dstPitch = fb.pitch ? fb.pitch : (fb.width * dstBpp);
    size_t bytesPerPixel = std::min(dstBpp, 4u);
    size_t rowBytes = copyW * bytesPerPixel;

    const uint8_t *src = static_cast<const uint8_t*>(mapped);
    uint32_t centerPixel = 0;
    if (copyW > 0 && copyH > 0 && bytesPerPixel == 4) {
        const uint32_t *midRow = reinterpret_cast<const uint32_t*>(src + (copyH / 2) * rowPitch);
        centerPixel = midRow[copyW / 2];
    }

    uint32_t p0 = (bytesPerPixel == 4) ? *reinterpret_cast<const uint32_t*>(src) : 0;
    bool isBlank = ((centerPixel & 0x00FFFFFF) == 0) &&
                   ((p0 & 0x00FFFFFF) == 0) &&
                   is_buffer_all_black_or_zero(mapped, copyW, copyH, rowPitch, bytesPerPixel);
    if (isBlank) {
        static uint32_t blank_client_warn = 0;
        if (blank_client_warn++ < 5 || (blank_client_warn % 500) == 0) {
            log_msg("[libqemu_svga3d] Client blit (%s, cid=%u): sid=%u is completely blank/zero, preserving fb.hva\n",
                    reason, cid, sid);
        }
        return;
    }

    for (uint32_t y = 0; y < copyH; ++y) {
        /* 64-bit offset: the old 32-bit product wrapped for large pitches.
         * Abort the blit rather than writing outside the framebuffer. */
        uint64_t dstOff = ((uint64_t)dstY + y) * dstPitch + (uint64_t)dstX * dstBpp;
        if (dstOff > fb.size || rowBytes > fb.size - dstOff) {
            return;
        }
        uint8_t *dst = fb.hva + (size_t)dstOff;
        memcpy(dst, src + y * rowPitch, rowBytes);
    }
    if (!reused) surf->storeReadback(surfW, surfH, rowPitch, mapped);

    dev->guestMem->notifyDisplayUpdate(dstX, dstY, copyW, copyH);

    if (lock.owns_lock()) lock.unlock();

    static uint32_t blit_count = 0;
    blit_count++;
    if (blit_count <= 10 || (blit_count % 50) == 0) {
        log_msg("[libqemu_svga3d] Client blit #%u (%s, cid=%u): sid=%u (%ux%u) to fb.hva at (%u,%u), centerPx=0x%08x\n",
                blit_count, reason, cid, sid, copyW, copyH, dstX, dstY, centerPixel);
    }
}

void svga3_vlkn_present_client_surfaces(Svga3VlknDevice *dev, const char *reason) {
    if (!dev) return;

    /* Close active passes before readback. A pending window readback submits
     * the draw and copy together, so flushing here would add a queue-idle per
     * frame before the readback's own queue-idle. */
    if (dev->contextMgr) dev->contextMgr->endAllRenderPasses();

    if (!dev->contextMgr || !dev->surfaceMgr || !dev->guestMem) return;
    auto pending = dev->contextMgr->collectPendingWindowPresents();
    for (const auto &item : pending) {
        blitClientSurfaceToFramebuffer(dev, item.first, item.second, reason);
    }

    /* The readback normally flushed the command buffer. This also completes
     * work submitted on paths that have no window surface to read back. */
    if (dev->backend) {
        Svga3VlknStatus fst = dev->backend->flushCommandBuffer();
        if (fst != SVGA3_VLKN_SUCCESS) {
            log_msg("[libqemu_svga3d] present_client_surfaces: flush failed (%d)\n", (int)fst);
        }
    }
}

Svga3VlknStatus processFifoPacket(Svga3VlknDevice *dev,
                                  uint32_t cmd,
                                  const uint8_t *payload,
                                  size_t payloadSize,
                                  size_t *bytesRead)
{
    *bytesRead = 0;

    /* Temporary: trace every FIFO command for rendering investigation. */
    if (getenv("SVGA3_VLKN_TRACE_FIFO")) {
        fprintf(stderr, "[fifo-trace] cmd=%u payloadSize=%zu\n", cmd, payloadSize);
    }

    switch (cmd) {
        case SVGA_3D_CMD_SURFACE_DEFINE: {
            if (payloadSize < sizeof(SVGA3dCmdDefineSurface)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdDefineSurface*>(payload);
            size_t offset = sizeof(SVGA3dCmdDefineSurface);

            uint32_t numFaces = (pCmd->surfaceFlags & SVGA3D_SURFACE_CUBEMAP) ? 6 : 1;
            uint32_t numMipLevels = pCmd->face[0].numMipLevels;
            /* 64-bit: numFaces * numMipLevels wrapped in 32-bit for huge
             * numMipLevels, shrinking sizesBytes and bypassing the bounds
             * check below. Cap mip levels at a sane maximum first. */
            if (numMipLevels > SVGA3_MAX_MIP_LEVELS) {
                return SVGA3_VLKN_ERROR_INVALID_PARAM;
            }
            uint64_t totalSizes = (uint64_t)numFaces * numMipLevels;

            size_t sizesBytes = (size_t)totalSizes * sizeof(SVGA3dSize);
            if (offset + sizesBytes > payloadSize) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }

            const auto *sizes = reinterpret_cast<const SVGA3dSize*>(payload + offset);
            offset += sizesBytes;

            Svga3VlknStatus st = dev->surfaceMgr->defineSurface(
                pCmd->sid, pCmd->surfaceFlags, pCmd->format, sizes, (uint32_t)totalSizes
            );
            if (st == SVGA3_VLKN_SUCCESS) {
                dev->stats.surfacesCreated++;
                if (totalSizes > 0) {
                    log_msg("[libqemu_svga3d] SURFACE_DEFINE: sid=%u, fmt=%u, w=%u, h=%u, mips=%u\n",
                            pCmd->sid, pCmd->format, sizes[0].width, sizes[0].height, numMipLevels);
                }
            }
            *bytesRead = offset;
            return st;
        }

        case SVGA_3D_CMD_SURFACE_DESTROY: {
            if (payloadSize < sizeof(SVGA3dCmdDestroySurface)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdDestroySurface*>(payload);
            Svga3VlknStatus st = dev->surfaceMgr->destroySurface(pCmd->sid);
            if (st == SVGA3_VLKN_SUCCESS) {
                dev->stats.surfacesDestroyed++;
            }
            *bytesRead = sizeof(SVGA3dCmdDestroySurface);
            return st;
        }

        case SVGA_3D_CMD_SURFACE_COPY: {
            if (payloadSize < sizeof(SVGA3dCmdSurfaceCopy)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdSurfaceCopy*>(payload);
            size_t offset = sizeof(SVGA3dCmdSurfaceCopy);

            size_t remaining = payloadSize - offset;
            uint32_t numBoxes = remaining / sizeof(SVGA3dCopyBox);
            const auto *boxes = reinterpret_cast<const SVGA3dCopyBox*>(payload + offset);
            offset += numBoxes * sizeof(SVGA3dCopyBox);

            if (dev->contextMgr) dev->contextMgr->endAllRenderPasses();
            Svga3VlknStatus st = dev->surfaceMgr->copy(pCmd->src.sid, pCmd->dest.sid, boxes, numBoxes,
                                                       pCmd->src.mipmap, pCmd->src.face,
                                                       pCmd->dest.mipmap, pCmd->dest.face);
            *bytesRead = offset;
            return st;
        }

        case SVGA_3D_CMD_SURFACE_STRETCHBLT: {
            if (payloadSize < sizeof(SVGA3dCmdSurfaceStretchBlt)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdSurfaceStretchBlt*>(payload);
            if (dev->contextMgr) dev->contextMgr->endAllRenderPasses();
            Svga3VlknStatus st = dev->surfaceMgr->stretchBlt(
                pCmd->src.sid, pCmd->dest.sid, pCmd->boxSrc, pCmd->boxDest, pCmd->mode
            );
            *bytesRead = sizeof(SVGA3dCmdSurfaceStretchBlt);
            return st;
        }

        case SVGA_3D_CMD_SURFACE_DMA: {
            if (payloadSize < sizeof(SVGA3dCmdSurfaceDMA)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdSurfaceDMA*>(payload);
            size_t offset = sizeof(SVGA3dCmdSurfaceDMA);

            size_t remaining = payloadSize - offset;
            uint32_t numBoxes = remaining / sizeof(SVGA3dCopyBox);
            const auto *boxes = reinterpret_cast<const SVGA3dCopyBox*>(payload + offset);

            if (dev->contextMgr) dev->contextMgr->endAllRenderPasses();

            static uint32_t dma_count = 0;
            dma_count++;
            if (dma_count <= 5 || (dma_count % 500) == 0 || pCmd->host.sid == 73) {
                log_msg("[libqemu_svga3d] SURFACE_DMA #%u: guest=(gmrId=%u, offset=%u, pitch=%u) host.sid=%u transfer=%u numBoxes=%u\n",
                        dma_count, pCmd->guest.ptr.gmrId, pCmd->guest.ptr.offset, pCmd->guest.pitch,
                        pCmd->host.sid, pCmd->transfer, numBoxes);
                for (uint32_t b = 0; b < numBoxes && b < 2; ++b) {
                    log_msg("   box[%u]: (%u,%u,%u %ux%ux%u) src=(%u,%u,%u)\n",
                            b, boxes[b].x, boxes[b].y, boxes[b].z, boxes[b].w, boxes[b].h, boxes[b].d,
                            boxes[b].srcx, boxes[b].srcy, boxes[b].srcz);
                }
            }

            Svga3VlknStatus st = dev->surfaceMgr->surfaceDMA(
                pCmd->guest, pCmd->host, pCmd->transfer,
                boxes, numBoxes, dev->guestMem.get()
            );

            *bytesRead = payloadSize;
            return st;
        }

        case SVGA_3D_CMD_CONTEXT_DEFINE: {
            if (payloadSize < sizeof(SVGA3dCmdDefineContext)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdDefineContext*>(payload);
            log_msg("[libqemu_svga3d] CONTEXT_DEFINE: cid=%u\n", pCmd->cid);
            Svga3VlknStatus st = dev->contextMgr->createContext(pCmd->cid);
            *bytesRead = sizeof(SVGA3dCmdDefineContext);
            return st;
        }

        case SVGA_3D_CMD_CONTEXT_DESTROY: {
            if (payloadSize < sizeof(SVGA3dCmdDestroyContext)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdDestroyContext*>(payload);
            log_msg("[libqemu_svga3d] CONTEXT_DESTROY: cid=%u\n", pCmd->cid);

            if (pCmd->cid != 246 && dev->contextMgr) {
                VlknContext *ctx = dev->contextMgr->getContext(pCmd->cid);
                if (ctx) {
                    const RenderTargetBinding *curRt = ctx->getRenderTarget(SVGA3D_RT_COLOR0);
                    if (curRt && curRt->sid != 0 && curRt->sid != SVGA3D_INVALID_ID) {
                        blitClientSurfaceToFramebuffer(dev, pCmd->cid, curRt->sid, "context_destroy");
                    }
                }
            }

            Svga3VlknStatus st = dev->contextMgr->destroyContext(pCmd->cid);
            *bytesRead = sizeof(SVGA3dCmdDestroyContext);
            return st;
        }

        case SVGA_3D_CMD_SETRENDERSTATE: {
            if (payloadSize < sizeof(SVGA3dCmdSetRenderState)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdSetRenderState*>(payload);
            size_t offset = sizeof(SVGA3dCmdSetRenderState);

            VlknContext *ctx = dev->contextMgr->getContext(pCmd->cid);
            if (!ctx) {
                return SVGA3_VLKN_ERROR_NOT_FOUND;
            }

            size_t remaining = payloadSize - offset;
            uint32_t numStates = remaining / sizeof(SVGA3dRenderState);
            const auto *states = reinterpret_cast<const SVGA3dRenderState*>(payload + offset);
            offset += numStates * sizeof(SVGA3dRenderState);

            for (uint32_t i = 0; i < numStates; ++i) {
                ctx->setRenderState(states[i].state, states[i].uintValue);
            }

            *bytesRead = offset;
            return SVGA3_VLKN_SUCCESS;
        }

        case SVGA_3D_CMD_SETRENDERTARGET: {
            if (payloadSize < sizeof(SVGA3dCmdSetRenderTarget)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdSetRenderTarget*>(payload);
            VlknContext *ctx = dev->contextMgr->getContext(pCmd->cid);
            if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;

            static uint32_t srt_count_246 = 0;
            static uint32_t srt_count_app = 0;
            bool is_246 = (pCmd->cid == 246);
            uint32_t cur_srt = is_246 ? ++srt_count_246 : ++srt_count_app;
            if (cur_srt <= 5 || (cur_srt % 500) == 0) {
                log_msg("[libqemu_svga3d] SETRENDERTARGET (cid=%u #%u): type=%u, target.sid=%u, face=%u, mip=%u\n",
                        pCmd->cid, cur_srt, pCmd->type, pCmd->target.sid, pCmd->target.face, pCmd->target.mipmap);
            }

            /* Detect double-buffered client window surface swap on application context */
            if (!is_246 && pCmd->type == SVGA3D_RT_COLOR0 && dev->guestMem && dev->surfaceMgr) {
                const RenderTargetBinding *curRt = ctx->getRenderTarget(SVGA3D_RT_COLOR0);
                uint32_t oldSid = curRt ? curRt->sid : 0;
                uint32_t newSid = pCmd->target.sid;
                if (oldSid != 0 && oldSid != SVGA3D_INVALID_ID && oldSid != newSid) {
                    VlknSurface *oldSurf = dev->surfaceMgr->getSurface(oldSid);
                    if (oldSurf && oldSurf->width() >= 320 && oldSurf->height() >= 240 && !oldSurf->isDepthStencil()) {
                        blitClientSurfaceToFramebuffer(dev, pCmd->cid, oldSid, "swap");
                        ctx->resetDrawnToWindow();
                    }
                }
            }

            Svga3VlknStatus st = ctx->setRenderTarget(
                pCmd->type, pCmd->target.sid, pCmd->target.face, pCmd->target.mipmap
            );
            *bytesRead = sizeof(SVGA3dCmdSetRenderTarget);
            return st;
        }

        case SVGA_3D_CMD_SETTEXTURESTATE: {
            if (payloadSize < sizeof(SVGA3dCmdSetTextureState)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdSetTextureState*>(payload);
            size_t offset = sizeof(SVGA3dCmdSetTextureState);

            VlknContext *ctx = dev->contextMgr->getContext(pCmd->cid);
            if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;

            size_t remaining = payloadSize - offset;
            uint32_t numStates = remaining / sizeof(SVGA3dTextureState);
            const auto *states = reinterpret_cast<const SVGA3dTextureState*>(payload + offset);
            offset += numStates * sizeof(SVGA3dTextureState);

            static uint32_t sts_count = 0;
            sts_count++;
            if (sts_count <= 5 || (sts_count % 500) == 0) {
                log_msg("[libqemu_svga3d] SETTEXTURESTATE #%u: cid=%u, numStates=%u\n", sts_count, pCmd->cid, numStates);
                for (uint32_t i = 0; i < numStates; ++i) {
                    log_msg("   [%u] stage=%u, name=%u, val=0x%x (%u)\n",
                            i, states[i].stage, states[i].name, states[i].value, states[i].value);
                }
            }

            for (uint32_t i = 0; i < numStates; ++i) {
                if (states[i].name == SVGA3D_TS_BIND_TEXTURE) {
                    ctx->setTexture(states[i].stage, states[i].value);
                } else {
                    ctx->setTextureStageState(states[i].stage, states[i].name, states[i].value);
                }
            }

            *bytesRead = offset;
            return SVGA3_VLKN_SUCCESS;
        }

        case SVGA_3D_CMD_SETTRANSFORM: {
            if (payloadSize < sizeof(SVGA3dCmdSetTransform)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdSetTransform*>(payload);
            VlknContext *ctx = dev->contextMgr->getContext(pCmd->cid);
            if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;

            Svga3VlknStatus st = ctx->setTransform(pCmd->type, pCmd->matrix);
            *bytesRead = sizeof(SVGA3dCmdSetTransform);
            return st;
        }

        case SVGA_3D_CMD_SETZRANGE: {
            if (payloadSize < sizeof(SVGA3dCmdSetZRange)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdSetZRange*>(payload);
            VlknContext *ctx = dev->contextMgr->getContext(pCmd->cid);
            if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;

            Svga3VlknStatus st = ctx->setZRange(&pCmd->zRange);
            *bytesRead = sizeof(SVGA3dCmdSetZRange);
            return st;
        }

        case SVGA_3D_CMD_SETMATERIAL: {
            if (payloadSize < sizeof(SVGA3dCmdSetMaterial)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdSetMaterial*>(payload);
            VlknContext *ctx = dev->contextMgr->getContext(pCmd->cid);
            if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;

            Svga3VlknStatus st = ctx->setMaterial(pCmd->face, &pCmd->material);
            *bytesRead = sizeof(SVGA3dCmdSetMaterial);
            return st;
        }

        case SVGA_3D_CMD_SETLIGHTDATA: {
            if (payloadSize < sizeof(SVGA3dCmdSetLightData)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdSetLightData*>(payload);
            VlknContext *ctx = dev->contextMgr->getContext(pCmd->cid);
            if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;

            Svga3VlknStatus st = ctx->setLightData(pCmd->index, &pCmd->data);
            *bytesRead = sizeof(SVGA3dCmdSetLightData);
            return st;
        }

        case SVGA_3D_CMD_SETLIGHTENABLED: {
            if (payloadSize < sizeof(SVGA3dCmdSetLightEnabled)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdSetLightEnabled*>(payload);
            VlknContext *ctx = dev->contextMgr->getContext(pCmd->cid);
            if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;

            Svga3VlknStatus st = ctx->setLightEnabled(pCmd->index, pCmd->enabled);
            *bytesRead = sizeof(SVGA3dCmdSetLightEnabled);
            return st;
        }

        case SVGA_3D_CMD_SETVIEWPORT: {
            if (payloadSize < sizeof(SVGA3dCmdSetViewport)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdSetViewport*>(payload);
            VlknContext *ctx = dev->contextMgr->getContext(pCmd->cid);
            if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;

            static uint32_t vp_count = 0;
            vp_count++;
            if (vp_count <= 5 || (vp_count % 500) == 0) {
                log_msg("[libqemu_svga3d] SETVIEWPORT #%u: cid=%u, rect=(%u,%u %ux%u)\n",
                        vp_count, pCmd->cid, pCmd->rect.x, pCmd->rect.y, pCmd->rect.w, pCmd->rect.h);
            }

            Svga3VlknStatus st = ctx->setViewport(&pCmd->rect);
            *bytesRead = sizeof(SVGA3dCmdSetViewport);
            return st;
        }

        case SVGA_3D_CMD_SETSCISSORRECT: {
            if (payloadSize < sizeof(SVGA3dCmdSetScissorRect)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdSetScissorRect*>(payload);
            VlknContext *ctx = dev->contextMgr->getContext(pCmd->cid);
            if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;

            Svga3VlknStatus st = ctx->setScissorRect(&pCmd->rect);
            *bytesRead = sizeof(SVGA3dCmdSetScissorRect);
            return st;
        }

        case SVGA_3D_CMD_SETCLIPPLANE: {
            if (payloadSize < sizeof(SVGA3dCmdSetClipPlane)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdSetClipPlane*>(payload);
            VlknContext *ctx = dev->contextMgr->getContext(pCmd->cid);
            if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;

            Svga3VlknStatus st = ctx->setClipPlane(pCmd->index, pCmd->plane);
            *bytesRead = sizeof(SVGA3dCmdSetClipPlane);
            return st;
        }

        case SVGA_3D_CMD_CLEAR: {
            if (payloadSize < sizeof(SVGA3dCmdClear)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdClear*>(payload);
            size_t offset = sizeof(SVGA3dCmdClear);

            VlknContext *ctx = dev->contextMgr->getContext(pCmd->cid);
            if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;

            size_t remaining = payloadSize - offset;
            uint32_t numRects = remaining / sizeof(SVGA3dRect);
            const auto *rects = reinterpret_cast<const SVGA3dRect*>(payload + offset);
            offset += numRects * sizeof(SVGA3dRect);

            Svga3VlknStatus st = ctx->clear(
                pCmd->clearFlag, pCmd->color, pCmd->depth, pCmd->stencil, rects, numRects
            );
            if (st == SVGA3_VLKN_SUCCESS) {
                dev->stats.renderPassesExecuted++;
                static uint32_t clr_count_246 = 0;
                static uint32_t clr_count_app = 0;
                bool is_246 = (pCmd->cid == 246);
                uint32_t cur_clr = is_246 ? ++clr_count_246 : ++clr_count_app;
                if (cur_clr <= 5 || (cur_clr % 500) == 0) {
                    log_msg("[libqemu_svga3d] CLEAR (cid=%u #%u): flags=0x%x, color=0x%08x, depth=%f, numRects=%u\n",
                            pCmd->cid, cur_clr, pCmd->clearFlag, pCmd->color, pCmd->depth, numRects);
                    for (uint32_t r = 0; r < numRects && r < 4; ++r) {
                        log_msg("   rect[%u]: (%u,%u %ux%u)\n", r, rects[r].x, rects[r].y, rects[r].w, rects[r].h);
                    }
                }
            }
            *bytesRead = offset;
            return st;
        }

        case SVGA_3D_CMD_DRAW_PRIMITIVES: {
            if (payloadSize < sizeof(SVGA3dCmdDrawPrimitives)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdDrawPrimitives*>(payload);
            size_t offset = sizeof(SVGA3dCmdDrawPrimitives);

            VlknContext *ctx = dev->contextMgr->getContext(pCmd->cid);
            if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;

            /* Cap guest-controlled counts: unbounded decls blow up pipeline
             * creation (binding vectors, vkCreateGraphicsPipelines past
             * device limits) and unbounded ranges bloat command buffers. */
            if (pCmd->numVertexDecls > SVGA3_MAX_VERTEX_DECLS) {
                log_msg("[libqemu_svga3d] DRAW_PRIMITIVES error: numVertexDecls %u exceeds max %u\n",
                        pCmd->numVertexDecls, SVGA3_MAX_VERTEX_DECLS);
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            if (pCmd->numRanges > SVGA3_MAX_PRIMITIVE_RANGES) {
                log_msg("[libqemu_svga3d] DRAW_PRIMITIVES error: numRanges %u exceeds max %u\n",
                        pCmd->numRanges, SVGA3_MAX_PRIMITIVE_RANGES);
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }

            size_t declsBytes = pCmd->numVertexDecls * sizeof(SVGA3dVertexDecl);
            size_t rangesBytes = pCmd->numRanges * sizeof(SVGA3dPrimitiveRange);

            if (offset + declsBytes + rangesBytes > payloadSize) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }

            const auto *decls = reinterpret_cast<const SVGA3dVertexDecl*>(payload + offset);
            offset += declsBytes;

            const auto *ranges = reinterpret_cast<const SVGA3dPrimitiveRange*>(payload + offset);
            offset += rangesBytes;

            /* Default primitive type: TriangleList */
            SVGA3dPrimitiveType primType = SVGA3D_PRIMITIVE_TRIANGLELIST;
            if (pCmd->numRanges > 0) {
                primType = (SVGA3dPrimitiveType)ranges[0].primType;
            }

            static uint32_t draw_count_246 = 0;
            static uint32_t draw_count_app = 0;
            bool is_246 = (pCmd->cid == 246);
            uint32_t cur_draw = is_246 ? ++draw_count_246 : ++draw_count_app;
            if (cur_draw <= 5 || (cur_draw % 500) == 0) {
                log_msg("[libqemu_svga3d] DRAW_PRIMITIVES (cid=%u #%u): primType=%u, numVertexDecls=%u, numRanges=%u\n",
                        pCmd->cid, cur_draw, (uint32_t)primType, pCmd->numVertexDecls, pCmd->numRanges);
                for (uint32_t i = 0; i < pCmd->numVertexDecls; ++i) {
                    log_msg("   decl[%u]: usage=%u, usageIdx=%u, type=%u, sid=%u, offset=%u, stride=%u\n",
                            i, decls[i].identity.usage, decls[i].identity.usageIndex,
                            decls[i].identity.type, decls[i].array.surfaceId,
                            decls[i].array.offset, decls[i].array.stride);
                }
                for (uint32_t i = 0; i < pCmd->numRanges; ++i) {
                    log_msg("   range[%u]: primType=%u, primCount=%u, idxSid=%u, idxOff=%u, idxStride=%u, idxBias=%d\n",
                            i, ranges[i].primType, ranges[i].primitiveCount,
                            ranges[i].indexArray.surfaceId, ranges[i].indexArray.offset,
                            ranges[i].indexArray.stride, ranges[i].indexBias);
                }
            }

            if (is_246) {
                if (dev->contextMgr) dev->contextMgr->endAllRenderPasses();
                const auto &fb = dev->guestMem->getFramebuffer();
                if (fb.hva && fb.width && fb.height) {
                    VlknSurface *surf1 = dev->surfaceMgr->getSurface(1);
                    if (surf1) {
                        SVGA3dBox box = { 0, 0, 0, std::min(fb.width, surf1->width()), std::min(fb.height, surf1->height()), 1 };
                        surf1->dmaUpload(0, &box, fb.hva, fb.pitch);
                    }
                }
            }

            Svga3VlknStatus st = ctx->draw(
                primType, decls, pCmd->numVertexDecls, ranges, pCmd->numRanges
            );
            if (st == SVGA3_VLKN_SUCCESS) {
                dev->stats.drawCallsSubmitted += pCmd->numRanges;
                for (uint32_t i = 0; i < pCmd->numRanges; ++i) {
                    dev->stats.primitivesRendered += ranges[i].primitiveCount;
                    dev->stats.verticesRendered += ranges[i].primitiveCount * 3;
                }
            }
            *bytesRead = offset;
            return st;
        }

        case SVGA_3D_CMD_BEGIN_QUERY: {
            if (payloadSize < sizeof(SVGA3dCmdBeginQuery)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdBeginQuery*>(payload);
            VlknContext *ctx = dev->contextMgr->getContext(pCmd->cid);
            if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;

            Svga3VlknStatus st = ctx->beginQuery(pCmd->type);
            *bytesRead = sizeof(SVGA3dCmdBeginQuery);
            return st;
        }

        case SVGA_3D_CMD_END_QUERY: {
            if (payloadSize < sizeof(SVGA3dCmdEndQuery)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdEndQuery*>(payload);
            VlknContext *ctx = dev->contextMgr->getContext(pCmd->cid);
            if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;

            Svga3VlknStatus st = ctx->endQuery(pCmd->type);
            *bytesRead = sizeof(SVGA3dCmdEndQuery);
            return st;
        }

        case SVGA_3D_CMD_WAIT_FOR_QUERY: {
            if (payloadSize < sizeof(SVGA3dCmdWaitForQuery)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdWaitForQuery*>(payload);
            VlknContext *ctx = dev->contextMgr->getContext(pCmd->cid);
            if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;

            uint32_t result = 0;
            Svga3VlknStatus st = ctx->waitForQuery(pCmd->type, &result);
            if (dev->guestMem && pCmd->guestResult.gmrId != SVGA_GMR_NULL) {
                SVGA3dQueryResult res = {};
                res.totalSize = sizeof(SVGA3dQueryResult);
                res.state = SVGA3D_QUERYSTATE_SUCCEEDED;
                res.result32 = result;
                dev->guestMem->writeGuest(pCmd->guestResult, &res, sizeof(res));
            }
            *bytesRead = sizeof(SVGA3dCmdWaitForQuery);
            return st;
        }

        case SVGA_3D_CMD_PRESENT: {
            if (payloadSize < sizeof(SVGA3dCmdPresent)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdPresent*>(payload);
            size_t offset = sizeof(SVGA3dCmdPresent);

            size_t remaining = payloadSize - offset;
            uint32_t numRects = remaining / sizeof(SVGA3dCopyRect);
            const auto *rects = reinterpret_cast<const SVGA3dCopyRect*>(payload + offset);
            offset += numRects * sizeof(SVGA3dCopyRect);

            if (dev->contextMgr) dev->contextMgr->endAllRenderPasses();
            static uint32_t pres_count = 0;
            pres_count++;
            if (pres_count <= 5 || (pres_count % 500) == 0) {
                log_msg("[libqemu_svga3d] PRESENT #%u: sid=%u, numRects=%u\n", pres_count, pCmd->sid, numRects);
                if (numRects > 0 && rects) {
                    log_msg("[libqemu_svga3d]   rect0: dst=(%u,%u %ux%u) src=(%u,%u)\n",
                            rects[0].x, rects[0].y, rects[0].w, rects[0].h, rects[0].srcx, rects[0].srcy);
                }
            }
            Svga3VlknStatus st = dev->surfaceMgr->present(pCmd->sid, rects, numRects, dev->guestMem.get());
            *bytesRead = offset;
            return st;
        }

        case SVGA_3D_CMD_SHADER_DEFINE: {
            if (payloadSize < sizeof(SVGA3dCmdDefineShader)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdDefineShader*>(payload);
            size_t offset = sizeof(SVGA3dCmdDefineShader);

            VlknContext *ctx = dev->contextMgr->getContext(pCmd->cid);
            if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;

            size_t remaining = payloadSize - offset;
            uint32_t numDwords = remaining / sizeof(uint32_t);
            const uint32_t *bytecode = reinterpret_cast<const uint32_t*>(payload + offset);
            offset += numDwords * sizeof(uint32_t);

            log_msg("[libqemu_svga3d] SHADER_DEFINE: cid=%u, shid=%u, type=%u, numDwords=%u\n",
                    pCmd->cid, pCmd->shid, pCmd->type, numDwords);
            Svga3VlknStatus st = ctx->defineShader(pCmd->shid, pCmd->type, bytecode, numDwords);
            *bytesRead = offset;
            return st;
        }

        case SVGA_3D_CMD_SHADER_DESTROY: {
            if (payloadSize < sizeof(SVGA3dCmdDestroyShader)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdDestroyShader*>(payload);
            VlknContext *ctx = dev->contextMgr->getContext(pCmd->cid);
            if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;

            Svga3VlknStatus st = ctx->destroyShader(pCmd->shid, pCmd->type);
            *bytesRead = sizeof(SVGA3dCmdDestroyShader);
            return st;
        }

        case SVGA_3D_CMD_SET_SHADER: {
            if (payloadSize < sizeof(SVGA3dCmdSetShader)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdSetShader*>(payload);
            VlknContext *ctx = dev->contextMgr->getContext(pCmd->cid);
            if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;

            Svga3VlknStatus st = ctx->setShader(pCmd->type, pCmd->shid);
            *bytesRead = sizeof(SVGA3dCmdSetShader);
            return st;
        }

        case SVGA_3D_CMD_SET_SHADER_CONST: {
            if (payloadSize < sizeof(SVGA3dCmdSetShaderConst)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdSetShaderConst*>(payload);
            VlknContext *ctx = dev->contextMgr->getContext(pCmd->cid);
            if (!ctx) return SVGA3_VLKN_ERROR_NOT_FOUND;

            /* Mesa batches consecutive constants in a single command: the
             * 16-byte header (cid, reg, type, ctype) is followed by N 16-byte
             * values[4] records. Apply every record; the old code silently
             * dropped all but the first. */
            size_t headerSize = offsetof(SVGA3dCmdSetShaderConst, values);
            size_t bodySize = payloadSize - headerSize;
            if (bodySize == 0 || bodySize % sizeof(pCmd->values) != 0) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            /* Validate shader type and constant type before any mutation. */
            if (pCmd->type != SVGA3D_SHADERTYPE_VS && pCmd->type != SVGA3D_SHADERTYPE_PS) {
                return SVGA3_VLKN_ERROR_INVALID_PARAM;
            }
            uint32_t maxReg;
            if (pCmd->ctype == SVGA3D_CONST_TYPE_FLOAT) {
                maxReg = 256;
            } else if (pCmd->ctype == SVGA3D_CONST_TYPE_INT ||
                       pCmd->ctype == SVGA3D_CONST_TYPE_BOOL) {
                maxReg = 16;
            } else {
                return SVGA3_VLKN_ERROR_INVALID_PARAM;
            }
            unsigned numConsts = static_cast<unsigned>(bodySize / sizeof(pCmd->values));
            /* Validate the entire batch upfront using subtraction to avoid
             * overflow in reg + numConsts. */
            if (pCmd->reg >= maxReg || numConsts > maxReg - pCmd->reg) {
                return SVGA3_VLKN_ERROR_INVALID_PARAM;
            }
            const float (*values)[4] =
                reinterpret_cast<const float (*)[4]>(payload + headerSize);
            Svga3VlknStatus st = SVGA3_VLKN_SUCCESS;
            for (unsigned i = 0; i < numConsts; ++i) {
                st = ctx->setShaderConst(pCmd->reg + i, pCmd->type, pCmd->ctype,
                                         reinterpret_cast<const uint32_t*>(values[i]));
                if (st != SVGA3_VLKN_SUCCESS) break;
            }
            *bytesRead = payloadSize;
            return st;
        }

        case SVGA_3D_CMD_BLIT_SURFACE_TO_SCREEN: {
            if (payloadSize < sizeof(SVGA3dCmdBlitSurfaceToScreen)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdBlitSurfaceToScreen*>(payload);
            size_t offset = sizeof(SVGA3dCmdBlitSurfaceToScreen);

            size_t remaining = payloadSize - offset;
            uint32_t numClipRects = remaining / sizeof(SVGASignedRect);
            const auto *clipRects = reinterpret_cast<const SVGASignedRect*>(payload + offset);
            offset += numClipRects * sizeof(SVGASignedRect);

            if (dev->contextMgr) dev->contextMgr->endAllRenderPasses();
            Svga3VlknStatus st = dev->surfaceMgr->blitSurfaceToScreen(
                pCmd->srcImage, pCmd->srcRect, pCmd->destScreenId, pCmd->destRect,
                clipRects, numClipRects, dev->guestMem.get()
            );
            *bytesRead = offset;
            return st;
        }

        case SVGA_3D_CMD_SURFACE_DEFINE_V2: {
            if (payloadSize < sizeof(SVGA3dCmdDefineSurface_v2)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdDefineSurface_v2*>(payload);
            size_t offset = sizeof(SVGA3dCmdDefineSurface_v2);

            uint32_t numFaces = (pCmd->surfaceFlags & SVGA3D_SURFACE_CUBEMAP) ? 6 : 1;
            uint32_t numMipLevels = pCmd->face[0].numMipLevels;
            /* 64-bit: numFaces * numMipLevels wrapped in 32-bit for huge
             * numMipLevels, shrinking sizesBytes and bypassing the bounds
             * check below. Cap mip levels at a sane maximum first. */
            if (numMipLevels > SVGA3_MAX_MIP_LEVELS) {
                return SVGA3_VLKN_ERROR_INVALID_PARAM;
            }
            uint64_t totalSizes = (uint64_t)numFaces * numMipLevels;

            size_t sizesBytes = (size_t)totalSizes * sizeof(SVGA3dSize);
            if (offset + sizesBytes > payloadSize) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }

            const auto *sizes = reinterpret_cast<const SVGA3dSize*>(payload + offset);
            offset += sizesBytes;

            Svga3VlknStatus st = dev->surfaceMgr->defineSurfaceV2(
                pCmd->sid, pCmd->surfaceFlags, pCmd->format,
                pCmd->multisampleCount, pCmd->autogenFilter,
                sizes, (uint32_t)totalSizes
            );
            if (st == SVGA3_VLKN_SUCCESS) {
                dev->stats.surfacesCreated++;
            }
            *bytesRead = offset;
            return st;
        }

        case SVGA_3D_CMD_GENERATE_MIPMAPS: {
            if (payloadSize < sizeof(SVGA3dCmdGenerateMipmaps)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGA3dCmdGenerateMipmaps*>(payload);
            Svga3VlknStatus st = dev->surfaceMgr->generateMipmaps(pCmd->sid, pCmd->filter);
            *bytesRead = sizeof(SVGA3dCmdGenerateMipmaps);
            return st;
        }

        case SVGA_3D_CMD_ACTIVATE_SURFACE: {
            if (payloadSize < sizeof(uint32_t)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            uint32_t sid = *reinterpret_cast<const uint32_t*>(payload);
            Svga3VlknStatus st = dev->surfaceMgr->setSurfaceActive(sid, true);
            *bytesRead = sizeof(uint32_t);
            return st;
        }

        case SVGA_3D_CMD_DEACTIVATE_SURFACE: {
            if (payloadSize < sizeof(uint32_t)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            uint32_t sid = *reinterpret_cast<const uint32_t*>(payload);
            Svga3VlknStatus st = dev->surfaceMgr->setSurfaceActive(sid, false);
            *bytesRead = sizeof(uint32_t);
            return st;
        }

        /* 2D / GMR / Host FIFO Commands */
        case SVGA_CMD_DEFINE_GMR2: {
            if (payloadSize < sizeof(SVGAFifoCmdDefineGMR2)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGAFifoCmdDefineGMR2*>(payload);
            Svga3VlknStatus st = SVGA3_VLKN_SUCCESS;
            if (dev->guestMem) {
                st = dev->guestMem->defineGMR2(pCmd->gmrId, pCmd->numPages);
            }
            *bytesRead = sizeof(SVGAFifoCmdDefineGMR2);
            return st;
        }

        case SVGA_CMD_REMAP_GMR2: {
            if (payloadSize < sizeof(SVGAFifoCmdRemapGMR2)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGAFifoCmdRemapGMR2*>(payload);
            size_t offset = sizeof(SVGAFifoCmdRemapGMR2);

            size_t descBytes = 0;
            if (pCmd->flags & SVGA_REMAP_GMR2_VIA_GMR) {
                descBytes = sizeof(SVGAGuestPtr);
            } else if (pCmd->flags & SVGA_REMAP_GMR2_SINGLE_PPN) {
                descBytes = (pCmd->flags & SVGA_REMAP_GMR2_PPN64) ? sizeof(uint64_t) : sizeof(uint32_t);
            } else {
                descBytes = static_cast<size_t>(pCmd->numPages) *
                    ((pCmd->flags & SVGA_REMAP_GMR2_PPN64) ? sizeof(uint64_t) : sizeof(uint32_t));
            }

            if (offset + descBytes > payloadSize) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }

            const void *descriptors = payload + offset;
            offset += descBytes;

            Svga3VlknStatus st = SVGA3_VLKN_SUCCESS;
            if (dev->guestMem) {
                st = dev->guestMem->remapGMR2(
                    pCmd->gmrId, pCmd->flags, pCmd->offsetPages, pCmd->numPages,
                    descriptors, descBytes
                );
            }
            *bytesRead = offset;
            return st;
        }

        case SVGA_CMD_UPDATE: {
            if (payloadSize < sizeof(SVGAFifoCmdUpdate)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGAFifoCmdUpdate*>(payload);
            if (dev->contextMgr) dev->contextMgr->endAllRenderPasses();
            if (dev->guestMem) {
                dev->guestMem->notifyDisplayUpdate(pCmd->x, pCmd->y, pCmd->width, pCmd->height);
            }
            *bytesRead = sizeof(SVGAFifoCmdUpdate);
            return SVGA3_VLKN_SUCCESS;
        }

        case SVGA_CMD_FENCE: {
            if (payloadSize < sizeof(uint32_t)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            if (dev->contextMgr) dev->contextMgr->endAllRenderPasses();
            if (dev->backend) {
                Svga3VlknStatus fst = dev->backend->flushCommandBuffer();
                if (fst != SVGA3_VLKN_SUCCESS) return fst;
            }
            *bytesRead = sizeof(uint32_t);
            return SVGA3_VLKN_SUCCESS;
        }

        case SVGA_CMD_ESCAPE: {
            if (payloadSize < sizeof(SVGAFifoCmdEscape)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGAFifoCmdEscape*>(payload);
            size_t totalEscapeBytes = sizeof(SVGAFifoCmdEscape) + ((static_cast<size_t>(pCmd->size) + 3) & ~3);
            if (totalEscapeBytes > payloadSize) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            *bytesRead = totalEscapeBytes;
            return SVGA3_VLKN_SUCCESS;
        }

        case SVGA_CMD_RECT_COPY: {
            if (payloadSize < sizeof(SVGAFifoCmdRectCopy)) {
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *pCmd = reinterpret_cast<const SVGAFifoCmdRectCopy*>(payload);
            if (dev->contextMgr) dev->contextMgr->endAllRenderPasses();
            if (dev->guestMem) {
                dev->guestMem->notifyDisplayUpdate(pCmd->destX, pCmd->destY, pCmd->width, pCmd->height);
            }
            *bytesRead = sizeof(SVGAFifoCmdRectCopy);
            return SVGA3_VLKN_SUCCESS;
        }

        default:
            if (cmd >= SVGA_3D_CMD_BASE && cmd < SVGA_3D_CMD_FUTURE_MAX) {
                /* Skip unrecognized 3D command gracefully when bounded by header */
                *bytesRead = payloadSize;
                return SVGA3_VLKN_SUCCESS;
            }
            /* Unknown command without header - cannot know its length */
            *bytesRead = 0;
            return SVGA3_VLKN_ERROR_UNSUPPORTED_COMMAND;
    }
}

} // namespace svga3_vlkn

extern "C" {

Svga3VlknStatus svga3_vlkn_fifo_execute(Svga3VlknDevice *dev,
                                        const void *commandBuffer,
                                        size_t bufferSizeBytes,
                                        size_t *bytesConsumed)
{
    if (!dev || !commandBuffer || bufferSizeBytes < sizeof(uint32_t)) {
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }

    std::lock_guard<std::mutex> lock(dev->mutex);
    const uint8_t *ptr = reinterpret_cast<const uint8_t*>(commandBuffer);
    size_t remaining = bufferSizeBytes;
    size_t totalConsumed = 0;

    while (remaining >= sizeof(uint32_t)) {
        uint32_t cmd = *reinterpret_cast<const uint32_t*>(ptr);
        ptr += sizeof(uint32_t);
        remaining -= sizeof(uint32_t);
        totalConsumed += sizeof(uint32_t);

        bool hasCmdHeader = false;
        size_t payloadSize = remaining;

        /* Every 3D command carries an SVGA3dCmdHeader size word. The outer
         * FIFO loop sizes the packet from that word, so it has to be
         * consumed here too or the body is read one dword off. */
        if (cmd >= SVGA_3D_CMD_BASE && cmd < SVGA_3D_CMD_FUTURE_MAX) {
            if (remaining < sizeof(SVGA3dCmdHeader)) {
                /* Truncated 3D packet: not even the full header is present. */
                if (bytesConsumed) *bytesConsumed = totalConsumed;
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            const auto *hdr = reinterpret_cast<const SVGA3dCmdHeader*>(ptr);
            if (hdr->size > remaining - sizeof(SVGA3dCmdHeader)) {
                /* Truncated 3D packet: the declared body extends past the
                 * end of the buffer. Reject it instead of parsing the
                 * header bytes as the command body. */
                if (bytesConsumed) *bytesConsumed = totalConsumed;
                return SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER;
            }
            hasCmdHeader = true;
            payloadSize = hdr->size;
            ptr += sizeof(SVGA3dCmdHeader);
            remaining -= sizeof(SVGA3dCmdHeader);
            totalConsumed += sizeof(SVGA3dCmdHeader);
        }

        size_t packetRead = 0;
        Svga3VlknStatus st = svga3_vlkn::processFifoPacket(
            dev, cmd, ptr, payloadSize, &packetRead
        );

        size_t toAdvance = hasCmdHeader ? ((payloadSize + 3) & ~3) : packetRead;
        if (!hasCmdHeader && toAdvance == 0) {
            toAdvance = sizeof(uint32_t);
        }
        /* Clamp to what is left: dword rounding (up to +3 bytes) or the
         * fallback above can otherwise exceed `remaining` — including the
         * case remaining == 0 — and wrap it to SIZE_MAX, sending the loop
         * past the end of the caller's buffer. The loop then exits cleanly. */
        if (toAdvance > remaining) {
            toAdvance = remaining;
        }
        ptr += toAdvance;
        remaining -= toAdvance;
        totalConsumed += toAdvance;
        dev->stats.totalCommandsProcessed++;

        if (st != SVGA3_VLKN_SUCCESS && st != SVGA3_VLKN_ERROR_UNSUPPORTED_COMMAND) {
            if (bytesConsumed) *bytesConsumed = totalConsumed;
            return st;
        }
    }

    if (bytesConsumed) *bytesConsumed = totalConsumed;
    return SVGA3_VLKN_SUCCESS;
}

} // extern "C"
