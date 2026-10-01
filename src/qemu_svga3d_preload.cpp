#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

#include <vector>
#include <mutex>
#include <algorithm>

#include "svga3_vlkn.h"
#include "svga3_device.h"
#include "svga3_guest_mem.h"
#include "svga3d_tables.h"

/* QEMU VMware SVGA Offsets */
#define OFFSET_INDEX            0x10a2c
#define OFFSET_ENABLE           0x10a14
#define OFFSET_CONFIG           0x10a18
#define OFFSET_SYNCING          0x10a54
#define OFFSET_FIFO_PTR         0x10b70
#define OFFSET_FIFO_SIZE        0x10b78
#define OFFSET_FIFO             0x10b80
#define OFFSET_FIFO_MIN         0x10b88
#define OFFSET_FIFO_MAX         0x10b8c
#define OFFSET_FIFO_NEXT        0x10b90
#define OFFSET_FIFO_STOP        0x10b94
#define OFFSET_REDRAW_FIFO      0x10b98
#define OFFSET_REDRAW_FIFO_LAST 0x12b98

/* QEMU 10.1.2 Binary Patch Points */
#define ADDR_PCIVMSVGA_REALIZE_SIZE1 0x470716
#define ADDR_PCIVMSVGA_REALIZE_SIZE2 0x470735
#define ADDR_VMSVGA_UPDATE_RECT_FLUSH 0x471100
#define ADDR_VMSVGA_FIFO_RUN          0x4712f0
#define ADDR_VMSVGA_IO_OPS            0x1965840
#define ADDR_VMSVGA_IO_READ           0x470860
#define ADDR_VMSVGA_IO_WRITE          0x472640
#define ADDR_VNC_POINTER              0x3c2868
#define ADDR_VNC_CONT                 0x3c2350
#define ADDR_QEMU_INPUT_UPDATE_BUTTONS 0x3b4510
#define ADDR_QEMU_INPUT_QUEUE_REL     0x3b4670
#define ADDR_QEMU_INPUT_QUEUE_ABS     0x3b4700
#define ADDR_QEMU_INPUT_EVENT_SYNC    0x3b3f20
#define ADDR_VNC_BUTTON_MAP           0x1b4a3a0

#define VS_VD                         0x151a0
#define VS_LAST_X                     0x151bc
#define VS_LAST_Y                     0x151c0
#define VS_LAST_BMASK                 0x151c4
#define VD_CONSOLE                    0x58

#define INPUT_AXIS_X                  0
#define INPUT_AXIS_Y                  1
#define VNC_LEFT_BUTTON               1
#define REL_STEP                      40
#define TAP_DWELL_NS                  150000000LL
#define SCREEN_W                      1280
#define SCREEN_H                      800

/* SVGA Capabilities Advertised to Guest (0x00d2c0e3):
 * Bit 0: RECT_COPY (0x01)
 * Bit 1: CURSOR (0x02)
 * Bit 5: CURSOR (legacy 0x20)
 * Bit 6: SYNCHRONIZATION (0x40)
 * Bit 7: GLYPH (0x80)
 * Bit 14: 3D (0x4000)
 * Bit 15: EXTENDED_FIFO (0x8000)
 * Bit 17: PITCHLOCK (0x20000)
 * Bit 18: IRQMASK cleared (0x0) -> forces synchronous polling
 * Bit 20: GMR (0x100000)
 * Bit 22: GMR2 (0x400000)
 * Bit 23: SCREEN_OBJECT_2 (0x800000)
 */
#ifndef SVGA_CAP_PITCHLOCK
#define SVGA_CAP_PITCHLOCK            0x00020000
#endif
#ifndef SVGA_CAP_SCREEN_OBJECT_2
#define SVGA_CAP_SCREEN_OBJECT_2      0x00800000
#endif
#ifndef SVGA_FIFO_CAP_SCREEN_OBJECT
#define SVGA_FIFO_CAP_SCREEN_OBJECT     (1 << 7)
#endif
#ifndef SVGA_FIFO_CAP_SCREEN_OBJECT_2
#define SVGA_FIFO_CAP_SCREEN_OBJECT_2   (1 << 9)
#endif
#define SVGA_CAPABILITIES_VALUE       (0x0050c0e3 | SVGA_CAP_PITCHLOCK | SVGA_CAP_SCREEN_OBJECT_2)
#define EXPANDED_FIFO_SIZE            0x400000
/* vmwgfx (GMR2 path) sets legacy surface memory to
 * SVGA_REG_MEMORY_SIZE - SVGA_REG_VRAM_SIZE. Equal values leave a 0 kB
 * surface pool and every surface validate fails. */
#define LEGACY_SURFACE_BYTES          (256u * 1024u * 1024u)

#ifndef SVGA_FIFO_CAP_FENCE
#define SVGA_FIFO_CAP_FENCE           (1 << 0)
#endif
#ifndef SVGA_FIFO_CAP_PITCHLOCK
#define SVGA_FIFO_CAP_PITCHLOCK       (1 << 2)
#endif
#ifndef SVGA_FIFO_CAP_GMR2
#define SVGA_FIFO_CAP_GMR2            (1 << 8)
#endif
#ifndef SVGA_FIFO_CAP_3D_HWVERSION_REVISED
#define SVGA_FIFO_CAP_3D_HWVERSION_REVISED SVGA_FIFO_CAP_GMR2
#endif
#ifndef SVGA_CMD_RECT_FILL
#define SVGA_CMD_RECT_FILL            2
#endif
#define SVGA_FIFO_NEXT                SVGA_FIFO_NEXT_CMD
#define SVGA3D_HWVERSION_WS65_B1      0x00020001
#define SVGA_FIFO_BUSY                290

struct vmsvga_rect_s {
    int x, y, w, h;
};

/* Function Pointers for QEMU Originals */
static void (*orig_update_rect_flush)(void *s) = NULL;
static uint64_t (*orig_io_read)(void *opaque, uint64_t addr, unsigned size) = NULL;
static void (*orig_io_write)(void *opaque, uint64_t addr, uint64_t data, unsigned size) = NULL;
static void (*orig_input_update_buttons)(void *con, uint32_t *map, uint32_t old, uint32_t newm) = NULL;
static void (*orig_input_queue_rel)(void *con, int axis, int value) = NULL;
static void (*orig_input_queue_abs)(void *con, int axis, int value, int min, int max) = NULL;
static void (*orig_input_event_sync)(void) = NULL;

/* Input & Pointer State */
static bool pending_press;
static void *pending_con;
static uint32_t *pending_map;
static uint32_t pending_old, pending_new;
static int64_t left_down_ns;

/* SVGA Registers State */
static uint32_t saved_gmr_id = 0;
static uint32_t saved_gmr_desc = 0;
static uint32_t saved_irqmask = 0;
static uint32_t saved_num_guest_displays = 1;
static FILE *log_file = NULL;

/* Production SVGA3=VLKN Vulkan Device State */
static Svga3VlknDevice *g_vlknDev = nullptr;
static void *g_vmsvga_state = nullptr;
static void *g_guest_ram_base = nullptr;
static size_t g_guest_ram_size = 0;
static bool g_vlkn_initialized = false;
static std::mutex g_vlkn_mutex;
static bool portrait_profile_enabled() {
    const char *profile = getenv("SVGA3_VLKN_GUEST_PROFILE");
    return profile && strcmp(profile, "playbook-portrait") == 0;
}

static SVGAFifoCmdDefineGMRFB g_display_gmrfb = {};
static bool g_display_gmrfb_defined = false;

extern "C" void log_msg(const char *fmt, ...) {
    if (!log_file) {
        /* O_NOFOLLOW: /tmp is world-writable; a symlinked log path would
         * otherwise let an attacker redirect our log writes. Skip file
         * logging entirely if the path cannot be opened safely. */
        int fd = open("/tmp/svga3d.log", O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
        if (fd >= 0) {
            log_file = fdopen(fd, "a");
            if (!log_file) {
                close(fd);
            }
        }
        if (log_file) setlinebuf(log_file);
    }
    if (log_file) {
        va_list args;
        va_start(args, fmt);
        vfprintf(log_file, fmt, args);
        va_end(args);
    }
}

static uint32_t reg_value(void *s, int index) {
    int *reg = (int *)((char *)s + OFFSET_INDEX);
    int saved = *reg;
    *reg = index;
    uint32_t value = orig_io_read(s, 1, 4);
    *reg = saved;
    return value;
}

static void set_qemu_reg_value(void *s, int index, uint32_t value) {
    int *reg = (int *)((char *)s + OFFSET_INDEX);
    int saved = *reg;
    *reg = index;
    /* Call QEMU's original port handler so its legacy VGA scanout is resized. */
    orig_io_write(s, 1, value, 4);
    *reg = saved;
}

static void redraw(void *s, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    int *last = (int *)((char *)s + OFFSET_REDRAW_FIFO_LAST);
    struct vmsvga_rect_s *rects = (struct vmsvga_rect_s *)((char *)s + OFFSET_REDRAW_FIFO);
    if (*last < 0 || *last > 512) return;
    if (*last == 512 && orig_update_rect_flush) orig_update_rect_flush(s);
    if (*last < 512) rects[(*last)++] = (struct vmsvga_rect_s){(int)x, (int)y, (int)w, (int)h};
}

static bool draw_rect(void *s, bool copy, uint32_t color,
                      uint32_t sx, uint32_t sy, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h) {
    uint32_t width = reg_value(s, SVGA_REG_WIDTH), height = reg_value(s, SVGA_REG_HEIGHT);
    uint32_t bpp = reg_value(s, SVGA_REG_BITS_PER_PIXEL);
    uint32_t stride = reg_value(s, SVGA_REG_BYTES_PER_LINE);
    uint32_t size = reg_value(s, SVGA_REG_VRAM_SIZE);
    uint8_t *vram = *(uint8_t **)((char *)s + 8);
    if (!vram || !width || !height || bpp == 0 || bpp > 32 || bpp % 8) return false;
    if (w == 0 || h == 0) return true;
    uint32_t bytes = bpp / 8;
    if ((uint64_t)width * bytes > stride || (uint64_t)height * stride > size) return false;
    if (x >= width || y >= height || w > width - x || h > height - y) return false;
    if (copy && (sx >= width || sy >= height || w > width - sx || h > height - sy)) return false;
    for (uint32_t row = 0; row < h; row++) {
        uint32_t r = copy && y > sy ? h - 1 - row : row;
        uint8_t *dst = vram + (uint64_t)(y + r) * stride + (uint64_t)x * bytes;
        if (copy) memmove(dst, vram + (uint64_t)(sy + r) * stride + (uint64_t)sx * bytes, (size_t)w * bytes);
        else for (uint32_t col = 0; col < w; col++) memcpy(dst + (size_t)col * bytes, &color, bytes);
    }
    redraw(s, x, y, w, h);
    return true;
}

/* vmwgfx renders the desktop into a guest framebuffer and then submits a
 * BLIT_GMRFB_TO_SCREEN command. QEMU's normal SVGA handler performs this DMA
 * copy into the legacy scanout surface; this preload consumes the command,
 * so reproduce the primary-screen 32-bit path here. */
static bool blit_gmrfb_to_legacy(void *s,
                                 int32_t src_x, int32_t src_y,
                                 int32_t left, int32_t top,
                                 int32_t right, int32_t bottom,
                                 uint32_t screen_id) {
    if (!s || (screen_id != 0 && screen_id != SVGA_ID_INVALID) || !g_display_gmrfb_defined ||
        g_display_gmrfb.ptr.gmrId != SVGA_GMR_FRAMEBUFFER ||
        g_display_gmrfb.format.s.bitsPerPixel != 32 ||
        g_display_gmrfb.format.s.colorDepth != 24 ||
        right <= left || bottom <= top || src_x < 0 || src_y < 0) {
        return false;
    }

    uint32_t width = reg_value(s, SVGA_REG_WIDTH);
    uint32_t height = reg_value(s, SVGA_REG_HEIGHT);
    uint32_t dst_pitch = reg_value(s, SVGA_REG_BYTES_PER_LINE);
    uint32_t vram_size = reg_value(s, SVGA_REG_VRAM_SIZE);
    uint8_t *vram = *(uint8_t **)((char *)s + 8);
    const uint32_t src_pitch = g_display_gmrfb.bytesPerLine;
    const uint64_t src_base = g_display_gmrfb.ptr.offset;
    if (!vram || !width || !height || !dst_pitch || !vram_size ||
        !src_pitch || (uint64_t)width * 4 > dst_pitch ||
        (uint64_t)height * dst_pitch > vram_size ||
        (uint64_t)(uint32_t)src_x * 4 >= src_pitch) {
        return false;
    }

    int64_t x0 = std::max<int64_t>(left, 0);
    int64_t y0 = std::max<int64_t>(top, 0);
    int64_t x1 = std::min<int64_t>(right, width);
    int64_t y1 = std::min<int64_t>(bottom, height);
    if (x0 >= x1 || y0 >= y1) return true;

    /* Clipping the destination's left/top also advances the source origin. */
    int64_t sx = (int64_t)src_x + (x0 - left);
    int64_t sy = (int64_t)src_y + (y0 - top);
    uint64_t row_bytes = (uint64_t)(x1 - x0) * 4;
    if (sx < 0 || sy < 0 || (uint64_t)sx * 4 + row_bytes > src_pitch) return false;

    for (int64_t row = 0; row < y1 - y0; ++row) {
        uint64_t src_offset = src_base + (uint64_t)(sy + row) * src_pitch + (uint64_t)sx * 4;
        uint64_t dst_offset = (uint64_t)(y0 + row) * dst_pitch + (uint64_t)x0 * 4;
        if (src_offset > vram_size || row_bytes > vram_size - src_offset ||
            dst_offset > vram_size || row_bytes > vram_size - dst_offset) {
            return false;
        }
        memmove(vram + dst_offset, vram + src_offset, (size_t)row_bytes);
    }
    return true;
}

static void vlkn_display_update_cb(void *opaque, int32_t x, int32_t y, int32_t w, int32_t h) {
    void *s = opaque;
    if (s && w > 0 && h > 0) {
        redraw(s, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
    }
}

static void find_guest_ram(void *vram_hva) {
    if (g_guest_ram_base) return;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        uintptr_t start = 0, end = 0;
        char perms[8] = {0};
        int n = sscanf(line, "%lx-%lx %7s", &start, &end, perms);
        if (n >= 3 && strstr(perms, "rw")) {
            size_t sz = end - start;
            if (sz >= 256 * 1024 * 1024 && (void*)start != vram_hva) {
                char *path = strchr(line, '/');
                if (!path || strstr(path, "memfd") || strstr(path, "zero")) {
                    g_guest_ram_base = (void*)start;
                    g_guest_ram_size = sz;
                    log_msg("[libqemu_svga3d] Found guest physical RAM: %p - %p (%zu MB)\n",
                            (void*)start, (void*)end, sz / (1024 * 1024));
                    break;
                }
            }
        }
    }
    fclose(f);
}

static void ensure_vlkn_device(void *s) {
    std::lock_guard<std::mutex> lock(g_vlkn_mutex);
    if (g_vlkn_initialized) return;
    g_vmsvga_state = s;

    uint8_t *vram = *(uint8_t **)((char *)s + 8);
    find_guest_ram(vram);

    Svga3VlknConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.appName = "PlayBook SVGA3D Vulkan";
    cfg.apiVersion = VK_API_VERSION_1_0;
    cfg.stagingBufferSize = 64 * 1024 * 1024;
    cfg.forceMockBackend = false;
    const char *validate = getenv("SVGA3_VLKN_VALIDATE");
    cfg.enableValidationLayers = validate && strcmp(validate, "1") == 0;

    g_vlknDev = svga3_vlkn_device_create(&cfg);
    if (!g_vlknDev) {
        log_msg("[libqemu_svga3d] WARNING: svga3_vlkn_device_create failed; fallback to CPU!\n");
        return;
    }

    if (g_guest_ram_base && g_guest_ram_size > 0) {
        svga3_vlkn_device_map_guest_ram(g_vlknDev, 0, g_guest_ram_base, g_guest_ram_size);
        log_msg("[libqemu_svga3d] Mapped Guest RAM GPA 0..%zu MB -> %p\n",
                g_guest_ram_size / (1024*1024), g_guest_ram_base);
    }

    uint32_t width = reg_value(s, SVGA_REG_WIDTH);
    if (!width || (portrait_profile_enabled() && width == 768)) width = SCREEN_W;
    uint32_t height = reg_value(s, SVGA_REG_HEIGHT);
    if (!height || (portrait_profile_enabled() && height == 1280)) height = SCREEN_H;
    uint32_t pitch = reg_value(s, SVGA_REG_BYTES_PER_LINE);
    if (!pitch) pitch = width * 4;
    uint32_t bpp = reg_value(s, SVGA_REG_BITS_PER_PIXEL);
    if (!bpp) bpp = 32;
    uint32_t vram_size = reg_value(s, SVGA_REG_VRAM_SIZE);
    if (!vram_size) vram_size = 128 * 1024 * 1024;

    log_msg("[libqemu_svga3d] Framebuffer init: vram=%p w=%u h=%u pitch=%u bpp=%u vram_size=%u\n",
            vram, width, height, pitch, bpp, vram_size);

    svga3_vlkn_device_set_framebuffer(g_vlknDev, vram, reg_value(s, SVGA_REG_FB_START), vram_size, width, height, pitch, bpp / 8);
    svga3_vlkn_device_set_display_callback(g_vlknDev, s, vlkn_display_update_cb);

    g_vlkn_initialized = true;
    log_msg("[libqemu_svga3d] SVGA3=VLKN Vulkan hardware 3D engine initialized successfully!\n");
}

static uint32_t advertised_devcap(const DevCapInfo &cap) {
    switch (cap.id) {
    case SVGA3D_DEVCAP_SURFACEFMT_X8R8G8B8:
    case SVGA3D_DEVCAP_SURFACEFMT_R5G6B5:
    case SVGA3D_DEVCAP_SURFACEFMT_X1R5G5B5:
        /* Mesa and vmwgfx only treat a format as a scanout when DISPLAYMODE
         * is set. 3DACCELERATION implies that bit on real SVGA3D devices. */
        return cap.expectedValue
            | SVGA3DFORMAT_OP_DISPLAYMODE
            | SVGA3DFORMAT_OP_3DACCELERATION
            | SVGA3DFORMAT_OP_MEMBEROFGROUP_ARGB
            | SVGA3DFORMAT_OP_CONVERT_TO_ARGB;
    case SVGA3D_DEVCAP_SURFACEFMT_Z_D16:
    case SVGA3D_DEVCAP_SURFACEFMT_Z_D24S8:
    case SVGA3D_DEVCAP_SURFACEFMT_Z_D24X8:
    case SVGA3D_DEVCAP_SURFACEFMT_Z_DF16:
    case SVGA3D_DEVCAP_SURFACEFMT_Z_DF24:
    case SVGA3D_DEVCAP_SURFACEFMT_Z_D24S8_INT:
        /* Mesa requires SVGA3DFORMAT_OP_ZSTENCIL before it exposes a
         * depth visual. 0x10 (SAME_FORMAT_RENDERTARGET) is not enough. */
        return SVGA3DFORMAT_OP_ZSTENCIL
            | SVGA3DFORMAT_OP_ZSTENCIL_WITH_ARBITRARY_COLOR_DEPTH
            | SVGA3DFORMAT_OP_TEXTURE;
    default:
        return cap.expectedValue;
    }
}

static void init_devcaps_record(uint32_t *fifo) {
    /* Header at &fifo[32]: length = 172 dwords, type = 0x100 (SVGA3DCAPS_RECORD_DEVCAPS) */
    fifo[32] = 172;
    fifo[33] = 0x100;

    size_t capCount = sizeof(g_DevCaps) / sizeof(g_DevCaps[0]);
    for (size_t i = 0; i < capCount; ++i) {
        fifo[34 + 2 * i] = g_DevCaps[i].id;
        fifo[34 + 2 * i + 1] = advertised_devcap(g_DevCaps[i]);
    }
    /* 85th dummy sentinel entry at index 84 (words 202, 203) */
    fifo[34 + 2 * capCount] = 0xFFFFFFFF;
    fifo[34 + 2 * capCount + 1] = 0;

    /* Terminate record chain at word 32 + 172 = 204 */
    fifo[32 + 172] = 0;
}

static uint32_t peek_word(uint32_t *fifo, uint32_t stop, uint32_t min, uint32_t max, uint32_t i) {
    return fifo[(min + ((uint64_t)stop-min+(uint64_t)i*4) % (max-min)) / 4];
}

extern "C" void my_vmsvga_fifo_run(void *s) {
    if (!s || !*(int *)((char *)s+OFFSET_CONFIG) || !*(int *)((char *)s+OFFSET_ENABLE)) return;
    uint32_t *fifo = *(uint32_t **)((char *)s+OFFSET_FIFO);
    uint32_t allocated = *(uint32_t *)((char *)s+OFFSET_FIFO_SIZE);
    if (!fifo || allocated < 16 || allocated > EXPANDED_FIFO_SIZE) return;
    uint32_t min=fifo[0], max=fifo[1], next=fifo[2], stop=fifo[3];
    if ((min|max|next|stop)&3 || min<16 || min>=max || max>allocated ||
        stop<min || stop>=max || next<min || next>=max) return;
    uint32_t available = (next>=stop ? next-stop : max-stop+next-min)/4;

    uint32_t cur_w = reg_value(s, SVGA_REG_WIDTH);
    uint32_t cur_h = reg_value(s, SVGA_REG_HEIGHT);
    if (portrait_profile_enabled() && cur_w == 768) cur_w = SCREEN_W;
    if (portrait_profile_enabled() && cur_h == 1280) cur_h = SCREEN_H;
    uint32_t cur_p = reg_value(s, SVGA_REG_BYTES_PER_LINE);
    if (!cur_p) cur_p = cur_w * 4;
    static uint32_t s_last_w = 0, s_last_h = 0, s_last_p = 0;
    if (g_vlknDev && (cur_w != s_last_w || cur_h != s_last_h || cur_p != s_last_p)) {
        s_last_w = cur_w; s_last_h = cur_h; s_last_p = cur_p;
        uint32_t bpp = reg_value(s, SVGA_REG_BITS_PER_PIXEL);
        if (!bpp) bpp = 32;
        uint32_t vram_sz = reg_value(s, SVGA_REG_VRAM_SIZE);
        if (!vram_sz) vram_sz = 128 * 1024 * 1024;
        uint8_t *vram = *(uint8_t **)((char *)s + 8);
        svga3_vlkn_device_set_framebuffer(g_vlknDev, vram, reg_value(s, SVGA_REG_FB_START), vram_sz, cur_w, cur_h, cur_p, bpp / 8);
        log_msg("[libqemu_svga3d] Display mode synchronized: w=%u h=%u pitch=%u bpp=%u\n", cur_w, cur_h, cur_p, bpp);
    }

    for (unsigned count=0; available && count<8192; count++) {
        uint32_t cmd=peek_word(fifo,stop,min,max,0);
        uint64_t words=0;
#define P(i) peek_word(fifo,stop,min,max,(i))
        if (cmd == SVGA_CMD_UPDATE || cmd == SVGA_CMD_UPDATE_VERBOSE) {
            words = 5;
        } else if (cmd == SVGA_CMD_RECT_FILL) {
            words = 6;
        } else if (cmd == SVGA_CMD_RECT_COPY) {
            words = 7;
        } else if (cmd == SVGA_CMD_FENCE) {
            words = 2;
        } else if (cmd == SVGA_CMD_ESCAPE) {
            if (available < 3) goto done;
            words = 3 + ((uint64_t)P(2) + 3) / 4;
        } else if (cmd == 19) { /* DEFINE_CURSOR */
            if (available < 8) goto done;
            if (P(4)>256 || P(5)>256 || P(7)>32) goto unsupported;
            words = 8 + (((uint64_t)P(4)+31)/32)*P(5) + (((uint64_t)P(4)*P(7)+31)/32)*P(5);
        } else if (cmd == 22) { /* DEFINE_ALPHA_CURSOR */
            if (available < 6) goto done;
            if (P(4)>256 || P(5)>256) goto unsupported;
            words = 6 + (uint64_t)P(4)*P(5);
        } else if (cmd == SVGA_CMD_DEFINE_SCREEN) {
            if (available < 2) goto done;
            words = 1 + ((uint64_t)P(1) + 3) / 4;
        } else if (cmd == SVGA_CMD_DESTROY_SCREEN) {
            words = (sizeof(uint32_t) + sizeof(SVGAFifoCmdDestroyScreen) + 3) / 4;
        } else if (cmd == SVGA_CMD_DEFINE_GMRFB) {
            words = (sizeof(uint32_t) + sizeof(SVGAFifoCmdDefineGMRFB) + 3) / 4;
        } else if (cmd == SVGA_CMD_BLIT_GMRFB_TO_SCREEN) {
            words = (sizeof(uint32_t) + sizeof(SVGAFifoCmdBlitGMRFBToScreen) + 3) / 4;
        } else if (cmd == SVGA_CMD_BLIT_SCREEN_TO_GMRFB) {
            words = (sizeof(uint32_t) + sizeof(SVGAFifoCmdBlitScreenToGMRFB) + 3) / 4;
        } else if (cmd == SVGA_CMD_ANNOTATION_FILL) {
            words = (sizeof(uint32_t) + sizeof(SVGAFifoCmdAnnotationFill) + 3) / 4;
        } else if (cmd == SVGA_CMD_ANNOTATION_COPY) {
            words = (sizeof(uint32_t) + sizeof(SVGAFifoCmdAnnotationCopy) + 3) / 4;
        } else if (cmd == SVGA_CMD_DEFINE_GMR2) {
            words = (sizeof(uint32_t) + sizeof(SVGAFifoCmdDefineGMR2)) / 4;
        } else if (cmd == SVGA_CMD_REMAP_GMR2) {
            if (available < 5) goto done;
            uint32_t flags = P(2);
            uint32_t numPages = P(4);
            size_t descBytes = 0;
            if (flags & SVGA_REMAP_GMR2_VIA_GMR) {
                descBytes = sizeof(SVGAGuestPtr);
            } else if (flags & SVGA_REMAP_GMR2_SINGLE_PPN) {
                descBytes = (flags & SVGA_REMAP_GMR2_PPN64) ? sizeof(uint64_t) : sizeof(uint32_t);
            } else {
                descBytes = (size_t)numPages * ((flags & SVGA_REMAP_GMR2_PPN64) ? sizeof(uint64_t) : sizeof(uint32_t));
            }
            words = (sizeof(uint32_t) + sizeof(SVGAFifoCmdRemapGMR2) + descBytes + 3) / 4;
        } else if (cmd >= SVGA_3D_CMD_BASE && cmd < SVGA_3D_CMD_FUTURE_MAX) {
            if (available < 2) goto done;
            uint32_t payloadSizeBytes = P(1);
            words = (sizeof(uint32_t) + sizeof(SVGA3dCmdHeader) + payloadSizeBytes + 3) / 4;
        } else {
            goto unsupported;
        }

        if (words > (max-min)/4-1) goto unsupported;
        if (words > available) break; /* Incomplete packet; await more words */

        if (cmd == SVGA_CMD_UPDATE || cmd == SVGA_CMD_UPDATE_VERBOSE) {
            redraw(s, P(1), P(2), P(3), P(4));
            if (g_vlknDev && g_vlknDev->surfaceMgr && g_vlknDev->guestMem) {
                /* Serialize with the FIFO thread: getSurface returns an
                 * unlocked raw pointer, so the surface must not be
                 * destroyed concurrently. */
                std::lock_guard<std::mutex> devLock(g_vlknDev->mutex);
                svga3_vlkn::VlknSurface *surf1 = g_vlknDev->surfaceMgr->getSurface(1);
                if (surf1) {
                    const auto &fb = g_vlknDev->guestMem->getFramebuffer();
                    if (fb.hva && fb.width && fb.height) {
                        uint32_t ux = P(1), uy = P(2), uw = P(3), uh = P(4);
                        if (ux < fb.width && uy < fb.height && uw > 0 && uh > 0) {
                            SVGA3dBox box = { ux, uy, 0, std::min(uw, fb.width - ux), std::min(uh, fb.height - uy), 1 };
                            const uint8_t *src = fb.hva + uy * fb.pitch + ux * (fb.bpp ? fb.bpp : 4);
                            if (g_vlknDev->contextMgr) g_vlknDev->contextMgr->endAllRenderPasses();
                            surf1->dmaUpload(0, &box, src, fb.pitch);
                        }
                    }
                }
            }
        } else if (cmd == SVGA_CMD_RECT_FILL) {
            draw_rect(s, false, P(1), 0, 0, P(2), P(3), P(4), P(5));
            if (g_vlknDev && g_vlknDev->surfaceMgr && g_vlknDev->guestMem) {
                std::lock_guard<std::mutex> devLock(g_vlknDev->mutex);
                svga3_vlkn::VlknSurface *surf1 = g_vlknDev->surfaceMgr->getSurface(1);
                if (surf1) {
                    const auto &fb = g_vlknDev->guestMem->getFramebuffer();
                    if (fb.hva && fb.width && fb.height) {
                        uint32_t fx = P(2), fy = P(3), fw = P(4), fh = P(5);
                        if (fx < fb.width && fy < fb.height && fw > 0 && fh > 0) {
                            SVGA3dBox box = { fx, fy, 0, std::min(fw, fb.width - fx), std::min(fh, fb.height - fy), 1 };
                            const uint8_t *src = fb.hva + fy * fb.pitch + fx * (fb.bpp ? fb.bpp : 4);
                            if (g_vlknDev->contextMgr) g_vlknDev->contextMgr->endAllRenderPasses();
                            surf1->dmaUpload(0, &box, src, fb.pitch);
                        }
                    }
                }
            }
        } else if (cmd == SVGA_CMD_RECT_COPY) {
            draw_rect(s, true, 0, P(1), P(2), P(3), P(4), P(5), P(6));
            if (g_vlknDev && g_vlknDev->surfaceMgr && g_vlknDev->guestMem) {
                std::lock_guard<std::mutex> devLock(g_vlknDev->mutex);
                svga3_vlkn::VlknSurface *surf1 = g_vlknDev->surfaceMgr->getSurface(1);
                if (surf1) {
                    const auto &fb = g_vlknDev->guestMem->getFramebuffer();
                    if (fb.hva && fb.width && fb.height) {
                        uint32_t cx = P(3), cy = P(4), cw = P(5), ch = P(6);
                        if (cx < fb.width && cy < fb.height && cw > 0 && ch > 0) {
                            SVGA3dBox box = { cx, cy, 0, std::min(cw, fb.width - cx), std::min(ch, fb.height - cy), 1 };
                            const uint8_t *src = fb.hva + cy * fb.pitch + cx * (fb.bpp ? fb.bpp : 4);
                            if (g_vlknDev->contextMgr) g_vlknDev->contextMgr->endAllRenderPasses();
                            surf1->dmaUpload(0, &box, src, fb.pitch);
                        }
                    }
                }
            }
        } else if (cmd == SVGA_CMD_FENCE) {
            static uint32_t fence_count = 0;
            fence_count++;
            if (fence_count <= 10 || (fence_count % 50) == 0) {
                log_msg("[libqemu_svga3d] SVGA_CMD_FENCE #%u: fence_id=%u\n", fence_count, P(1));
            }
            if (g_vlknDev) {
                /* Mesa waits on this fence after a window swap. Present the
                 * surfaces drawn since the previous fence before signalling. */
                svga3_vlkn::svga3_vlkn_present_client_surfaces(g_vlknDev, "fence");
            }
            if (min >= 28) fifo[SVGA_FIFO_FENCE] = P(1);
        } else if (cmd == SVGA_CMD_ESCAPE) {
            /* Video overlay no-ops */
        } else if (cmd == SVGA_CMD_DEFINE_SCREEN) {
            uint32_t structSize = P(1);
            if (structSize >= sizeof(uint32_t) * 5) {
                uint32_t screenId = P(2);
                uint32_t flags = P(3);
                uint32_t sw = P(4);
                uint32_t sh = P(5);
                int32_t sx = (int32_t)P(6);
                int32_t sy = (int32_t)P(7);
                log_msg("[libqemu_svga3d] DEFINE_SCREEN: id=%u flags=0x%x size=%ux%u pos=(%d,%d)\n",
                        screenId, flags, sw, sh, sx, sy);
                if (sw > 0 && sh > 0 && !(flags & SVGA_SCREEN_DEACTIVATE)) {
                    uint32_t pitch = (structSize >= sizeof(uint32_t) * 9) ? P(10) : 0;
                    if (screenId == 0) {
                        /*
                         * This shim consumes screen-object commands instead
                         * of passing them to QEMU's FIFO parser. Mirror the
                         * primary mode into QEMU's legacy SVGA registers so
                         * its DisplaySurface (and VNC/noVNC) gets a scanout.
                         */
                        set_qemu_reg_value(s, SVGA_REG_WIDTH, sw);
                        set_qemu_reg_value(s, SVGA_REG_HEIGHT, sh);
                        if (pitch) set_qemu_reg_value(s, SVGA_REG_BYTES_PER_LINE, pitch);
                        log_msg("[libqemu_svga3d] Mirrored primary screen mode to legacy scanout: %ux%u pitch=%u\n",
                                sw, sh, pitch);
                    }
                    redraw(s, (sx < 0) ? 0 : (uint32_t)sx, (sy < 0) ? 0 : (uint32_t)sy, sw, sh);
                    if (g_vlknDev && screenId == 0) {
                        if (!pitch) pitch = sw * 4;
                        uint8_t *vram = *(uint8_t **)((char *)s + 8);
                        uint32_t vram_sz = reg_value(s, SVGA_REG_VRAM_SIZE);
                        if (!vram_sz) vram_sz = 128 * 1024 * 1024;
                        svga3_vlkn_device_set_framebuffer(g_vlknDev, vram, reg_value(s, SVGA_REG_FB_START), vram_sz, sw, sh, pitch, 4);
                    }
                }
            }
        } else if (cmd == SVGA_CMD_DESTROY_SCREEN) {
            log_msg("[libqemu_svga3d] DESTROY_SCREEN: id=%u\n", P(1));
        } else if (cmd == SVGA_CMD_DEFINE_GMRFB) {
            g_display_gmrfb.ptr.gmrId = P(1);
            g_display_gmrfb.ptr.offset = P(2);
            g_display_gmrfb.bytesPerLine = P(3);
            g_display_gmrfb.format.value = P(4);
            g_display_gmrfb_defined = true;
            log_msg("[libqemu_svga3d] DEFINE_GMRFB: gmrId=%u offset=0x%x bytesPerLine=%u format=0x%x\n",
                    g_display_gmrfb.ptr.gmrId, g_display_gmrfb.ptr.offset,
                    g_display_gmrfb.bytesPerLine, g_display_gmrfb.format.value);
        } else if (cmd == SVGA_CMD_BLIT_GMRFB_TO_SCREEN) {
            int32_t src_x = (int32_t)P(1);
            int32_t src_y = (int32_t)P(2);
            int32_t left = (int32_t)P(3);
            int32_t top = (int32_t)P(4);
            int32_t right = (int32_t)P(5);
            int32_t bottom = (int32_t)P(6);
            uint32_t screen_id = P(7);
            bool copied = blit_gmrfb_to_legacy(s, src_x, src_y, left, top, right, bottom, screen_id);
            static uint32_t blit_log_count = 0;
            if (blit_log_count++ < 8) {
                log_msg("[libqemu_svga3d] BLIT_GMRFB_TO_SCREEN #%u: gmr=%u offset=0x%x pitch=%u format=0x%x src=(%d,%d) dst=(%d,%d)-(%d,%d) screen=%u copied=%d\n",
                        blit_log_count, g_display_gmrfb.ptr.gmrId, g_display_gmrfb.ptr.offset,
                        g_display_gmrfb.bytesPerLine, g_display_gmrfb.format.value,
                        src_x, src_y, left, top, right, bottom, screen_id, copied ? 1 : 0);
            }
            if (copied &&
                right > left && bottom > top) {
                uint32_t width = reg_value(s, SVGA_REG_WIDTH);
                uint32_t height = reg_value(s, SVGA_REG_HEIGHT);
                int32_t x0 = std::max<int32_t>(left, 0);
                int32_t y0 = std::max<int32_t>(top, 0);
                int32_t x1 = std::min<int32_t>(right, width);
                int32_t y1 = std::min<int32_t>(bottom, height);
                if (x1 > x0 && y1 > y0)
                    redraw(s, (uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0));
            }
        } else if (cmd == SVGA_CMD_BLIT_SCREEN_TO_GMRFB) {
            log_msg("[libqemu_svga3d] BLIT_SCREEN_TO_GMRFB\n");
        } else if (cmd == SVGA_CMD_ANNOTATION_FILL || cmd == SVGA_CMD_ANNOTATION_COPY) {
            /* Handled / consumed */
        } else if ((cmd >= SVGA_3D_CMD_BASE && cmd < SVGA_3D_CMD_FUTURE_MAX) ||
                   cmd == SVGA_CMD_DEFINE_GMR2 || cmd == SVGA_CMD_REMAP_GMR2) {
            ensure_vlkn_device(s);
            if (g_vlknDev) {
                size_t packetBytes = words * 4;
                size_t bytesConsumed = 0;
                static uint32_t total_3d = 0;
                total_3d++;
                if (total_3d <= 30 || (total_3d % 500) == 0) {
                    log_msg("[libqemu_svga3d] 3D cmd %u (payloadBytes=%u, words=%lu, total=%u)\n",
                            cmd, (cmd >= SVGA_3D_CMD_BASE) ? P(1) : 0, (unsigned long)words, total_3d);
                }
                /* Snapshot the packet into a host-private buffer before parsing:
                 * the FIFO ring lives in guest-shared memory, so a second
                 * guest vCPU can mutate packet bytes between the length
                 * check above and field validation inside the handlers
                 * (TOCTOU). The wraparound path already copied; the fast
                 * path parsed live guest memory. Cap the snapshot: the
                 * packet length was validated against available words. */
                size_t wordsToCopy = packetBytes / 4;
                std::vector<uint32_t> packetWords(wordsToCopy);
                if (stop + packetBytes <= max) {
                    memcpy(packetWords.data(), (const uint8_t *)fifo + stop, wordsToCopy * 4);
                } else {
                    for (uint64_t w = 0; w < wordsToCopy; ++w) {
                        packetWords[w] = P(w);
                    }
                }
                svga3_vlkn_fifo_execute(g_vlknDev, packetWords.data(), packetBytes, &bytesConsumed);
            }
        }

        stop = min + ((uint64_t)stop - min + words * 4) % (max - min);
        fifo[SVGA_FIFO_STOP] = stop;
        *(uint32_t *)((char *)s + OFFSET_FIFO_STOP) = stop;
        available -= words;
        continue;

unsupported:
        {
            static uint32_t last_cmd = UINT32_MAX;
            if (last_cmd != cmd) {
                log_msg("[libqemu_svga3d] Unsupported/malformed FIFO command %u at stop=%u (min=%u, max=%u, next=%u, words: [%u, %u, %u, %u]); stopped\n",
                        cmd, stop, min, max, next, P(0), P(1), P(2), P(3));
                last_cmd = cmd;
            }
        }
        break;
#undef P
    }

done:
    if (min > SVGA_FIFO_BUSY * 4) fifo[SVGA_FIFO_BUSY] = (stop != next);
    *(int *)((char *)s + OFFSET_SYNCING) = 0;
    if (orig_update_rect_flush) orig_update_rect_flush(s);
}

extern "C" uint64_t my_vmsvga_io_read(void *opaque, uint64_t addr, unsigned size) {
    void *s = opaque;
    if (addr == 0) {
        return *(int *)((char *)s + OFFSET_INDEX);
    }
    if (addr == 1) {
        int index = *(int *)((char *)s + OFFSET_INDEX);
        switch (index) {
        case SVGA_REG_CAPABILITIES:
            return SVGA_CAPABILITIES_VALUE;
        case SVGA_REG_MEM_SIZE:
            return EXPANDED_FIFO_SIZE;
        case SVGA_REG_CONFIG_DONE:
            return *(int *)((char *)s + OFFSET_CONFIG);
        case SVGA_REG_SYNC:
            return 0;
        case SVGA_REG_BUSY: {
            my_vmsvga_fifo_run(s);
            uint32_t *fifo = *(uint32_t **)((char *)s + OFFSET_FIFO);
            if (fifo && fifo[SVGA_FIFO_STOP] != fifo[SVGA_FIFO_NEXT]) {
                return 1;
            }
            return 0;
        }
        case SVGA_REG_NUM_DISPLAYS:
            return 1;
        case SVGA_REG_NUM_GUEST_DISPLAYS:
            return saved_num_guest_displays ? saved_num_guest_displays : 1;
        case SVGA_REG_GMR_ID:
            return saved_gmr_id;
        case SVGA_REG_GMR_DESCRIPTOR:
            return saved_gmr_desc;
        case SVGA_REG_GMR_MAX_IDS:
            return 256;
        case SVGA_REG_GMR_MAX_DESCRIPTOR_LENGTH:
            return 4096;
        case SVGA_REG_TRACES:
            return 0;
        case SVGA_REG_GMRS_MAX_PAGES:
            return 65536;
        case SVGA_REG_MEMORY_SIZE: {
            uint32_t vram = reg_value(s, SVGA_REG_VRAM_SIZE);
            if (!vram) vram = 128 * 1024 * 1024;
            return vram + LEGACY_SURFACE_BYTES;
        }
        case SVGA_REG_IRQMASK:
            return saved_irqmask;
        default:
            break;
        }
    }
    return orig_io_read(opaque, addr, size);
}

extern "C" void my_vmsvga_io_write(void *opaque, uint64_t addr, uint64_t data, unsigned size) {
    void *s = opaque;
    if (addr == 0) {
        *(int *)((char *)s + OFFSET_INDEX) = (int)data;
        return;
    }
    if (addr == 1) {
        int index = *(int *)((char *)s + OFFSET_INDEX);
        switch (index) {
        case SVGA_REG_CONFIG_DONE: {
            orig_io_write(opaque, addr, data, size);
            if (data) {
                uint32_t *fifo = *(uint32_t **)((char *)s + OFFSET_FIFO);
                if (!fifo) {
                    uint8_t *fifo_ptr = *(uint8_t **)((char *)s + OFFSET_FIFO_PTR);
                    if (fifo_ptr) {
                        fifo = (uint32_t *)fifo_ptr;
                        *(uint32_t **)((char *)s + OFFSET_FIFO) = fifo;
                    }
                }
                if (fifo) {
                    if (fifo[SVGA_FIFO_MIN] < 32 || fifo[SVGA_FIFO_MIN] > EXPANDED_FIFO_SIZE) return;
                    fifo[SVGA_FIFO_CAPABILITIES] |= SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_PITCHLOCK |
                                                    SVGA_FIFO_CAP_3D_HWVERSION_REVISED |
                                                    SVGA_FIFO_CAP_SCREEN_OBJECT | SVGA_FIFO_CAP_SCREEN_OBJECT_2;
                    fifo[SVGA_FIFO_FLAGS] = 0;
                    fifo[SVGA_FIFO_FENCE] = 0;
                    fifo[SVGA_FIFO_3D_HWVERSION] = SVGA3D_HWVERSION_WS65_B1;
                    fifo[SVGA_FIFO_3D_HWVERSION_REVISED] = SVGA3D_HWVERSION_WS65_B1;
                    if (fifo[SVGA_FIFO_MIN] > (SVGA_FIFO_BUSY * 4)) {
                        fifo[SVGA_FIFO_BUSY] = 0;
                    }

                    init_devcaps_record(fifo);

                    log_msg("[libqemu_svga3d] CONFIG_DONE=1: initialized extended FIFO with DevCaps (caps=0x%x, hwversion=0x%x)\n",
                            fifo[SVGA_FIFO_CAPABILITIES], fifo[SVGA_FIFO_3D_HWVERSION]);
                }
                ensure_vlkn_device(s);
            }
            return;
        }
        case SVGA_REG_SYNC:
        case SVGA_REG_BUSY:
            my_vmsvga_fifo_run(s);
            return;
        case SVGA_REG_ENABLE:
        case SVGA_REG_BYTES_PER_LINE:
        case SVGA_REG_WIDTH:
        case SVGA_REG_HEIGHT: {
            if (portrait_profile_enabled() && index == SVGA_REG_WIDTH && data == 768) {
                log_msg("[libqemu_svga3d] Suppressing portrait SVGA_REG_WIDTH %lu -> %d\n", (unsigned long)data, SCREEN_W);
                data = SCREEN_W;
            } else if (portrait_profile_enabled() && index == SVGA_REG_HEIGHT && data == 1280) {
                log_msg("[libqemu_svga3d] Suppressing portrait SVGA_REG_HEIGHT %lu -> %d\n", (unsigned long)data, SCREEN_H);
                data = SCREEN_H;
            }
            orig_io_write(opaque, addr, data, size);
            if (g_vlknDev && *(int *)((char *)s + OFFSET_ENABLE)) {
                uint32_t w = reg_value(s, SVGA_REG_WIDTH);
                uint32_t h = reg_value(s, SVGA_REG_HEIGHT);
                if (portrait_profile_enabled() && w == 768) w = SCREEN_W;
                if (portrait_profile_enabled() && h == 1280) h = SCREEN_H;
                uint32_t p = reg_value(s, SVGA_REG_BYTES_PER_LINE);
                uint32_t bpp = reg_value(s, SVGA_REG_BITS_PER_PIXEL);
                uint32_t vram_sz = reg_value(s, SVGA_REG_VRAM_SIZE);
                uint8_t *vram = *(uint8_t **)((char *)s + 8);
                if (w && h && vram) {
                    if (!p) p = w * 4;
                    if (!bpp) bpp = 32;
                    if (!vram_sz) vram_sz = 128 * 1024 * 1024;
                    svga3_vlkn_device_set_framebuffer(g_vlknDev, vram, reg_value(s, SVGA_REG_FB_START), vram_sz, w, h, p, bpp / 8);
                }
            }
            return;
        }
        case SVGA_REG_NUM_DISPLAYS:
            return;
        case SVGA_REG_NUM_GUEST_DISPLAYS:
            saved_num_guest_displays = (uint32_t)data;
            log_msg("[libqemu_svga3d] SVGA_REG_NUM_GUEST_DISPLAYS write %u\n", saved_num_guest_displays);
            return;
        case SVGA_REG_GMR_ID:
            saved_gmr_id = (uint32_t)data;
            log_msg("[libqemu_svga3d] SVGA_REG_GMR_ID write 0x%x\n", saved_gmr_id);
            return;
        case SVGA_REG_GMR_DESCRIPTOR:
            saved_gmr_desc = (uint32_t)data;
            log_msg("[libqemu_svga3d] SVGA_REG_GMR_DESCRIPTOR write 0x%x\n", saved_gmr_desc);
            ensure_vlkn_device(s);
            if (g_vlknDev && g_vlknDev->guestMem) {
                Svga3VlknStatus st = g_vlknDev->guestMem->registerLegacyGMR(saved_gmr_id, saved_gmr_desc);
                log_msg("[libqemu_svga3d] registerLegacyGMR id=%u, descPPN=0x%x -> status=%d\n",
                        saved_gmr_id, saved_gmr_desc, st);
            }
            return;
        case SVGA_REG_GMR_MAX_IDS:
        case SVGA_REG_GMR_MAX_DESCRIPTOR_LENGTH:
        case SVGA_REG_TRACES:
        case SVGA_REG_GMRS_MAX_PAGES:
        case SVGA_REG_MEMORY_SIZE:
            return;
        case SVGA_REG_IRQMASK:
            saved_irqmask = (uint32_t)data;
            return;
        default:
            break;
        }
    }
    orig_io_write(opaque, addr, data, size);
}

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void install_abs_jmp(uintptr_t from, void *to) {
    unsigned char *p = (unsigned char *)from;
    p[0] = 0xff; p[1] = 0x25; p[2] = p[3] = p[4] = p[5] = 0;
    uint64_t target = (uint64_t)to;
    memcpy(p + 6, &target, 8);
}

static void *make_orig_tramp(uintptr_t orig, int stolen, int call_off, uintptr_t call_target) {
    unsigned char *t = (unsigned char *)MAP_FAILED;
    for (uintptr_t off = 0x200000; off < 0x40000000UL && t == (unsigned char *)MAP_FAILED; off += 0x200000) {
        void *hint = (void *)((orig + off) & ~((uintptr_t)0xfff));
        t = (unsigned char *)mmap(hint, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    }
    if (t == (unsigned char *)MAP_FAILED) return NULL;
    intptr_t back = (intptr_t)(orig + (unsigned)stolen) - (intptr_t)(t + 4 + stolen + 5);
    if (back != (int32_t)back) {
        munmap(t, 4096);
        return NULL;
    }
    /* Indirect calls to this trampoline require an IBT landing pad. */
    t[0] = 0xf3; t[1] = 0x0f; t[2] = 0x1e; t[3] = 0xfa;
    memcpy(t + 4, (void *)orig, (size_t)stolen);
    if (call_off >= 0) {
        int32_t rel = (int32_t)(call_target - ((uintptr_t)t + 4 + (size_t)call_off + 5));
        memcpy(t + 4 + call_off + 1, &rel, 4);
    }
    t[4 + stolen] = 0xe9;
    int32_t jrel = (int32_t)back;
    memcpy(t + 4 + stolen + 1, &jrel, 4);
    return t;
}

extern "C" void my_input_update_buttons(void *con, uint32_t *map, uint32_t old, uint32_t newm) {
    bool press = (newm & VNC_LEFT_BUTTON) && !(old & VNC_LEFT_BUTTON);
    bool release = !(newm & VNC_LEFT_BUTTON) && (old & VNC_LEFT_BUTTON);
    if (press) {
        pending_press = true;
        pending_con = con;
        pending_map = map;
        pending_old = old;
        pending_new = newm;
        return;
    }
    if (release) {
        int64_t remain = TAP_DWELL_NS - (now_ns() - left_down_ns);
        if (remain > 0 && remain < 250000000LL) {
            struct timespec ts = { 0, (long)remain };
            nanosleep(&ts, NULL);
        }
        log_msg("[libqemu_svga3d] left release after dwell\n");
    }
    orig_input_update_buttons(con, map, old, newm);
}

extern "C" void my_input_queue_rel(void *con, int axis, int value) {
    while (value > REL_STEP) {
        orig_input_queue_rel(con, axis, REL_STEP);
        orig_input_event_sync();
        value -= REL_STEP;
    }
    while (value < -REL_STEP) {
        orig_input_queue_rel(con, axis, -REL_STEP);
        orig_input_event_sync();
        value += REL_STEP;
    }
    orig_input_queue_rel(con, axis, value);
}

extern "C" void my_input_event_sync(void) {
    if (pending_press) {
        orig_input_update_buttons(pending_con, pending_map, pending_old, pending_new);
        pending_press = false;
        left_down_ns = now_ns();
        log_msg("[libqemu_svga3d] left press after relative move\n");
    }
    orig_input_event_sync();
}

static uint32_t guest_bmask;
static uint32_t vnc_bmap[10] = { 0x01, 0x04, 0x02, 0x08, 0x10, 0x20, 0x40 };
__attribute__((visibility("hidden"))) void *vnc_pointer_cont = NULL;

extern "C" void my_vnc_pointer_event(void *vs, uint32_t button_mask, int x, int y) {
    if (!vs || !orig_input_queue_abs) return;
    void *vnc_display = *(void **)((char *)vs + VS_VD);
    void *con = vnc_display ? *(void **)((char *)vnc_display + VD_CONSOLE) : NULL;
    const int width = g_vmsvga_state ? static_cast<int>(reg_value(g_vmsvga_state, SVGA_REG_WIDTH)) : SCREEN_W;
    const int height = g_vmsvga_state ? static_cast<int>(reg_value(g_vmsvga_state, SVGA_REG_HEIGHT)) : SCREEN_H;
    if (width <= 0 || height <= 0) return;
    x = std::clamp(x, 0, width - 1);
    y = std::clamp(y, 0, height - 1);
    /* QEMU's own VNC handler uses qemu_input_queue_abs for absolute-pointer
     * devices. Feed the same guest console and coordinate range directly so
     * the USB tablet and VMware absolute pointer stay aligned with VNC. */
    orig_input_queue_abs(con, INPUT_AXIS_X, x, 0, width);
    orig_input_queue_abs(con, INPUT_AXIS_Y, y, 0, height);
    orig_input_event_sync();

    uint32_t prev = guest_bmask;
    if ((button_mask & VNC_LEFT_BUTTON) && !(prev & VNC_LEFT_BUTTON)) {
        orig_input_update_buttons(con, vnc_bmap, prev, button_mask);
        orig_input_event_sync();
        left_down_ns = now_ns();
        log_msg("[libqemu_svga3d] default-console press %d,%d\n", x, y);
    } else if (!(button_mask & VNC_LEFT_BUTTON) && (prev & VNC_LEFT_BUTTON)) {
        int64_t remain = TAP_DWELL_NS - (now_ns() - left_down_ns);
        if (remain > 0 && remain < 250000000LL) {
            struct timespec ts = { 0, (long)remain };
            nanosleep(&ts, NULL);
        }
        orig_input_update_buttons(con, vnc_bmap, prev, button_mask);
        orig_input_event_sync();
        log_msg("[libqemu_svga3d] default-console release %d,%d\n", x, y);
    } else if (prev != button_mask) {
        orig_input_update_buttons(con, vnc_bmap, prev, button_mask);
        orig_input_event_sync();
    }
    guest_bmask = button_mask;
    *(int *)((char *)vs + VS_LAST_X) = x;
    *(int *)((char *)vs + VS_LAST_Y) = y;
    *(uint32_t *)((char *)vs + VS_LAST_BMASK) = button_mask;
}

asm(
    ".text\n"
    ".p2align 4\n"
    ".globl vnc_pointer_hook\n"
    "vnc_pointer_hook:\n"
    "  movq %r15, %rdi\n"
    "  movl %ebx, %esi\n"
    "  movl (%rsp), %edx\n"
    "  movl 4(%rsp), %ecx\n"
    "  subq $8, %rsp\n"
    "  call my_vnc_pointer_event\n"
    "  addq $8, %rsp\n"
    "  movq vnc_pointer_cont(%rip), %rax\n"
    "  jmp *%rax\n"
);
extern "C" void vnc_pointer_hook(void);

static uintptr_t qemu_base = 0;
static bool supported_build = false;
static const unsigned char expected_build[] = {0x2e,0x87,0x0e,0x40,0x0e,0x16,0x92,0xf5,0xf5,0xa5,0x4d,0xa8,0x97,0xd1,0xdc,0x15,0x2b,0x18,0x72,0x78};

static int phdr_callback(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size; (void)data;
    if (info->dlpi_name == NULL || info->dlpi_name[0] == 0) {
        qemu_base = info->dlpi_addr;
        for (int i=0;i<info->dlpi_phnum;i++) {
            const ElfW(Phdr) *ph=&info->dlpi_phdr[i];
            if (ph->p_type!=PT_NOTE) continue;
            const unsigned char *p=(const unsigned char *)(info->dlpi_addr+ph->p_vaddr), *end=p+ph->p_memsz;
            while ((size_t)(end-p)>=sizeof(ElfW(Nhdr))) {
                const ElfW(Nhdr) *n=(const ElfW(Nhdr) *)p;
                size_t ns=((size_t)n->n_namesz+3)&~(size_t)3, ds=((size_t)n->n_descsz+3)&~(size_t)3;
                p+=sizeof(*n);
                if (ns>(size_t)(end-p) || ds>(size_t)(end-p)-ns) break;
                if (n->n_type==NT_GNU_BUILD_ID && n->n_namesz==4 && !memcmp(p,"GNU",4) &&
                    n->n_descsz==sizeof(expected_build) && !memcmp(p+ns,expected_build,sizeof(expected_build))) supported_build=true;
                p+=ns+ds;
            }
        }
        return 1;
    }
    return 0;
}

__attribute__((constructor))
static void svga3d_init(void) {
    if (program_invocation_name == NULL || strstr(program_invocation_name, "qemu-system") == NULL) {
        return;
    }

    dl_iterate_phdr(phdr_callback, NULL);
    log_msg("[libqemu_svga3d] Loaded in QEMU! qemu_base=0x%lx\n", (unsigned long)qemu_base);
    if (!qemu_base || !supported_build) {
        fprintf(stderr, "[libqemu_svga3d] ERROR: unsupported QEMU build; refusing unsafe patch!\n");
        _exit(78);
    }

    const unsigned char expected1[]={0xb9,0,0,1,0};
    const unsigned char expected2[]={0x41,0xc7,0x86,0x38,0x16,1,0,0,0,1,0};
    const unsigned char expected_fifo[]={0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x55,0x53,0x4c,0x8d,0x9c,0x24};
    const unsigned char expected_btn[]={0xf3,0x0f,0x1e,0xfa,0x41,0x57,0x41,0x56,0x49,0x89,0xfe};
    const unsigned char expected_rel[]={0xf3,0x0f,0x1e,0xfa,0x53,0x48,0x63,0xd2,0x48,0x83,0xec,0x30};
    const unsigned char expected_abs[]={0xf3,0x0f,0x1e,0xfa,0x53,0x48,0x63,0xc9,0x4d,0x63,0xc0,0xb8};
    const unsigned char expected_sync[]={0xf3,0x0f,0x1e,0xfa,0x48,0x83,0xec,0x08,0xe8};
    const unsigned char expected_ptr[]={0x49,0x8b,0x87,0xa0,0x51,0x01,0x00,0xf3,0x0f,0x7e,0x24,0x24,0x48,0x8b};
    if (memcmp((void *)(qemu_base+ADDR_PCIVMSVGA_REALIZE_SIZE1),expected1,sizeof(expected1)) ||
        memcmp((void *)(qemu_base+ADDR_PCIVMSVGA_REALIZE_SIZE2),expected2,sizeof(expected2)) ||
        memcmp((void *)(qemu_base+ADDR_VMSVGA_FIFO_RUN),expected_fifo,sizeof(expected_fifo)) ||
        memcmp((void *)(qemu_base+ADDR_QEMU_INPUT_UPDATE_BUTTONS),expected_btn,sizeof(expected_btn)) ||
        memcmp((void *)(qemu_base+ADDR_QEMU_INPUT_QUEUE_REL),expected_rel,sizeof(expected_rel)) ||
        memcmp((void *)(qemu_base+ADDR_QEMU_INPUT_QUEUE_ABS),expected_abs,sizeof(expected_abs)) ||
        memcmp((void *)(qemu_base+ADDR_QEMU_INPUT_EVENT_SYNC),expected_sync,sizeof(expected_sync)) ||
        memcmp((void *)(qemu_base+ADDR_VNC_POINTER),expected_ptr,sizeof(expected_ptr))) {
        fprintf(stderr,"SVGA shim: unexpected instruction bytes; refusing patch\n"); _exit(78);
    }

    /* 1. Patch pci_vmsvga_realize to allocate 4MB FIFO RAM and set fifo_size = 4MB */
    uintptr_t page_text = (qemu_base + ADDR_PCIVMSVGA_REALIZE_SIZE1) & ~0xFFF;
    if (mprotect((void *)page_text, 4096 * 2, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
        unsigned char *p1 = (unsigned char *)(qemu_base + ADDR_PCIVMSVGA_REALIZE_SIZE1);
        p1[3] = 0x40; /* 0x01 -> 0x40 (4MB) */

        unsigned char *p2 = (unsigned char *)(qemu_base + ADDR_PCIVMSVGA_REALIZE_SIZE2);
        p2[9] = 0x40; /* 0x01 -> 0x40 (4MB) */

        mprotect((void *)page_text, 4096 * 2, PROT_READ | PROT_EXEC);
        log_msg("[libqemu_svga3d] Patched FIFO size in pci_vmsvga_realize to 4MB\n");
    } else {
        perror("[libqemu_svga3d] mprotect text failed"); _exit(78);
    }

    /* 2. Resolve original update_rect_flush */
    orig_update_rect_flush = (void (*)(void *))(qemu_base + ADDR_VMSVGA_UPDATE_RECT_FLUSH);

    /* 3. Hook vmsvga_fifo_run with a 14-byte indirect jump */
    uintptr_t page_fifo = (qemu_base + ADDR_VMSVGA_FIFO_RUN) & ~0xFFF;
    if (mprotect((void *)page_fifo, 4096 * 2, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
        unsigned char *p_fifo = (unsigned char *)(qemu_base + ADDR_VMSVGA_FIFO_RUN);
        p_fifo[0] = 0xff;
        p_fifo[1] = 0x25;
        p_fifo[2] = 0x00;
        p_fifo[3] = 0x00;
        p_fifo[4] = 0x00;
        p_fifo[5] = 0x00;
        uint64_t target = (uint64_t)&my_vmsvga_fifo_run;
        memcpy(&p_fifo[6], &target, 8);
        mprotect((void *)page_fifo, 4096 * 2, PROT_READ | PROT_EXEC);
        log_msg("[libqemu_svga3d] Hooked vmsvga_fifo_run -> %p\n", (void *)target);
    } else {
        perror("[libqemu_svga3d] mprotect fifo_run failed"); _exit(78);
    }

    /* 4. Hook vmsvga_io_ops.read and vmsvga_io_ops.write */
    uintptr_t page_ops = (qemu_base + ADDR_VMSVGA_IO_OPS) & ~0xFFF;
    if (mprotect((void *)page_ops, 4096 * 2, PROT_READ | PROT_WRITE) == 0) {
        void **p_ops = (void **)(qemu_base + ADDR_VMSVGA_IO_OPS);
        orig_io_read = (uint64_t (*)(void *, uint64_t, unsigned))p_ops[0];
        orig_io_write = (void (*)(void *, uint64_t, uint64_t, unsigned))p_ops[1];
        p_ops[0] = (void *)&my_vmsvga_io_read;
        p_ops[1] = (void *)&my_vmsvga_io_write;
        log_msg("[libqemu_svga3d] Hooked vmsvga_io_ops (read=%p->%p, write=%p->%p)\n",
                orig_io_read, my_vmsvga_io_read, orig_io_write, my_vmsvga_io_write);
    } else {
        perror("[libqemu_svga3d] mprotect io_ops failed"); _exit(78);
    }

    orig_input_update_buttons = (void (*)(void *, uint32_t *, uint32_t, uint32_t))make_orig_tramp(qemu_base + ADDR_QEMU_INPUT_UPDATE_BUTTONS, 22, -1, 0);
    orig_input_queue_rel = (void (*)(void *, int, int))make_orig_tramp(qemu_base + ADDR_QEMU_INPUT_QUEUE_REL, 21, -1, 0);
    orig_input_queue_abs = (void (*)(void *, int, int, int, int))(qemu_base + ADDR_QEMU_INPUT_QUEUE_ABS);
    orig_input_event_sync = (void (*)(void))make_orig_tramp(qemu_base + ADDR_QEMU_INPUT_EVENT_SYNC, 15, 8,
                                                            qemu_base + 0x607150);
    if (!orig_input_update_buttons || !orig_input_queue_rel || !orig_input_event_sync) {
        perror("[libqemu_svga3d] mmap input trampoline failed"); _exit(78);
    }

    uintptr_t page_in = (qemu_base + ADDR_QEMU_INPUT_EVENT_SYNC) & ~0xFFF;
    if (mprotect((void *)page_in, 4096 * 2, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        perror("[libqemu_svga3d] mprotect input failed"); _exit(78);
    }
    install_abs_jmp(qemu_base + ADDR_QEMU_INPUT_UPDATE_BUTTONS, (void *)my_input_update_buttons);
    install_abs_jmp(qemu_base + ADDR_QEMU_INPUT_QUEUE_REL, (void *)my_input_queue_rel);
    install_abs_jmp(qemu_base + ADDR_QEMU_INPUT_EVENT_SYNC, (void *)my_input_event_sync);
    mprotect((void *)page_in, 4096 * 2, PROT_READ | PROT_EXEC);
    log_msg("[libqemu_svga3d] Hooked qemu_input buttons/rel/sync for PlayBook tap dwell\n");

    vnc_pointer_cont = (void *)(qemu_base + ADDR_VNC_CONT);
    uintptr_t page_ptr = (qemu_base + ADDR_VNC_POINTER) & ~0xFFF;
    if (mprotect((void *)page_ptr, 4096 * 2, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        perror("[libqemu_svga3d] mprotect vnc pointer failed"); _exit(78);
    }
    install_abs_jmp(qemu_base + ADDR_VNC_POINTER, (void *)vnc_pointer_hook);
    mprotect((void *)page_ptr, 4096 * 2, PROT_READ | PROT_EXEC);
    log_msg("[libqemu_svga3d] Hooked VNC pointer preamble with %ux%u absolute mapping\n",
            SCREEN_W, SCREEN_H);

}
