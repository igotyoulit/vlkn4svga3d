// Exercise the actual lab-adapter walker without loading/patching QEMU.
#define SVGA3_PRELOAD_TEST
#include "../src/qemu_svga3d_preload.cpp"
#include <cstdio>
#include <vector>

static bool appendDuringRead = false;
static bool sawAcknowledgement = false;
static uint64_t fakeRead(void *opaque, uint64_t, unsigned) {
  auto *state = static_cast<char *>(opaque);
  auto *fifo = *reinterpret_cast<uint32_t **>(state + OFFSET_FIFO);
  if (appendDuringRead) {
    sawAcknowledgement = fifo[SVGA_FIFO_BUSY] == 0;
    uint32_t end = fifo[SVGA_FIFO_NEXT];
    fifo[end / 4] = SVGA_CMD_ESCAPE;
    fifo[end / 4 + 1] = 0;
    fifo[end / 4 + 2] = 0;
    fifo[SVGA_FIFO_NEXT] = end + 12;
    // Producer sent another SYNC after seeing the acknowledgement.
    fifo[SVGA_FIFO_BUSY] = 1;
    appendDuringRead = false;
  }
  return 0;
}

// Reproduce the mid-function VNC patch point: QEMU has already aligned its
// stack before jumping to the hook. The register-read probe requires the
// normal SysV entry alignment, just like QEMU's MOVAPS-based port handler.
extern "C" uint64_t alignedReadProbe(void *, uint64_t, unsigned);
extern "C" void runVncHookProbe(void *);
extern "C" void vncHookProbeReturn(void);
asm(
    ".text\n"
    ".globl alignedReadProbe\n"
    "alignedReadProbe:\n"
    "  endbr64\n"
    "  pxor %xmm0, %xmm0\n"
    "  movaps %xmm0, -24(%rsp)\n"
    "  movl $640, %eax\n"
    "  ret\n"
    ".globl runVncHookProbe\n"
    "runVncHookProbe:\n"
    "  endbr64\n"
    "  pushq %rbx\n"
    "  pushq %r15\n"
    "  subq $24, %rsp\n"
    "  movq %rdi, %r15\n"
    "  xorl %ebx, %ebx\n"
    "  movl $100, (%rsp)\n"
    "  movl $120, 4(%rsp)\n"
    "  jmp vnc_pointer_hook\n"
    ".globl vncHookProbeReturn\n"
    "vncHookProbeReturn:\n"
    "  endbr64\n"
    "  addq $24, %rsp\n"
    "  popq %r15\n"
    "  popq %rbx\n"
    "  ret\n"
);
static unsigned pointerAxes = 0;
static void fakeAbsolute(void *, int axis, int value, int min, int max) {
  if ((axis == INPUT_AXIS_X && value == 100) ||
      (axis == INPUT_AXIS_Y && value == 120)) {
    if (min == 0 && max == 640) ++pointerAxes;
  }
}
static void fakeSync() {}

static unsigned displayCreates, displayReplaces, displayUpdates, legacyFlushes;
static uint8_t *displayPixels;
static uint64_t screenRead(void *opaque, uint64_t, unsigned) {
  int index = *reinterpret_cast<int *>(static_cast<char *>(opaque) + OFFSET_INDEX);
  switch (index) {
    case SVGA_REG_WIDTH: return 8;
    case SVGA_REG_HEIGHT: return 8;
    case SVGA_REG_BYTES_PER_LINE: return 32;
    case SVGA_REG_BITS_PER_PIXEL: return 32;
    case SVGA_REG_VRAM_SIZE: return 65536;
    default: return 0;
  }
}
static void *createDisplay(int, int, uint32_t, int, uint8_t *pixels) {
  ++displayCreates; displayPixels = pixels;
  return reinterpret_cast<void *>(1);
}
static void replaceDisplay(void *, void *) { ++displayReplaces; }
static void updateDisplay(void *, int, int, int, int) { ++displayUpdates; }
static void flushLegacy(void *) { ++legacyFlushes; }
static void screenWrite(void *, uint64_t, uint64_t, unsigned) {}

int main() {
  std::vector<uint64_t> storage(0x20000 / 8);
  auto *state = reinterpret_cast<char *>(storage.data());
  std::vector<uint32_t> ring(32768);
  auto *fifo = ring.data();
  *reinterpret_cast<int *>(state + OFFSET_CONFIG) = 1;
  *reinterpret_cast<int *>(state + OFFSET_ENABLE) = 1;
  *reinterpret_cast<uint32_t *>(state + OFFSET_FIFO_SIZE) = ring.size() * 4;
  *reinterpret_cast<uint32_t **>(state + OFFSET_FIFO) = fifo;
  orig_io_read = fakeRead;
  bool ok = true;
  auto reset = [&](uint32_t start) {
    std::fill(ring.begin(), ring.end(), 0);
    fifo[0] = 4096; fifo[1] = ring.size() * 4;
    fifo[2] = fifo[3] = start; fifo[SVGA_FIFO_BUSY] = 1;
  };
  auto append = [&](uint32_t value) {
    fifo[fifo[2] / 4] = value;
    fifo[2] += 4;
    if (fifo[2] == fifo[1]) fifo[2] = fifo[0];
  };
  for (bool wrap : {false, true}) {
    reset(wrap ? uint32_t(ring.size() * 4 - 8) : 4096);
    for (unsigned i = 0; i < 8300; ++i) {
      append(SVGA_CMD_ESCAPE); append(0); append(0);
    }
    append(SVGA_CMD_FENCE); append(42);
    my_vmsvga_fifo_run(state);
    bool drained = fifo[3] == fifo[2] && fifo[SVGA_FIFO_FENCE] == 42 && fifo[SVGA_FIFO_BUSY] == 0;
    printf("8300-command %s batch and final fence: %s\n", wrap ? "wrapped" : "linear", drained ? "PASS" : "FAIL");
    ok &= drained;
  }
  reset(4096);
  append(SVGA_CMD_ESCAPE); append(0); append(0);
  uint32_t firstEnd = fifo[2];
  appendDuringRead = true;
  my_vmsvga_fifo_run(state);
  bool queued = sawAcknowledgement && fifo[3] == firstEnd && fifo[2] == firstEnd + 12 && fifo[SVGA_FIFO_BUSY] == 1;
  my_vmsvga_fifo_run(state);
  queued &= fifo[3] == fifo[2] && fifo[SVGA_FIFO_BUSY] == 0;
  printf("producer notification during snapshot drain: %s\n", queued ? "PASS" : "FAIL");
  ok &= queued;
  reset(4096); append(SVGA_CMD_ESCAPE); append(0);
  my_vmsvga_fifo_run(state);
  bool incomplete = fifo[3] == 4096 && fifo[SVGA_FIFO_BUSY] == 0;
  append(0); fifo[SVGA_FIFO_BUSY] = 1;
  my_vmsvga_fifo_run(state);
  incomplete &= fifo[3] == fifo[2] && fifo[SVGA_FIFO_BUSY] == 0;
  printf("incomplete packet completion can notify host: %s\n", incomplete ? "PASS" : "FAIL");
  ok &= incomplete;
  std::vector<uint64_t> vncState(0x16000 / 8);
  std::vector<uint64_t> display(0x60 / 8);
  auto *vs = reinterpret_cast<char *>(vncState.data());
  *reinterpret_cast<void **>(vs + VS_VD) = display.data();
  g_vmsvga_state = state;
  orig_io_read = alignedReadProbe;
  orig_input_queue_abs = fakeAbsolute;
  orig_input_event_sync = fakeSync;
  vnc_pointer_cont = reinterpret_cast<void *>(vncHookProbeReturn);
  runVncHookProbe(vs);
  bool pointerOk = pointerAxes == 2 &&
      *reinterpret_cast<int *>(vs + VS_LAST_X) == 100 &&
      *reinterpret_cast<int *>(vs + VS_LAST_Y) == 120;
  printf("VNC mid-function hook stack alignment and coordinates: %s\n", pointerOk ? "PASS" : "FAIL");
  ok &= pointerOk;
  // A screen object owns its declared backing store; VRAM zero may be a
  // vertex buffer. Blits and notifications must leave that area untouched.
  std::vector<uint8_t> vram(65536, 0x9b);
  *reinterpret_cast<uint8_t **>(state + 8) = vram.data();
  *reinterpret_cast<void **>(state + 0xa40) = reinterpret_cast<void *>(1);
  orig_io_read = screenRead;
  orig_create_display_surface = createDisplay;
  orig_replace_display_surface = replaceDisplay;
  orig_dpy_gfx_update = updateDisplay;
  orig_update_rect_flush = flushLegacy;
  bool scanoutOk = !bind_screen_scanout(state,8,8,32,SVGA_GMR_FRAMEBUFFER,65536) &&
      !bind_screen_scanout(state,8,8,16,SVGA_GMR_FRAMEBUFFER,4096) &&
      !bind_screen_scanout(state,8,8,33,SVGA_GMR_FRAMEBUFFER,4096) &&
      !bind_screen_scanout(state,8,8,32,SVGA_GMR_FRAMEBUFFER,4097) &&
      !bind_screen_scanout(state,8,8,32,7,4096) && displayCreates == 0;
  scanoutOk &= bind_screen_scanout(state,8,8,32,SVGA_GMR_FRAMEBUFFER,4096) &&
      displayPixels == vram.data()+4096 && displayCreates == 1 && displayReplaces == 1;
  g_display_gmrfb_defined = true;
  g_display_gmrfb.ptr = {SVGA_GMR_FRAMEBUFFER,8192};
  g_display_gmrfb.bytesPerLine = 32;
  g_display_gmrfb.format.value = 0x1820;
  for (unsigned row=0; row<8; ++row) for (unsigned col=0; col<8; ++col) {
    uint32_t value = 0xff000000 | (row<<8) | col;
    memcpy(vram.data()+8192+row*32+col*4,&value,4);
  }
  scanoutOk &= blit_gmrfb_to_legacy(state,1,2,3,4,5,6,0);
  for (unsigned row=0; row<2; ++row) for (unsigned col=0; col<2; ++col) {
    uint32_t value; memcpy(&value,vram.data()+4096+(4+row)*32+(3+col)*4,4);
    scanoutOk &= value == (0xff000000 | ((2+row)<<8) | (1+col));
  }
  scanoutOk &= std::all_of(vram.begin(),vram.begin()+4096,[](uint8_t b){return b==0x9b;});
  *reinterpret_cast<int *>(state+OFFSET_REDRAW_FIFO_LAST)=0;
  redraw(state,3,4,2,2);
  my_vmsvga_update_rect_flush(state);
  scanoutOk &= displayUpdates==1 && legacyFlushes==0 &&
      *reinterpret_cast<int *>(state+OFFSET_REDRAW_FIFO_LAST)==0;
  scanoutOk &= !bind_screen_scanout(state,8,8,32,SVGA_GMR_FRAMEBUFFER,65536) &&
      g_screen_scanout_offset==4096 && displayCreates==1;
  auto unchanged = vram;
  g_display_gmrfb.ptr.offset = 65520;
  scanoutOk &= !blit_gmrfb_to_legacy(state,0,0,0,0,2,2,0) && vram == unchanged;
  g_display_gmrfb.ptr.offset = 8192;
  unbind_screen_scanout(state,true);
  scanoutOk &= g_screen_deactivated && !g_screen_scanout_active && displayPixels==nullptr &&
      !blit_gmrfb_to_legacy(state,0,0,0,0,2,2,0) && vram==unchanged;
  unbind_screen_scanout(state,false);
  scanoutOk &= !g_screen_deactivated && displayPixels==vram.data();
  printf("screen backing-store bounds, ownership and display notification: %s\n",scanoutOk?"PASS":"FAIL");
  ok &= scanoutOk;
  reset(4096);
  append(SVGA_CMD_DEFINE_SCREEN); append(20); append(0); append(3); append(8); append(8);
  my_vmsvga_fifo_run(state);
  bool shortScreenOk=fifo[3]==4096 && displayCreates==3;
  printf("truncated screen object rejected before side effects: %s\n",shortScreenOk?"PASS":"FAIL");
  ok &= shortScreenOk;
  orig_io_write=screenWrite;
  reset(4096);
  for (uint32_t word : {uint32_t(SVGA_CMD_DEFINE_SCREEN), uint32_t(sizeof(SVGAScreenObject)),
      0u, 3u, 8u, 8u, 0u, 0u, uint32_t(SVGA_GMR_FRAMEBUFFER), 4096u, 32u, 0u,
      uint32_t(SVGA_CMD_FENCE), 43u}) append(word);
  my_vmsvga_fifo_run(state);
  bool screenPacketOk = fifo[3]==fifo[2] && fifo[SVGA_FIFO_FENCE]==43 &&
      g_screen_scanout_active && g_screen_scanout_offset==4096 && displayPixels==vram.data()+4096;
  reset(4096);
  append(SVGA_CMD_DESTROY_SCREEN); append(0); append(SVGA_CMD_FENCE); append(44);
  my_vmsvga_fifo_run(state);
  screenPacketOk &= fifo[3]==fifo[2] && fifo[SVGA_FIFO_FENCE]==44 &&
      g_screen_deactivated && !g_screen_scanout_active && displayPixels==nullptr;
  printf("screen define/destroy FIFO packets and completion fences: %s\n",screenPacketOk?"PASS":"FAIL");
  ok &= screenPacketOk;
  // Screen backing-store pitch and size need not match legacy mode registers.
  // Copy beyond the legacy height into a screen with padding between rows.
  std::fill(vram.begin(), vram.end(), 0x9b);
  for (unsigned row=0; row<8; ++row) for (unsigned col=0; col<8; ++col) {
    uint32_t value = 0xff000000 | (row<<8) | col;
    memcpy(vram.data()+8192+row*32+col*4,&value,4);
  }
  bool paddedScreenOk = bind_screen_scanout(state,8,10,64,SVGA_GMR_FRAMEBUFFER,4096) &&
      blit_gmrfb_to_legacy(state,1,2,3,8,5,10,0);
  for (unsigned row=0; row<2; ++row) for (unsigned col=0; col<2; ++col) {
    uint32_t value; memcpy(&value,vram.data()+4096+(8+row)*64+(3+col)*4,4);
    paddedScreenOk &= value == (0xff000000 | ((2+row)<<8) | (1+col));
  }
  unsigned updatesBefore = displayUpdates;
  redraw(state,3,8,2,2);
  my_vmsvga_update_rect_flush(state);
  paddedScreenOk &= displayUpdates == updatesBefore+1;
  paddedScreenOk &= std::all_of(vram.begin()+4096+8*64+32,
      vram.begin()+4096+9*64,[](uint8_t b){return b==0x9b;});
  Svga3VlknConfig cfg{};
  cfg.forceMockBackend = true;
  g_vlknDev = svga3_vlkn_device_create(&cfg);
  paddedScreenOk &= g_vlknDev &&
      bind_screen_scanout(state,8,10,64,SVGA_GMR_FRAMEBUFFER,4096);
  unsigned flushesBefore = legacyFlushes;
  updatesBefore = displayUpdates;
  reset(4096);
  append(SVGA_CMD_BLIT_GMRFB_TO_SCREEN);
  for (uint32_t word : {1u,2u,3u,8u,5u,10u,0u}) append(word);
  append(SVGA_CMD_FENCE); append(45);
  my_vmsvga_fifo_run(state);
  paddedScreenOk &= legacyFlushes == flushesBefore && displayUpdates == updatesBefore+1;
  if (g_vlknDev) {
    const auto &fb = g_vlknDev->guestMem->getFramebuffer();
    paddedScreenOk &= fb.width==8 && fb.height==10 && fb.pitch==64 &&
        fb.scanoutOffset==4096 && fifo[SVGA_FIFO_FENCE]==45;
    svga3_vlkn_device_destroy(g_vlknDev);
    g_vlknDev = nullptr;
  }
  printf("screen-object pitch and dimensions override legacy mode: %s\n",paddedScreenOk?"PASS":"FAIL");
  ok &= paddedScreenOk;
  return ok ? 0 : 1;
}
