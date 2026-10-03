/* QEMU host adapter.
 *
 * Implements Svga3HostAdapter for QEMU. This is the first adapter; the
 * core rendering library never includes this header.
 *
 * QEMU-specific details (preload, binary patch, /proc guest-RAM discovery)
 * live in qemu_svga3d_preload.cpp and qemu_vmsvga.cpp, not in the core.
 */

#ifndef QEMU_ADAPTER_H
#define QEMU_ADAPTER_H

#include "svga3_vlkn.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build a Svga3HostAdapter for the QEMU device. The opaque pointer is the
 * QemuVmsvgaDevice instance. */
Svga3HostAdapter qemu_adapter_for_device(void *qemuDevice);


#ifdef __cplusplus
}
#endif

#endif /* QEMU_ADAPTER_H */
