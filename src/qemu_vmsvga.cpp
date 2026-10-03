/*
 * SVGA3=VLKN - Deliverable 6: QEMU VMware SVGA Device Emulation & Guest Driver
 *
 * Source: qemu_vmsvga.cpp
 *
 * Implements:
 * - Emulated QEMU VMware SVGA PCI hardware device
 * - BAR0 (I/O ports), BAR1 (Display Surface Framebuffer), BAR2 (FIFO memory)
 * - PCI configuration space (Vendor 0x15AD, Device 0x0405)
 * - Extended FIFO ring buffer engine with wraparound handling
 * - Fences and guest synchronization
 * - Guest physical address translation (GPA -> HVA) via production svga3_guest_mem
 * - Minimal Guest Test Driver for end-to-end in-VM execution
 */

#include "qemu_vmsvga.h"
#include "svga3_device.h"
#include "svga3_guest_mem.h"
#include "adapter/qemu_adapter.h"

#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>

namespace qemu_vmsvga {

/* =========================================================================
 * QemuVmsvgaDevice Implementation
 * ========================================================================= */

QemuVmsvgaDevice::QemuVmsvgaDevice(uint64_t fbGpa,
                                   size_t fbSize,
                                   uint64_t fifoGpa,
                                   size_t fifoSize,
                                   uint16_t ioPortBase)
    : m_fbGpa(fbGpa)
    , m_fbSize(fbSize)
    , m_fifoGpa(fifoGpa)
    , m_fifoSize(fifoSize)
    , m_ioPortBase(ioPortBase)
{
    m_fbMem.resize(m_fbSize, 0);
    m_fifoMem.resize(m_fifoSize, 0);

    initPciConfig();
    reset();
}

QemuVmsvgaDevice::~QemuVmsvgaDevice()
{
    if (m_vlknDev) {
        svga3_vlkn_device_destroy(m_vlknDev);
        m_vlknDev = nullptr;
    }
}

void QemuVmsvgaDevice::initPciConfig()
{
    std::memset(&m_pciConfig, 0, sizeof(m_pciConfig));
    m_pciConfig.vendorId = 0x15AD;       /* VMware PCI Vendor ID */
    m_pciConfig.deviceId = 0x0405;       /* VMware SVGA II Device ID */
    m_pciConfig.command = 0x0007;        /* I/O, Memory, Bus Master enabled */
    m_pciConfig.status = 0x0290;         /* Capabilities list, fast back-to-back */
    m_pciConfig.revisionId = 0x00;
    m_pciConfig.progIf = 0x00;
    m_pciConfig.subClass = 0x00;         /* VGA-compatible */
    m_pciConfig.baseClass = 0x03;        /* Display controller */
    m_pciConfig.headerType = 0x00;       /* Standard header */
    m_pciConfig.bar0 = (m_ioPortBase & ~0x3) | 0x1; /* I/O space */
    m_pciConfig.bar1 = static_cast<uint32_t>(m_fbGpa & ~0xF) | 0x0; /* 32-bit Memory space */
    m_pciConfig.bar2 = static_cast<uint32_t>(m_fifoGpa & ~0xF) | 0x0; /* 32-bit Memory space */
    m_pciConfig.subsystemVendorId = 0x15AD;
    m_pciConfig.subsystemId = 0x0405;
    m_pciConfig.interruptPin = 1;        /* INTA# */
}

void QemuVmsvgaDevice::initFifoRegs()
{
    std::memset(m_fifoMem.data(), 0, m_fifoMem.size());

    uint32_t *fifo = reinterpret_cast<uint32_t*>(m_fifoMem.data());
    uint32_t minOffset = 4 * 1024; /* 4KB for FIFO registers (up to index 1024) */
    if (minOffset > m_fifoSize / 2) {
        minOffset = 1024;
    }

    fifo[SVGA_FIFO_MIN] = minOffset;
    fifo[SVGA_FIFO_MAX] = static_cast<uint32_t>(m_fifoSize);
    fifo[SVGA_FIFO_NEXT_CMD] = minOffset;
    fifo[SVGA_FIFO_STOP] = minOffset;
    fifo[SVGA_FIFO_CAPABILITIES] = SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_3D_HWVERSION_REVISED;
    fifo[SVGA_FIFO_FLAGS] = 0;
    fifo[SVGA_FIFO_FENCE] = 0;
    fifo[SVGA_FIFO_3D_HWVERSION] = SVGA3D_HWVERSION_WS6_B1;
    fifo[SVGA_FIFO_3D_HWVERSION_REVISED] = SVGA3D_HWVERSION_WS6_B1;
}

void QemuVmsvgaDevice::displayUpdateCallback(void *opaque, int32_t x, int32_t y, int32_t w, int32_t h)
{
    auto *dev = reinterpret_cast<QemuVmsvgaDevice*>(opaque);
    if (dev) {
        dev->m_displayUpdates.push_back({x, y, w, h});
    }
}

bool QemuVmsvgaDevice::init(bool enableValidation, bool forceMock)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_vlknDev) {
        svga3_vlkn_device_destroy(m_vlknDev);
        m_vlknDev = nullptr;
    }

    Svga3VlknConfig config;
    std::memset(&config, 0, sizeof(config));
    config.enableValidationLayers = enableValidation;
    config.forceMockBackend = forceMock;

    m_vlknDev = svga3_vlkn_device_create(&config);
    if (!m_vlknDev) {
        std::cerr << "[QEMU VMSVGA] Failed to create SVGA3=VLKN device." << std::endl;
        return false;
    }

    /* Configure Framebuffer mapping for Presentation */
    uint32_t w = m_regs[SVGA_REG_WIDTH] ? m_regs[SVGA_REG_WIDTH] : 1024;
    uint32_t h = m_regs[SVGA_REG_HEIGHT] ? m_regs[SVGA_REG_HEIGHT] : 768;
    uint32_t pitch = m_regs[SVGA_REG_BYTES_PER_LINE] ? m_regs[SVGA_REG_BYTES_PER_LINE] : (w * 4);
    uint32_t bpp = m_regs[SVGA_REG_BITS_PER_PIXEL] ? (m_regs[SVGA_REG_BITS_PER_PIXEL] / 8) : 4;

    svga3_vlkn_device_set_framebuffer(m_vlknDev, m_fbMem.data(), m_fbGpa, m_fbSize, w, h, pitch, bpp);
    const auto adapter = qemu_adapter_for_device(this);
    svga3_vlkn_device_set_host_adapter(m_vlknDev, &adapter);
    return true;
}

void QemuVmsvgaDevice::reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::memset(m_regs, 0, sizeof(m_regs));
    m_regs[SVGA_REG_ID] = SVGA_ID_2;
    m_regs[SVGA_REG_MAX_WIDTH] = 2560;
    m_regs[SVGA_REG_MAX_HEIGHT] = 1600;
    m_regs[SVGA_REG_VRAM_SIZE] = static_cast<uint32_t>(m_fbSize);
    m_regs[SVGA_REG_FB_SIZE] = static_cast<uint32_t>(m_fbSize);
    m_regs[SVGA_REG_FB_OFFSET] = 0;
    m_regs[SVGA_REG_MEM_SIZE] = static_cast<uint32_t>(m_fifoSize);
    m_regs[SVGA_REG_MEM_REGS] = SVGA_FIFO_NUM_REGS;

    /*
     * Advertise ONLY capabilities the implementation actually supports:
     * - Rect copy (2D)
     * - 3D acceleration
     * - Extended FIFO
     * - Pitchlock
     * - GMR & GMR2 (Guest Memory Regions)
     */
    m_regs[SVGA_REG_CAPABILITIES] =
        SVGA_CAP_RECT_COPY |
        SVGA_CAP_3D |
        SVGA_CAP_EXTENDED_FIFO |
        SVGA_CAP_PITCHLOCK |
        SVGA_CAP_GMR |
        SVGA_CAP_GMR2;

    m_regs[SVGA_REG_GMR_MAX_IDS] = 256;
    m_regs[SVGA_REG_GMR_MAX_DESCRIPTOR_LENGTH] = 4096;
    m_regs[SVGA_REG_GMRS_MAX_PAGES] = 65536;

    m_indexReg = 0;
    m_gmrId = 0;
    m_displayUpdates.clear();

    initFifoRegs();

    if (m_vlknDev) {
        svga3_vlkn_device_reset(m_vlknDev);
        svga3_vlkn_device_set_framebuffer(m_vlknDev, m_fbMem.data(), m_fbGpa, m_fbSize, 1024, 768, 1024 * 4, 4);
        const auto adapter = qemu_adapter_for_device(this);
    svga3_vlkn_device_set_host_adapter(m_vlknDev, &adapter);
    }
}

uint32_t QemuVmsvgaDevice::pciConfigRead(uint8_t offset, int size)
{
    if (offset + static_cast<size_t>(size) > sizeof(m_pciConfig)) return 0xFFFFFFFF;
    const uint8_t *raw = reinterpret_cast<const uint8_t*>(&m_pciConfig);
    uint32_t val = 0;
    std::memcpy(&val, raw + offset, size);
    return val;
}

void QemuVmsvgaDevice::pciConfigWrite(uint8_t offset, uint32_t val, int size)
{
    if (offset + static_cast<size_t>(size) > sizeof(m_pciConfig)) return;
    uint8_t *raw = reinterpret_cast<uint8_t*>(&m_pciConfig);
    std::memcpy(raw + offset, &val, size);
}

uint32_t QemuVmsvgaDevice::ioRead(uint16_t portOffset, int size)
{
    (void)size;
    std::lock_guard<std::mutex> lock(m_mutex);

    switch (portOffset) {
        case SVGA_INDEX_PORT:
            return m_indexReg;

        case SVGA_VALUE_PORT: {
            if (m_indexReg < SVGA_REG_TOP) {
                return m_regs[m_indexReg];
            }
            return 0xFFFFFFFF;
        }

        case SVGA_BIOS_PORT:
            return 0;

        case SVGA_IRQSTATUS_PORT:
            return 0;

        default:
            return 0xFFFFFFFF;
    }
}

void QemuVmsvgaDevice::ioWrite(uint16_t portOffset, uint32_t val, int size)
{
    (void)size;
    std::lock_guard<std::mutex> lock(m_mutex);

    switch (portOffset) {
        case SVGA_INDEX_PORT:
            m_indexReg = val;
            break;

        case SVGA_VALUE_PORT: {
            if (m_indexReg >= SVGA_REG_TOP) {
                break;
            }

            switch (m_indexReg) {
                case SVGA_REG_ID:
                    /* Guest writes version it wants; device clamps to max supported */
                    if (val == SVGA_ID_2 || val == SVGA_ID_1 || val == SVGA_ID_0) {
                        m_regs[SVGA_REG_ID] = val;
                    } else {
                        m_regs[SVGA_REG_ID] = SVGA_ID_2;
                    }
                    break;

                case SVGA_REG_ENABLE:
                    m_regs[SVGA_REG_ENABLE] = val ? 1 : 0;
                    break;

                case SVGA_REG_WIDTH:
                    m_regs[SVGA_REG_WIDTH] = val;
                    m_regs[SVGA_REG_BYTES_PER_LINE] = val * (m_regs[SVGA_REG_BITS_PER_PIXEL] / 8);
                    break;

                case SVGA_REG_HEIGHT:
                    m_regs[SVGA_REG_HEIGHT] = val;
                    break;

                case SVGA_REG_BITS_PER_PIXEL:
                    m_regs[SVGA_REG_BITS_PER_PIXEL] = val;
                    m_regs[SVGA_REG_BYTES_PER_LINE] = m_regs[SVGA_REG_WIDTH] * (val / 8);
                    break;

                case SVGA_REG_CONFIG_DONE:
                    m_regs[SVGA_REG_CONFIG_DONE] = val;
                    if (m_vlknDev && m_regs[SVGA_REG_WIDTH] && m_regs[SVGA_REG_HEIGHT]) {
                        uint32_t bppBytes = m_regs[SVGA_REG_BITS_PER_PIXEL] ? (m_regs[SVGA_REG_BITS_PER_PIXEL] / 8) : 4;
                        svga3_vlkn_device_set_framebuffer(
                            m_vlknDev,
                            m_fbMem.data(),
                            m_fbGpa,
                            m_fbSize,
                            m_regs[SVGA_REG_WIDTH],
                            m_regs[SVGA_REG_HEIGHT],
                            m_regs[SVGA_REG_BYTES_PER_LINE],
                            bppBytes
                        );
                    }
                    break;

                case SVGA_REG_SYNC:
                    m_regs[SVGA_REG_SYNC] = val;
                    /* Run FIFO synchronously */
                    if (val) {
                        fifoRun();
                        m_regs[SVGA_REG_BUSY] = 0;
                    }
                    break;

                case SVGA_REG_GMR_ID:
                    m_gmrId = val;
                    break;

                case SVGA_REG_GMR_DESCRIPTOR: {
                    /* GMR1 descriptor registration: val is descriptor PPN */
                    if (m_vlknDev && m_vlknDev->guestMem) {
                        m_vlknDev->guestMem->registerLegacyGMR(m_gmrId, val);
                    }
                    break;
                }

                default:
                    m_regs[m_indexReg] = val;
                    break;
            }
            break;
        }

        default:
            break;
    }
}

bool QemuVmsvgaDevice::mapGuestRam(uint64_t gpa, void *hva, size_t size)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vlknDev) return false;
    return svga3_vlkn_device_map_guest_ram(m_vlknDev, gpa, hva, size) == SVGA3_VLKN_SUCCESS;
}

bool QemuVmsvgaDevice::unmapGuestRam(uint64_t gpa)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vlknDev) return false;
    return svga3_vlkn_device_unmap_guest_ram(m_vlknDev, gpa) == SVGA3_VLKN_SUCCESS;
}

void QemuVmsvgaDevice::fifoRun()
{
    if (!m_vlknDev) return;

    m_totalFifoRuns++;

    uint32_t *fifo = reinterpret_cast<uint32_t*>(m_fifoMem.data());
    uint32_t min = fifo[SVGA_FIFO_MIN];
    uint32_t max = fifo[SVGA_FIFO_MAX];
    uint32_t stop = fifo[SVGA_FIFO_STOP];
    uint32_t nextCmd = fifo[SVGA_FIFO_NEXT_CMD];

    if (max > m_fifoSize || min >= max || stop >= max || nextCmd >= max) {
        std::cerr << "[QEMU VMSVGA] Malformed FIFO registers: min=" << min
                  << " max=" << max << " stop=" << stop << " next=" << nextCmd << std::endl;
        return;
    }

    /*
     * FIFO execution loop with wraparound handling:
     * While stop != nextCmd, read packets.
     * When stop reaches max, wrap to min.
     */
    while (stop != nextCmd) {
        if (stop >= max) {
            stop = min;
            m_totalWraparoundsHandled++;
            if (stop == nextCmd) break;
        }

        /* Check if we have at least a command ID (4 bytes) before buffer end */
        if (stop + sizeof(uint32_t) > max) {
            /* Wraparound at the end of the buffer */
            stop = min;
            m_totalWraparoundsHandled++;
            if (stop == nextCmd) break;
        }

        uint32_t cmd = *reinterpret_cast<const uint32_t*>(m_fifoMem.data() + stop);

        /* Special handle for FENCE command */
        if (cmd == SVGA_CMD_FENCE) {
            stop += sizeof(uint32_t);
            if (stop + sizeof(uint32_t) > max) {
                stop = min;
            }
            uint32_t fenceValue = *reinterpret_cast<const uint32_t*>(m_fifoMem.data() + stop);
            stop += sizeof(uint32_t);

            if (m_vlknDev) {
                if (m_vlknDev->contextMgr) m_vlknDev->contextMgr->endAllRenderPasses();
                if (m_vlknDev->backend) m_vlknDev->backend->flushCommandBuffer();
            }

            fifo[SVGA_FIFO_FENCE] = fenceValue;
            m_totalFencesProcessed++;
            if (stop >= max) {
                stop = min;
                m_totalWraparoundsHandled++;
            }
            continue;
        }

        /* Handle INVALID_CMD (used by guest to pad end of buffer before wrap) */
        if (cmd == SVGA_CMD_INVALID_CMD) {
            stop = min;
            m_totalWraparoundsHandled++;
            continue;
        }

        /* Determine exact single packet length */
        size_t packetLength = sizeof(uint32_t);
        if (cmd >= SVGA_3D_CMD_BASE && cmd < SVGA_3D_CMD_FUTURE_MAX) {
            if (stop + sizeof(uint32_t) + sizeof(SVGA3dCmdHeader) <= max) {
                const auto *hdr = reinterpret_cast<const SVGA3dCmdHeader*>(m_fifoMem.data() + stop + sizeof(uint32_t));
                packetLength = sizeof(uint32_t) + sizeof(SVGA3dCmdHeader) + hdr->size;
            }
        } else if (cmd == SVGA_CMD_DEFINE_GMR2) {
            packetLength = sizeof(uint32_t) + sizeof(SVGAFifoCmdDefineGMR2);
        } else if (cmd == SVGA_CMD_REMAP_GMR2) {
            if (stop + sizeof(uint32_t) + sizeof(SVGAFifoCmdRemapGMR2) <= max) {
                const auto *remapCmd = reinterpret_cast<const SVGAFifoCmdRemapGMR2*>(m_fifoMem.data() + stop + sizeof(uint32_t));
                size_t descBytes = 0;
                if (remapCmd->flags & SVGA_REMAP_GMR2_VIA_GMR) {
                    descBytes = sizeof(SVGAGuestPtr);
                } else if (remapCmd->flags & SVGA_REMAP_GMR2_SINGLE_PPN) {
                    descBytes = (remapCmd->flags & SVGA_REMAP_GMR2_PPN64) ? sizeof(uint64_t) : sizeof(uint32_t);
                } else {
                    descBytes = static_cast<size_t>(remapCmd->numPages) *
                        ((remapCmd->flags & SVGA_REMAP_GMR2_PPN64) ? sizeof(uint64_t) : sizeof(uint32_t));
                }
                packetLength = sizeof(uint32_t) + sizeof(SVGAFifoCmdRemapGMR2) + descBytes;
            }
        } else if (cmd == SVGA_CMD_UPDATE || cmd == SVGA_CMD_UPDATE_VERBOSE) {
            packetLength = sizeof(uint32_t) + sizeof(SVGAFifoCmdUpdate);
        } else if (cmd == SVGA_CMD_RECT_COPY) {
            packetLength = sizeof(uint32_t) + sizeof(SVGAFifoCmdRectCopy);
        } else if (cmd == SVGA_CMD_ESCAPE) {
            if (stop + sizeof(uint32_t) + sizeof(SVGAFifoCmdEscape) <= max) {
                const auto *escCmd = reinterpret_cast<const SVGAFifoCmdEscape*>(m_fifoMem.data() + stop + sizeof(uint32_t));
                packetLength = sizeof(uint32_t) + sizeof(SVGAFifoCmdEscape) + ((static_cast<size_t>(escCmd->size) + 3) & ~3);
            }
        }

        /* Ensure we don't read beyond nextCmd or max */
        size_t maxContig = (nextCmd > stop) ? (nextCmd - stop) : (max - stop);
        size_t toExecute = std::min(packetLength, maxContig);

        size_t bytesConsumed = 0;
        Svga3VlknStatus st = svga3_vlkn_fifo_execute(
            m_vlknDev,
            m_fifoMem.data() + stop,
            toExecute,
            &bytesConsumed
        );

        if (st != SVGA3_VLKN_SUCCESS || bytesConsumed == 0) {
            /* Error or malformed packet: advance by at least 4 bytes to avoid infinite loop */
            stop += sizeof(uint32_t);
        } else {
            stop += bytesConsumed;
        }

        if (stop >= max) {
            stop = min;
            m_totalWraparoundsHandled++;
        }
    }

    fifo[SVGA_FIFO_STOP] = stop;
}

/* =========================================================================
 * GuestVmsvgaDriver Implementation
 * ========================================================================= */

GuestVmsvgaDriver::GuestVmsvgaDriver(QemuVmsvgaDevice *device, size_t guestRamSize)
    : m_device(device)
{
    m_guestRam.resize(guestRamSize, 0);

    /* Map guest RAM into device's memory manager */
    m_device->mapGuestRam(m_ramGpaBase, m_guestRam.data(), m_guestRam.size());

    m_fifo = reinterpret_cast<uint32_t*>(m_device->getFifoHva());
    m_fb = reinterpret_cast<uint32_t*>(m_device->getFramebufferHva());
}

GuestVmsvgaDriver::~GuestVmsvgaDriver()
{
    m_device->unmapGuestRam(m_ramGpaBase);
}

void GuestVmsvgaDriver::outl(uint16_t port, uint32_t val)
{
    m_device->ioWrite(port - m_ioBase, val, 4);
}

uint32_t GuestVmsvgaDriver::inl(uint16_t port)
{
    return m_device->ioRead(port - m_ioBase, 4);
}

void GuestVmsvgaDriver::writeReg(uint32_t index, uint32_t val)
{
    outl(m_ioBase + SVGA_INDEX_PORT, index);
    outl(m_ioBase + SVGA_VALUE_PORT, val);
}

uint32_t GuestVmsvgaDriver::readReg(uint32_t index)
{
    outl(m_ioBase + SVGA_INDEX_PORT, index);
    return inl(m_ioBase + SVGA_VALUE_PORT);
}

bool GuestVmsvgaDriver::discoverDevice()
{
    /* Read PCI Config Space */
    uint16_t vendorId = static_cast<uint16_t>(m_device->pciConfigRead(0x00, 2));
    uint16_t deviceId = static_cast<uint16_t>(m_device->pciConfigRead(0x02, 2));
    uint32_t classCode = m_device->pciConfigRead(0x08, 4) >> 8;
    uint32_t bar0 = m_device->pciConfigRead(0x10, 4);
    uint32_t bar1 = m_device->pciConfigRead(0x14, 4);
    uint32_t bar2 = m_device->pciConfigRead(0x18, 4);

    if (vendorId != 0x15AD || deviceId != 0x0405) {
        return false;
    }
    if ((classCode >> 8) != 0x0300) { /* Display class */
        return false;
    }
    if ((bar0 & 0x1) != 0x1) { /* BAR0 must be I/O */
        return false;
    }

    m_ioBase = bar0 & ~0x3;
    (void)bar1;
    (void)bar2;
    return true;
}

bool GuestVmsvgaDriver::initializeDevice(uint32_t width, uint32_t height, uint32_t bpp)
{
    /* 1. Negotiate Version ID (Guest requests ID_2) */
    writeReg(SVGA_REG_ID, SVGA_ID_2);
    m_negotiatedId = readReg(SVGA_REG_ID);
    if (m_negotiatedId != SVGA_ID_2) {
        return false;
    }

    /* 2. Read Advertised Capabilities */
    m_caps = readReg(SVGA_REG_CAPABILITIES);

    /* 3. Configure Display Geometry */
    m_width = width;
    m_height = height;
    m_bpp = bpp;
    writeReg(SVGA_REG_WIDTH, width);
    writeReg(SVGA_REG_HEIGHT, height);
    writeReg(SVGA_REG_BITS_PER_PIXEL, bpp);

    m_pitch = readReg(SVGA_REG_BYTES_PER_LINE);

    /* 4. Enable Device */
    writeReg(SVGA_REG_ENABLE, 1);
    writeReg(SVGA_REG_CONFIG_DONE, 1);

    /* 5. Initialize Extended FIFO */
    uint32_t minOffset = 4 * 1024;
    uint32_t maxOffset = static_cast<uint32_t>(m_device->getFifoSize());
    m_fifo[SVGA_FIFO_MIN] = minOffset;
    m_fifo[SVGA_FIFO_MAX] = maxOffset;
    m_fifo[SVGA_FIFO_NEXT_CMD] = minOffset;
    m_fifo[SVGA_FIFO_STOP] = minOffset;
    m_fifo[SVGA_FIFO_CAPABILITIES] = SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_3D_HWVERSION_REVISED;
    m_fifo[SVGA_FIFO_FLAGS] = 0;
    m_fifo[SVGA_FIFO_FENCE] = 0;
    m_fifo[SVGA_FIFO_3D_HWVERSION] = SVGA3D_HWVERSION_WS6_B1;

    return true;
}

bool GuestVmsvgaDriver::registerGuestMemoryRegion(uint32_t gmrId, uint64_t gpa, size_t size)
{
    /* Use SVGA_CMD_DEFINE_GMR2 and SVGA_CMD_REMAP_GMR2 via FIFO */
    uint32_t numPages = static_cast<uint32_t>((size + 4095) / 4096);

    struct {
        uint32_t cmd;
        SVGAFifoCmdDefineGMR2 defineCmd;
    } definePacket;

    definePacket.cmd = SVGA_CMD_DEFINE_GMR2;
    definePacket.defineCmd.gmrId = gmrId;
    definePacket.defineCmd.numPages = numPages;

    if (!writeFifo(&definePacket, sizeof(definePacket))) {
        return false;
    }

    /* Remap with single PPN flag */
#pragma pack(push, 1)
    struct {
        uint32_t cmd;
        SVGAFifoCmdRemapGMR2 remapCmd;
        uint64_t ppn;
    } remapPacket;
#pragma pack(pop)

    remapPacket.cmd = SVGA_CMD_REMAP_GMR2;
    remapPacket.remapCmd.gmrId = gmrId;
    remapPacket.remapCmd.flags = static_cast<SVGARemapGMR2Flags>(SVGA_REMAP_GMR2_PPN64 | SVGA_REMAP_GMR2_SINGLE_PPN);
    remapPacket.remapCmd.offsetPages = 0;
    remapPacket.remapCmd.numPages = numPages;
    remapPacket.ppn = gpa >> 12;

    if (!writeFifo(&remapPacket, sizeof(remapPacket))) {
        return false;
    }

    return true;
}

bool GuestVmsvgaDriver::writeFifo(const void *data, size_t sizeBytes)
{
    if (sizeBytes == 0) return true;

    /* Align size to 4-byte multiple */
    size_t alignedSize = (sizeBytes + 3) & ~3;

    uint32_t min = m_fifo[SVGA_FIFO_MIN];
    uint32_t max = m_fifo[SVGA_FIFO_MAX];
    uint32_t next = m_fifo[SVGA_FIFO_NEXT_CMD];
    uint8_t *fifoBytes = reinterpret_cast<uint8_t*>(m_fifo);

    /*
     * Check if packet fits before buffer end (max).
     * If not, we trigger wraparound:
     * - Write SVGA_CMD_INVALID_CMD if there is space for padding
     * - Wrap next to min
     */
    if (next + alignedSize > max) {
        if (next + sizeof(uint32_t) <= max) {
            *reinterpret_cast<uint32_t*>(fifoBytes + next) = SVGA_CMD_INVALID_CMD;
        }
        next = min;
        m_wraparounds++;
    }

    /* Copy packet data */
    std::memcpy(fifoBytes + next, data, sizeBytes);
    if (alignedSize > sizeBytes) {
        std::memset(fifoBytes + next + sizeBytes, 0, alignedSize - sizeBytes);
    }

    next += static_cast<uint32_t>(alignedSize);
    if (next >= max) {
        next = min;
        m_wraparounds++;
    }

    m_fifo[SVGA_FIFO_NEXT_CMD] = next;
    return true;
}

bool GuestVmsvgaDriver::sync(uint32_t fenceId, uint32_t timeoutLoops)
{
    /* Insert SVGA_CMD_FENCE command */
    struct {
        uint32_t cmd;
        uint32_t fence;
    } fencePacket;

    fencePacket.cmd = SVGA_CMD_FENCE;
    fencePacket.fence = fenceId;

    if (!writeFifo(&fencePacket, sizeof(fencePacket))) {
        return false;
    }

    /* Kick FIFO via I/O Port */
    writeReg(SVGA_REG_SYNC, 1);

    /* Spin wait until hardware posts fence */
    for (uint32_t i = 0; i < timeoutLoops; ++i) {
        if (m_fifo[SVGA_FIFO_FENCE] >= fenceId) {
            return true;
        }
        std::this_thread::yield();
    }

    return false;
}

uint32_t GuestVmsvgaDriver::readFbPixel(uint32_t x, uint32_t y) const
{
    if (x >= m_width || y >= m_height || !m_fb) return 0;
    uint32_t pitchDwords = m_pitch / 4;
    return m_fb[y * pitchDwords + x];
}

} /* namespace qemu_vmsvga */
