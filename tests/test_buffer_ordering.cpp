/* Real Vulkan regression: a queued draw retains vertices after a CPU update. */
#include "svga3_device.h"
#include <cstdio>
#include <cstring>
#include <vector>
#define DST(t, n) (0x800f0000u | (((t) & 7) << 28) | (((t) & 24) << 8) | (n))
#define SRC(t, n) (0x80e40000u | (((t) & 7) << 28) | (((t) & 24) << 8) | (n))
int main() {
  Svga3VlknConfig cfg{};
  cfg.apiVersion = VK_API_VERSION_1_1;
  cfg.enableValidationLayers = true;
  auto *d = svga3_vlkn_device_create(&cfg);
  if (!d)
    return 1;
  svga3_vlkn_context_create(d, 1);
  SVGA3dSize rt{64, 64, 1};
  svga3_vlkn_surface_define(d, 1, SVGA3D_SURFACE_HINT_RENDERTARGET,
                            SVGA3D_A8R8G8B8, &rt, 1);
  svga3_vlkn_context_set_render_target(d, 1, SVGA3D_RT_COLOR0, 1, 0, 0);
  SVGA3dRect vp{0, 0, 64, 64};
  svga3_vlkn_context_set_viewport(d, 1, &vp);
  svga3_vlkn_context_set_render_state(d, 1, SVGA3D_RS_CULLMODE,
                                      SVGA3D_FACE_NONE);
  const uint32_t vs[] = {0xfffe0300,    31 | (2 << 24), 0x80000000, DST(1, 0),
                         1 | (2 << 24), DST(4, 0),      SRC(1, 0),  0xffff};
  const uint32_t ps[] = {0xffff0300, 1 | (2 << 24), DST(8, 0), SRC(2, 0),
                         0xffff};
  printf("shader statuses=%d/%d\n",
         svga3_vlkn_context_define_shader(d, 1, 1, SVGA3D_SHADERTYPE_VS, vs,
                                          sizeof(vs) / 4),
         svga3_vlkn_context_define_shader(d, 1, 1, SVGA3D_SHADERTYPE_PS, ps,
                                          sizeof(ps) / 4));
  svga3_vlkn_context_set_shader(d, 1, SVGA3D_SHADERTYPE_VS, 1);
  svga3_vlkn_context_set_shader(d, 1, SVGA3D_SHADERTYPE_PS, 1);
  float red[4] = {1, 0, 0, 1};
  svga3_vlkn_context_set_shader_const(d, 1, 0, SVGA3D_SHADERTYPE_PS,
                                      SVGA3D_CONST_TYPE_FLOAT,
                                      reinterpret_cast<uint32_t *>(red));
  float verts[3][4] = {{-1, -1, .5, 1}, {0, -1, .5, 1}, {-1, 1, .5, 1}};
  SVGA3dSize vb{sizeof(verts), 1, 1};
  svga3_vlkn_surface_define(d, 2, SVGA3D_SURFACE_HINT_VERTEXBUFFER,
                            SVGA3D_BUFFER, &vb, 1);
  SVGA3dBox box{0, 0, 0, sizeof(verts), 1, 1};
  svga3_vlkn_surface_dma_upload(d, 2, 0, &box, verts, sizeof(verts));
  svga3_vlkn_context_clear(d, 1, SVGA3D_CLEAR_COLOR, 0xff000000, 1, 0, nullptr,
                           0);
  SVGA3dVertexDecl decl{};
  decl.identity.type = SVGA3D_DECLTYPE_FLOAT4;
  decl.identity.usage = SVGA3D_DECLUSAGE_POSITION;
  decl.array.surfaceId = 2;
  decl.array.stride = 16;
  SVGA3dPrimitiveRange r{};
  r.primType = SVGA3D_PRIMITIVE_TRIANGLELIST;
  r.primitiveCount = 1;
  r.indexArray.surfaceId = SVGA3D_INVALID_ID;
  printf("draw status=%d\n",
         svga3_vlkn_context_draw(d, 1, SVGA3D_PRIMITIVE_TRIANGLELIST, &decl, 1,
                                 &r, 1));
  for (auto &v : verts)
    v[0] += 1;
  svga3_vlkn_surface_dma_upload(d, 2, 0, &box, verts, sizeof(verts));
  std::vector<uint32_t> pixels(64 * 64);
  svga3_vlkn_surface_dma_download(d, 1, 0, nullptr, pixels.data(), 64 * 4);
  unsigned left = 0, right = 0;
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++)
      if (pixels[y * 64 + x] & 0x00ff0000)
        (x < 32 ? left : right)++;
  printf("red pixels left=%u right=%u (expected left>0 right=0)\n", left,
         right);
  bool correct = left > 0 && right == 0;
  SVGA3dSize compressedSize{4, 4, 1};
  correct &=
      svga3_vlkn_surface_define(d, 3, SVGA3D_SURFACE_HINT_TEXTURE, SVGA3D_DXT1,
                                &compressedSize, 1) == SVGA3_VLKN_SUCCESS;
  const uint8_t block[8] = {0, 0xf8, 0xe0, 7, 0x55, 0xaa, 0x33, 0xcc};
  uint8_t output[8] = {};
  correct &= svga3_vlkn_surface_dma_upload(d, 3, 0, nullptr, block, 8) ==
             SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_surface_dma_download(d, 3, 0, nullptr, output, 8) ==
             SVGA3_VLKN_SUCCESS;
  correct &= memcmp(block, output, sizeof(block)) == 0;
  correct &= svga3_vlkn_surface_define(d, 4, SVGA3D_SURFACE_HINT_RENDERTARGET,
                                       SVGA3D_DXT1, &compressedSize, 1) ==
             SVGA3_VLKN_ERROR_UNSUPPORTED_FORMAT;
  printf("compressed texture block roundtrip and attachment rejection: %s\n",
         correct ? "PASS" : "FAIL");
  // The guest adapter must use block rows as well as the direct DMA API.
  SVGA3dSize bcSize{8, 8, 1};
  correct &=
      svga3_vlkn_surface_define(d, 20, SVGA3D_SURFACE_HINT_TEXTURE, SVGA3D_DXT1,
                                &bcSize, 1) == SVGA3_VLKN_SUCCESS;
  correct &=
      svga3_vlkn_surface_define(d, 21, SVGA3D_SURFACE_HINT_TEXTURE, SVGA3D_DXT1,
                                &compressedSize, 1) == SVGA3_VLKN_SUCCESS;
  uint8_t blocks[32], bcRead[32]{};
  for (unsigned i = 0; i < 32; ++i)
    blocks[i] = uint8_t(i + 1);
  SVGA3dGuestImage guest{};
  guest.pitch = 16;
  SVGA3dSurfaceImageId image{20, 0, 0};
  SVGA3dCopyBox bcBox{};
  bcBox.w = 8;
  bcBox.h = 8;
  bcBox.d = 1;
  // FIFO DMA must end a recorded draw before transfer commands.
  correct &= svga3_vlkn_context_draw(d, 1, SVGA3D_PRIMITIVE_TRIANGLELIST,
                                      &decl, 1, &r, 1) == SVGA3_VLKN_SUCCESS;
  correct &= d->surfaceMgr->surfaceDMA(guest, image, SVGA3D_WRITE_HOST_VRAM,
                                       &bcBox, 1, nullptr, blocks,
                                       sizeof(blocks)) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_context_draw(d, 1, SVGA3D_PRIMITIVE_TRIANGLELIST,
                                      &decl, 1, &r, 1) == SVGA3_VLKN_SUCCESS;
  correct &= d->surfaceMgr->surfaceDMA(guest, image, SVGA3D_READ_HOST_VRAM,
                                       &bcBox, 1, nullptr, bcRead,
                                       sizeof(bcRead)) == SVGA3_VLKN_SUCCESS;
  correct &= memcmp(blocks, bcRead, 32) == 0;
  correct &= d->surfaceMgr->surfaceDMA(guest, image, SVGA3D_WRITE_HOST_VRAM,
                                       &bcBox, 1, nullptr, blocks,
                                       31) == SVGA3_VLKN_ERROR_INVALID_PARAM;
  image.sid = 21;
  bcBox.w = 4;
  bcBox.h = 4;
  bcBox.srcx = 4;
  bcBox.srcy = 4;
  correct &=
      d->surfaceMgr->surfaceDMA(guest, image, SVGA3D_WRITE_HOST_VRAM, &bcBox, 1,
                                nullptr, blocks, 32) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_surface_dma_download(d, 21, 0, nullptr, bcRead, 8) ==
             SVGA3_VLKN_SUCCESS;
  correct &= memcmp(blocks + 24, bcRead, 8) == 0;
  std::vector<uint8_t> guestPage(4096, 0xcd);
  memcpy(guestPage.data(), blocks, 32);
  correct &= d->guestMem->setFramebuffer(guestPage.data(), 0, guestPage.size(),
                                         8, 8, 32, 4) == SVGA3_VLKN_SUCCESS;
  guest.ptr.gmrId = SVGA_GMR_FRAMEBUFFER;
  correct &=
      d->surfaceMgr->surfaceDMA(guest, image, SVGA3D_WRITE_HOST_VRAM, &bcBox, 1,
                                d->guestMem.get()) == SVGA3_VLKN_SUCCESS;
  memset(guestPage.data(), 0xcd, guestPage.size());
  correct &=
      d->surfaceMgr->surfaceDMA(guest, image, SVGA3D_READ_HOST_VRAM, &bcBox, 1,
                                d->guestMem.get()) == SVGA3_VLKN_SUCCESS;
  correct &= memcmp(guestPage.data() + 24, blocks + 24, 8) == 0 &&
             guestPage[23] == 0xcd && guestPage[32] == 0xcd;
  printf("compressed guest/FIFO adapter and exact buffer bounds: %s\n",
         correct ? "PASS" : "FAIL");
  SVGA3dSize depthSize{64, 64, 1};
  correct &= svga3_vlkn_surface_define(d, 5, 0, SVGA3D_Z_D16, &depthSize, 1) ==
             SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_context_set_render_target(d, 1, SVGA3D_RT_DEPTH, 5, 0,
                                                  0) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_context_clear(d, 1, SVGA3D_CLEAR_DEPTH, 0, 0.25f, 0,
                                      nullptr, 0) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_context_set_render_target(d, 1, SVGA3D_RT_DEPTH,
                                                  SVGA3D_INVALID_ID, 0,
                                                  0) == SVGA3_VLKN_SUCCESS;
  const uint32_t depthPS[] = {0xffff0300, 31 | (2 << 24), 0x90000000,
                              DST(10, 0), 66 | (3 << 24), DST(0, 0),
                              SRC(2, 1),  SRC(10, 0),     1 | (2 << 24),
                              DST(8, 0),  SRC(0, 0),      0xffff};
  correct &= svga3_vlkn_context_define_shader(d, 1, 2, SVGA3D_SHADERTYPE_PS,
                                              depthPS, sizeof(depthPS) / 4) ==
             SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_context_set_shader(d, 1, SVGA3D_SHADERTYPE_PS, 2) ==
             SVGA3_VLKN_SUCCESS;
  const float uv[4] = {0.5f, 0.5f, 0, 0};
  correct &= svga3_vlkn_context_set_shader_const(
                 d, 1, 1, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT,
                 reinterpret_cast<const uint32_t *>(uv)) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_context_set_texture(d, 1, 0, 5) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_context_clear(d, 1, SVGA3D_CLEAR_COLOR, 0xff000000, 1,
                                      0, nullptr, 0) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_context_draw(d, 1, SVGA3D_PRIMITIVE_TRIANGLELIST, &decl,
                                     1, &r, 1) == SVGA3_VLKN_SUCCESS;
  /* Changing sampler state must preserve descriptors for earlier draws and
   * reuse identical states rather than exhausting the global pool. */
  for (unsigned i = 0; i < 320; ++i) {
    correct &= svga3_vlkn_context_set_texture_stage_state(
                   d, 1, 0, SVGA3D_TS_MINFILTER,
                   i % 2 ? SVGA3D_TEX_FILTER_LINEAR
                         : SVGA3D_TEX_FILTER_NEAREST) == SVGA3_VLKN_SUCCESS;
    correct &= svga3_vlkn_context_draw(d, 1, SVGA3D_PRIMITIVE_TRIANGLELIST,
                                       &decl, 1, &r, 1) == SVGA3_VLKN_SUCCESS;
  }
  correct &= svga3_vlkn_surface_dma_download(d, 1, 0, nullptr, pixels.data(),
                                             64 * 4) == SVGA3_VLKN_SUCCESS;
  unsigned grayCount = 0;
  for (auto p : pixels)
    if ((p & 0xffffff) == 0x404040)
      ++grayCount;
  correct &= grayCount > 0;
  const uint32_t depthPixel = pixels[48 * 64 + 40];
  correct &= (depthPixel & 0x00ffffff) == 0x00404040;
  printf("depth texture sample pixel=0x%08x expected=0xff404040\n", depthPixel);
  /* Retire a cache with more than 64 distinct sampler states safely. */
  for (unsigned i = 0; i < 80; ++i) {
    float bias = float(i) / 16;
    uint32_t bits;
    memcpy(&bits, &bias, 4);
    correct &=
        svga3_vlkn_context_set_texture_stage_state(
            d, 1, 0, SVGA3D_TS_TEXTURE_LOD_BIAS, bits) == SVGA3_VLKN_SUCCESS;
    correct &= svga3_vlkn_context_draw(d, 1, SVGA3D_PRIMITIVE_TRIANGLELIST,
                                       &decl, 1, &r, 1) == SVGA3_VLKN_SUCCESS;
  }
  /* A single tracked layout must stay valid across base and non-base mip
   * transfers, GPU copies, scaled blits and mip generation. */
  const SVGA3dSize levels[] = {{8, 8, 1}, {4, 4, 1}, {2, 2, 1}, {1, 1, 1}};
  correct &= svga3_vlkn_surface_define(d, 6, SVGA3D_SURFACE_HINT_TEXTURE,
                                       SVGA3D_A8R8G8B8, levels,
                                       4) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_surface_define(d, 7, SVGA3D_SURFACE_HINT_TEXTURE,
                                       SVGA3D_A8R8G8B8, levels,
                                       4) == SVGA3_VLKN_SUCCESS;
  std::vector<uint32_t> solid(64, 0xff123456), small(16, 0xffabcdef), read(64);
  correct &= svga3_vlkn_surface_dma_upload(d, 6, 0, nullptr, solid.data(),
                                           32) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_surface_dma_upload(d, 6, 1, nullptr, small.data(),
                                           16) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_surface_dma_download(d, 6, 0, nullptr, read.data(),
                                             32) == SVGA3_VLKN_SUCCESS;
  correct &= read == solid;
  SVGA3dCopyBox region{};
  region.w = 8;
  region.h = 8;
  region.d = 1;
  correct &= svga3_vlkn_surface_copy(d, 6, 7, &region, 1) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_surface_dma_download(d, 7, 0, nullptr, read.data(),
                                             32) == SVGA3_VLKN_SUCCESS;
  correct &= read == solid;
  SVGA3dBox full{0, 0, 0, 8, 8, 1};
  correct &= svga3_vlkn_surface_stretch_blt(d, 6, 7, &full, &full,
                                            SVGA3D_STRETCH_BLT_LINEAR) ==
             SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_surface_generate_mipmaps(
                 d, 7, SVGA3D_TEX_FILTER_LINEAR) == SVGA3_VLKN_SUCCESS;
  std::vector<uint32_t> generated(16);
  correct &= svga3_vlkn_surface_dma_download(d, 7, 1, nullptr, generated.data(),
                                             16) == SVGA3_VLKN_SUCCESS;
  correct &= generated == std::vector<uint32_t>(16, solid[0]);
  printf("sampler retirement and image transfer regressions: %s\n",
         correct ? "PASS" : "FAIL");
  /* Allocating constants before a framebuffer-change flush used to let
   * the next draw reuse the first draw's slot before it was submitted. */
  correct &= svga3_vlkn_device_wait_idle(d) == SVGA3_VLKN_SUCCESS;
  correct &=
      svga3_vlkn_surface_define(d, 8, SVGA3D_SURFACE_HINT_RENDERTARGET,
                                SVGA3D_A8R8G8B8, &rt, 1) == SVGA3_VLKN_SUCCESS;
  const float pair[6][4] = {{-1, -1, .5, 1}, {0, -1, .5, 1}, {-1, 1, .5, 1},
                            {0, -1, .5, 1},  {1, -1, .5, 1}, {0, 1, .5, 1}};
  SVGA3dSize pairSize{sizeof(pair), 1, 1};
  correct &= svga3_vlkn_surface_define(d, 9, SVGA3D_SURFACE_HINT_VERTEXBUFFER,
                                       SVGA3D_BUFFER, &pairSize,
                                       1) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_surface_dma_upload(d, 9, 0, nullptr, pair,
                                           sizeof(pair)) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_context_set_shader(d, 1, SVGA3D_SHADERTYPE_PS, 1) ==
             SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_context_set_texture(d, 1, 0, SVGA3D_INVALID_ID) ==
             SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_context_set_render_target(d, 1, SVGA3D_RT_COLOR0, 1, 0,
                                                  0) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_context_clear(d, 1, SVGA3D_CLEAR_COLOR, 0xff000000, 1,
                                      0, nullptr, 0) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_context_set_render_target(d, 1, SVGA3D_RT_COLOR0, 8, 0,
                                                  0) == SVGA3_VLKN_SUCCESS;
  decl.array.surfaceId = 9;
  r.indexBias = 0;
  correct &= svga3_vlkn_context_set_shader_const(
                 d, 1, 0, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT,
                 reinterpret_cast<uint32_t *>(red)) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_context_draw(d, 1, SVGA3D_PRIMITIVE_TRIANGLELIST, &decl,
                                     1, &r, 1) == SVGA3_VLKN_SUCCESS;
  const float blue[4] = {0, 0, 1, 1};
  r.indexBias = 3;
  correct &=
      svga3_vlkn_context_set_shader_const(
          d, 1, 0, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT,
          reinterpret_cast<const uint32_t *>(blue)) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_context_draw(d, 1, SVGA3D_PRIMITIVE_TRIANGLELIST, &decl,
                                     1, &r, 1) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_surface_dma_download(d, 8, 0, nullptr, pixels.data(),
                                             256) == SVGA3_VLKN_SUCCESS;
  correct &= (pixels[48 * 64 + 8] & 0xffffff) == 0xff0000 &&
             (pixels[48 * 64 + 40] & 0xffffff) == 0x0000ff;
  printf("constants across framebuffer submission: %s (%08x/%08x)\n",
         correct ? "PASS" : "FAIL", pixels[48 * 64 + 8], pixels[48 * 64 + 40]);
  const uint32_t comparePS[] = {0xffff0300, 12 | (3 << 24), DST(8, 0),
                                SRC(2, 0),  SRC(2, 1),      0xffff};
  correct &= svga3_vlkn_context_define_shader(
                 d, 1, 3, SVGA3D_SHADERTYPE_PS, comparePS,
                 sizeof(comparePS) / 4) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_context_set_shader(d, 1, SVGA3D_SHADERTYPE_PS, 3) ==
             SVGA3_VLKN_SUCCESS;
  const float lessA[4] = {0.2f, 0.5f, 0.8f, 1},
              lessB[4] = {0.1f, 0.6f, 0.8f, 2};
  correct &=
      svga3_vlkn_context_set_shader_const(
          d, 1, 0, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT,
          reinterpret_cast<const uint32_t *>(lessA)) == SVGA3_VLKN_SUCCESS;
  correct &=
      svga3_vlkn_context_set_shader_const(
          d, 1, 1, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT,
          reinterpret_cast<const uint32_t *>(lessB)) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_context_draw(d, 1, SVGA3D_PRIMITIVE_TRIANGLELIST, &decl,
                                     1, &r, 1) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_surface_dma_download(d, 8, 0, nullptr, pixels.data(),
                                             256) == SVGA3_VLKN_SUCCESS;
  correct &= pixels[48 * 64 + 40] == 0xff00ff00;
  printf("SLT less/equal/greater comparisons: %s\n", correct ? "PASS" : "FAIL");
  const uint32_t discardPS[] = {
      0xffff0300, 1 | (2 << 24), DST(0, 0), SRC(2, 0), 65 | (1 << 24),
      DST(0, 0),  1 | (2 << 24), DST(8, 0), SRC(2, 1), 0xffff};
  correct &= svga3_vlkn_context_define_shader(
                 d, 1, 4, SVGA3D_SHADERTYPE_PS, discardPS,
                 sizeof(discardPS) / 4) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_context_set_shader(d, 1, SVGA3D_SHADERTYPE_PS, 4) ==
             SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_context_clear(d, 1, SVGA3D_CLEAR_COLOR, 0xff0000ff, 1,
                                      0, nullptr, 0) == SVGA3_VLKN_SUCCESS;
  const float negativeZ[4] = {0, 0, -1, 1}, positive[4] = {0, 0, 0, 1};
  correct &=
      svga3_vlkn_context_set_shader_const(
          d, 1, 0, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT,
          reinterpret_cast<const uint32_t *>(negativeZ)) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_context_set_shader_const(
                 d, 1, 1, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT,
                 reinterpret_cast<const uint32_t *>(red)) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_context_draw(d, 1, SVGA3D_PRIMITIVE_TRIANGLELIST, &decl,
                                     1, &r, 1) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_surface_dma_download(d, 8, 0, nullptr, pixels.data(),
                                             256) == SVGA3_VLKN_SUCCESS;
  correct &= pixels[48 * 64 + 40] == 0xff0000ff;
  correct &=
      svga3_vlkn_context_set_shader_const(
          d, 1, 0, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT,
          reinterpret_cast<const uint32_t *>(positive)) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_context_draw(d, 1, SVGA3D_PRIMITIVE_TRIANGLELIST, &decl,
                                     1, &r, 1) == SVGA3_VLKN_SUCCESS;
  correct &= svga3_vlkn_surface_dma_download(d, 8, 0, nullptr, pixels.data(),
                                             256) == SVGA3_VLKN_SUCCESS;
  correct &= pixels[48 * 64 + 40] == 0xffff0000;
  printf("TEXKILL negative and nonnegative pixel tests: %s\n",
         correct ? "PASS" : "FAIL");
  for (unsigned negate = 0; negate < 2; ++negate) {
    const uint32_t outputPredPS[] = {0xffff0300,
                                     1 | (2 << 24),
                                     DST(8, 0),
                                     SRC(2, 2),
                                     94 | (3 << 24) | (4 << 16),
                                     DST(19, 0),
                                     SRC(2, 0),
                                     SRC(2, 1),
                                     1 | (3 << 24) | (1u << 28),
                                     DST(8, 0),
                                     SRC(19, 0) | (negate ? (1u << 13) : 0),
                                     SRC(2, 3),
                                     0xffff};
    correct &= svga3_vlkn_context_define_shader(
                   d, 1, 5, SVGA3D_SHADERTYPE_PS, outputPredPS,
                   sizeof(outputPredPS) / 4) == SVGA3_VLKN_SUCCESS;
    correct &= svga3_vlkn_context_set_shader(d, 1, SVGA3D_SHADERTYPE_PS, 5) ==
               SVGA3_VLKN_SUCCESS;
    for (unsigned ci = 0; ci < 4; ++ci) {
      const float *c = ci == 0 ? lessA : ci == 1 ? lessB : ci == 2 ? blue : red;
      correct &=
          svga3_vlkn_context_set_shader_const(
              d, 1, ci, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_FLOAT,
              reinterpret_cast<const uint32_t *>(c)) == SVGA3_VLKN_SUCCESS;
    }
    correct &= svga3_vlkn_context_draw(d, 1, SVGA3D_PRIMITIVE_TRIANGLELIST,
                                       &decl, 1, &r, 1) == SVGA3_VLKN_SUCCESS;
    correct &= svga3_vlkn_surface_dma_download(d, 8, 0, nullptr, pixels.data(),
                                               256) == SVGA3_VLKN_SUCCESS;
    correct &= pixels[48 * 64 + 40] == (negate ? 0xffff0000 : 0xff0000ff);
  }
  printf("predicated output and negated predicate pixels: %s\n",
         correct ? "PASS" : "FAIL");
  d->contextMgr->clear();
  d->surfaceMgr->clear();
  correct &= d->backend->waitIdle() == SVGA3_VLKN_SUCCESS;
  d->backend->shutdown();
  correct &= d->backend->validationErrors() == 0 &&
             d->backend->validationWarnings() == 0;
  svga3_vlkn_device_destroy(d);
  return correct ? 0 : 1;
}
