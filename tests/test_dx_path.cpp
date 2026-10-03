/* DX command path tests: dispatch, fail-closed behavior, D3D9 isolation. */
#include "svga3_vlkn.h"
#include "svga3_dx.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;
#define TEST_CHECK(cond, desc) do { \
    if (!(cond)) { \
        printf("  [FAIL] %s\n", desc); \
        failures++; \
    } else { \
        printf("  [ok] %s\n", desc); \
    } \
} while (0)

/* Build a FIFO packet: cmd + SVGA3dCmdHeader(size) + payload. */
static std::vector<uint8_t> makePacket(uint32_t cmd, const void *payload, size_t payloadSize) {
    std::vector<uint8_t> pkt;
    pkt.resize(sizeof(uint32_t) + sizeof(SVGA3dCmdHeader) + payloadSize);
    uint8_t *p = pkt.data();
    memcpy(p, &cmd, sizeof(uint32_t));
    p += sizeof(uint32_t);
    SVGA3dCmdHeader hdr;
    hdr.size = (uint32_t)payloadSize;
    memcpy(p, &hdr, sizeof(hdr));
    p += sizeof(hdr);
    if (payloadSize) memcpy(p, payload, payloadSize);
    return pkt;
}

static Svga3VlknDevice *makeDevice() {
    Svga3VlknConfig cfg{};
    cfg.forceMockBackend = true;
    return svga3_vlkn_device_create(&cfg);
}

static void testDefineDestroyContext() {
    printf("DX define/destroy context:\n");
    auto *dev = makeDevice();
    TEST_CHECK(dev != nullptr, "device created");
    if (!dev) return;

    SVGA3dCmdDXDefineContext dc{};
    dc.cid = 100;
    auto pkt = makePacket(SVGA_3D_CMD_DX_DEFINE_CONTEXT, &dc, sizeof(dc));
    size_t consumed = 0;
    Svga3VlknStatus st = svga3_vlkn_fifo_execute(dev, pkt.data(), pkt.size(), &consumed);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "DX_DEFINE_CONTEXT succeeds");

    SVGA3dCmdDXDestroyContext xc{};
    xc.cid = 100;
    pkt = makePacket(SVGA_3D_CMD_DX_DESTROY_CONTEXT, &xc, sizeof(xc));
    st = svga3_vlkn_fifo_execute(dev, pkt.data(), pkt.size(), &consumed);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS, "DX_DESTROY_CONTEXT succeeds");

    /* Destroying again fails (not found) but does not crash. */
    st = svga3_vlkn_fifo_execute(dev, pkt.data(), pkt.size(), &consumed);
    TEST_CHECK(st != SVGA3_VLKN_SUCCESS, "double destroy fails cleanly");

    svga3_vlkn_device_destroy(dev);
}

static void testSetRenderTargets() {
    printf("DX set render targets:\n");
    auto *dev = makeDevice();
    TEST_CHECK(dev != nullptr, "device created");
    if (!dev) return;

    SVGA3dSize size{64, 64, 1};
    TEST_CHECK(svga3_vlkn_surface_define(dev, 200, SVGA3D_SURFACE_HINT_RENDERTARGET,
                                        SVGA3D_A8R8G8B8, &size, 1) == SVGA3_VLKN_SUCCESS,
               "render target surface defined");

    SVGA3dCmdDXDefineContext dc{};
    dc.cid = 101;
    auto pkt = makePacket(SVGA_3D_CMD_DX_DEFINE_CONTEXT, &dc, sizeof(dc));
    size_t consumed = 0;
    TEST_CHECK(svga3_vlkn_fifo_execute(dev, pkt.data(), pkt.size(), &consumed) == SVGA3_VLKN_SUCCESS,
               "DX context created");

    struct {
        SVGA3dCmdDXSetRenderTargets hdr;
        uint32_t sids[1];
    } srt{};
    srt.hdr.cid = 101;
    srt.hdr.numTargets = 1;
    srt.sids[0] = 200;
    pkt = makePacket(SVGA_3D_CMD_DX_SET_RENDER_TARGETS, &srt, sizeof(srt));
    TEST_CHECK(svga3_vlkn_fifo_execute(dev, pkt.data(), pkt.size(), &consumed) == SVGA3_VLKN_SUCCESS,
               "DX_SET_RENDER_TARGETS succeeds");

    /* Unknown surface fails. */
    srt.sids[0] = 9999;
    pkt = makePacket(SVGA_3D_CMD_DX_SET_RENDER_TARGETS, &srt, sizeof(srt));
    TEST_CHECK(svga3_vlkn_fifo_execute(dev, pkt.data(), pkt.size(), &consumed) != SVGA3_VLKN_SUCCESS,
               "unknown surface fails");

    /* Truncated payload fails. */
    pkt = makePacket(SVGA_3D_CMD_DX_SET_RENDER_TARGETS, &srt, sizeof(srt) - 1);
    TEST_CHECK(svga3_vlkn_fifo_execute(dev, pkt.data(), pkt.size(), &consumed) != SVGA3_VLKN_SUCCESS,
               "truncated payload fails");

    svga3_vlkn_device_destroy(dev);
}

static void testClearAndPresent() {
    printf("DX clear and present:\n");
    auto *dev = makeDevice();
    TEST_CHECK(dev != nullptr, "device created");
    if (!dev) return;

    SVGA3dSize size{64, 64, 1};
    TEST_CHECK(svga3_vlkn_surface_define(dev, 201, SVGA3D_SURFACE_HINT_RENDERTARGET,
                                        SVGA3D_A8R8G8B8, &size, 1) == SVGA3_VLKN_SUCCESS,
               "surface defined");

    SVGA3dCmdDXDefineContext dc{};
    dc.cid = 102;
    auto pkt = makePacket(SVGA_3D_CMD_DX_DEFINE_CONTEXT, &dc, sizeof(dc));
    size_t consumed = 0;
    svga3_vlkn_fifo_execute(dev, pkt.data(), pkt.size(), &consumed);

    struct {
        SVGA3dCmdDXSetRenderTargets hdr;
        uint32_t sids[1];
    } srt{};
    srt.hdr.cid = 102;
    srt.hdr.numTargets = 1;
    srt.sids[0] = 201;
    pkt = makePacket(SVGA_3D_CMD_DX_SET_RENDER_TARGETS, &srt, sizeof(srt));
    svga3_vlkn_fifo_execute(dev, pkt.data(), pkt.size(), &consumed);

    SVGA3dCmdDXClearRenderTarget clr{};
    clr.cid = 102;
    clr.sid = 201;
    clr.color[0] = 1.0f; clr.color[1] = 0.0f; clr.color[2] = 0.0f; clr.color[3] = 1.0f;
    pkt = makePacket(SVGA_3D_CMD_DX_CLEAR_RENDER_TARGET, &clr, sizeof(clr));
    TEST_CHECK(svga3_vlkn_fifo_execute(dev, pkt.data(), pkt.size(), &consumed) == SVGA3_VLKN_SUCCESS,
               "DX_CLEAR_RENDER_TARGET succeeds");

    SVGA3dCmdDXPresent pr{};
    pr.cid = 102;
    pr.sid = 201;
    pkt = makePacket(SVGA_3D_CMD_DX_PRESENT, &pr, sizeof(pr));
    TEST_CHECK(svga3_vlkn_fifo_execute(dev, pkt.data(), pkt.size(), &consumed) == SVGA3_VLKN_SUCCESS,
               "DX_PRESENT succeeds");

    svga3_vlkn_device_destroy(dev);
}

static void testFailClosed() {
    printf("DX fail-closed:\n");
    auto *dev = makeDevice();
    TEST_CHECK(dev != nullptr, "device created");
    if (!dev) return;

    size_t bytesRead = 0;
    /* Unimplemented DX command ID in range: dispatcher must fail, not skip. */
    uint32_t cmd = SVGA_3D_CMD_DX_BASE + 7;
    Svga3VlknStatus st = svga3_vlkn::svga3_dx_dispatch(dev, cmd, nullptr, 0, &bytesRead);
    TEST_CHECK(st == SVGA3_VLKN_ERROR_INVALID_PARAM,
               "null payload rejected");
    uint8_t dummy[4] = {0};
    st = svga3_vlkn::svga3_dx_dispatch(dev, cmd, dummy, sizeof(dummy), &bytesRead);
    TEST_CHECK(st == SVGA3_VLKN_ERROR_UNSUPPORTED_COMMAND,
               "unimplemented DX command fails closed");

    /* GB-flagged surface: DX path rejects it. */
    SVGA3dSize size{16, 16, 1};
    uint32_t gbFlags = SVGA3D_SURFACE_HINT_RENDERTARGET | SVGA3D_SURFACE_FLAG_GB;
    TEST_CHECK(svga3_vlkn_surface_define(dev, 202, gbFlags,
                                        SVGA3D_A8R8G8B8, &size, 1) == SVGA3_VLKN_SUCCESS,
               "GB surface defined");

    SVGA3dCmdDXPresent pr{};
    pr.cid = 103;
    pr.sid = 202;
    st = svga3_vlkn::svga3_dx_dispatch(dev, SVGA_3D_CMD_DX_PRESENT,
                                      reinterpret_cast<uint8_t*>(&pr), sizeof(pr), &bytesRead);
    TEST_CHECK(st == SVGA3_VLKN_ERROR_UNSUPPORTED_COMMAND,
               "GB surface fails closed on DX present");

    svga3_vlkn_device_destroy(dev);
}

static void testD3D9Unaffected() {
    printf("D3D9 path unaffected:\n");
    auto *dev = makeDevice();
    TEST_CHECK(dev != nullptr, "device created");
    if (!dev) return;

    /* D3D9 context define still works through the classic path. */
    TEST_CHECK(svga3_vlkn_context_create(dev, 300) == SVGA3_VLKN_SUCCESS,
               "D3D9 context create works");

    SVGA3dSize size{32, 32, 1};
    TEST_CHECK(svga3_vlkn_surface_define(dev, 300, SVGA3D_SURFACE_HINT_RENDERTARGET,
                                        SVGA3D_A8R8G8B8, &size, 1) == SVGA3_VLKN_SUCCESS,
               "D3D9 surface define works");

    /* A DX command ID outside the DX range but in the 3D range still
     * hits the graceful skip, not the DX dispatcher. */
    size_t consumed = 0;
    auto pkt = makePacket(SVGA_3D_CMD_BASE + 35, nullptr, 0);
    Svga3VlknStatus st = svga3_vlkn_fifo_execute(dev, pkt.data(), pkt.size(), &consumed);
    TEST_CHECK(st == SVGA3_VLKN_SUCCESS,
               "non-DX unknown 3D command still skips gracefully");

    svga3_vlkn_device_destroy(dev);
}

int main() {
    printf("=== DX command path tests ===\n");
    testDefineDestroyContext();
    testSetRenderTargets();
    testClearAndPresent();
    testFailClosed();
    testD3D9Unaffected();
    printf("=== %s (%d failures) ===\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
