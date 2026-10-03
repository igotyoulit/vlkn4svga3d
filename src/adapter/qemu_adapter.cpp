/* QEMU adapter glue. Guest RAM is registered directly with the core;
 * this adapter supplies the device-specific display notification. */
#include "qemu_adapter.h"
#include "qemu_vmsvga.h"
using namespace qemu_vmsvga;
extern "C" Svga3HostAdapter qemu_adapter_for_device(void *qemuDevice) {
    Svga3HostAdapter adapter{};
    adapter.opaque = qemuDevice;
    adapter.displayUpdate = QemuVmsvgaDevice::displayUpdateCallback;
    return adapter;
}
