/* QEMU host adapter implementation.
 *
 * Implements Svga3HostAdapter for QEMU. The opaque pointer is the
 * QemuVmsvgaDevice instance. QEMU-specific details (preload, binary
 * patch, /proc guest-RAM discovery) live in qemu_svga3d_preload.cpp
 * and qemu_vmsvga.cpp; this file is only the interface glue.
 */

#include "qemu_adapter.h"
#include "qemu_vmsvga.h"

#include <cstring>

using namespace qemu_vmsvga;

static void* qemuGuestRamMap(void *opaque, uint64_t gpa, size_t size, bool isWrite) {
    (void)opaque; (void)gpa; (void)size; (void)isWrite;
    /* QEMU registers RAM blocks via svga3_vlkn_device_map_guest_ram;
     * the core checks those before calling the adapter. */
    return nullptr;
}

static bool qemuGuestRamRead(void *opaque, uint64_t gpa, void *dst, size_t size) {
    void *hva = qemuGuestRamMap(opaque, gpa, size, false);
    if (!hva) return false;
    memcpy(dst, hva, size);
    return true;
}

static bool qemuGuestRamWrite(void *opaque, uint64_t gpa, const void *src, size_t size) {
    void *hva = qemuGuestRamMap(opaque, gpa, size, true);
    if (!hva) return false;
    memcpy(hva, src, size);
    return true;
}

static Svga3VlknStatus qemuPresent(void *opaque, uint32_t sid) {
    auto *dev = reinterpret_cast<QemuVmsvgaDevice*>(opaque);
    if (!dev) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    /* Presentation goes through the core surface manager; the adapter
     * hook exists so a future host can override it. */
    (void)sid;
    return SVGA3_VLKN_SUCCESS;
}

static void qemuDisplayUpdate(void *opaque, int32_t x, int32_t y, int32_t w, int32_t h) {
    auto *dev = reinterpret_cast<QemuVmsvgaDevice*>(opaque);
    if (dev) {
        QemuVmsvgaDevice::displayUpdateCallback(opaque, x, y, w, h);
    }
}

static Svga3VlknStatus qemuFenceSync(void *opaque, uint32_t fenceId) {
    auto *dev = reinterpret_cast<QemuVmsvgaDevice*>(opaque);
    if (!dev) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    /* Fence sync is handled by the core FIFO dispatch; the adapter
     * hook exists so a future host can override it. */
    (void)fenceId;
    return SVGA3_VLKN_SUCCESS;
}

static Svga3HostAdapter g_qemuAdapter = {
    nullptr, /* opaque: set per-device by qemu_adapter_for_device */
    qemuGuestRamMap,
    qemuGuestRamRead,
    qemuGuestRamWrite,
    qemuPresent,
    qemuDisplayUpdate,
    qemuFenceSync,
};

extern "C" {

const Svga3HostAdapter *qemu_adapter_for_device(void *qemuDevice) {
    g_qemuAdapter.opaque = qemuDevice;
    return &g_qemuAdapter;
}

void qemu_adapter_shutdown(void) {
    g_qemuAdapter.opaque = nullptr;
}

} // extern "C"
