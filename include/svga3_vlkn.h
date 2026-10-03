/*
 * SVGA3=VLKN - High Performance SVGA3D to Vulkan Translation Engine
 *
 * Header: svga3_vlkn.h
 * Description: Public C and C++ API for SVGA3D protocol execution on Vulkan.
 */

#ifndef ___SVGA3_VLKN_H___
#define ___SVGA3_VLKN_H___

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "vulkan/vulkan.h"
#include "vmsvga/svga_reg.h"
#include "vmsvga/svga3d_reg.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Status and error codes
 */
typedef enum Svga3VlknStatus {
    SVGA3_VLKN_SUCCESS = 0,
    SVGA3_VLKN_ERROR_INVALID_PARAM = -1,
    SVGA3_VLKN_ERROR_OUT_OF_MEMORY = -2,
    SVGA3_VLKN_ERROR_NOT_FOUND = -3,
    SVGA3_VLKN_ERROR_ALREADY_EXISTS = -4,
    SVGA3_VLKN_ERROR_VULKAN_INIT_FAILED = -5,
    SVGA3_VLKN_ERROR_DEVICE_LOST = -6,
    SVGA3_VLKN_ERROR_UNSUPPORTED_FORMAT = -7,
    SVGA3_VLKN_ERROR_UNSUPPORTED_COMMAND = -8,
    SVGA3_VLKN_ERROR_PIPELINE_CREATION = -9,
    SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER = -10,
    SVGA3_VLKN_ERROR_UNSUPPORTED_SHADER = -11,
    SVGA3_VLKN_ERROR_SHADER_TRANSLATION = -12,
} Svga3VlknStatus;

/*
 * Configuration structure for device initialization
 */
typedef struct Svga3VlknConfig {
    const char *appName;
    uint32_t    apiVersion;             /* e.g. VK_API_VERSION_1_0 */
    bool        enableValidationLayers; /* Enable VK_LAYER_KHRONOS_validation */
    bool        preferIntegratedGpu;    /* For power savings or testing */
    bool        forceMockBackend;       /* Force headless mock Vulkan driver for testing */
    uint32_t    maxSurfaces;            /* Surface capacity (default: 4096) */
    uint32_t    maxContexts;            /* Context capacity (default: 256) */
    size_t      stagingBufferSize;      /* Staging buffer size in bytes (default: 64MB) */
} Svga3VlknConfig;

/*
 * Device performance and execution statistics
 */
typedef struct Svga3VlknStats {
    uint64_t totalCommandsProcessed;
    uint64_t drawCallsSubmitted;
    uint64_t verticesRendered;
    uint64_t primitivesRendered;
    uint64_t pipelinesCreated;
    uint64_t surfacesCreated;
    uint64_t surfacesDestroyed;
    uint64_t dmaBytesTransferred;
    uint64_t renderPassesExecuted;
} Svga3VlknStats;

/*
 * Opaque device and resource handles
 */
typedef struct Svga3VlknDevice Svga3VlknDevice;
typedef struct Svga3VlknContext Svga3VlknContext;
typedef struct Svga3VlknSurface Svga3VlknSurface;

/*
 * Device Lifecycle API
 */
Svga3VlknDevice *svga3_vlkn_device_create(const Svga3VlknConfig *config);
void             svga3_vlkn_device_destroy(Svga3VlknDevice *dev);
Svga3VlknStatus  svga3_vlkn_device_reset(Svga3VlknDevice *dev);
Svga3VlknStatus  svga3_vlkn_device_wait_idle(Svga3VlknDevice *dev);
void             svga3_vlkn_get_stats(Svga3VlknDevice *dev, Svga3VlknStats *outStats);

/*
 * Capability queries (Oracle-validated)
 */
uint32_t         svga3_vlkn_query_cap(Svga3VlknDevice *dev, uint32_t capIndex, uint32_t *outCapValue);

/*
 * Context Management API
 */
Svga3VlknStatus  svga3_vlkn_context_create(Svga3VlknDevice *dev, uint32_t cid);
Svga3VlknStatus  svga3_vlkn_context_destroy(Svga3VlknDevice *dev, uint32_t cid);
bool             svga3_vlkn_context_exists(Svga3VlknDevice *dev, uint32_t cid);

/*
 * Surface Management API
 */
Svga3VlknStatus  svga3_vlkn_surface_define(Svga3VlknDevice *dev,
                                           uint32_t sid,
                                           uint32_t surfaceFlags,
                                           SVGA3dSurfaceFormat format,
                                           const SVGA3dSize *sizes,
                                           uint32_t numSizes);

Svga3VlknStatus  svga3_vlkn_surface_define_v2(Svga3VlknDevice *dev,
                                              uint32_t sid,
                                              uint32_t surfaceFlags,
                                              SVGA3dSurfaceFormat format,
                                              uint32_t multisampleCount,
                                              SVGA3dTextureFilter autogenFilter,
                                              const SVGA3dSize *sizes,
                                              uint32_t numSizes);

Svga3VlknStatus  svga3_vlkn_surface_destroy(Svga3VlknDevice *dev, uint32_t sid);
bool             svga3_vlkn_surface_exists(Svga3VlknDevice *dev, uint32_t sid);
Svga3VlknStatus  svga3_vlkn_surface_set_active(Svga3VlknDevice *dev, uint32_t sid, bool active);

/*
 * Surface Data Transfer (DMA) & Blit
 */
Svga3VlknStatus  svga3_vlkn_surface_dma_upload(Svga3VlknDevice *dev,
                                               uint32_t sid,
                                               uint32_t mipLevel,
                                               const SVGA3dBox *box,
                                               const void *guestData,
                                               size_t guestStride);

Svga3VlknStatus  svga3_vlkn_surface_dma_download(Svga3VlknDevice *dev,
                                                 uint32_t sid,
                                                 uint32_t mipLevel,
                                                 const SVGA3dBox *box,
                                                 void *outGuestData,
                                                 size_t guestStride);

Svga3VlknStatus  svga3_vlkn_surface_copy(Svga3VlknDevice *dev,
                                         uint32_t srcSid,
                                         uint32_t dstSid,
                                         const SVGA3dCopyBox *boxes,
                                         uint32_t numBoxes);

Svga3VlknStatus  svga3_vlkn_surface_stretch_blt(Svga3VlknDevice *dev,
                                               uint32_t srcSid,
                                               uint32_t dstSid,
                                               const SVGA3dBox *boxSrc,
                                               const SVGA3dBox *boxDest,
                                               SVGA3dStretchBltMode mode);

Svga3VlknStatus  svga3_vlkn_surface_generate_mipmaps(Svga3VlknDevice *dev,
                                                     uint32_t sid,
                                                     SVGA3dTextureFilter filter);

Svga3VlknStatus  svga3_vlkn_surface_blit_to_screen(Svga3VlknDevice *dev,
                                                   const SVGA3dSurfaceImageId *srcImage,
                                                   const SVGASignedRect *srcRect,
                                                   uint32_t destScreenId,
                                                   const SVGASignedRect *destRect,
                                                   const SVGASignedRect *clipRects,
                                                   uint32_t numClipRects);

Svga3VlknStatus  svga3_vlkn_surface_present(Svga3VlknDevice *dev,
                                            uint32_t sid,
                                            const SVGA3dCopyRect *rects,
                                            uint32_t numRects);

/*
 * FIFO Command Stream Processor
 *
 * Processes a raw SVGA3D command buffer (as written by guest graphics drivers
 * into the FIFO register space).
 */
Svga3VlknStatus  svga3_vlkn_fifo_execute(Svga3VlknDevice *dev,
                                         const void *commandBuffer,
                                         size_t bufferSizeBytes,
                                         size_t *bytesConsumed);

/*
 * Context State Inspection & Direct Manipulation (used by testing and diagnostic tools)
 */
Svga3VlknStatus  svga3_vlkn_context_set_render_state(Svga3VlknDevice *dev,
                                                     uint32_t cid,
                                                     SVGA3dRenderStateName state,
                                                     uint32_t value);

Svga3VlknStatus  svga3_vlkn_context_get_render_state(Svga3VlknDevice *dev,
                                                     uint32_t cid,
                                                     SVGA3dRenderStateName state,
                                                     uint32_t *outValue);

Svga3VlknStatus  svga3_vlkn_context_set_render_target(Svga3VlknDevice *dev,
                                                      uint32_t cid,
                                                      SVGA3dRenderTargetType type,
                                                      uint32_t sid,
                                                      uint32_t face,
                                                      uint32_t mipmap);

Svga3VlknStatus  svga3_vlkn_context_set_viewport(Svga3VlknDevice *dev,
                                                 uint32_t cid,
                                                 const SVGA3dRect *rect);

Svga3VlknStatus  svga3_vlkn_context_set_scissor_rect(Svga3VlknDevice *dev,
                                                     uint32_t cid,
                                                     const SVGA3dRect *rect);

Svga3VlknStatus  svga3_vlkn_context_set_transform(Svga3VlknDevice *dev,
                                                  uint32_t cid,
                                                  SVGA3dTransformType type,
                                                  const float matrix[16]);

Svga3VlknStatus  svga3_vlkn_context_get_transform(Svga3VlknDevice *dev,
                                                  uint32_t cid,
                                                  SVGA3dTransformType type,
                                                  float outMatrix[16]);

Svga3VlknStatus  svga3_vlkn_context_set_zrange(Svga3VlknDevice *dev,
                                               uint32_t cid,
                                               const SVGA3dZRange *zRange);

Svga3VlknStatus  svga3_vlkn_context_get_zrange(Svga3VlknDevice *dev,
                                               uint32_t cid,
                                               SVGA3dZRange *outZRange);

Svga3VlknStatus  svga3_vlkn_context_set_texture_stage_state(Svga3VlknDevice *dev,
                                                           uint32_t cid,
                                                           uint32_t stage,
                                                           SVGA3dTextureStateName name,
                                                           uint32_t value);

Svga3VlknStatus  svga3_vlkn_context_set_texture(Svga3VlknDevice *dev,
                                               uint32_t cid,
                                               uint32_t stage,
                                               uint32_t sid);

Svga3VlknStatus  svga3_vlkn_context_set_clip_plane(Svga3VlknDevice *dev,
                                                   uint32_t cid,
                                                   uint32_t index,
                                                   const float plane[4]);

Svga3VlknStatus  svga3_vlkn_context_set_material(Svga3VlknDevice *dev,
                                                 uint32_t cid,
                                                 SVGA3dFace face,
                                                 const SVGA3dMaterial *material);

Svga3VlknStatus  svga3_vlkn_context_set_light_data(Svga3VlknDevice *dev,
                                                   uint32_t cid,
                                                   uint32_t index,
                                                   const SVGA3dLightData *data);

Svga3VlknStatus  svga3_vlkn_context_set_light_enabled(Svga3VlknDevice *dev,
                                                      uint32_t cid,
                                                      uint32_t index,
                                                      uint32_t enabled);

/*
 * Shader Management & Constants
 */
Svga3VlknStatus  svga3_vlkn_context_define_shader(Svga3VlknDevice *dev,
                                                  uint32_t cid,
                                                  uint32_t shid,
                                                  SVGA3dShaderType type,
                                                  const uint32_t *bytecode,
                                                  uint32_t numDwords);

Svga3VlknStatus  svga3_vlkn_context_destroy_shader(Svga3VlknDevice *dev,
                                                   uint32_t cid,
                                                   uint32_t shid,
                                                   SVGA3dShaderType type);

Svga3VlknStatus  svga3_vlkn_context_set_shader(Svga3VlknDevice *dev,
                                               uint32_t cid,
                                               SVGA3dShaderType type,
                                               uint32_t shid);

Svga3VlknStatus  svga3_vlkn_context_set_shader_const(Svga3VlknDevice *dev,
                                                     uint32_t cid,
                                                     uint32_t reg,
                                                     SVGA3dShaderType type,
                                                     SVGA3dShaderConstType ctype,
                                                     const uint32_t values[4]);

Svga3VlknStatus  svga3_vlkn_context_get_shader_const(Svga3VlknDevice *dev,
                                                     uint32_t cid,
                                                     uint32_t reg,
                                                     SVGA3dShaderType type,
                                                     SVGA3dShaderConstType ctype,
                                                     uint32_t outValues[4]);

Svga3VlknStatus  svga3_vlkn_context_clear(Svga3VlknDevice *dev,
                                          uint32_t cid,
                                          SVGA3dClearFlag flags,
                                          uint32_t colorRGBA,
                                          float depth,
                                          uint32_t stencil,
                                          const SVGA3dRect *rects,
                                          uint32_t numRects);

Svga3VlknStatus  svga3_vlkn_context_draw(Svga3VlknDevice *dev,
                                         uint32_t cid,
                                         SVGA3dPrimitiveType primitiveType,
                                         const SVGA3dVertexDecl *decls,
                                         uint32_t numDecls,
                                         const SVGA3dPrimitiveRange *ranges,
                                         uint32_t numRanges);

/*
 * Occlusion & Hardware Query API
 */
Svga3VlknStatus  svga3_vlkn_context_begin_query(Svga3VlknDevice *dev,
                                                uint32_t cid,
                                                SVGA3dQueryType type);

Svga3VlknStatus  svga3_vlkn_context_end_query(Svga3VlknDevice *dev,
                                              uint32_t cid,
                                              SVGA3dQueryType type);

Svga3VlknStatus  svga3_vlkn_context_wait_for_query(Svga3VlknDevice *dev,
                                                    uint32_t cid,
                                                    SVGA3dQueryType type,
                                                    uint32_t *outResult);

/*
 * Host Adapter Interface
 *
 * The core rendering library (FIFO decode, surfaces, contexts, shader
 * translation, Vulkan backend) talks to the host through this small
 * interface. It covers guest RAM access and display notifications.
 * Submission, presentation copies and fence synchronization remain in the core. QEMU is the first adapter; adding another VM host
 * means implementing this interface in a new adapter file, not editing
 * the core. No QEMU types appear here or in any core header.
 */
typedef struct Svga3HostAdapter {
    void *opaque;
    /* Guest RAM */
    void* (*guestRamMap)(void *opaque, uint64_t gpa, size_t size, bool isWrite);
    bool  (*guestRamRead)(void *opaque, uint64_t gpa, void *dst, size_t size);
    bool  (*guestRamWrite)(void *opaque, uint64_t gpa, const void *src, size_t size);
    /* Presentation */
    void  (*displayUpdate)(void *opaque, int32_t x, int32_t y, int32_t w, int32_t h);
} Svga3HostAdapter;

/* Copies the callback table. opaque must remain valid until detached or device
 * destruction. Passing NULL detaches it. Callbacks run under core locks and
 * must not reenter the device API. Registered RAM/framebuffer ranges take
 * precedence over adapter RAM callbacks. */
Svga3VlknStatus svga3_vlkn_device_set_host_adapter(
    Svga3VlknDevice *dev,
    const Svga3HostAdapter *adapter
);

Svga3VlknStatus svga3_vlkn_device_map_guest_ram(
    Svga3VlknDevice *dev,
    uint64_t gpaBase,
    void *hvaBase,
    size_t size
);

Svga3VlknStatus svga3_vlkn_device_unmap_guest_ram(
    Svga3VlknDevice *dev,
    uint64_t gpaBase
);

Svga3VlknStatus svga3_vlkn_device_set_framebuffer(
    Svga3VlknDevice *dev,
    void *fbHva,
    uint64_t fbGpa,
    size_t fbSize,
    uint32_t width,
    uint32_t height,
    uint32_t pitch,
    uint32_t bpp
);

Svga3VlknStatus svga3_vlkn_device_set_scanout_offset(
    Svga3VlknDevice *dev,
    uint32_t scanoutOffset
);

Svga3VlknStatus svga3_vlkn_gmr_define(
    Svga3VlknDevice *dev,
    uint32_t gmrId,
    uint32_t numPages
);

Svga3VlknStatus svga3_vlkn_gmr_remap(
    Svga3VlknDevice *dev,
    uint32_t gmrId,
    uint32_t flags,
    uint32_t offsetPages,
    uint32_t numPages,
    const void *descriptors,
    size_t descriptorBytes
);

Svga3VlknStatus svga3_vlkn_gmr_register_legacy(
    Svga3VlknDevice *dev,
    uint32_t gmrId,
    uint32_t descriptorPPN
);

Svga3VlknStatus svga3_vlkn_gmr_destroy(
    Svga3VlknDevice *dev,
    uint32_t gmrId
);

Svga3VlknStatus svga3_vlkn_guest_read(
    Svga3VlknDevice *dev,
    SVGAGuestPtr ptr,
    void *dstHost,
    size_t size
);

Svga3VlknStatus svga3_vlkn_guest_write(
    Svga3VlknDevice *dev,
    SVGAGuestPtr ptr,
    const void *srcHost,
    size_t size
);

/*
 * Format and Topology Conversion Utilities (backed by reference tables)
 */
VkFormat             svga3_format_to_vk(SVGA3dSurfaceFormat svgaFormat);
VkPrimitiveTopology  svga3_primitive_to_vk(SVGA3dPrimitiveType svgaTopology);
VkCompareOp          svga3_cmp_func_to_vk(SVGA3dCmpFunc cmp);
VkBlendOp            svga3_blend_eq_to_vk(SVGA3dBlendEquation eq);
VkBlendFactor        svga3_blend_factor_to_vk(SVGA3dBlendOp factor);
VkStencilOp          svga3_stencil_op_to_vk(SVGA3dStencilOp op);
VkSamplerAddressMode svga3_texture_address_to_vk(SVGA3dTextureAddress addr);

#ifdef __cplusplus
}
#endif

#endif /* ___SVGA3_VLKN_H___ */
