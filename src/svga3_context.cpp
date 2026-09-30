/*
 * SVGA3=VLKN - SVGA3D Context & Vulkan Pipeline State Tracker Implementation
 */

#include "svga3_context.h"
#include "svga3_shader_translator.h"
#include "../data/svga3d_reference.h"
#include <cstring>
#include <algorithm>
#include <iostream>
#include <cstdlib>
#include <type_traits>

extern "C" void log_msg(const char *fmt, ...);

namespace {
template <typename Handle>
static uint64_t descriptorHandleKey(Handle handle) {
    if constexpr (std::is_pointer<Handle>::value) {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
    } else {
        return static_cast<uint64_t>(handle);
    }
}
}

/* Extern "C" conversion functions from svga3_vlkn.h */
extern "C" {

VkFormat svga3_format_to_vk(SVGA3dSurfaceFormat svgaFormat) {
    return (VkFormat)svga3d_to_vk_format((uint32_t)svgaFormat);
}

VkPrimitiveTopology svga3_primitive_to_vk(SVGA3dPrimitiveType svgaTopology) {
    return (VkPrimitiveTopology)svga3d_to_vk_primitive_topology((uint32_t)svgaTopology);
}

VkCompareOp svga3_cmp_func_to_vk(SVGA3dCmpFunc cmp) {
    switch (cmp) {
        case SVGA3D_CMP_NEVER:        return VK_COMPARE_OP_NEVER;
        case SVGA3D_CMP_LESS:         return VK_COMPARE_OP_LESS;
        case SVGA3D_CMP_EQUAL:        return VK_COMPARE_OP_EQUAL;
        case SVGA3D_CMP_LESSEQUAL:    return VK_COMPARE_OP_LESS_OR_EQUAL;
        case SVGA3D_CMP_GREATER:      return VK_COMPARE_OP_GREATER;
        case SVGA3D_CMP_NOTEQUAL:     return VK_COMPARE_OP_NOT_EQUAL;
        case SVGA3D_CMP_GREATEREQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case SVGA3D_CMP_ALWAYS:       return VK_COMPARE_OP_ALWAYS;
        default:                      return VK_COMPARE_OP_ALWAYS;
    }
}

VkBlendOp svga3_blend_eq_to_vk(SVGA3dBlendEquation eq) {
    switch (eq) {
        case SVGA3D_BLENDEQ_ADD:         return VK_BLEND_OP_ADD;
        case SVGA3D_BLENDEQ_SUBTRACT:    return VK_BLEND_OP_SUBTRACT;
        case SVGA3D_BLENDEQ_REVSUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case SVGA3D_BLENDEQ_MINIMUM:     return VK_BLEND_OP_MIN;
        case SVGA3D_BLENDEQ_MAXIMUM:     return VK_BLEND_OP_MAX;
        default:                         return VK_BLEND_OP_ADD;
    }
}

VkBlendFactor svga3_blend_factor_to_vk(SVGA3dBlendOp factor) {
    switch (factor) {
        case SVGA3D_BLENDOP_ZERO:            return VK_BLEND_FACTOR_ZERO;
        case SVGA3D_BLENDOP_ONE:             return VK_BLEND_FACTOR_ONE;
        case SVGA3D_BLENDOP_SRCCOLOR:        return VK_BLEND_FACTOR_SRC_COLOR;
        case SVGA3D_BLENDOP_INVSRCCOLOR:     return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case SVGA3D_BLENDOP_SRCALPHA:        return VK_BLEND_FACTOR_SRC_ALPHA;
        case SVGA3D_BLENDOP_INVSRCALPHA:     return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case SVGA3D_BLENDOP_DESTALPHA:       return VK_BLEND_FACTOR_DST_ALPHA;
        case SVGA3D_BLENDOP_INVDESTALPHA:    return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case SVGA3D_BLENDOP_DESTCOLOR:       return VK_BLEND_FACTOR_DST_COLOR;
        case SVGA3D_BLENDOP_INVDESTCOLOR:    return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case SVGA3D_BLENDOP_SRCALPHASAT:     return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case SVGA3D_BLENDOP_BLENDFACTOR:     return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case SVGA3D_BLENDOP_INVBLENDFACTOR:  return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        default:                             return VK_BLEND_FACTOR_ONE;
    }
}

VkStencilOp svga3_stencil_op_to_vk(SVGA3dStencilOp op) {
    switch (op) {
        case SVGA3D_STENCILOP_KEEP:    return VK_STENCIL_OP_KEEP;
        case SVGA3D_STENCILOP_ZERO:    return VK_STENCIL_OP_ZERO;
        case SVGA3D_STENCILOP_REPLACE: return VK_STENCIL_OP_REPLACE;
        case SVGA3D_STENCILOP_INCRSAT: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case SVGA3D_STENCILOP_DECRSAT: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case SVGA3D_STENCILOP_INVERT:  return VK_STENCIL_OP_INVERT;
        case SVGA3D_STENCILOP_INCR:    return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case SVGA3D_STENCILOP_DECR:    return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default:                       return VK_STENCIL_OP_KEEP;
    }
}

VkSamplerAddressMode svga3_texture_address_to_vk(SVGA3dTextureAddress addr) {
    switch (addr) {
        case SVGA3D_TEX_ADDRESS_WRAP:       return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case SVGA3D_TEX_ADDRESS_MIRROR:     return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case SVGA3D_TEX_ADDRESS_CLAMP:      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case SVGA3D_TEX_ADDRESS_BORDER:     return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case SVGA3D_TEX_ADDRESS_MIRRORONCE: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        case SVGA3D_TEX_ADDRESS_EDGE:       return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        default:                            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

} // extern "C"

namespace svga3_vlkn {

static VkFormat svga3_decl_type_to_vk(SVGA3dDeclType type) {
    switch (type) {
        case SVGA3D_DECLTYPE_FLOAT1:    return VK_FORMAT_R32_SFLOAT;
        case SVGA3D_DECLTYPE_FLOAT2:    return VK_FORMAT_R32G32_SFLOAT;
        case SVGA3D_DECLTYPE_FLOAT3:    return VK_FORMAT_R32G32B32_SFLOAT;
        case SVGA3D_DECLTYPE_FLOAT4:    return VK_FORMAT_R32G32B32A32_SFLOAT;
        case SVGA3D_DECLTYPE_D3DCOLOR:  return VK_FORMAT_B8G8R8A8_UNORM;
        case SVGA3D_DECLTYPE_UBYTE4:    return VK_FORMAT_R8G8B8A8_USCALED;
        case SVGA3D_DECLTYPE_SHORT2:    return VK_FORMAT_R16G16_SSCALED;
        case SVGA3D_DECLTYPE_SHORT4:    return VK_FORMAT_R16G16B16A16_SSCALED;
        case SVGA3D_DECLTYPE_UBYTE4N:   return VK_FORMAT_R8G8B8A8_UNORM;
        case SVGA3D_DECLTYPE_SHORT2N:   return VK_FORMAT_R16G16_SNORM;
        case SVGA3D_DECLTYPE_SHORT4N:   return VK_FORMAT_R16G16B16A16_SNORM;
        case SVGA3D_DECLTYPE_USHORT2N:  return VK_FORMAT_R16G16_UNORM;
        case SVGA3D_DECLTYPE_USHORT4N:  return VK_FORMAT_R16G16B16A16_UNORM;
        case SVGA3D_DECLTYPE_FLOAT16_2: return VK_FORMAT_R16G16_SFLOAT;
        case SVGA3D_DECLTYPE_FLOAT16_4: return VK_FORMAT_R16G16B16A16_SFLOAT;
        default:                        return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

/* VlknContext implementation */

VlknContext::VlknContext(VlknBackend *backend, VlknSurfaceManager *surfaceMgr, uint32_t cid)
    : m_backend(backend)
    , m_surfaceMgr(surfaceMgr)
    , m_cid(cid)
    , m_activeFramebuffer(VK_NULL_HANDLE)
    , m_activeRenderPass(VK_NULL_HANDLE)
    , m_inRenderPass(false)
    , m_defaultPipelineLayout(VK_NULL_HANDLE)
    , m_defaultVS(VK_NULL_HANDLE)
    , m_defaultFS(VK_NULL_HANDLE)
    , m_defaultFSTex(VK_NULL_HANDLE)
    , m_descriptorSet(VK_NULL_HANDLE)
    , m_descriptorSetInitialized(false)
    , m_descriptorSetDirty(true)
    , m_constantsDirty(true)
    , m_vsConstsUploadedForFf(false)
    , m_lastFfMvp{}
    , m_constantRingBuffer(VK_NULL_HANDLE)
    , m_constantRingMemory(VK_NULL_HANDLE)
    , m_constantRingMapped(nullptr)
    , m_constantRingSize(4 * 1024 * 1024)
    , m_constantRingCursor(0)
    , m_constantRingStride(0)
    , m_vsConstDynamicOffset(0)
    , m_psConstDynamicOffset(0)
    , m_constantRingSubmissionSerial(0)
    , m_dummyImage(VK_NULL_HANDLE)
    , m_dummyMemory(VK_NULL_HANDLE)
    , m_dummyView(VK_NULL_HANDLE)
    , m_whiteImage(VK_NULL_HANDLE)
    , m_whiteMemory(VK_NULL_HANDLE)
    , m_whiteView(VK_NULL_HANDLE)
    , m_dummySampler(VK_NULL_HANDLE)
    , m_drawCount(0)
    , m_vertexCount(0)
    , m_clearCount(0)
    , m_lastDrawnWindowSid(SVGA3D_INVALID_ID)
    , m_hasDrawnToWindow(false)
    , m_dummyVb(VK_NULL_HANDLE)
    , m_dummyVbMemory(VK_NULL_HANDLE)
    , m_defaultVsInputMask(0)
    , m_fbColorSid(SVGA3D_INVALID_ID)
    , m_fbColorMip(0)
    , m_fbColorFace(0)
    , m_fbColorView(VK_NULL_HANDLE)
    , m_fbDepthSid(SVGA3D_INVALID_ID)
    , m_fbDepthMip(0)
    , m_fbDepthFace(0)
    , m_fbDepthView(VK_NULL_HANDLE)
    , m_fbWidth(0)
    , m_fbHeight(0)
{
    memset(m_renderTargets, 0, sizeof(m_renderTargets));
    memset(&m_depthStencilTarget, 0, sizeof(m_depthStencilTarget));
    memset(m_stages, 0, sizeof(m_stages));
    for (uint32_t i = 0; i < SVGA3_MAX_TEXTURE_STAGES; ++i) {
        m_stages[i].sid = SVGA3D_INVALID_ID;
        m_boundImageViews[i] = VK_NULL_HANDLE;
        m_boundSamplers[i] = VK_NULL_HANDLE;
    }

    m_viewport.x = 0.0f;
    m_viewport.y = 0.0f;
    m_viewport.width = 1.0f;
    m_viewport.height = 1.0f;
    m_viewport.minDepth = 0.0f;
    m_viewport.maxDepth = 1.0f;

    m_scissor.offset.x = 0;
    m_scissor.offset.y = 0;
    m_scissor.extent.width = 1;
    m_scissor.extent.height = 1;

    m_zRange.min = 0.0f;
    m_zRange.max = 1.0f;
    memset(m_clipPlanes, 0, sizeof(m_clipPlanes));
    memset(m_materials, 0, sizeof(m_materials));
    memset(m_lights, 0, sizeof(m_lights));

    m_boundVS = SVGA3D_INVALID_ID;
    m_boundPS = SVGA3D_INVALID_ID;
    memset(&m_vsConsts, 0, sizeof(m_vsConsts));
    memset(&m_psConsts, 0, sizeof(m_psConsts));

    m_queryPool = VK_NULL_HANDLE;
    m_queryActive = false;
    m_queryEnded = false;
    m_lastQueryResult = 0;

    initDefaultRenderStates();

    /* Identity MVP matrix in VS constant c0..c3 */
    m_vsConsts.floatConsts[0][0] = 1.0f;
    m_vsConsts.floatConsts[1][1] = 1.0f;
    m_vsConsts.floatConsts[2][2] = 1.0f;
    m_vsConsts.floatConsts[3][3] = 1.0f;

    /* Create descriptor set layout for UBOs (bindings 0, 1) and samplers (bindings 2..9) */
    VkDescriptorSetLayoutBinding descBindings[10] = {};
    descBindings[0].binding = 0;
    descBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    descBindings[0].descriptorCount = 1;
    descBindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    descBindings[1].binding = 1;
    descBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    descBindings[1].descriptorCount = 1;
    descBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    for (int i = 0; i < 8; ++i) {
        descBindings[2 + i].binding = 2 + i;
        descBindings[2 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descBindings[2 + i].descriptorCount = 1;
        descBindings[2 + i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo dslInfo = {};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 10;
    dslInfo.pBindings = descBindings;
    m_backend->dispatch().vkCreateDescriptorSetLayout(m_backend->device(), &dslInfo, nullptr, &m_descriptorSetLayout);

    /* Create pipeline layout */
    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descriptorSetLayout;
    m_backend->dispatch().vkCreatePipelineLayout(m_backend->device(), &layoutInfo, nullptr, &m_defaultPipelineLayout);

    /* Dynamic UBO slices let each draw keep its constants without waiting for
     * the GPU before the next update. Retire and wrap the ring only after a
     * completed queue submission. */
    VkDeviceSize alignment = m_backend->properties().limits.minUniformBufferOffsetAlignment;
    if (alignment == 0) alignment = 1;
    m_constantRingStride = (4096 + alignment - 1) / alignment * alignment;
    m_constantRingSize -= m_constantRingSize % (m_constantRingStride * 2);
    m_backend->createBuffer(
        m_constantRingSize,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &m_constantRingBuffer,
        &m_constantRingMemory
    );
    if (m_constantRingMemory) {
        m_backend->dispatch().vkMapMemory(m_backend->device(), m_constantRingMemory, 0,
                                          m_constantRingSize, 0, &m_constantRingMapped);
    }
    m_constantRingSubmissionSerial = m_backend->completedSubmissionSerial();

    /* Create 1x1 dummy texture and sampler for unbound sampler stages */
    VkImageCreateInfo imgInfo = {};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imgInfo.extent.width = 1;
    imgInfo.extent.height = 1;
    imgInfo.extent.depth = 1;
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    /* Create one 1x1 sampled image with every step checked. Any failure —
     * no suitable memory type, allocation, bind, or view creation —
     * releases whatever was created and nulls all three handles, so the
     * caller can skip layout initialization for that image and the
     * destructor (which null-checks every handle) stays safe. Previously
     * these results were unchecked: a failed bind or a missing memory
     * type left an image with no bound memory that layout barriers and
     * buffer copies were then recorded against. */
    auto createUnitImage = [&](VkImage *outImage, VkDeviceMemory *outMemory, VkImageView *outView) -> bool {
        VlknDispatchTable &disp = m_backend->dispatch();
        VkDevice device = m_backend->device();
        if (disp.vkCreateImage(device, &imgInfo, nullptr, outImage) != VK_SUCCESS) {
            *outImage = VK_NULL_HANDLE;
            return false;
        }
        VkMemoryRequirements reqs;
        disp.vkGetImageMemoryRequirements(device, *outImage, &reqs);
        int type = m_backend->findMemoryType(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        bool ok = type >= 0
            && m_backend->allocateMemory(reqs.size, (uint32_t)type, outMemory) == SVGA3_VLKN_SUCCESS
            && disp.vkBindImageMemory(device, *outImage, *outMemory, 0) == VK_SUCCESS;
        if (ok) {
            VkImageViewCreateInfo unitViewInfo = {};
            unitViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            unitViewInfo.image = *outImage;
            unitViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            unitViewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            unitViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            unitViewInfo.subresourceRange.levelCount = 1;
            unitViewInfo.subresourceRange.layerCount = 1;
            ok = disp.vkCreateImageView(device, &unitViewInfo, nullptr, outView) == VK_SUCCESS;
        }
        if (!ok) {
            if (*outView) {
                disp.vkDestroyImageView(device, *outView, nullptr);
                *outView = VK_NULL_HANDLE;
            }
            if (*outImage) {
                disp.vkDestroyImage(device, *outImage, nullptr);
                *outImage = VK_NULL_HANDLE;
            }
            if (*outMemory) {
                m_backend->freeMemory(*outMemory);
                *outMemory = VK_NULL_HANDLE;
            }
        }
        return ok;
    };

    bool dummyOk = createUnitImage(&m_dummyImage, &m_dummyMemory, &m_dummyView);
    if (!dummyOk) {
        log_msg("[libqemu_svga3d] context init: dummy image creation failed; unbound-stage sampling fallback unavailable\n");
    }

    VkSamplerCreateInfo sampInfo = {};
    sampInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampInfo.magFilter = VK_FILTER_LINEAR;
    sampInfo.minFilter = VK_FILTER_LINEAR;
    sampInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if (m_backend->dispatch().vkCreateSampler(m_backend->device(), &sampInfo, nullptr, &m_dummySampler) != VK_SUCCESS) {
        log_msg("[libqemu_svga3d] context init: vkCreateSampler failed for dummy sampler\n");
        m_dummySampler = VK_NULL_HANDLE;
    }

    /* Initialize m_dummyImage to SHADER_READ_ONLY_OPTIMAL layout with a cleared white pixel.
     * Skipped entirely when creation failed above: recording barriers or
     * copies against an image with no bound memory is invalid usage. */
    VkCommandBuffer initCb = m_backend->getActiveCommandBuffer();
    VkImageMemoryBarrier dummyBarrier = {};
    if (dummyOk) {
        dummyBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        dummyBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        dummyBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dummyBarrier.srcAccessMask = 0;
        dummyBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dummyBarrier.image = m_dummyImage;
        dummyBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        dummyBarrier.subresourceRange.baseMipLevel = 0;
        dummyBarrier.subresourceRange.levelCount = 1;
        dummyBarrier.subresourceRange.baseArrayLayer = 0;
        dummyBarrier.subresourceRange.layerCount = 1;

        m_backend->dispatch().vkCmdPipelineBarrier(
            initCb,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &dummyBarrier
        );

        if (m_backend->stagingBuffer() && m_backend->stagingMapped()) {
            std::lock_guard<std::mutex> lock(m_backend->stagingMutex());
            uint32_t white = 0xFFFFFFFF;
            memcpy(m_backend->stagingMapped(), &white, sizeof(white));
            VkBufferImageCopy region = {};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = { 1, 1, 1 };
            m_backend->dispatch().vkCmdCopyBufferToImage(
                initCb, m_backend->stagingBuffer(), m_dummyImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region
            );
        }

        dummyBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dummyBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        dummyBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dummyBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        m_backend->dispatch().vkCmdPipelineBarrier(
            initCb,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &dummyBarrier
        );
    }

    /* White 1x1 for a stage the guest bound before its image view exists. */
    bool whiteOk = createUnitImage(&m_whiteImage, &m_whiteMemory, &m_whiteView);
    if (!whiteOk) {
        log_msg("[libqemu_svga3d] context init: white image creation failed; unbound-stage sampling fallback unavailable\n");
    }
    if (whiteOk) {
        dummyBarrier.image = m_whiteImage;
        dummyBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        dummyBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dummyBarrier.srcAccessMask = 0;
        dummyBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        m_backend->dispatch().vkCmdPipelineBarrier(
            initCb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &dummyBarrier);
        if (m_backend->stagingBuffer() && m_backend->stagingMapped()) {
            std::lock_guard<std::mutex> lock(m_backend->stagingMutex());
            uint32_t white = 0xFFFFFFFF;
            memcpy(m_backend->stagingMapped(), &white, sizeof(white));
            VkBufferImageCopy region = {};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = { 1, 1, 1 };
            m_backend->dispatch().vkCmdCopyBufferToImage(
                initCb, m_backend->stagingBuffer(), m_whiteImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        dummyBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dummyBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        dummyBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dummyBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        m_backend->dispatch().vkCmdPipelineBarrier(
            initCb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &dummyBarrier);
    }

    m_backend->flushCommandBuffer();

    /* Create occlusion query pool */
    VkQueryPoolCreateInfo qpInfo = {};
    qpInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qpInfo.queryType = VK_QUERY_TYPE_OCCLUSION;
    qpInfo.queryCount = 16;
    m_backend->dispatch().vkCreateQueryPool(m_backend->device(), &qpInfo, nullptr, &m_queryPool);

    /* Create dummy vertex buffer initialized to (1.0, 1.0, 1.0, 1.0) for missing vertex attributes */
    m_backend->createBuffer(
        256,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &m_dummyVb,
        &m_dummyVbMemory
    );
    if (m_dummyVb && m_dummyVbMemory) {
        void *mapped = nullptr;
        if (m_backend->dispatch().vkMapMemory(m_backend->device(), m_dummyVbMemory, 0, 256, 0, &mapped) == VK_SUCCESS) {
            float dummyVal[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            memcpy(mapped, dummyVal, sizeof(dummyVal));
            m_backend->dispatch().vkUnmapMemory(m_backend->device(), m_dummyVbMemory);
        }
    }

    /* Create valid default VS & FS using D3D9 translator into standard SPIR-V */
#define D3D9_REG(regType, regNum) \
    (0x80000000u | (((regType) & 0x7) << 28) | ((((regType) >> 3) & 0x3) << 11) | ((regNum) & 0x7FF))
#define D3D9_DST(regType, regNum, mask) \
    (D3D9_REG(regType, regNum) | (((mask) & 0xF) << 16))
#define D3D9_SRC(regType, regNum, swiz) \
    (D3D9_REG(regType, regNum) | (((swiz) & 0xFF) << 16))

    static const uint32_t defVsTokens[] = {
        0xFFFE0300, /* vs_3_0 */
        (31) | (2 << 24), 0x80000000 | 0,  D3D9_DST(1, 0, 0xF), /* dcl_position v0 */
        (31) | (2 << 24), 0x80000000 | 10, D3D9_DST(1, 1, 0xF), /* dcl_color v1 */
        (31) | (2 << 24), 0x80000000 | 5,  D3D9_DST(1, 2, 0xF), /* dcl_texcoord0 v2 */
        (31) | (2 << 24), 0x80000000 | (1 << 16) | 5, D3D9_DST(1, 3, 0xF), /* dcl_texcoord1 v3 */
        (20) | (3 << 24), D3D9_DST(4, 0, 0xF), D3D9_SRC(1, 0, 0xE4), D3D9_SRC(2, 0, 0xE4), /* m4x4 oPos, v0, c0 */
        (1)  | (2 << 24), D3D9_DST(5, 0, 0xF), D3D9_SRC(1, 1, 0xE4), /* mov oD0, v1 */
        (1)  | (2 << 24), D3D9_DST(6, 0, 0xF), D3D9_SRC(1, 2, 0xE4), /* mov oT0, v2 */
        (1)  | (2 << 24), D3D9_DST(6, 1, 0xF), D3D9_SRC(1, 3, 0xE4), /* mov oT1, v3 */
        0x0000FFFF
    };
    std::vector<uint32_t> defVsSpirv;
    std::string defErr;
    svga3_translate_shader_d3d9(SVGA3D_SHADERTYPE_VS, defVsTokens, sizeof(defVsTokens)/sizeof(uint32_t), defVsSpirv, defErr, &m_defaultVsInputMask);
    VkShaderModuleCreateInfo smInfo = {};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = defVsSpirv.size() * sizeof(uint32_t);
    smInfo.pCode = defVsSpirv.data();
    m_backend->dispatch().vkCreateShaderModule(m_backend->device(), &smInfo, nullptr, &m_defaultVS);

    static const uint32_t defPsTokens[] = {
        0xFFFF0300, /* ps_3_0 */
        (31) | (2 << 24), 0x80000000 | 10, D3D9_DST(1, 0, 0xF), /* dcl_color v0 */
        (1)  | (2 << 24), D3D9_DST(8, 0, 0xF), D3D9_SRC(1, 0, 0xE4), /* mov oC0, v0 */
        0x0000FFFF
    };
    std::vector<uint32_t> defPsSpirv;
    svga3_translate_shader_d3d9(SVGA3D_SHADERTYPE_PS, defPsTokens, sizeof(defPsTokens)/sizeof(uint32_t), defPsSpirv, defErr);
    smInfo.codeSize = defPsSpirv.size() * sizeof(uint32_t);
    smInfo.pCode = defPsSpirv.data();
    m_backend->dispatch().vkCreateShaderModule(m_backend->device(), &smInfo, nullptr, &m_defaultFS);

    /* Create textured default FS: samples texture stage 0 and modulates with vertex color */
    static const uint32_t defPsTexTokens[] = {
        0xFFFF0300, /* ps_3_0 */
        (31) | (2 << 24), 0x80000000 | 10, D3D9_DST(1, 0, 0xF), /* dcl_color v0 */
        (31) | (2 << 24), 0x80000000 | 5,  D3D9_DST(1, 1, 0xF), /* dcl_texcoord v1 */
        (31) | (2 << 24), 0x80000000 | (2 << 28), D3D9_DST(10, 0, 0xF), /* dcl_2d s0 */
        (66) | (3 << 24), D3D9_DST(0, 0, 0xF), D3D9_SRC(1, 1, 0xE4), D3D9_SRC(10, 0, 0xE4), /* texld r0, v1, s0 */
        (5)  | (3 << 24), D3D9_DST(8, 0, 0xF), D3D9_SRC(0, 0, 0xE4), D3D9_SRC(1, 0, 0xE4), /* mul oC0, r0, v0 */
        0x0000FFFF
    };
    std::vector<uint32_t> defPsTexSpirv;
    svga3_translate_shader_d3d9(SVGA3D_SHADERTYPE_PS, defPsTexTokens, sizeof(defPsTexTokens)/sizeof(uint32_t), defPsTexSpirv, defErr);
    smInfo.codeSize = defPsTexSpirv.size() * sizeof(uint32_t);
    smInfo.pCode = defPsTexSpirv.data();
    m_backend->dispatch().vkCreateShaderModule(m_backend->device(), &smInfo, nullptr, &m_defaultFSTex);

    /* Create pure textured default FS: samples texture stage 0 directly without vertex color */
    static const uint32_t defPsTexPureTokens[] = {
        0xFFFF0300, /* ps_3_0 */
        (31) | (2 << 24), 0x80000000 | 5,  D3D9_DST(1, 0, 0xF), /* dcl_texcoord v0 */
        (31) | (2 << 24), 0x80000000 | (2 << 28), D3D9_DST(10, 0, 0xF), /* dcl_2d s0 */
        (66) | (3 << 24), D3D9_DST(0, 0, 0xF), D3D9_SRC(1, 0, 0xE4), D3D9_SRC(10, 0, 0xE4), /* texld r0, v0, s0 */
        (1)  | (2 << 24), D3D9_DST(8, 0, 0xF), D3D9_SRC(0, 0, 0xE4), /* mov oC0, r0 */
        0x0000FFFF
    };
    std::vector<uint32_t> defPsTexPureSpirv;
    svga3_translate_shader_d3d9(SVGA3D_SHADERTYPE_PS, defPsTexPureTokens, sizeof(defPsTexPureTokens)/sizeof(uint32_t), defPsTexPureSpirv, defErr);
    smInfo.codeSize = defPsTexPureSpirv.size() * sizeof(uint32_t);
    smInfo.pCode = defPsTexPureSpirv.data();
    m_backend->dispatch().vkCreateShaderModule(m_backend->device(), &smInfo, nullptr, &m_defaultFSTexPure);
#undef D3D9_REG
#undef D3D9_DST
#undef D3D9_SRC
}

/* Aggregate budget accounting for one shader-table entry: the stored D3D9
 * bytecode bytes, plus one VkShaderModule per owned module (the base module
 * plus any depth variants that own a distinct module; failed variants
 * borrow the base module and own nothing). */
static uint64_t shaderBudgetBytes(const Svga3Shader &shader) {
    return (uint64_t)shader.bytecode.size() * sizeof(uint32_t);
}
static uint64_t shaderBudgetModules(const Svga3Shader &shader) {
    uint64_t n = 1; /* base module */
    for (const auto &variant : shader.depthVariants) {
        if (variant.second && variant.second != shader.module) ++n;
    }
    return n;
}

VlknContext::~VlknContext() {
    endRenderPassIfActive();
    if (m_backend) {
        m_backend->flushCommandBuffer();
        clearDescriptorSetCache();
    }

    for (auto &pair : m_pipelineCache) {
        if (pair.second) {
            m_backend->dispatch().vkDestroyPipeline(m_backend->device(), pair.second, nullptr);
        }
    }
    m_pipelineCache.clear();

    for (uint32_t i = 0; i < SVGA3_MAX_TEXTURE_STAGES; ++i) {
        if (m_stages[i].sampler) {
            m_backend->dispatch().vkDestroySampler(m_backend->device(), m_stages[i].sampler, nullptr);
            m_stages[i].sampler = VK_NULL_HANDLE;
        }
    }

    for (auto &pair : m_vertexShaders) {
        if (pair.second.module && pair.second.module != m_defaultVS) {
            m_backend->dispatch().vkDestroyShaderModule(m_backend->device(), pair.second.module, nullptr);
        }
        m_backend->resourceBudgets().releaseShaderBytes(shaderBudgetBytes(pair.second));
        m_backend->resourceBudgets().releaseShaderModules(shaderBudgetModules(pair.second));
    }
    m_vertexShaders.clear();

    for (auto &pair : m_pixelShaders) {
        if (pair.second.module && pair.second.module != m_defaultFS && pair.second.module != m_defaultFSTex && pair.second.module != m_defaultFSTexPure) {
            m_backend->dispatch().vkDestroyShaderModule(m_backend->device(), pair.second.module, nullptr);
        }
        for (auto &variant : pair.second.depthVariants) {
            if (variant.second && variant.second != pair.second.module) {
                m_backend->dispatch().vkDestroyShaderModule(m_backend->device(), variant.second, nullptr);
            }
        }
        m_backend->resourceBudgets().releaseShaderBytes(shaderBudgetBytes(pair.second));
        m_backend->resourceBudgets().releaseShaderModules(shaderBudgetModules(pair.second));
    }
    m_pixelShaders.clear();

    if (m_defaultVS) {
        m_backend->dispatch().vkDestroyShaderModule(m_backend->device(), m_defaultVS, nullptr);
        m_defaultVS = VK_NULL_HANDLE;
    }
    if (m_defaultFS) {
        m_backend->dispatch().vkDestroyShaderModule(m_backend->device(), m_defaultFS, nullptr);
        m_defaultFS = VK_NULL_HANDLE;
    }
    if (m_defaultFSTex) {
        m_backend->dispatch().vkDestroyShaderModule(m_backend->device(), m_defaultFSTex, nullptr);
        m_defaultFSTex = VK_NULL_HANDLE;
    }
    if (m_defaultFSTexPure) {
        m_backend->dispatch().vkDestroyShaderModule(m_backend->device(), m_defaultFSTexPure, nullptr);
        m_defaultFSTexPure = VK_NULL_HANDLE;
    }
    if (m_dummySampler) {
        m_backend->dispatch().vkDestroySampler(m_backend->device(), m_dummySampler, nullptr);
        m_dummySampler = VK_NULL_HANDLE;
    }
    if (m_whiteView) {
        m_backend->dispatch().vkDestroyImageView(m_backend->device(), m_whiteView, nullptr);
        m_whiteView = VK_NULL_HANDLE;
    }
    if (m_whiteImage) {
        m_backend->dispatch().vkDestroyImage(m_backend->device(), m_whiteImage, nullptr);
        m_whiteImage = VK_NULL_HANDLE;
    }
    if (m_whiteMemory) {
        m_backend->freeMemory(m_whiteMemory);
        m_whiteMemory = VK_NULL_HANDLE;
    }
    if (m_dummyView) {
        m_backend->dispatch().vkDestroyImageView(m_backend->device(), m_dummyView, nullptr);
        m_dummyView = VK_NULL_HANDLE;
    }
    if (m_dummyImage) {
        m_backend->dispatch().vkDestroyImage(m_backend->device(), m_dummyImage, nullptr);
        m_dummyImage = VK_NULL_HANDLE;
    }
    if (m_dummyMemory) {
        m_backend->freeMemory(m_dummyMemory);
        m_dummyMemory = VK_NULL_HANDLE;
    }
    if (m_dummyVb) {
        m_backend->dispatch().vkDestroyBuffer(m_backend->device(), m_dummyVb, nullptr);
        m_dummyVb = VK_NULL_HANDLE;
    }
    if (m_dummyVbMemory) {
        m_backend->freeMemory(m_dummyVbMemory);
        m_dummyVbMemory = VK_NULL_HANDLE;
    }
    if (m_constantRingMemory && m_constantRingMapped) {
        m_backend->dispatch().vkUnmapMemory(m_backend->device(), m_constantRingMemory);
        m_constantRingMapped = nullptr;
    }
    if (m_constantRingBuffer) {
        m_backend->dispatch().vkDestroyBuffer(m_backend->device(), m_constantRingBuffer, nullptr);
        m_constantRingBuffer = VK_NULL_HANDLE;
    }
    if (m_constantRingMemory) {
        m_backend->freeMemory(m_constantRingMemory);
        m_constantRingMemory = VK_NULL_HANDLE;
    }
    if (m_defaultPipelineLayout) {
        m_backend->dispatch().vkDestroyPipelineLayout(m_backend->device(), m_defaultPipelineLayout, nullptr);
        m_defaultPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_descriptorSetLayout) {
        m_backend->dispatch().vkDestroyDescriptorSetLayout(m_backend->device(), m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }

    if (m_activeFramebuffer) {
        m_backend->dispatch().vkDestroyFramebuffer(m_backend->device(), m_activeFramebuffer, nullptr);
        m_activeFramebuffer = VK_NULL_HANDLE;
    }

    if (m_queryPool != VK_NULL_HANDLE) {
        m_backend->dispatch().vkDestroyQueryPool(m_backend->device(), m_queryPool, nullptr);
        m_queryPool = VK_NULL_HANDLE;
    }
}

void VlknContext::initDefaultRenderStates() {
    m_renderStates[SVGA3D_RS_ZENABLE] = 1;
    m_renderStates[SVGA3D_RS_ZWRITEENABLE] = 1;
    m_renderStates[SVGA3D_RS_ZFUNC] = SVGA3D_CMP_LESSEQUAL;
    m_renderStates[SVGA3D_RS_ALPHATESTENABLE] = 0;
    m_renderStates[SVGA3D_RS_SRCBLEND] = SVGA3D_BLENDOP_ONE;
    m_renderStates[SVGA3D_RS_DSTBLEND] = SVGA3D_BLENDOP_ZERO;
    m_renderStates[SVGA3D_RS_CULLMODE] = SVGA3D_FACE_BACK;
    m_renderStates[SVGA3D_RS_DITHERENABLE] = 0;
    m_renderStates[SVGA3D_RS_BLENDENABLE] = 0;
    m_renderStates[SVGA3D_RS_FOGENABLE] = 0;
    m_renderStates[SVGA3D_RS_SPECULARENABLE] = 0;
    m_renderStates[SVGA3D_RS_STENCILENABLE] = 0;
    m_renderStates[SVGA3D_RS_STENCILFAIL] = SVGA3D_STENCILOP_KEEP;
    m_renderStates[SVGA3D_RS_STENCILZFAIL] = SVGA3D_STENCILOP_KEEP;
    m_renderStates[SVGA3D_RS_STENCILPASS] = SVGA3D_STENCILOP_KEEP;
    m_renderStates[SVGA3D_RS_STENCILFUNC] = SVGA3D_CMP_ALWAYS;
    m_renderStates[SVGA3D_RS_STENCILREF] = 0;
    m_renderStates[SVGA3D_RS_STENCILMASK] = 0xFFFFFFFF;
    m_renderStates[SVGA3D_RS_STENCILWRITEMASK] = 0xFFFFFFFF;
    m_renderStates[SVGA3D_RS_TEXTUREFACTOR] = 0xFFFFFFFF;
    m_renderStates[SVGA3D_RS_CLIPPING] = 1;
    m_renderStates[SVGA3D_RS_LIGHTINGENABLE] = 1;
    m_renderStates[SVGA3D_RS_AMBIENT] = 0;
    m_renderStates[SVGA3D_RS_COLORWRITEENABLE] = 0xF; /* RGBA */
    m_renderStates[SVGA3D_RS_BLENDEQUATION] = SVGA3D_BLENDEQ_ADD;
    m_renderStates[SVGA3D_RS_SCISSORTESTENABLE] = 0;
    m_renderStates[SVGA3D_RS_SLOPESCALEDEPTHBIAS] = 0;
    m_renderStates[SVGA3D_RS_DEPTHBIAS] = 0;
    m_renderStates[SVGA3D_RS_SEPARATEALPHABLENDENABLE] = 0;
    m_renderStates[SVGA3D_RS_SRCBLENDALPHA] = SVGA3D_BLENDOP_ONE;
    m_renderStates[SVGA3D_RS_DSTBLENDALPHA] = SVGA3D_BLENDOP_ZERO;
    m_renderStates[SVGA3D_RS_BLENDEQUATIONALPHA] = SVGA3D_BLENDEQ_ADD;

    SVGA3dFillMode fm = {};
    fm.s.mode = SVGA3D_FILLMODE_FILL;
    fm.s.face = SVGA3D_FACE_FRONT_BACK;
    m_renderStates[SVGA3D_RS_FILLMODE] = fm.uintValue;
}

Svga3VlknStatus VlknContext::setRenderState(SVGA3dRenderStateName state, uint32_t value) {
    m_renderStates[(uint32_t)state] = value;
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknContext::getRenderState(SVGA3dRenderStateName state, uint32_t *outValue) const {
    if (!outValue) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    auto it = m_renderStates.find((uint32_t)state);
    if (it != m_renderStates.end()) {
        *outValue = it->second;
        return SVGA3_VLKN_SUCCESS;
    }
    return SVGA3_VLKN_ERROR_NOT_FOUND;
}

Svga3VlknStatus VlknContext::setRenderTarget(SVGA3dRenderTargetType type, uint32_t sid, uint32_t face, uint32_t mipmap) {
    endRenderPassIfActive();

    if (type >= SVGA3D_RT_COLOR0 && type <= SVGA3D_RT_COLOR3) {
        uint32_t idx = type - SVGA3D_RT_COLOR0;
        m_renderTargets[idx].sid = sid;
        m_renderTargets[idx].face = face;
        m_renderTargets[idx].mipmap = mipmap;

        if (type == SVGA3D_RT_COLOR0 && sid != 0 && sid != SVGA3D_INVALID_ID) {
            if (m_viewport.width <= 1.0f || m_viewport.height <= 1.0f) {
                VlknSurface *surf = m_surfaceMgr->getSurface(sid);
                if (surf && surf->width() > 1 && surf->height() > 1) {
                    m_viewport.x = 0.0f;
                    m_viewport.y = 0.0f;
                    m_viewport.width = static_cast<float>(surf->width());
                    m_viewport.height = static_cast<float>(surf->height());
                    m_scissor.offset.x = 0;
                    m_scissor.offset.y = 0;
                    m_scissor.extent.width = surf->width();
                    m_scissor.extent.height = surf->height();
                }
            }
        }
    } else if (type == SVGA3D_RT_DEPTH) {
        m_depthStencilTarget.sid = sid;
        m_depthStencilTarget.face = face;
        m_depthStencilTarget.mipmap = mipmap;
    } else if (type == SVGA3D_RT_STENCIL) {
        /* Depth and stencil share one attachment. Mesa binds a depth-only
         * shadow map and then sets the stencil target to INVALID, which must
         * not drop the depth surface. */
        bool unbound = (sid == SVGA3D_INVALID_ID || sid == 0);
        bool depthBound = m_depthStencilTarget.sid != SVGA3D_INVALID_ID && m_depthStencilTarget.sid != 0;
        if (!unbound && !depthBound) {
            m_depthStencilTarget.sid = sid;
            m_depthStencilTarget.face = face;
            m_depthStencilTarget.mipmap = mipmap;
        }
    } else {
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }
    return SVGA3_VLKN_SUCCESS;
}

const RenderTargetBinding* VlknContext::getRenderTarget(SVGA3dRenderTargetType type) const {
    if (type >= SVGA3D_RT_COLOR0 && type <= SVGA3D_RT_COLOR3) {
        return &m_renderTargets[type - SVGA3D_RT_COLOR0];
    } else if (type == SVGA3D_RT_DEPTH || type == SVGA3D_RT_STENCIL) {
        return &m_depthStencilTarget;
    }
    return nullptr;
}

Svga3VlknStatus VlknContext::setTexture(uint32_t stage, uint32_t sid) {
    if (stage >= SVGA3_MAX_TEXTURE_STAGES) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    if (m_stages[stage].sid != sid) {
        m_stages[stage].sid = sid;
        m_stages[stage].samplerDirty = true;
    }
    m_descriptorSetDirty = true;
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknContext::setTextureStageState(uint32_t stage, SVGA3dTextureStateName name, uint32_t value) {
    if (stage >= SVGA3_MAX_TEXTURE_STAGES) return SVGA3_VLKN_ERROR_INVALID_PARAM;

    TextureStageState &s = m_stages[stage];
    m_descriptorSetDirty = true;
    switch (name) {
        case SVGA3D_TS_ADDRESSU:
            s.addressU = value;
            s.samplerDirty = true;
            break;
        case SVGA3D_TS_ADDRESSV:
            s.addressV = value;
            s.samplerDirty = true;
            break;
        case SVGA3D_TS_ADDRESSW:
            s.addressW = value;
            s.samplerDirty = true;
            break;
        case SVGA3D_TS_MINFILTER:
            s.minFilter = value;
            s.samplerDirty = true;
            break;
        case SVGA3D_TS_MAGFILTER:
            s.magFilter = value;
            s.samplerDirty = true;
            break;
        case SVGA3D_TS_MIPFILTER:
            s.mipFilter = value;
            s.samplerDirty = true;
            break;
        case SVGA3D_TS_TEXTURE_ANISOTROPIC_LEVEL:
            s.maxAnisotropy = value;
            s.samplerDirty = true;
            break;
        case SVGA3D_TS_TEXTURE_LOD_BIAS: {
            union { uint32_t u; float f; } u;
            u.u = value;
            s.mipLodBias = u.f;
            s.samplerDirty = true;
            break;
        }
        default:
            break;
    }
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknContext::setViewport(const SVGA3dRect *rect) {
    if (!rect) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    m_viewport.x = (float)rect->x;
    m_viewport.y = (float)rect->y;
    m_viewport.width = (float)rect->w;
    m_viewport.height = (float)rect->h;
    m_viewport.minDepth = 0.0f;
    m_viewport.maxDepth = 1.0f;
    if (!m_renderStates[SVGA3D_RS_SCISSORTESTENABLE]) {
        m_scissor.offset.x = rect->x;
        m_scissor.offset.y = rect->y;
        m_scissor.extent.width = rect->w;
        m_scissor.extent.height = rect->h;
    }
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknContext::setScissorRect(const SVGA3dRect *rect) {
    if (!rect) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    m_scissor.offset.x = (int32_t)rect->x;
    m_scissor.offset.y = (int32_t)rect->y;
    m_scissor.extent.width = rect->w;
    m_scissor.extent.height = rect->h;
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknContext::setTransform(SVGA3dTransformType type, const float matrix[16]) {
    if (!matrix) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    std::array<float, 16> mat;
    std::copy(matrix, matrix + 16, mat.begin());
    m_transforms[(uint32_t)type] = mat;
    if (m_boundVS == SVGA3D_INVALID_ID) {
        m_constantsDirty = true;
    }
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknContext::getTransform(SVGA3dTransformType type, float outMatrix[16]) const {
    if (!outMatrix) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    auto it = m_transforms.find((uint32_t)type);
    if (it == m_transforms.end()) {
        memset(outMatrix, 0, sizeof(float) * 16);
        outMatrix[0] = outMatrix[5] = outMatrix[10] = outMatrix[15] = 1.0f;
        return SVGA3_VLKN_SUCCESS;
    }
    std::copy(it->second.begin(), it->second.end(), outMatrix);
    return SVGA3_VLKN_SUCCESS;
}

std::array<float, 16> VlknContext::getTransformOrDefault(SVGA3dTransformType type) const {
    auto it = m_transforms.find((uint32_t)type);
    if (it != m_transforms.end()) {
        return it->second;
    }
    std::array<float, 16> identity = {};
    identity[0] = identity[5] = identity[10] = identity[15] = 1.0f;
    return identity;
}

static std::array<float, 16> multiplyMatrix4x4(const std::array<float, 16> &a, const std::array<float, 16> &b) {
    std::array<float, 16> r = {};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[i * 4 + k] * b[k * 4 + j];
            }
            r[i * 4 + j] = sum;
        }
    }
    return r;
}

Svga3VlknStatus VlknContext::setZRange(const SVGA3dZRange *zRange) {
    if (!zRange) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    m_zRange = *zRange;
    m_viewport.minDepth = zRange->min;
    m_viewport.maxDepth = zRange->max;
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknContext::setClipPlane(uint32_t index, const float plane[4]) {
    if (index >= 6 || !plane) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    memcpy(m_clipPlanes[index].plane, plane, sizeof(float) * 4);
    m_clipPlanes[index].enabled = true;
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknContext::getClipPlane(uint32_t index, float outPlane[4]) const {
    if (index >= 6 || !outPlane) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    memcpy(outPlane, m_clipPlanes[index].plane, sizeof(float) * 4);
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknContext::setMaterial(SVGA3dFace face, const SVGA3dMaterial *mat) {
    if (!mat) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    if (face == SVGA3D_FACE_FRONT || face == SVGA3D_FACE_FRONT_BACK) {
        m_materials[0] = *mat;
    }
    if (face == SVGA3D_FACE_BACK || face == SVGA3D_FACE_FRONT_BACK) {
        m_materials[1] = *mat;
    }
    return SVGA3_VLKN_SUCCESS;
}

const SVGA3dMaterial* VlknContext::getMaterial(SVGA3dFace face) const {
    if (face == SVGA3D_FACE_BACK) return &m_materials[1];
    return &m_materials[0];
}

Svga3VlknStatus VlknContext::setLightData(uint32_t index, const SVGA3dLightData *data) {
    if (index >= 8 || !data) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    m_lights[index].data = *data;
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknContext::setLightEnabled(uint32_t index, uint32_t enabled) {
    if (index >= 8) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    m_lights[index].enabled = (enabled != 0);
    return SVGA3_VLKN_SUCCESS;
}

bool VlknContext::isLightEnabled(uint32_t index) const {
    if (index >= 8) return false;
    return m_lights[index].enabled;
}

Svga3VlknStatus VlknContext::defineShader(uint32_t shid, SVGA3dShaderType type, const uint32_t *bytecode, uint32_t numDwords) {
    if (!bytecode || numDwords == 0) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    /* Reject unknown shader types instead of silently treating them as PS. */
    if (type != SVGA3D_SHADERTYPE_VS && type != SVGA3D_SHADERTYPE_PS) {
        log_msg("[libqemu_svga3d] defineShader error: bad shader type %u\n", (unsigned)type);
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }

    auto &table = (type == SVGA3D_SHADERTYPE_VS) ? m_vertexShaders : m_pixelShaders;
    auto it = table.find(shid);
    /* Cap the shader table: without a limit a guest can define an
     * unbounded number of shaders and exhaust host memory. */
    if (it == table.end() && table.size() >= SVGA3_MAX_SHADERS_PER_CONTEXT) {
        log_msg("[libqemu_svga3d] defineShader error: shader table full (%zu)\n", table.size());
        return SVGA3_VLKN_ERROR_OUT_OF_MEMORY;
    }
    if (it != table.end()) {
        /* Only flush if cached pipelines actually reference this shader.
         * A redefine with no dependent pipelines needs no queue drain;
         * flushing unconditionally would stall the draw/shader-creation
         * path on every redefine. */
        bool hasDependentPipelines = false;
        for (const auto &entry : m_pipelineCache) {
            bool usesShader = (type == SVGA3D_SHADERTYPE_VS) ? (entry.first.boundVS == shid)
                                                             : (entry.first.boundPS == shid);
            if (usesShader) {
                hasDependentPipelines = true;
                break;
            }
        }
        if (hasDependentPipelines) {
            /* Pipelines can remain referenced by recorded draw commands.
             * Complete those commands before evicting their cached pipelines. */
            endRenderPassIfActive();
            Svga3VlknStatus flushStatus = m_backend->flushCommandBuffer();
            if (flushStatus != SVGA3_VLKN_SUCCESS) return flushStatus;
            /* Evict before destroying the module so a later draw cannot hit a
             * pipeline cache entry built from the old shader. */
            for (auto pcIt = m_pipelineCache.begin(); pcIt != m_pipelineCache.end(); ) {
                bool usesShader = (type == SVGA3D_SHADERTYPE_VS) ? (pcIt->first.boundVS == shid)
                                                                 : (pcIt->first.boundPS == shid);
                if (usesShader) {
                    if (pcIt->second) {
                        m_backend->dispatch().vkDestroyPipeline(m_backend->device(), pcIt->second, nullptr);
                    }
                    pcIt = m_pipelineCache.erase(pcIt);
                } else {
                    ++pcIt;
                }
            }
        }
        if (it->second.module && it->second.module != m_defaultVS && it->second.module != m_defaultFS) {
            m_backend->dispatch().vkDestroyShaderModule(m_backend->device(), it->second.module, nullptr);
        }
        for (auto &variant : it->second.depthVariants) {
            /* Failed variants borrow the base module; they do not own it. */
            if (variant.second && variant.second != it->second.module) {
                m_backend->dispatch().vkDestroyShaderModule(m_backend->device(), variant.second, nullptr);
            }
        }
        /* Redefine replaces the old shader: release its aggregate budget
         * first so the new reservation is not charged on top of it. */
        m_backend->resourceBudgets().releaseShaderBytes(shaderBudgetBytes(it->second));
        m_backend->resourceBudgets().releaseShaderModules(shaderBudgetModules(it->second));
        table.erase(it);
    }

    /* Aggregate device budget: reject before translating or creating the
     * module, so a guest cannot exhaust host memory with many
     * per-object-legal shaders. */
    uint64_t newBytecodeBytes = (uint64_t)numDwords * sizeof(uint32_t);
    if (!m_backend->resourceBudgets().tryReserveShaderBytes(newBytecodeBytes)) {
        log_msg("[libqemu_svga3d] defineShader error: aggregate shader bytecode budget exhausted (shid=%u)\n",
                shid);
        return SVGA3_VLKN_ERROR_OUT_OF_MEMORY;
    }
    if (!m_backend->resourceBudgets().tryReserveShaderModules(1)) {
        m_backend->resourceBudgets().releaseShaderBytes(newBytecodeBytes);
        log_msg("[libqemu_svga3d] defineShader error: aggregate shader module budget exhausted (shid=%u)\n",
                shid);
        return SVGA3_VLKN_ERROR_OUT_OF_MEMORY;
    }
    /* Exception-safe release of the reservation on every failure path
     * below; committed once the shader lands in the table. */
    struct ShaderBudgetGuard {
        VlknBackend *backend;
        uint64_t bytes;
        bool active = true;
        ~ShaderBudgetGuard() {
            if (active && backend) {
                backend->resourceBudgets().releaseShaderBytes(bytes);
                backend->resourceBudgets().releaseShaderModules(1);
            }
        }
    } budgetGuard{m_backend, newBytecodeBytes};

    Svga3Shader shader;
    shader.shid = shid;
    shader.type = type;
    shader.bytecode.assign(bytecode, bytecode + numDwords);
    shader.inputLocationMask = 0;

    static const uint32_t SPIRV_MAGIC = 0x07230203;
    /* Raw guest SPIR-V passthrough: the D3D9 translator is bypassed
     * entirely and guest bytes go straight into vkCreateShaderModule,
     * exposing the host Vulkan driver's SPIR-V parser to the guest.
     * Rejected unconditionally in production builds. To enable for local
     * development only, compile with -DSVGA3_VLKN_DEV_ALLOW_GUEST_SPIRV.
     * There is intentionally no runtime (environment variable) escape:
     * a guest that can set host environment variables has already won. */
#ifdef SVGA3_VLKN_DEV_ALLOW_GUEST_SPIRV
    static const bool allowGuestSpirv = true;
#else
    static const bool allowGuestSpirv = false;
#endif
    if (bytecode[0] == SPIRV_MAGIC) {
        if (!allowGuestSpirv) {
            log_msg("[libqemu_svga3d] defineShader error: raw guest SPIR-V rejected (dev-only gate)\n");
            return SVGA3_VLKN_ERROR_INVALID_PARAM;
        }
        VkShaderModuleCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = numDwords * sizeof(uint32_t);
        info.pCode = bytecode;
        VkResult res = m_backend->dispatch().vkCreateShaderModule(m_backend->device(), &info, nullptr, &shader.module);
        if (res != VK_SUCCESS) {
            return SVGA3_VLKN_ERROR_PIPELINE_CREATION;
        }
    } else {
        std::vector<uint32_t> spirv;
        std::string err;
        uint32_t inMask = 0;
        Svga3VlknStatus st = svga3_translate_shader_d3d9(type, bytecode, numDwords, spirv, err, &inMask);
        if (st != SVGA3_VLKN_SUCCESS) {
            return st; /* Explicit error on unsupported instructions or malformed tokens */
        }
        shader.inputLocationMask = inMask;
        VkShaderModuleCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = spirv.size() * sizeof(uint32_t);
        info.pCode = spirv.data();
        VkResult res = m_backend->dispatch().vkCreateShaderModule(m_backend->device(), &info, nullptr, &shader.module);
        if (res != VK_SUCCESS) {
            return SVGA3_VLKN_ERROR_PIPELINE_CREATION;
        }
    }

    table[shid] = std::move(shader);
    budgetGuard.active = false;

    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknContext::destroyShader(uint32_t shid, SVGA3dShaderType type) {
    auto &table = (type == SVGA3D_SHADERTYPE_VS) ? m_vertexShaders : m_pixelShaders;
    auto it = table.find(shid);
    if (it == table.end()) {
        return SVGA3_VLKN_ERROR_NOT_FOUND;
    }
    /* Only flush if cached pipelines actually reference this shader.
     * A destroy with no dependent pipelines needs no queue drain. */
    bool hasDependentPipelines = false;
    for (const auto &entry : m_pipelineCache) {
        bool usesShader = (type == SVGA3D_SHADERTYPE_VS) ? (entry.first.boundVS == shid)
                                                         : (entry.first.boundPS == shid);
        if (usesShader) {
            hasDependentPipelines = true;
            break;
        }
    }
    if (hasDependentPipelines) {
        /* Wait for recorded users before evicting the cached pipeline. */
        endRenderPassIfActive();
        Svga3VlknStatus flushStatus = m_backend->flushCommandBuffer();
        if (flushStatus != SVGA3_VLKN_SUCCESS) return flushStatus;
        for (auto pcIt = m_pipelineCache.begin(); pcIt != m_pipelineCache.end(); ) {
            bool usesShader = (type == SVGA3D_SHADERTYPE_VS) ? (pcIt->first.boundVS == shid)
                                                             : (pcIt->first.boundPS == shid);
            if (usesShader) {
                if (pcIt->second) {
                    m_backend->dispatch().vkDestroyPipeline(m_backend->device(), pcIt->second, nullptr);
                }
                pcIt = m_pipelineCache.erase(pcIt);
            } else {
                ++pcIt;
            }
        }
    }
    if (it->second.module && it->second.module != m_defaultVS && it->second.module != m_defaultFS) {
        m_backend->dispatch().vkDestroyShaderModule(m_backend->device(), it->second.module, nullptr);
    }
    for (auto &variant : it->second.depthVariants) {
        /* Failed variants borrow the base module; they do not own it. */
        if (variant.second && variant.second != it->second.module) {
            m_backend->dispatch().vkDestroyShaderModule(m_backend->device(), variant.second, nullptr);
        }
    }
    m_backend->resourceBudgets().releaseShaderBytes(shaderBudgetBytes(it->second));
    m_backend->resourceBudgets().releaseShaderModules(shaderBudgetModules(it->second));
    table.erase(it);
    if (type == SVGA3D_SHADERTYPE_VS && m_boundVS == shid) {
        m_boundVS = SVGA3D_INVALID_ID;
        m_constantsDirty = true;
    }
    if (type == SVGA3D_SHADERTYPE_PS && m_boundPS == shid) m_boundPS = SVGA3D_INVALID_ID;
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknContext::setShader(SVGA3dShaderType type, uint32_t shid) {
    if (shid == SVGA3D_INVALID_ID) {
        if (type == SVGA3D_SHADERTYPE_VS) {
            if (m_boundVS != SVGA3D_INVALID_ID) m_constantsDirty = true;
            m_boundVS = SVGA3D_INVALID_ID;
        } else {
            m_boundPS = SVGA3D_INVALID_ID;
        }
        return SVGA3_VLKN_SUCCESS;
    }
    const auto &table = (type == SVGA3D_SHADERTYPE_VS) ? m_vertexShaders : m_pixelShaders;
    if (table.find(shid) == table.end()) {
        return SVGA3_VLKN_ERROR_NOT_FOUND;
    }
    if (type == SVGA3D_SHADERTYPE_VS) {
        if (m_boundVS != shid) m_constantsDirty = true;
        m_boundVS = shid;
    } else {
        m_boundPS = shid;
    }
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknContext::setShaderConst(uint32_t reg, SVGA3dShaderType type, SVGA3dShaderConstType ctype, const uint32_t values[4]) {
    if (!values) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    ShaderConstantBank &bank = (type == SVGA3D_SHADERTYPE_VS) ? m_vsConsts : m_psConsts;

    if (ctype == SVGA3D_CONST_TYPE_FLOAT) {
        if (reg >= 256) return SVGA3_VLKN_ERROR_INVALID_PARAM;
        const float *f = reinterpret_cast<const float*>(values);
        bank.floatConsts[reg][0] = f[0];
        bank.floatConsts[reg][1] = f[1];
        bank.floatConsts[reg][2] = f[2];
        bank.floatConsts[reg][3] = f[3];
    } else if (ctype == SVGA3D_CONST_TYPE_INT) {
        if (reg >= 16) return SVGA3_VLKN_ERROR_INVALID_PARAM;
        const int32_t *i = reinterpret_cast<const int32_t*>(values);
        bank.intConsts[reg][0] = i[0];
        bank.intConsts[reg][1] = i[1];
        bank.intConsts[reg][2] = i[2];
        bank.intConsts[reg][3] = i[3];
    } else if (ctype == SVGA3D_CONST_TYPE_BOOL) {
        if (reg >= 16) return SVGA3_VLKN_ERROR_INVALID_PARAM;
        if (values[0]) {
            bank.boolConsts |= (1u << reg);
        } else {
            bank.boolConsts &= ~(1u << reg);
        }
    } else {
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }
    m_constantsDirty = true;
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknContext::getShaderConst(uint32_t reg, SVGA3dShaderType type, SVGA3dShaderConstType ctype, uint32_t outValues[4]) const {
    if (!outValues) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    const ShaderConstantBank &bank = (type == SVGA3D_SHADERTYPE_VS) ? m_vsConsts : m_psConsts;

    if (ctype == SVGA3D_CONST_TYPE_FLOAT) {
        if (reg >= 256) return SVGA3_VLKN_ERROR_INVALID_PARAM;
        memcpy(outValues, bank.floatConsts[reg], sizeof(float) * 4);
    } else if (ctype == SVGA3D_CONST_TYPE_INT) {
        if (reg >= 16) return SVGA3_VLKN_ERROR_INVALID_PARAM;
        memcpy(outValues, bank.intConsts[reg], sizeof(int32_t) * 4);
    } else if (ctype == SVGA3D_CONST_TYPE_BOOL) {
        if (reg >= 16) return SVGA3_VLKN_ERROR_INVALID_PARAM;
        outValues[0] = (bank.boolConsts & (1u << reg)) ? 1 : 0;
        outValues[1] = outValues[2] = outValues[3] = 0;
    } else {
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }
    return SVGA3_VLKN_SUCCESS;
}

uint32_t VlknContext::getBoundShader(SVGA3dShaderType type) const {
    return (type == SVGA3D_SHADERTYPE_VS) ? m_boundVS : m_boundPS;
}

bool VlknContext::hasShader(uint32_t shid, SVGA3dShaderType type) const {
    const auto &table = (type == SVGA3D_SHADERTYPE_VS) ? m_vertexShaders : m_pixelShaders;
    return table.find(shid) != table.end();
}

VkSampler VlknContext::getOrCreateSampler(uint32_t stage) {
    TextureStageState &s = m_stages[stage];
    if (s.sampler && !s.samplerDirty) {
        return s.sampler;
    }

    if (s.sampler) {
        m_backend->dispatch().vkDestroySampler(m_backend->device(), s.sampler, nullptr);
        s.sampler = VK_NULL_HANDLE;
    }

    VkSamplerCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = (s.magFilter == SVGA3D_TEX_FILTER_LINEAR) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.minFilter = (s.minFilter == SVGA3D_TEX_FILTER_LINEAR) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.mipmapMode = (s.mipFilter == SVGA3D_TEX_FILTER_LINEAR) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = ::svga3_texture_address_to_vk((SVGA3dTextureAddress)s.addressU);
    info.addressModeV = ::svga3_texture_address_to_vk((SVGA3dTextureAddress)s.addressV);
    info.addressModeW = ::svga3_texture_address_to_vk((SVGA3dTextureAddress)s.addressW);
    info.mipLodBias = s.mipLodBias;
    info.anisotropyEnable = (s.maxAnisotropy > 1) ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = (float)std::max(1u, s.maxAnisotropy);
    info.minLod = 0.0f;
    /* FIX: Clamp maxLod = 0.0f when mipmapping is disabled or surface has <= 1 mip */
    VlknSurface *surf = m_surfaceMgr ? m_surfaceMgr->getSurface(s.sid) : nullptr;
    if (s.mipFilter == SVGA3D_TEX_FILTER_NONE || (surf && surf->viewMipLevels() <= 1)) {
        info.maxLod = 0.0f;
    } else if (surf && surf->viewMipLevels() > 1) {
        info.maxLod = (float)(surf->viewMipLevels() - 1);
    } else {
        info.maxLod = 16.0f;
    }

    m_backend->dispatch().vkCreateSampler(m_backend->device(), &info, nullptr, &s.sampler);
    s.samplerDirty = false;
    return s.sampler;
}

Svga3VlknStatus VlknContext::ensureRenderPassActive() {
    if (m_inRenderPass) return SVGA3_VLKN_SUCCESS;

    VlknSurface *colorSurf = m_surfaceMgr->getSurface(m_renderTargets[0].sid);
    VlknSurface *depthSurf = m_surfaceMgr->getSurface(m_depthStencilTarget.sid);

    static uint32_t erp_count = 0;
    erp_count++;
    if (erp_count <= 5 || (erp_count % 500) == 0) {
        log_msg("[libqemu_svga3d] ensureRenderPassActive #%u: colorSid=%u (surf=%p), depthSid=%u (surf=%p)\n",
                erp_count, m_renderTargets[0].sid, colorSurf, m_depthStencilTarget.sid, depthSurf);
    }

    VkFormat colorFmt = colorSurf ? colorSurf->vkFormat() : VK_FORMAT_UNDEFINED;
    VkFormat depthFmt = depthSurf ? depthSurf->vkFormat() : VK_FORMAT_UNDEFINED;

    if (colorFmt == VK_FORMAT_UNDEFINED && depthFmt == VK_FORMAT_UNDEFINED) {
        colorFmt = VK_FORMAT_B8G8R8A8_UNORM; /* Default fallback target */
    }

    m_activeRenderPass = m_backend->getOrCreateRenderPass(colorFmt, depthFmt);
    if (m_activeRenderPass == VK_NULL_HANDLE) {
        log_msg("[libqemu_svga3d] ensureRenderPassActive error: render pass creation failed\n");
        return SVGA3_VLKN_ERROR_DEVICE_LOST;
    }

    /* Build Framebuffer */
    uint32_t fbWidth = 800;
    uint32_t fbHeight = 600;
    if (colorSurf) {
        fbWidth = colorSurf->width();
        fbHeight = colorSurf->height();
    } else if (depthSurf) {
        fbWidth = depthSurf->width();
        fbHeight = depthSurf->height();
    }

    VkImageView colorView = colorSurf ? colorSurf->getRenderTargetView(m_renderTargets[0].mipmap, m_renderTargets[0].face) : VK_NULL_HANDLE;
    VkImageView depthView = depthSurf ? depthSurf->getRenderTargetView(m_depthStencilTarget.mipmap, m_depthStencilTarget.face) : VK_NULL_HANDLE;

    bool fbNeedsRebuild = (m_activeFramebuffer == VK_NULL_HANDLE) ||
                          (m_fbColorSid != m_renderTargets[0].sid) ||
                          (m_fbColorMip != m_renderTargets[0].mipmap) ||
                          (m_fbColorFace != m_renderTargets[0].face) ||
                          (m_fbColorView != colorView) ||
                          (m_fbDepthSid != m_depthStencilTarget.sid) ||
                          (m_fbDepthMip != m_depthStencilTarget.mipmap) ||
                          (m_fbDepthFace != m_depthStencilTarget.face) ||
                          (m_fbDepthView != depthView) ||
                          (m_fbWidth != fbWidth) ||
                          (m_fbHeight != fbHeight);

    if (fbNeedsRebuild) {
        if (m_activeFramebuffer != VK_NULL_HANDLE) {
            m_backend->flushCommandBuffer();
            m_backend->dispatch().vkDestroyFramebuffer(m_backend->device(), m_activeFramebuffer, nullptr);
            m_activeFramebuffer = VK_NULL_HANDLE;
        }

        VkImageView attachments[2];
        uint32_t attachCount = 0;
        if (colorSurf && colorView != VK_NULL_HANDLE) {
            attachments[attachCount++] = colorView;
        }
        if (depthSurf && depthView != VK_NULL_HANDLE) {
            attachments[attachCount++] = depthView;
        }

        VkFramebufferCreateInfo fbInfo = {};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = m_activeRenderPass;
        fbInfo.attachmentCount = attachCount;
        fbInfo.pAttachments = attachments;
        fbInfo.width = fbWidth;
        fbInfo.height = fbHeight;
        fbInfo.layers = 1;

        VkResult fbRes = m_backend->dispatch().vkCreateFramebuffer(m_backend->device(), &fbInfo, nullptr, &m_activeFramebuffer);
        if (fbRes != VK_SUCCESS) {
            log_msg("[libqemu_svga3d] ERROR: vkCreateFramebuffer failed with %d\n", fbRes);
            return SVGA3_VLKN_ERROR_DEVICE_LOST;
        }

        m_fbColorSid = m_renderTargets[0].sid;
        m_fbColorMip = m_renderTargets[0].mipmap;
        m_fbColorFace = m_renderTargets[0].face;
        m_fbColorView = colorView;
        m_fbDepthSid = m_depthStencilTarget.sid;
        m_fbDepthMip = m_depthStencilTarget.mipmap;
        m_fbDepthFace = m_depthStencilTarget.face;
        m_fbDepthView = depthView;
        m_fbWidth = fbWidth;
        m_fbHeight = fbHeight;
    }

    VkCommandBuffer cb = m_backend->getActiveCommandBuffer();

    if (colorSurf && colorSurf->currentLayout() != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = colorSurf->currentLayout();
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        barrier.image = colorSurf->image();
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = colorSurf->mipLevels();
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = colorSurf->arrayLayers();

        m_backend->dispatch().vkCmdPipelineBarrier(
            cb,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier
        );
        colorSurf->setLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }

    if (depthSurf && depthSurf->currentLayout() != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = depthSurf->currentLayout();
        barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        barrier.image = depthSurf->image();
        barrier.subresourceRange.aspectMask = svga3_format_has_stencil(depthSurf->svgaFormat()) ?
            (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = depthSurf->mipLevels();
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = depthSurf->arrayLayers();

        m_backend->dispatch().vkCmdPipelineBarrier(
            cb,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier
        );
        depthSurf->setLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    }

    VkRenderPassBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    beginInfo.renderPass = m_activeRenderPass;
    beginInfo.framebuffer = m_activeFramebuffer;
    beginInfo.renderArea.offset.x = 0;
    beginInfo.renderArea.offset.y = 0;
    beginInfo.renderArea.extent.width = fbWidth;
    beginInfo.renderArea.extent.height = fbHeight;

    m_backend->dispatch().vkCmdBeginRenderPass(cb, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
    m_inRenderPass = true;

    return SVGA3_VLKN_SUCCESS;
}

void VlknContext::endRenderPassIfActive() {
    if (m_inRenderPass) {
        VkCommandBuffer cb = m_backend->getActiveCommandBuffer();
        m_backend->dispatch().vkCmdEndRenderPass(cb);
        m_inRenderPass = false;
    }
}

void VlknContext::clearDescriptorSetCache() {
    if (!m_backend || m_descriptorSetCache.empty()) {
        m_descriptorSet = VK_NULL_HANDLE;
        m_descriptorSetInitialized = false;
        return;
    }

    std::vector<VkDescriptorSet> sets;
    sets.reserve(m_descriptorSetCache.size());
    for (const auto &entry : m_descriptorSetCache) {
        sets.push_back(entry.second);
    }
    if (m_backend->dispatch().vkFreeDescriptorSets) {
        m_backend->dispatch().vkFreeDescriptorSets(m_backend->device(), m_backend->descriptorPool(),
                                                    static_cast<uint32_t>(sets.size()), sets.data());
    }
    m_descriptorSetCache.clear();
    m_descriptorSet = VK_NULL_HANDLE;
    m_descriptorSetInitialized = false;
}

void VlknContext::invalidateSurface(uint32_t sid) {
    endRenderPassIfActive();
    /* The surface may still be referenced by recorded draws or descriptors. */
    m_backend->flushCommandBuffer();
    /* Descriptor sets are immutable while commands using them are pending.
     * Retire the cache after the flush so destroyed image views are released. */
    clearDescriptorSetCache();
    m_descriptorSetDirty = true;

    bool fbAffected = false;
    for (uint32_t i = 0; i < SVGA3_MAX_RENDER_TARGETS; ++i) {
        if (m_renderTargets[i].sid == sid) {
            fbAffected = true;
            break;
        }
    }
    if (m_depthStencilTarget.sid == sid) {
        fbAffected = true;
    }

    if (fbAffected) {
        if (m_activeFramebuffer != VK_NULL_HANDLE) {
            m_backend->dispatch().vkDestroyFramebuffer(m_backend->device(), m_activeFramebuffer, nullptr);
            m_activeFramebuffer = VK_NULL_HANDLE;
        }
        m_fbColorSid = SVGA3D_INVALID_ID;
        m_fbDepthSid = SVGA3D_INVALID_ID;
        m_fbColorView = VK_NULL_HANDLE;
        m_fbDepthView = VK_NULL_HANDLE;
    }

    for (uint32_t i = 0; i < SVGA3_MAX_TEXTURE_STAGES; ++i) {
        if (m_stages[i].sid == sid) {
            m_descriptorSetDirty = true;
            m_boundImageViews[i] = VK_NULL_HANDLE;
            m_boundSamplers[i] = VK_NULL_HANDLE;
        }
    }
}

Svga3VlknStatus VlknContext::clear(SVGA3dClearFlag flags,
                                  uint32_t colorRGBA,
                                  float depth,
                                  uint32_t stencil,
                                  const SVGA3dRect *rects,
                                  uint32_t numRects)
{
    bool wantColor = (flags & SVGA3D_CLEAR_COLOR) != 0;
    bool wantDepth = (flags & (SVGA3D_CLEAR_DEPTH | SVGA3D_CLEAR_STENCIL)) != 0;
    bool haveColor = m_renderTargets[0].sid != SVGA3D_INVALID_ID && m_renderTargets[0].sid != 0;
    bool haveDepth = m_depthStencilTarget.sid != SVGA3D_INVALID_ID && m_depthStencilTarget.sid != 0;
    if (wantColor && !haveColor) flags = (SVGA3dClearFlag)(flags & ~SVGA3D_CLEAR_COLOR);
    if (wantDepth && !haveDepth) flags = (SVGA3dClearFlag)(flags & ~(SVGA3D_CLEAR_DEPTH | SVGA3D_CLEAR_STENCIL));
    if (flags == 0) return SVGA3_VLKN_SUCCESS;

    ensureRenderPassActive();
    VkCommandBuffer cb = m_backend->getActiveCommandBuffer();

    VkClearAttachment attachments[2];
    uint32_t attachCount = 0;

    if (flags & SVGA3D_CLEAR_COLOR) {
        VkClearAttachment &a = attachments[attachCount++];
        a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        a.colorAttachment = 0;
        /* Unpack RGBA */
        float r = ((colorRGBA >> 16) & 0xFF) / 255.0f;
        float g = ((colorRGBA >> 8) & 0xFF) / 255.0f;
        float b = (colorRGBA & 0xFF) / 255.0f;
        float alpha = ((colorRGBA >> 24) & 0xFF) / 255.0f;
        a.clearValue.color.float32[0] = r;
        a.clearValue.color.float32[1] = g;
        a.clearValue.color.float32[2] = b;
        a.clearValue.color.float32[3] = alpha;
    }

    if ((flags & SVGA3D_CLEAR_DEPTH) || (flags & SVGA3D_CLEAR_STENCIL)) {
        VkClearAttachment &a = attachments[attachCount++];
        a.aspectMask = 0;
        if (flags & SVGA3D_CLEAR_DEPTH) a.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (flags & SVGA3D_CLEAR_STENCIL) a.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        a.colorAttachment = 0;
        a.clearValue.depthStencil.depth = depth;
        a.clearValue.depthStencil.stencil = stencil;
    }

    std::vector<VkClearRect> clearRects;
    if (numRects > 0 && rects) {
        for (uint32_t i = 0; i < numRects; ++i) {
            VkClearRect r = {};
            r.rect.offset.x = (int32_t)rects[i].x;
            r.rect.offset.y = (int32_t)rects[i].y;
            r.rect.extent.width = rects[i].w;
            r.rect.extent.height = rects[i].h;
            r.baseArrayLayer = 0;
            r.layerCount = 1;
            clearRects.push_back(r);
        }
    } else {
        VkClearRect r = {};
        r.rect.offset.x = (int32_t)m_viewport.x;
        r.rect.offset.y = (int32_t)m_viewport.y;
        r.rect.extent.width = (uint32_t)m_viewport.width;
        r.rect.extent.height = (uint32_t)m_viewport.height;
        r.baseArrayLayer = 0;
        r.layerCount = 1;
        clearRects.push_back(r);
    }

    if (attachCount > 0 && !clearRects.empty()) {
        m_backend->dispatch().vkCmdClearAttachments(
            cb, attachCount, attachments, (uint32_t)clearRects.size(), clearRects.data()
        );
    }

    m_clearCount++;
    uint32_t rtSid = m_renderTargets[0].sid;
    if (rtSid != 0 && rtSid != SVGA3D_INVALID_ID) {
        VlknSurface *rtSurf = m_surfaceMgr->getSurface(rtSid);
        if (rtSurf && rtSurf->width() >= 128 && rtSurf->height() >= 128 && !rtSurf->isDepthStencil()) {
            rtSurf->invalidateReadback();
            markWindowDrawn(rtSid);
        }
    }
    return SVGA3_VLKN_SUCCESS;
}

VkPipeline VlknContext::getOrCreatePipeline(SVGA3dPrimitiveType primitiveType,
                                            const SVGA3dVertexDecl *decls,
                                            uint32_t numDecls,
                                            VkRenderPass renderPass)
{
    PipelineKey key = {};
    key.topology = (uint32_t)::svga3_primitive_to_vk(primitiveType);

    SVGA3dFillMode fm;
    fm.uintValue = m_renderStates[SVGA3D_RS_FILLMODE];
    key.fillMode = fm.s.mode;

    bool hasDepthAttachment = (m_depthStencilTarget.sid != SVGA3D_INVALID_ID && m_depthStencilTarget.sid != 0);
    key.cullMode = m_renderStates[SVGA3D_RS_CULLMODE];
    key.depthTestEnable = hasDepthAttachment ? m_renderStates[SVGA3D_RS_ZENABLE] : 0;
    key.depthWriteEnable = hasDepthAttachment ? m_renderStates[SVGA3D_RS_ZWRITEENABLE] : 0;
    key.depthFunc = m_renderStates[SVGA3D_RS_ZFUNC];
    key.blendEnable = m_renderStates[SVGA3D_RS_BLENDENABLE];
    key.srcColorBlend = m_renderStates[SVGA3D_RS_SRCBLEND];
    key.dstColorBlend = m_renderStates[SVGA3D_RS_DSTBLEND];
    key.colorBlendEq = m_renderStates[SVGA3D_RS_BLENDEQUATION];
    key.srcAlphaBlend = m_renderStates[SVGA3D_RS_SRCBLENDALPHA];
    key.dstAlphaBlend = m_renderStates[SVGA3D_RS_DSTBLENDALPHA];
    key.alphaBlendEq = m_renderStates[SVGA3D_RS_BLENDEQUATIONALPHA];
    key.stencilEnable = hasDepthAttachment ? m_renderStates[SVGA3D_RS_STENCILENABLE] : 0;
    key.stencilFunc = m_renderStates[SVGA3D_RS_STENCILFUNC];
    key.colorWriteMask = m_renderStates[SVGA3D_RS_COLORWRITEENABLE];
    key.numVertexDecls = numDecls;
    key.renderPass = renderPass;

    /* Hash vertex decls into key */
    uint64_t h = 0;
    if (numDecls > 0 && decls) {
        for (uint32_t i = 0; i < numDecls; ++i) {
            h ^= ((uint64_t)decls[i].identity.type << 24) |
                 ((uint64_t)decls[i].identity.usage << 16) |
                 ((uint64_t)decls[i].identity.usageIndex << 8) |
                 decls[i].array.stride;
            h = (h << 5) | (h >> 59);
        }
    }
    key.vertexDeclHash = h;

    key.boundVS = m_boundVS;
    key.boundPS = m_boundPS;
    key.depthSamplerMask = 0;
    if (m_boundPS != SVGA3D_INVALID_ID && m_surfaceMgr) {
        for (uint32_t i = 0; i < SVGA3_MAX_TEXTURE_STAGES && i < 8; ++i) {
            if (m_stages[i].sid == SVGA3D_INVALID_ID || m_stages[i].sid == 0) continue;
            VlknSurface *stageSurf = m_surfaceMgr->getSurface(m_stages[i].sid);
            if (stageSurf && stageSurf->isDepthStencil()) {
                key.depthSamplerMask |= (1u << i);
            }
        }
    }
    bool useFfTex = (m_boundPS == SVGA3D_INVALID_ID && m_stages[0].sid != SVGA3D_INVALID_ID && m_stages[0].sid != 0 && m_surfaceMgr->getSurface(m_stages[0].sid) != nullptr);
    bool hasVertexColor = false;
    if (useFfTex && numDecls > 0 && decls) {
        for (uint32_t i = 0; i < numDecls; ++i) {
            if (decls[i].identity.usage == SVGA3D_DECLUSAGE_COLOR) {
                hasVertexColor = true;
                break;
            }
        }
    }
    key.ffTextureStage0 = useFfTex ? (hasVertexColor ? 1 : 2) : 0;

    auto it = m_pipelineCache.find(key);
    if (it != m_pipelineCache.end()) {
        return it->second;
    }

    /* Create Graphics Pipeline */
    VkGraphicsPipelineCreateInfo pipeInfo = {};
    pipeInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;

    /* Shader stages */
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = m_defaultVS;
    stages[0].pName = "main";
    if (m_boundVS != SVGA3D_INVALID_ID) {
        auto vit = m_vertexShaders.find(m_boundVS);
        if (vit != m_vertexShaders.end() && vit->second.module) {
            stages[0].module = vit->second.module;
        }
    }

    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    if (key.ffTextureStage0 == 1) {
        stages[1].module = m_defaultFSTex;
    } else if (key.ffTextureStage0 == 2) {
        stages[1].module = m_defaultFSTexPure;
    } else {
        stages[1].module = m_defaultFS;
    }
    stages[1].pName = "main";
    if (m_boundPS != SVGA3D_INVALID_ID) {
        auto pit = m_pixelShaders.find(m_boundPS);
        if (pit != m_pixelShaders.end() && pit->second.module) {
            stages[1].module = pit->second.module;
            if (key.depthSamplerMask != 0) {
                auto variant = pit->second.depthVariants.find(key.depthSamplerMask);
                if (variant == pit->second.depthVariants.end()) {
                    std::vector<uint32_t> spirv;
                    std::string err;
                    Svga3VlknStatus st = svga3_translate_shader_d3d9(
                        SVGA3D_SHADERTYPE_PS, pit->second.bytecode.data(),
                        (uint32_t)pit->second.bytecode.size(), spirv, err, nullptr,
                        key.depthSamplerMask);
                    VkShaderModule depthModule = VK_NULL_HANDLE;
                    if (st == SVGA3_VLKN_SUCCESS) {
                        VkShaderModuleCreateInfo info = {};
                        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                        info.codeSize = spirv.size() * sizeof(uint32_t);
                        info.pCode = spirv.data();
                        m_backend->dispatch().vkCreateShaderModule(m_backend->device(), &info, nullptr, &depthModule);
                    }
                    if (depthModule != VK_NULL_HANDLE) {
                        /* Aggregate module budget: a guest that forces many
                         * distinct depth-sampler masks must not mint
                         * unbounded shader modules. On exhaustion, borrow
                         * the base module like a failed variant. */
                        if (!m_backend->resourceBudgets().tryReserveShaderModules(1)) {
                            m_backend->dispatch().vkDestroyShaderModule(m_backend->device(), depthModule, nullptr);
                            depthModule = VK_NULL_HANDLE;
                        }
                    }
                    if (depthModule == VK_NULL_HANDLE) {
                        log_msg("[libqemu_svga3d] depth shader variant failed for ps %u mask 0x%x: %s\n",
                                m_boundPS, key.depthSamplerMask, err.c_str());
                        depthModule = pit->second.module;
                    }
                    pit->second.depthVariants[key.depthSamplerMask] = depthModule;
                    variant = pit->second.depthVariants.find(key.depthSamplerMask);
                }
                if (variant->second) {
                    stages[1].module = variant->second;
                }
            }
        }
    }

    pipeInfo.stageCount = 2;
    pipeInfo.pStages = stages;

    /* Vertex Input State */
    /* Cap guest-controlled decl count: uncapped counts exceeded device
     * vertex-binding limits and wasted pipeline-cache entries. */
    uint32_t effNumDecls = std::min(numDecls, SVGA3_MAX_VERTEX_DECLS);

    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attrs;

    if (effNumDecls > 0 && decls) {
        for (uint32_t i = 0; i < effNumDecls; ++i) {
            VkVertexInputBindingDescription b = {};
            b.binding = i;
            b.stride = decls[i].array.stride;
            b.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            bindings.push_back(b);

            VkVertexInputAttributeDescription a = {};
            a.binding = i;
            uint32_t loc = i;
            if (decls[i].identity.usage == SVGA3D_DECLUSAGE_POSITION) {
                loc = 0;
            } else if (decls[i].identity.usage == SVGA3D_DECLUSAGE_COLOR) {
                loc = (decls[i].identity.usageIndex == 0) ? 1 : 7;
            } else if (decls[i].identity.usage == SVGA3D_DECLUSAGE_TEXCOORD) {
                /* usageIndex is guest-controlled: 2+0xFFFFFFFF wrapped in
                 * 32-bit. Clamp it. */
                uint32_t uidx = std::min(decls[i].identity.usageIndex, SVGA3_MAX_DECL_USAGE_INDEX);
                loc = 2 + uidx;
            } else if (decls[i].identity.usage == SVGA3D_DECLUSAGE_NORMAL) {
                loc = 6;
            }
            a.location = loc;
            a.format = svga3_decl_type_to_vk((SVGA3dDeclType)decls[i].identity.type);
            a.offset = 0;
            attrs.push_back(a);
        }
    }

    uint32_t providedInputMask = 0;
    for (const auto &a : attrs) {
        /* a.location is guest-derived; shifting by >= 32 is UB. */
        if (a.location < 32) {
            providedInputMask |= (1u << a.location);
        }
    }

    uint32_t requiredInputMask = (m_boundVS != SVGA3D_INVALID_ID) ?
        m_vertexShaders[m_boundVS].inputLocationMask : m_defaultVsInputMask;

    uint32_t missingInputMask = requiredInputMask & ~providedInputMask;
    VkBuffer fallbackBuf = (m_backend && m_backend->fallbackBuffer()) ? m_backend->fallbackBuffer() : m_dummyVb;
    if (missingInputMask != 0 && fallbackBuf) {
        uint32_t dummyBindingIdx = (uint32_t)bindings.size();
        VkVertexInputBindingDescription dummyBinding = {};
        dummyBinding.binding = dummyBindingIdx;
        dummyBinding.stride = 0;
        dummyBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        bindings.push_back(dummyBinding);

        for (uint32_t loc = 0; loc < 16; ++loc) {
            if (missingInputMask & (1u << loc)) {
                VkVertexInputAttributeDescription dummyAttr = {};
                dummyAttr.binding = dummyBindingIdx;
                dummyAttr.location = loc;
                dummyAttr.format = VK_FORMAT_R32G32B32A32_SFLOAT;
                dummyAttr.offset = 0;
                attrs.push_back(dummyAttr);
            }
        }
    }

    VkPipelineVertexInputStateCreateInfo viInfo = {};
    viInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    viInfo.vertexBindingDescriptionCount = (uint32_t)bindings.size();
    viInfo.pVertexBindingDescriptions = bindings.data();
    viInfo.vertexAttributeDescriptionCount = (uint32_t)attrs.size();
    viInfo.pVertexAttributeDescriptions = attrs.data();
    pipeInfo.pVertexInputState = &viInfo;

    /* Input Assembly */
    VkPipelineInputAssemblyStateCreateInfo iaInfo = {};
    iaInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    iaInfo.topology = (VkPrimitiveTopology)key.topology;
    iaInfo.primitiveRestartEnable = VK_FALSE;
    pipeInfo.pInputAssemblyState = &iaInfo;

    /* Viewport / Scissor */
    VkPipelineViewportStateCreateInfo vpInfo = {};
    vpInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vpInfo.viewportCount = 1;
    vpInfo.pViewports = &m_viewport;
    vpInfo.scissorCount = 1;
    vpInfo.pScissors = &m_scissor;
    pipeInfo.pViewportState = &vpInfo;

    /* Rasterization */
    VkPipelineRasterizationStateCreateInfo rastInfo = {};
    rastInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rastInfo.polygonMode = (key.fillMode == SVGA3D_FILLMODE_LINE) ? VK_POLYGON_MODE_LINE :
                           (key.fillMode == SVGA3D_FILLMODE_POINT) ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL;

    /* Compensate for vertex shader Y-inversion epilogue (pos.y = -pos.y):
     * D3D9 front-face is CW, but clip-space Y-flip inverts winding order to CCW. */
    rastInfo.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    /* In SVGA3D / D3D9:
     * SVGA3D_FACE_FRONT = 2 (D3DCULL_CW: cull clockwise triangles)
     * SVGA3D_FACE_BACK  = 3 (D3DCULL_CCW: cull counter-clockwise triangles)
     * With frontFace = CCW in Vulkan:
     *   CW triangles are BACK faces  -> VK_CULL_MODE_BACK_BIT
     *   CCW triangles are FRONT faces -> VK_CULL_MODE_FRONT_BIT */
    switch (key.cullMode) {
        case SVGA3D_FACE_FRONT:
            rastInfo.cullMode = (rastInfo.frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE) ?
                                VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_BIT;
            break;
        case SVGA3D_FACE_BACK:
            rastInfo.cullMode = (rastInfo.frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE) ?
                                VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
            break;
        case SVGA3D_FACE_FRONT_BACK:
            rastInfo.cullMode = VK_CULL_MODE_FRONT_AND_BACK;
            break;
        case SVGA3D_FACE_NONE:
        default:
            rastInfo.cullMode = VK_CULL_MODE_NONE;
            break;
    }
    rastInfo.lineWidth = 1.0f;
    pipeInfo.pRasterizationState = &rastInfo;

    /* Multisample */
    VkPipelineMultisampleStateCreateInfo msInfo = {};
    msInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    pipeInfo.pMultisampleState = &msInfo;

    /* Depth / Stencil */
    VkPipelineDepthStencilStateCreateInfo dsInfo = {};
    dsInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dsInfo.depthTestEnable = key.depthTestEnable ? VK_TRUE : VK_FALSE;
    dsInfo.depthWriteEnable = key.depthWriteEnable ? VK_TRUE : VK_FALSE;
    dsInfo.depthCompareOp = ::svga3_cmp_func_to_vk((SVGA3dCmpFunc)key.depthFunc);
    dsInfo.stencilTestEnable = key.stencilEnable ? VK_TRUE : VK_FALSE;
    pipeInfo.pDepthStencilState = &dsInfo;

    /* Color Blend */
    VkPipelineColorBlendAttachmentState cbAttach = {};
    cbAttach.blendEnable = key.blendEnable ? VK_TRUE : VK_FALSE;
    cbAttach.srcColorBlendFactor = ::svga3_blend_factor_to_vk((SVGA3dBlendOp)key.srcColorBlend);
    cbAttach.dstColorBlendFactor = ::svga3_blend_factor_to_vk((SVGA3dBlendOp)key.dstColorBlend);
    cbAttach.colorBlendOp = ::svga3_blend_eq_to_vk((SVGA3dBlendEquation)key.colorBlendEq);
    cbAttach.srcAlphaBlendFactor = ::svga3_blend_factor_to_vk((SVGA3dBlendOp)key.srcAlphaBlend);
    cbAttach.dstAlphaBlendFactor = ::svga3_blend_factor_to_vk((SVGA3dBlendOp)key.dstAlphaBlend);
    cbAttach.alphaBlendOp = ::svga3_blend_eq_to_vk((SVGA3dBlendEquation)key.alphaBlendEq);
    cbAttach.colorWriteMask = key.colorWriteMask & 0xF;

    VkPipelineColorBlendStateCreateInfo blendInfo = {};
    blendInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blendInfo.attachmentCount = 1;
    blendInfo.pAttachments = &cbAttach;
    pipeInfo.pColorBlendState = &blendInfo;

    /* Dynamic State */
    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynInfo = {};
    dynInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynInfo.dynamicStateCount = 2;
    dynInfo.pDynamicStates = dynStates;
    pipeInfo.pDynamicState = &dynInfo;

    pipeInfo.layout = m_defaultPipelineLayout;
    pipeInfo.renderPass = renderPass;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult pipeRes = m_backend->dispatch().vkCreateGraphicsPipelines(
        m_backend->device(), VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &pipeline
    );
    if (pipeRes != VK_SUCCESS) {
        log_msg("[libqemu_svga3d] ERROR: vkCreateGraphicsPipelines failed with VkResult=%d\n", pipeRes);
        return VK_NULL_HANDLE;
    }

    m_pipelineCache[key] = pipeline;
    return pipeline;
}

/* Overflow-safe vertex count: primCount is guest-controlled, and
 * primCount*3 wrapped in 32-bit. Clamp to a sane maximum. */
static uint32_t calcVertexCount(SVGA3dPrimitiveType type, uint32_t primCount) {
    if (primCount > SVGA3_MAX_PRIMITIVES_PER_DRAW) {
        primCount = SVGA3_MAX_PRIMITIVES_PER_DRAW;
    }
    uint64_t count = 0;
    switch (type) {
        case SVGA3D_PRIMITIVE_POINTLIST:     count = primCount; break;
        case SVGA3D_PRIMITIVE_LINELIST:      count = (uint64_t)primCount * 2; break;
        case SVGA3D_PRIMITIVE_LINESTRIP:     count = (uint64_t)primCount + 1; break;
        case SVGA3D_PRIMITIVE_TRIANGLELIST:  count = (uint64_t)primCount * 3; break;
        case SVGA3D_PRIMITIVE_TRIANGLESTRIP: count = (uint64_t)primCount + 2; break;
        case SVGA3D_PRIMITIVE_TRIANGLEFAN:   count = (uint64_t)primCount + 2; break;
        default:                             count = (uint64_t)primCount * 3; break;
    }
    if (count > SVGA3_MAX_VERTICES_PER_DRAW) {
        count = SVGA3_MAX_VERTICES_PER_DRAW;
    }
    return (uint32_t)count;
}

Svga3VlknStatus VlknContext::draw(SVGA3dPrimitiveType primitiveType,
                                 const SVGA3dVertexDecl *decls,
                                 uint32_t numDecls,
                                 const SVGA3dPrimitiveRange *ranges,
                                 uint32_t numRanges)
{
    if (getenv("SVGA3_VLKN_TRACE_FIFO")) {
        fprintf(stderr, "[draw-state] cullMode=%u (1=NONE,2=FRONT,3=BACK)\n",
                m_renderStates[SVGA3D_RS_CULLMODE]);
        fprintf(stderr, "[draw-state] viewport=(%g,%g %gx%g) scissor=(%d,%d %ux%u) rt0=%u\n",
                m_viewport.x, m_viewport.y, m_viewport.width, m_viewport.height,
                m_scissor.offset.x, m_scissor.offset.y,
                m_scissor.extent.width, m_scissor.extent.height,
                m_renderTargets[0].sid);
    }
    /* Compute MVP = World * View * Projection when using fixed-function vertex shader */
    std::array<float, 16> ffMvp{};
    if (m_boundVS == SVGA3D_INVALID_ID) {
        std::array<float, 16> world = getTransformOrDefault(SVGA3D_TRANSFORM_WORLD);
        std::array<float, 16> view = getTransformOrDefault(SVGA3D_TRANSFORM_VIEW);
        std::array<float, 16> proj = getTransformOrDefault(SVGA3D_TRANSFORM_PROJECTION);
        std::array<float, 16> wv = multiplyMatrix4x4(world, view);
        ffMvp = multiplyMatrix4x4(wv, proj);
        if (!m_vsConstsUploadedForFf || ffMvp != m_lastFfMvp) {
            m_constantsDirty = true;
        }
    } else {
        if (m_vsConstsUploadedForFf) {
            m_constantsDirty = true;
        }
    }

    /* 1. Update Constant Buffers (UBOs) */
    if (m_constantsDirty) {
        if (m_constantRingMapped && m_constantRingBuffer) {
            const uint64_t submissionSerial = m_backend->completedSubmissionSerial();
            if (submissionSerial != m_constantRingSubmissionSerial) {
                m_constantRingCursor = 0;
                m_constantRingSubmissionSerial = submissionSerial;
            }
            const size_t slotSize = m_constantRingStride * 2;
            if (m_constantRingCursor + slotSize > m_constantRingSize) {
                /* The ring is full. Submit and wait before reusing any slice. */
                endRenderPassIfActive();
                m_backend->flushCommandBuffer();
                m_constantRingCursor = 0;
                m_constantRingSubmissionSerial = m_backend->completedSubmissionSerial();
            }

            const size_t vsOffset = m_constantRingCursor;
            const size_t psOffset = vsOffset + m_constantRingStride;
            uint8_t *mapped = static_cast<uint8_t*>(m_constantRingMapped);
            if (m_boundVS == SVGA3D_INVALID_ID) {
                float tempConsts[256][4];
                memcpy(tempConsts, m_vsConsts.floatConsts, sizeof(tempConsts));
                for (int col = 0; col < 4; ++col) {
                    for (int row = 0; row < 4; ++row) {
                        tempConsts[col][row] = ffMvp[row * 4 + col];
                    }
                }
                memcpy(mapped + vsOffset, tempConsts, sizeof(tempConsts));
                m_lastFfMvp = ffMvp;
                m_vsConstsUploadedForFf = true;
            } else {
                memcpy(mapped + vsOffset, m_vsConsts.floatConsts, sizeof(m_vsConsts.floatConsts));
                m_vsConstsUploadedForFf = false;
            }
            memcpy(mapped + psOffset, m_psConsts.floatConsts, sizeof(m_psConsts.floatConsts));
            if (getenv("SVGA3_VLKN_TRACE_FIFO")) {
                fprintf(stderr, "[const-upload] vs c0=[%g %g %g %g] c1=[%g %g %g %g]\n",
                        m_vsConsts.floatConsts[0][0], m_vsConsts.floatConsts[0][1],
                        m_vsConsts.floatConsts[0][2], m_vsConsts.floatConsts[0][3],
                        m_vsConsts.floatConsts[1][0], m_vsConsts.floatConsts[1][1],
                        m_vsConsts.floatConsts[1][2], m_vsConsts.floatConsts[1][3]);
            }
            m_vsConstDynamicOffset = static_cast<uint32_t>(vsOffset);
            m_psConstDynamicOffset = static_cast<uint32_t>(psOffset);
            m_constantRingCursor += slotSize;
        }
        m_constantsDirty = false;
    }

    /* Descriptor sets are immutable once a recorded draw references them.
     * Cache one set per texture/sampler tuple so state changes never force a
     * queue-idle just to rewrite a shared set. */
    using DescriptorKey = std::array<uint64_t, SVGA3_MAX_TEXTURE_STAGES * 2>;
    DescriptorKey descriptorKey{};
    VkDescriptorImageInfo imageInfos[SVGA3_MAX_TEXTURE_STAGES] = {};
    for (uint32_t i = 0; i < SVGA3_MAX_TEXTURE_STAGES; ++i) {
        imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bool stageBound = (m_stages[i].sid != SVGA3D_INVALID_ID && m_stages[i].sid != 0);
        VlknSurface *surf = stageBound ? m_surfaceMgr->getSurface(m_stages[i].sid) : nullptr;
        if (surf && surf->imageView()) {
            imageInfos[i].imageView = surf->imageView();
            imageInfos[i].sampler = getOrCreateSampler(i);
        } else {
            imageInfos[i].imageView = m_whiteView;
            imageInfos[i].sampler = m_dummySampler;
        }
        m_boundImageViews[i] = imageInfos[i].imageView;
        m_boundSamplers[i] = imageInfos[i].sampler;
        descriptorKey[i * 2] = descriptorHandleKey(imageInfos[i].imageView);
        descriptorKey[i * 2 + 1] = descriptorHandleKey(imageInfos[i].sampler);
    }

    auto descriptorIt = m_descriptorSetCache.find(descriptorKey);
    if (descriptorIt != m_descriptorSetCache.end()) {
        m_descriptorSet = descriptorIt->second;
    } else {
        VkDescriptorSet newSet = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo dsAlloc = {};
        dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsAlloc.descriptorPool = m_backend->descriptorPool();
        dsAlloc.descriptorSetCount = 1;
        dsAlloc.pSetLayouts = &m_descriptorSetLayout;
        VkResult allocResult = m_backend->dispatch().vkAllocateDescriptorSets(
            m_backend->device(), &dsAlloc, &newSet);
        if (allocResult != VK_SUCCESS) {
            log_msg("[libqemu_svga3d] vkAllocateDescriptorSets failed: %d\n", allocResult);
            return SVGA3_VLKN_ERROR_OUT_OF_MEMORY;
        }

        VkDescriptorBufferInfo vsBufInfo = {};
        vsBufInfo.buffer = m_constantRingBuffer;
        vsBufInfo.offset = 0;
        vsBufInfo.range = sizeof(m_vsConsts.floatConsts);
        VkDescriptorBufferInfo psBufInfo = {};
        psBufInfo.buffer = m_constantRingBuffer;
        psBufInfo.offset = 0;
        psBufInfo.range = sizeof(m_psConsts.floatConsts);

        VkWriteDescriptorSet writes[10] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = newSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[0].pBufferInfo = &vsBufInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = newSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[1].pBufferInfo = &psBufInfo;
        for (uint32_t i = 0; i < SVGA3_MAX_TEXTURE_STAGES; ++i) {
            writes[2 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2 + i].dstSet = newSet;
            writes[2 + i].dstBinding = 2 + i;
            writes[2 + i].descriptorCount = 1;
            writes[2 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[2 + i].pImageInfo = &imageInfos[i];
        }
        m_backend->dispatch().vkUpdateDescriptorSets(m_backend->device(), 10, writes, 0, nullptr);
        m_descriptorSetCache.emplace(descriptorKey, newSet);
        m_descriptorSet = newSet;
    }
    m_descriptorSetInitialized = true;
    m_descriptorSetDirty = false;

    /* Ensure all bound textures are transitioned to SHADER_READ_ONLY_OPTIMAL */
    for (uint32_t i = 0; i < SVGA3_MAX_TEXTURE_STAGES; ++i) {
        if (m_stages[i].sid != SVGA3D_INVALID_ID && m_stages[i].sid != 0) {
            VlknSurface *surf = m_surfaceMgr->getSurface(m_stages[i].sid);
            if (surf && surf->image() && surf->currentLayout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                endRenderPassIfActive();
                VkCommandBuffer cb = m_backend->getActiveCommandBuffer();
                VkImageMemoryBarrier barrier = {};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.oldLayout = surf->currentLayout();
                barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.srcAccessMask = (surf->currentLayout() == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) ?
                                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                barrier.image = surf->image();
                barrier.subresourceRange.aspectMask = surf->isDepthStencil() ?
                    (svga3_format_has_stencil(surf->svgaFormat()) ?
                        (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) :
                        VK_IMAGE_ASPECT_DEPTH_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.baseMipLevel = 0;
                barrier.subresourceRange.levelCount = surf->mipLevels();
                barrier.subresourceRange.baseArrayLayer = 0;
                barrier.subresourceRange.layerCount = surf->arrayLayers();

                m_backend->dispatch().vkCmdPipelineBarrier(
                    cb,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &barrier
                );
                surf->setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
        }
    }

    /* Ensure buffer capacity for vertex declarations BEFORE starting render pass.
     * Size for the actual draw: offset + stride * maxVertexCount, in 64-bit
     * with overflow checks. The old code only covered 4096 vertices, so any
     * larger draw let the GPU read out of bounds. */
    uint32_t maxVertexCount = 0;
    for (uint32_t i = 0; i < numRanges; ++i) {
        SVGA3dPrimitiveType ptype = (ranges[i].primType != SVGA3D_PRIMITIVE_INVALID)
            ? (SVGA3dPrimitiveType)ranges[i].primType : primitiveType;
        uint32_t c = calcVertexCount(ptype, ranges[i].primitiveCount);
        if (c > maxVertexCount) maxVertexCount = c;
    }
    if (numDecls > 0 && decls) {
        uint32_t cappedDecls = std::min(numDecls, SVGA3_MAX_VERTEX_DECLS);
        for (uint32_t i = 0; i < cappedDecls; ++i) {
            uint32_t sid = decls[i].array.surfaceId;
            VlknSurface *surf = m_surfaceMgr->getSurface(sid);
            if (surf) {
                if (surf->height() <= 1 && surf->depth() <= 1) {
                    surf->addFlags(SVGA3D_SURFACE_HINT_VERTEXBUFFER);
                }
                uint64_t stride = decls[i].array.stride ? decls[i].array.stride : 1;
                uint64_t attrMax = 0;
                bool ov = __builtin_mul_overflow(stride, (uint64_t)maxVertexCount, &attrMax) ||
                          __builtin_add_overflow(attrMax, (uint64_t)decls[i].array.offset, &attrMax);
                if (ov || attrMax > SVGA3_MAX_DMA_BYTES) {
                    log_msg("[libqemu_svga3d] draw error: vertex range overflow (decl=%u)\n", i);
                    return SVGA3_VLKN_ERROR_INVALID_PARAM;
                }
                if (attrMax > surf->bufferSize()) {
                    Svga3VlknStatus ensSt = surf->ensureBufferSize((size_t)attrMax);
                    if (ensSt != SVGA3_VLKN_SUCCESS) return ensSt;
                }
            }
        }
    }

    /* Pre-check and ensure capacity for all index buffers BEFORE starting render pass */
    for (uint32_t i = 0; i < numRanges; ++i) {
        const SVGA3dPrimitiveRange &r = ranges[i];
        if (r.indexArray.surfaceId != SVGA3D_INVALID_ID && r.indexArray.surfaceId != 0 && r.indexArray.stride > 0) {
            VlknSurface *idxSurf = m_surfaceMgr->getSurface(r.indexArray.surfaceId);
            if (idxSurf) {
                if (idxSurf->height() <= 1 && idxSurf->depth() <= 1) {
                    idxSurf->addFlags(SVGA3D_SURFACE_HINT_INDEXBUFFER);
                }
                SVGA3dPrimitiveType ptype = (r.primType != SVGA3D_PRIMITIVE_INVALID) ? (SVGA3dPrimitiveType)r.primType : primitiveType;
                uint32_t count = calcVertexCount(ptype, r.primitiveCount);
                /* Index width must be 2 or 4; anything else is rejected
                 * rather than misinterpreted. 64-bit: count*stride wrapped. */
                uint32_t idxStride = r.indexWidth ? r.indexWidth : r.indexArray.stride;
                if (idxStride != 2 && idxStride != 4) {
                    log_msg("[libqemu_svga3d] draw error: bad index stride %u\n", idxStride);
                    return SVGA3_VLKN_ERROR_INVALID_PARAM;
                }
                uint64_t neededIdxBytes = 0;
                bool ov = __builtin_mul_overflow((uint64_t)count, (uint64_t)idxStride, &neededIdxBytes) ||
                          __builtin_add_overflow(neededIdxBytes, (uint64_t)r.indexArray.offset, &neededIdxBytes);
                if (ov || neededIdxBytes > SVGA3_MAX_DMA_BYTES) {
                    log_msg("[libqemu_svga3d] draw error: index range overflow\n");
                    return SVGA3_VLKN_ERROR_INVALID_PARAM;
                }
                if (neededIdxBytes > idxSurf->bufferSize()) {
                    Svga3VlknStatus ensSt = idxSurf->ensureBufferSize((size_t)neededIdxBytes);
                    if (ensSt != SVGA3_VLKN_SUCCESS) return ensSt;
                }
            }
        }
    }

    ensureRenderPassActive();
    VkCommandBuffer cb = m_backend->getActiveCommandBuffer();

    /* Draw tracing inspects and maps vertex buffers. Keep it opt-in so normal
     * rendering avoids per-draw log I/O and host-memory map/unmap calls. */
    static const bool traceDraws = []() {
        const char *value = std::getenv("SVGA3D_TRACE_DRAWS");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    bool isHand = false;
    if (traceDraws) {
        for (uint32_t i = 0; i < numRanges; ++i) {
            if (ranges[i].primitiveCount == 12) {
                isHand = true;
                break;
            }
        }
    }
    static uint32_t handLogCount = 0;
    bool shouldLogHand = isHand && (handLogCount < 20 || (handLogCount % 300) == 0);
    if (isHand) handLogCount++;

    if (traceDraws && (shouldLogHand || m_drawCount <= 5 || (m_drawCount % 500) == 0 || m_boundVS == 7 || m_stages[0].sid == 73)) {
        bool useFfTex = (m_boundPS == SVGA3D_INVALID_ID && m_stages[0].sid != SVGA3D_INVALID_ID && m_stages[0].sid != 0 && m_surfaceMgr->getSurface(m_stages[0].sid) != nullptr);
        log_msg("[libqemu_svga3d] ctx::draw #%u (cid=%u, isHand=%d): boundVS=%u, boundPS=%u, useFfTex=%d, stage0.sid=%u, rtSid=%u, cull=%u, vp=(%.1f,%.1f %.1fx%.1f)\n",
                m_drawCount + 1, m_cid, (int)isHand, m_boundVS, m_boundPS, (int)useFfTex, m_stages[0].sid, m_renderTargets[0].sid, m_renderStates[SVGA3D_RS_CULLMODE],
                m_viewport.x, m_viewport.y, m_viewport.width, m_viewport.height);
        float activeMvp[4][4];
        if (m_boundVS == SVGA3D_INVALID_ID) {
            for (int col = 0; col < 4; ++col) {
                for (int row = 0; row < 4; ++row) {
                    activeMvp[col][row] = ffMvp[row * 4 + col];
                }
            }
        } else {
            for (int col = 0; col < 4; ++col) {
                for (int row = 0; row < 4; ++row) {
                    activeMvp[col][row] = m_vsConsts.floatConsts[col][row];
                }
            }
        }
        log_msg("[libqemu_svga3d]   mvp: [%g, %g, %g, %g] [%g, %g, %g, %g] [%g, %g, %g, %g] [%g, %g, %g, %g]\n",
                activeMvp[0][0], activeMvp[0][1], activeMvp[0][2], activeMvp[0][3],
                activeMvp[1][0], activeMvp[1][1], activeMvp[1][2], activeMvp[1][3],
                activeMvp[2][0], activeMvp[2][1], activeMvp[2][2], activeMvp[2][3],
                activeMvp[3][0], activeMvp[3][1], activeMvp[3][2], activeMvp[3][3]);
        for (uint32_t s = 0; s < 4; ++s) {
            uint32_t sid = m_stages[s].sid;
            VlknSurface *surf = (sid != SVGA3D_INVALID_ID && sid != 0) ? m_surfaceMgr->getSurface(sid) : nullptr;
            log_msg("   stage[%u]: sid=%u, surf=%p, dim=(%ux%u), fmt=%u, isWhite=%d\n",
                    s, sid, (void*)surf, surf ? surf->width() : 0, surf ? surf->height() : 0,
                    surf ? surf->svgaFormat() : 0, (int)(m_boundImageViews[s] == m_whiteView));
        }
        for (uint32_t d = 0; d < numDecls; ++d) {
            log_msg("   decl[%u]: sid=%u, off=%u, stride=%u, usage=%u, usageIdx=%u, type=%u\n",
                    d, decls[d].array.surfaceId, decls[d].array.offset, decls[d].array.stride,
                    decls[d].identity.usage, decls[d].identity.usageIndex, decls[d].identity.type);
        }
        for (uint32_t r_i = 0; r_i < numRanges; ++r_i) {
            const SVGA3dPrimitiveRange &r = ranges[r_i];
            log_msg("   range[%u]: primType=%u, primCount=%u, idxSid=%u, idxStride=%u, idxOff=%u, idxBias=%d\n",
                    r_i, r.primType, r.primitiveCount, r.indexArray.surfaceId, r.indexArray.stride, r.indexArray.offset, r.indexBias);
        }
        log_msg("   psConsts c0=[%g,%g,%g,%g] c1=[%g,%g,%g,%g]\n",
                m_psConsts.floatConsts[0][0], m_psConsts.floatConsts[0][1], m_psConsts.floatConsts[0][2], m_psConsts.floatConsts[0][3],
                m_psConsts.floatConsts[1][0], m_psConsts.floatConsts[1][1], m_psConsts.floatConsts[1][2], m_psConsts.floatConsts[1][3]);
        if (numDecls > 0 && decls) {
            for (uint32_t d = 0; d < numDecls; ++d) {
                VlknSurface *vsurf = m_surfaceMgr->getSurface(decls[d].array.surfaceId);
                if (vsurf && vsurf->bufferMemory() && decls[d].array.offset + decls[d].array.stride <= vsurf->bufferSize()) {
                    void *mapped = nullptr;
                    if (m_backend->dispatch().vkMapMemory(m_backend->device(), vsurf->bufferMemory(), 0, vsurf->bufferSize(), 0, &mapped) == VK_SUCCESS) {
                        const uint8_t *vptr = (const uint8_t*)mapped + decls[d].array.offset;
                        const float *f = (const float*)vptr;
                        const uint32_t *u = (const uint32_t*)vptr;
                        log_msg("   decl[%u] v0 raw: [%08x %08x %08x %08x] (float: %g %g %g %g)\n",
                                d, u[0], u[1], u[2], u[3], f[0], f[1], f[2], f[3]);
                        if (decls[d].array.stride >= 8 && decls[d].array.offset + decls[d].array.stride * 2 <= vsurf->bufferSize()) {
                            const float *f1 = (const float*)(vptr + decls[d].array.stride);
                            const uint32_t *u1 = (const uint32_t*)(vptr + decls[d].array.stride);
                            log_msg("   decl[%u] v1 raw: [%08x %08x %08x %08x] (float: %g %g %g %g)\n",
                                    d, u1[0], u1[1], u1[2], u1[3], f1[0], f1[1], f1[2], f1[3]);
                        }
                        m_backend->dispatch().vkUnmapMemory(m_backend->device(), vsurf->bufferMemory());
                    }
                }
            }
        }
    }

    /* 3. Bind Pipeline and Descriptor Sets */
    VkPipeline pipeline = getOrCreatePipeline(primitiveType, decls, numDecls, m_activeRenderPass);
    if (pipeline == VK_NULL_HANDLE) {
        endRenderPassIfActive();
        return SVGA3_VLKN_SUCCESS;
    }
    m_backend->dispatch().vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    if (m_descriptorSet) {
        uint32_t dynamicOffsets[2] = {m_vsConstDynamicOffset, m_psConstDynamicOffset};
        m_backend->dispatch().vkCmdBindDescriptorSets(
            cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_defaultPipelineLayout, 0, 1, &m_descriptorSet, 2, dynamicOffsets
        );
    }

    m_backend->dispatch().vkCmdSetViewport(cb, 0, 1, &m_viewport);
    VkRect2D activeScissor = m_scissor;
    if (!m_renderStates[SVGA3D_RS_SCISSORTESTENABLE]) {
        activeScissor.offset.x = (int32_t)m_viewport.x;
        activeScissor.offset.y = (int32_t)m_viewport.y;
        activeScissor.extent.width = (uint32_t)m_viewport.width;
        activeScissor.extent.height = (uint32_t)m_viewport.height;
    }
    m_backend->dispatch().vkCmdSetScissor(cb, 0, 1, &activeScissor);

    /* 4. Bind Vertex Buffers */
    VkBuffer fallbackBuf = (m_backend && m_backend->fallbackBuffer()) ? m_backend->fallbackBuffer() : m_dummyVb;

    if (numDecls > 0 && decls) {
        VkBuffer vbufs[SVGA3_MAX_VERTEX_DECLS];
        VkDeviceSize offsets[SVGA3_MAX_VERTEX_DECLS];
        uint32_t bindCount = std::min(numDecls, SVGA3_MAX_VERTEX_DECLS);
        for (uint32_t i = 0; i < bindCount; ++i) {
            uint32_t sid = decls[i].array.surfaceId;
            VlknSurface *surf = m_surfaceMgr->getSurface(sid);
            if (surf && surf->buffer() && decls[i].array.offset < surf->bufferSize()) {
                vbufs[i] = surf->buffer();
                offsets[i] = decls[i].array.offset;
            } else if (fallbackBuf) {
                vbufs[i] = fallbackBuf;
                offsets[i] = 0;
            } else {
                vbufs[i] = VK_NULL_HANDLE;
                offsets[i] = 0;
            }
        }
        m_backend->dispatch().vkCmdBindVertexBuffers(cb, 0, bindCount, vbufs, offsets);
    }

    /* Bind dummy vertex buffer if any required shader inputs were missing from decls */
    uint32_t requiredInputMask = (m_boundVS != SVGA3D_INVALID_ID) ?
        m_vertexShaders[m_boundVS].inputLocationMask : m_defaultVsInputMask;
    uint32_t providedInputMask = 0;
    for (uint32_t i = 0; i < std::min(numDecls, SVGA3_MAX_VERTEX_DECLS); ++i) {
        uint32_t loc = i;
        if (decls[i].identity.usage == SVGA3D_DECLUSAGE_POSITION) loc = 0;
        else if (decls[i].identity.usage == SVGA3D_DECLUSAGE_COLOR) loc = (decls[i].identity.usageIndex == 0) ? 1 : 7;
        else if (decls[i].identity.usage == SVGA3D_DECLUSAGE_TEXCOORD) loc = 2 + std::min(decls[i].identity.usageIndex, SVGA3_MAX_DECL_USAGE_INDEX);
        else if (decls[i].identity.usage == SVGA3D_DECLUSAGE_NORMAL) loc = 6;
        /* loc is guest-derived (usageIndex); shifting by >= 32 is UB. */
        if (loc < 32) {
            providedInputMask |= (1u << loc);
        }
    }
    if (shouldLogHand) {
        log_msg("   inputMasks: required=0x%x, provided=0x%x, missing=0x%x\n",
                requiredInputMask, providedInputMask, requiredInputMask & ~providedInputMask);
    }
    if ((requiredInputMask & ~providedInputMask) != 0 && fallbackBuf) {
        VkDeviceSize offset = 0;
        uint32_t dummyBindingIdx = std::min(numDecls, SVGA3_MAX_VERTEX_DECLS);
        m_backend->dispatch().vkCmdBindVertexBuffers(cb, dummyBindingIdx, 1, &fallbackBuf, &offset);
    }

    /* 5. Draw Primitive Ranges */
    SVGA3dPrimitiveType currentBoundType = primitiveType;
    for (uint32_t i = 0; i < numRanges; ++i) {
        const SVGA3dPrimitiveRange &r = ranges[i];
        SVGA3dPrimitiveType ptype = (r.primType != SVGA3D_PRIMITIVE_INVALID) ? (SVGA3dPrimitiveType)r.primType : primitiveType;
        if (i == 0 || ptype != currentBoundType) {
            VkPipeline pipe = getOrCreatePipeline(ptype, decls, numDecls, m_activeRenderPass);
            if (pipe == VK_NULL_HANDLE) continue;
            m_backend->dispatch().vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            currentBoundType = ptype;
        }
        uint32_t count = calcVertexCount(ptype, r.primitiveCount);
        if (r.indexArray.surfaceId != SVGA3D_INVALID_ID && r.indexArray.surfaceId != 0 && r.indexArray.stride > 0) {
            /* Indexed draw */
            VlknSurface *idxSurf = m_surfaceMgr->getSurface(r.indexArray.surfaceId);
            VkBuffer idxBuf = (idxSurf && idxSurf->buffer()) ? idxSurf->buffer() : fallbackBuf;
            if (idxBuf) {
                VkIndexType idxType = (r.indexArray.stride == 2) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
                VkDeviceSize idxOffset = (idxSurf && idxSurf->buffer() && r.indexArray.offset < idxSurf->bufferSize()) ? r.indexArray.offset : 0;
                m_backend->dispatch().vkCmdBindIndexBuffer(cb, idxBuf, idxOffset, idxType);
                m_backend->dispatch().vkCmdDrawIndexed(
                    cb, count, 1, 0, r.indexBias, 0
                );
            }
        } else {
            /* Non-indexed draw */
            uint32_t firstVertex = (r.indexBias > 0) ? (uint32_t)r.indexBias : 0;
            m_backend->dispatch().vkCmdDraw(
                cb, count, 1, firstVertex, 0
            );
        }
        m_vertexCount += count;
    }

    m_drawCount += numRanges;
    uint32_t rtSid = m_renderTargets[0].sid;
    if (rtSid != 0 && rtSid != SVGA3D_INVALID_ID) {
        VlknSurface *rtSurf = m_surfaceMgr->getSurface(rtSid);
        if (rtSurf && rtSurf->width() >= 128 && rtSurf->height() >= 128 && !rtSurf->isDepthStencil()) {
            rtSurf->invalidateReadback();
            markWindowDrawn(rtSid);
        }
    }
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknContext::beginQuery(SVGA3dQueryType type) {
    if (type >= SVGA3D_QUERYTYPE_MAX) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    if (m_queryActive) return SVGA3_VLKN_ERROR_INVALID_PARAM;

    if (m_queryPool != VK_NULL_HANDLE) {
        VkCommandBuffer cb = m_backend->getActiveCommandBuffer();
        m_backend->dispatch().vkCmdResetQueryPool(cb, m_queryPool, 0, 1);
        m_backend->dispatch().vkCmdBeginQuery(cb, m_queryPool, 0, 0);
    }
    m_queryActive = true;
    m_queryEnded = false;
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknContext::endQuery(SVGA3dQueryType type) {
    if (type >= SVGA3D_QUERYTYPE_MAX) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    if (!m_queryActive) return SVGA3_VLKN_ERROR_INVALID_PARAM;

    if (m_queryPool != VK_NULL_HANDLE) {
        VkCommandBuffer cb = m_backend->getActiveCommandBuffer();
        m_backend->dispatch().vkCmdEndQuery(cb, m_queryPool, 0);
        m_backend->flushCommandBuffer();
    }
    m_queryActive = false;
    m_queryEnded = true;
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknContext::waitForQuery(SVGA3dQueryType type, uint32_t *outResult) {
    if (type >= SVGA3D_QUERYTYPE_MAX) return SVGA3_VLKN_ERROR_INVALID_PARAM;
    if (!m_queryEnded && !m_queryActive) return SVGA3_VLKN_ERROR_INVALID_PARAM;

    m_backend->waitIdle();
    uint64_t pixels = 0;
    if (m_queryPool != VK_NULL_HANDLE) {
        VkResult res = m_backend->dispatch().vkGetQueryPoolResults(
            m_backend->device(),
            m_queryPool,
            0, 1,
            sizeof(uint64_t),
            &pixels,
            sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT
        );
        if (res == VK_SUCCESS) {
            m_lastQueryResult = (uint32_t)pixels;
        } else {
            m_lastQueryResult = (uint32_t)(m_vertexCount > 0 ? (m_drawCount * 1920) : 0);
        }
    } else {
        m_lastQueryResult = (uint32_t)(m_vertexCount > 0 ? (m_drawCount * 1920) : 0);
    }

    if (outResult) {
        *outResult = m_lastQueryResult;
    }
    m_queryEnded = false;
    return SVGA3_VLKN_SUCCESS;
}

bool VlknContext::isQueryActive(SVGA3dQueryType type) const {
    (void)type;
    return m_queryActive;
}

/* Context Manager */

VlknContextManager::VlknContextManager(VlknBackend *backend, VlknSurfaceManager *surfaceMgr)
    : m_backend(backend)
    , m_surfaceMgr(surfaceMgr)
{}

VlknContextManager::~VlknContextManager() {
    clear();
}

Svga3VlknStatus VlknContextManager::createContext(uint32_t cid) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_contexts.find(cid) != m_contexts.end()) {
        return SVGA3_VLKN_ERROR_ALREADY_EXISTS;
    }
    /* Cap contexts: each holds pipelines, shaders and buffers, so an
     * unbounded count exhausts host memory. */
    if (m_contexts.size() >= SVGA3_MAX_CONTEXTS) {
        log_msg("[libqemu_svga3d] createContext error: context limit %u reached\n",
                SVGA3_MAX_CONTEXTS);
        return SVGA3_VLKN_ERROR_OUT_OF_MEMORY;
    }

    m_contexts[cid] = std::make_unique<VlknContext>(m_backend, m_surfaceMgr, cid);
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknContextManager::destroyContext(uint32_t cid) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_contexts.find(cid);
    if (it == m_contexts.end()) {
        return SVGA3_VLKN_ERROR_NOT_FOUND;
    }

    m_contexts.erase(it);
    return SVGA3_VLKN_SUCCESS;
}

VlknContext* VlknContextManager::getContext(uint32_t cid) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_contexts.find(cid);
    if (it != m_contexts.end()) {
        return it->second.get();
    }
    return nullptr;
}

bool VlknContextManager::exists(uint32_t cid) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_contexts.find(cid) != m_contexts.end();
}

void VlknContextManager::clear() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_contexts.clear();
}

size_t VlknContextManager::count() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_contexts.size();
}

void VlknContextManager::endAllRenderPasses() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (auto &pair : m_contexts) {
        if (pair.second) {
            pair.second->endRenderPassIfActive();
        }
    }
}

void VlknContextManager::endAllRenderPassesExcept(uint32_t cid) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (auto &pair : m_contexts) {
        if (pair.first != cid && pair.second) {
            pair.second->endRenderPassIfActive();
        }
    }
}

std::vector<std::pair<uint32_t, uint32_t>> VlknContextManager::collectPendingWindowPresents() {
    std::vector<std::pair<uint32_t, uint32_t>> list;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (auto &pair : m_contexts) {
        if (pair.first != 246 && pair.second && pair.second->hasDrawnToWindow()) {
            uint32_t sid = pair.second->lastDrawnWindowSid();
            if (sid != 0 && sid != SVGA3D_INVALID_ID) {
                list.push_back({pair.first, sid});
            }
            pair.second->resetDrawnToWindow();
        }
    }
    return list;
}

void VlknContextManager::invalidateSurface(uint32_t sid) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (auto &pair : m_contexts) {
        if (pair.second) {
            pair.second->invalidateSurface(sid);
        }
    }
}

} // namespace svga3_vlkn
