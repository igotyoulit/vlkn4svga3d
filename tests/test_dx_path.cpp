/* Real wire numbers, namespace isolation, malformed packets and reset. */
#include "svga3_device.h"
#include "svga3_dx.h"
#include <cstdio>
#include <cstring>
#include <vector>
static unsigned failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static Svga3VlknStatus packet(Svga3VlknDevice *d, uint32_t cmd,
                             const void *data, uint32_t size) {
    std::vector<uint8_t> wire(8 + size);
    memcpy(wire.data(), &cmd, 4);
    memcpy(wire.data() + 4, &size, 4);
    if (size) memcpy(wire.data() + 8, data, size);
    size_t consumed = 0;
    auto st = svga3_vlkn_fifo_execute(d, wire.data(), wire.size(), &consumed);
    CHECK(consumed == wire.size());
    return st;
}
int main() {
    Svga3VlknConfig cfg{};
    cfg.forceMockBackend = true;
    cfg.maxContexts = 2;
    auto *d = svga3_vlkn_device_create(&cfg);
    if (!d) return 1;
    uint32_t cid = 7;
    CHECK(svga3_vlkn_context_create(d, cid) == SVGA3_VLKN_SUCCESS);
    // Literal wire opcodes ensure the fixtures do not mirror invented constants.
    CHECK(packet(d, 1143, &cid, 4) == SVGA3_VLKN_SUCCESS);
    CHECK(d->dxContexts.count(cid) == 1);
    CHECK(d->contextMgr->getContext(cid) != nullptr);
    CHECK(packet(d, 1143, &cid, 4) == SVGA3_VLKN_ERROR_ALREADY_EXISTS);
    uint32_t second = 8, third = 9;
    CHECK(packet(d, 1143, &second, 4) == SVGA3_VLKN_SUCCESS);
    CHECK(packet(d, 1143, &third, 4) == SVGA3_VLKN_ERROR_OUT_OF_MEMORY);
    CHECK(packet(d, 1144, &cid, 4) == SVGA3_VLKN_SUCCESS);
    CHECK(d->contextMgr->getContext(cid) != nullptr);
    CHECK(packet(d, 1144, &cid, 4) == SVGA3_VLKN_ERROR_NOT_FOUND);
    CHECK(packet(d, 1143, &cid, 3) == SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER);
    const uint32_t extra[] = {7, 0};
    CHECK(packet(d, 1143, extra, 8) == SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER);
    CHECK(packet(d, 1143, nullptr, 0) == SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER);
    uint32_t invalid = SVGA3D_INVALID_ID;
    CHECK(packet(d, 1143, &invalid, 4) == SVGA3_VLKN_ERROR_INVALID_PARAM);
    size_t read = 123;
    // Binding, real view-based RT/clear, presentblt and invented opcodes reject.
    for (auto cmd : {1145u, 1161u, 1176u, 1180u, 1291u}) {
        CHECK(svga3_vlkn::processFifoPacket(d, cmd, (const uint8_t*)extra,
                                           sizeof(extra), &read) == SVGA3_VLKN_ERROR_UNSUPPORTED_COMMAND);
        CHECK(d->contextMgr->getContext(cid) != nullptr);
    }
    CHECK(svga3_vlkn::svga3_dx_dispatch(d, 1500, (const uint8_t*)extra,
                                       sizeof(extra), &read) == SVGA3_VLKN_ERROR_UNSUPPORTED_COMMAND);
    CHECK(packet(d, 1500, extra, 8) == SVGA3_VLKN_SUCCESS);
    CHECK(d->dxContexts.count(cid) == 0);
    // FIFO skips unsupported packets but must preserve framing for the next command.
    CHECK(packet(d, 1161, extra, 8) == SVGA3_VLKN_SUCCESS);
    CHECK(packet(d, 1143, &cid, 4) == SVGA3_VLKN_SUCCESS);
    // Unaligned payload is valid wire data; dispatch must copy rather than dereference.
    uint8_t unaligned[5]; memcpy(unaligned + 1, &cid, 4);
    CHECK(svga3_vlkn::svga3_dx_dispatch(d, 1144, unaligned + 1, 4, &read) == SVGA3_VLKN_SUCCESS);
    CHECK(read == 4);
    CHECK(svga3_vlkn_device_reset(d) == SVGA3_VLKN_SUCCESS);
    CHECK(d->dxContexts.empty());
    CHECK(packet(d, 1143, &second, 4) == SVGA3_VLKN_SUCCESS);
    CHECK(packet(d, 1144, &second, 4) == SVGA3_VLKN_SUCCESS);
    svga3_vlkn_device_destroy(d);
    printf("DX wire lifecycle and isolation: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
