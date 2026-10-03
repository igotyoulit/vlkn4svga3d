/*
 * SVGA3=VLKN - Deliverable 6: QEMU VMware SVGA Device Emulation & Guest Driver
 *
 * Header: qemu_vmsvga.h
 *
 * Defines:
 * - Emulated QEMU VMware SVGA PCI hardware device (hw/display/vmware_vga)
 * - BAR0 (I/O ports), BAR1 (Display Surface Framebuffer), BAR2 (FIFO memory)
 * - PCI configuration space (Vendor 0x15AD, Device 0x0405)
 * - Extended FIFO ring buffer engine with wraparound handling
 * - Fences and guest synchronization
 * - Guest memory translation (GPA -> HVA)
 * - Production SVGA3=VLKN Vulkan backend integration
 * - Minimal Guest Test Driver simulating the in-VM operating system
 */

#ifndef ___QEMU_VMSVGA_H___
#define ___QEMU_VMSVGA_H___

#include "svga3_vlkn.h"
#include "vmsvga/svga_reg.h"
#include "vmsvga/svga3d_reg.h"

#include <vector>
#include <memory>
#include <mutex>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <iostream>

namespace qemu_vmsvga {

/*
 * PCI Configuration Space (Header Type 00h)
 */
struct PciConfigSpace {
    uint16_t vendorId;       /* 0x00: 0x15AD (VMware) */
    uint16_t deviceId;       /* 0x02: 0x0405 (SVGA II) */
    uint16_t command;        /* 0x04: I/O & Memory Enable */
    uint16_t status;         /* 0x06: Device Status */
    uint8_t  revisionId;     /* 0x08: 0x00 */
    uint8_t  progIf;         /* 0x09: 0x00 */
    uint8_t  subClass;       /* 0x0A: 0x00 (VGA Compatible) */
    uint8_t  baseClass;      /* 0x0B: 0x03 (Display Controller) */
    uint8_t  cacheLineSize;  /* 0x0C */
    uint8_t  latencyTimer;   /* 0x0D */
    uint8_t  headerType;     /* 0x0E: 0x00 (Normal device) */
    uint8_t  bist;           /* 0x0F */
    uint32_t bar0;           /* 0x10: I/O Space Base */
    uint32_t bar1;           /* 0x14: Framebuffer GPA Base */
    uint32_t bar2;           /* 0x18: FIFO GPA Base */
    uint32_t bar3;           /* 0x1C: Reserved */
    uint32_t bar4;           /* 0x20: Reserved */
    uint32_t bar5;           /* 0x24: Reserved */
    uint32_t cardbusCis;     /* 0x28 */
    uint16_t subsystemVendorId; /* 0x2C: 0x15AD */
    uint16_t subsystemId;       /* 0x2E: 0x0405 */
    uint32_t expansionRomBar;   /* 0x30 */
    uint8_t  capabilitiesPtr;   /* 0x34 */
    uint8_t  reserved[7];
    uint8_t  interruptLine;  /* 0x3C */
    uint8_t  interruptPin;   /* 0x3D: 1 (INTA#) */
    uint8_t  minGnt;         /* 0x3E */
    uint8_t  maxLat;         /* 0x3F */
};

/*
 * QEMU VMware SVGA Device Emulation Model
 */
class QemuVmsvgaDevice {
public:
    /* Adapter hook: record a display update rect. Public so the QEMU
     * host adapter can forward core notifications. */
    static void displayUpdateCallback(void *opaque, int32_t x, int32_t y, int32_t w, int32_t h);

    QemuVmsvgaDevice(uint64_t fbGpa = 0xE0000000ULL,
                     size_t fbSize = 16 * 1024 * 1024,
                     uint64_t fifoGpa = 0xF0000000ULL,
                     size_t fifoSize = 256 * 1024,
                     uint16_t ioPortBase = 0x1000);
    ~QemuVmsvgaDevice();

    /* Initialize hardware emulation with real Vulkan backend */
    bool init(bool enableValidation = true, bool forceMock = false);

    /* Hardware Reset / Power Cycle (Deliverable 6: Fresh VM Start verification) */
    void reset();

    /* PCI Bus Access */
    uint32_t pciConfigRead(uint8_t offset, int size);
    void     pciConfigWrite(uint8_t offset, uint32_t val, int size);

    /* I/O Port Dispatch (BAR0) */
    uint32_t ioRead(uint16_t portOffset, int size);
    void     ioWrite(uint16_t portOffset, uint32_t val, int size);

    /* Memory Mappings (BAR1 & BAR2) */
    void*    getFramebufferHva() { return m_fbMem.data(); }
    uint64_t getFramebufferGpa() const { return m_fbGpa; }
    size_t   getFramebufferSize() const { return m_fbSize; }

    void*    getFifoHva() { return m_fifoMem.data(); }
    uint64_t getFifoGpa() const { return m_fifoGpa; }
    size_t   getFifoSize() const { return m_fifoSize; }

    /* Guest Physical Memory Mapping (RAM translation) */
    bool mapGuestRam(uint64_t gpa, void *hva, size_t size);
    bool unmapGuestRam(uint64_t gpa);

    /* FIFO Ring Buffer Processor (processes commands and handles wraparound) */
    void fifoRun();

    /* Underlying Production SVGA3=VLKN device */
    Svga3VlknDevice* vlknDevice() { return m_vlknDev; }

    /* Display update tracking */
    struct DisplayUpdateRect {
        int32_t x, y, w, h;
    };
    const std::vector<DisplayUpdateRect>& displayUpdates() const { return m_displayUpdates; }
    void clearDisplayUpdates() { m_displayUpdates.clear(); }

    /* Statistics */
    uint64_t totalFifoRuns() const { return m_totalFifoRuns; }
    uint64_t totalFencesProcessed() const { return m_totalFencesProcessed; }
    uint64_t totalWraparoundsHandled() const { return m_totalWraparoundsHandled; }

private:
    void initPciConfig();
    void initFifoRegs();

    uint64_t m_fbGpa;
    size_t   m_fbSize;
    uint64_t m_fifoGpa;
    size_t   m_fifoSize;
    uint16_t m_ioPortBase;

    PciConfigSpace m_pciConfig;
    std::vector<uint8_t> m_fbMem;
    std::vector<uint8_t> m_fifoMem;

    /* SVGA Registers */
    uint32_t m_indexReg = 0;
    uint32_t m_regs[SVGA_REG_TOP] = { 0 };

    /* Vulkan Renderer */
    Svga3VlknDevice *m_vlknDev = nullptr;
    std::vector<DisplayUpdateRect> m_displayUpdates;

    /* GMR Descriptors */
    uint32_t m_gmrId = 0;

    /* Counters */
    uint64_t m_totalFifoRuns = 0;
    uint64_t m_totalFencesProcessed = 0;
    uint64_t m_totalWraparoundsHandled = 0;
    std::mutex m_mutex;
};

/*
 * Minimal Guest Test Driver
 *
 * Implements the in-VM operating system side that drives the hardware
 * strictly through PCI discovery, I/O ports, BAR1, BAR2, and Guest RAM.
 * NEVER calls host-side rendering functions directly.
 */
class GuestVmsvgaDriver {
public:
    GuestVmsvgaDriver(QemuVmsvgaDevice *device, size_t guestRamSize = 16 * 1024 * 1024);
    ~GuestVmsvgaDriver();

    /* 1. Discover device via PCI configuration space */
    bool discoverDevice();

    /* 2. Read advertised capabilities and negotiate version */
    bool initializeDevice(uint32_t width, uint32_t height, uint32_t bpp);

    /* 3. Register a Guest Physical Address memory region with the device */
    bool registerGuestMemoryRegion(uint32_t gmrId, uint64_t gpa, size_t size);

    /* 4. Write command packet into FIFO ring buffer (handles wraparound) */
    bool writeFifo(const void *data, size_t sizeBytes);

    /* 5. Synchronize and wait for fence completion */
    bool sync(uint32_t fenceId, uint32_t timeoutLoops = 1000000);

    /* Direct guest memory access */
    uint8_t* guestRam() { return m_guestRam.data(); }
    uint64_t guestRamBaseGpa() const { return m_ramGpaBase; }
    size_t   guestRamSize() const { return m_guestRam.size(); }

    /* Read pixel directly from BAR1 Framebuffer (as guest OS would) */
    uint32_t readFbPixel(uint32_t x, uint32_t y) const;

    /* Advertised capabilities read by guest */
    uint32_t advertisedCapabilities() const { return m_caps; }
    uint32_t negotiatedId() const { return m_negotiatedId; }

    /* Wraparound counter on guest side */
    uint32_t guestWraparounds() const { return m_wraparounds; }

private:
    void outl(uint16_t port, uint32_t val);
    uint32_t inl(uint16_t port);
    void writeReg(uint32_t index, uint32_t val);
    uint32_t readReg(uint32_t index);

    QemuVmsvgaDevice *m_device;
    std::vector<uint8_t> m_guestRam;
    uint64_t m_ramGpaBase = 0x10000000ULL; /* Guest RAM GPA: 256MB mark */

    uint16_t m_ioBase = 0;
    uint32_t *m_fifo = nullptr;
    uint32_t *m_fb = nullptr;
    uint32_t m_caps = 0;
    uint32_t m_negotiatedId = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_bpp = 32;
    uint32_t m_pitch = 0;
    uint32_t m_wraparounds = 0;
};

} /* namespace qemu_vmsvga */

#endif /* ___QEMU_VMSVGA_H___ */
