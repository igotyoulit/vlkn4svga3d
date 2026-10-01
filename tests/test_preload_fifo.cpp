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
  return ok ? 0 : 1;
}
