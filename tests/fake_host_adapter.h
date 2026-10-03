/* Fake host adapter for core tests. No QEMU headers, no QEMU code. */
#ifndef FAKE_HOST_ADAPTER_H
#define FAKE_HOST_ADAPTER_H

#include "svga3_vlkn.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build a Svga3HostAdapter with the given displayUpdate callback.
 * All other hooks are null (core uses registered RAM blocks). */
static inline Svga3HostAdapter fake_host_adapter(
    void (*displayUpdate)(void*, int32_t, int32_t, int32_t, int32_t),
    void *opaque)
{
    Svga3HostAdapter adapter;
    adapter.opaque = opaque;
    adapter.guestRamMap = 0;
    adapter.guestRamRead = 0;
    adapter.guestRamWrite = 0;
    adapter.displayUpdate = displayUpdate;
    return adapter;
}

#ifdef __cplusplus
}
#endif

#endif /* FAKE_HOST_ADAPTER_H */
