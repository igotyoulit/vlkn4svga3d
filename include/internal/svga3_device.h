/*
 * SVGA3=VLKN - Main Device Engine Implementation
 */

#ifndef ___SVGA3_DEVICE_H___
#define ___SVGA3_DEVICE_H___

#include "svga3_vlkn.h"
#include "vlkn_backend.h"
#include "svga3_surface.h"
#include "svga3_context.h"
#include "svga3_guest_mem.h"
#include <memory>
#include <mutex>

struct Svga3VlknDevice {
    std::unique_ptr<svga3_vlkn::VlknBackend> backend;
    std::unique_ptr<svga3_vlkn::VlknSurfaceManager> surfaceMgr;
    std::unique_ptr<svga3_vlkn::VlknContextManager> contextMgr;
    std::unique_ptr<svga3_vlkn::GuestMemoryManager> guestMem;

    Svga3VlknStats stats;
    std::mutex mutex;

    Svga3VlknDevice() {
        memset(&stats, 0, sizeof(stats));
    }
};

namespace svga3_vlkn {

/* FIFO packet execution worker */
Svga3VlknStatus processFifoPacket(Svga3VlknDevice *dev,
                                  uint32_t cmd,
                                  const uint8_t *payload,
                                  size_t payloadSize,
                                  size_t *bytesRead);

/* Present rendered client window surfaces to framebuffer */
Svga3VlknStatus svga3_vlkn_present_client_surfaces(Svga3VlknDevice *dev, const char *reason);

} // namespace svga3_vlkn

#endif /* ___SVGA3_DEVICE_H___ */
