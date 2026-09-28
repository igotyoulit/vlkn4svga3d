/*
 * SVGA3=VLKN - Vulkan Dispatch & Dynamic Loader Implementation
 */

#include "vlkn_dispatch.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <vector>

/*
 * Mock Vulkan Implementation Structures
 */
typedef struct MockDevice_T {
    uint32_t magic;
} MockDevice_T;

typedef struct MockBuffer_T {
    VkDeviceSize size;
    VkBufferUsageFlags usage;
    void *memory;
    VkDeviceSize memoryOffset;
} MockBuffer_T;

typedef struct MockImage_T {
    VkImageType imageType;
    VkFormat format;
    VkExtent3D extent;
    uint32_t mipLevels;
    uint32_t arrayLayers;
    VkImageUsageFlags usage;
    void *memory;
    VkDeviceSize memoryOffset;
    VkDeviceSize memorySize;
} MockImage_T;

typedef struct MockMemory_T {
    VkDeviceSize size;
    uint32_t memoryTypeIndex;
    void *data;
} MockMemory_T;

typedef struct MockCommandBuffer_T {
    VkCommandPool pool;
    bool recording;
    uint32_t drawCount;
    uint32_t clearCount;
    uint32_t pipelineCount;
} MockCommandBuffer_T;

/*
 * Mock function implementations
 */
static VkResult VKAPI_CALL mock_vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance) {
    (void)pCreateInfo; (void)pAllocator;
    *pInstance = (VkInstance)(uintptr_t)0x1001;
    return VK_SUCCESS;
}

static void VKAPI_CALL mock_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    (void)instance; (void)pAllocator;
}

static VkResult VKAPI_CALL mock_vkEnumeratePhysicalDevices(VkInstance instance, uint32_t* pCount, VkPhysicalDevice* pDevices) {
    (void)instance;
    if (!pDevices) {
        *pCount = 1;
        return VK_SUCCESS;
    }
    if (*pCount > 0) {
        pDevices[0] = (VkPhysicalDevice)(uintptr_t)0x2001;
        *pCount = 1;
        return VK_SUCCESS;
    }
    return VK_INCOMPLETE;
}

static void VKAPI_CALL mock_vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties* pProperties) {
    (void)physicalDevice;
    memset(pProperties, 0, sizeof(*pProperties));
    pProperties->apiVersion = VK_API_VERSION_1_1;
    pProperties->driverVersion = 1;
    pProperties->vendorID = 0x15AD; /* VMware / Virtual */
    pProperties->deviceID = 0x0405; /* SVGA3D */
    pProperties->deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    strncpy(pProperties->deviceName, "SVGA3=VLKN Emulated Vulkan Adapter", sizeof(pProperties->deviceName) - 1);

    /* Set realistic limits matching high-end hardware */
    pProperties->limits.maxImageDimension1D = 16384;
    pProperties->limits.maxImageDimension2D = 16384;
    pProperties->limits.maxImageDimension3D = 2048;
    pProperties->limits.maxImageDimensionCube = 16384;
    pProperties->limits.maxImageArrayLayers = 2048;
    pProperties->limits.maxTexelBufferElements = 134217728;
    pProperties->limits.maxUniformBufferRange = 65536;
    pProperties->limits.maxStorageBufferRange = 1073741824;
    pProperties->limits.maxPushConstantsSize = 128;
    pProperties->limits.maxMemoryAllocationCount = 4096;
    pProperties->limits.maxSamplerAllocationCount = 4000;
    pProperties->limits.maxBoundDescriptorSets = 8;
    pProperties->limits.maxPerStageDescriptorSamplers = 16;
    pProperties->limits.maxPerStageDescriptorUniformBuffers = 16;
    pProperties->limits.maxPerStageDescriptorStorageBuffers = 8;
    pProperties->limits.maxPerStageDescriptorSampledImages = 128;
    pProperties->limits.maxPerStageDescriptorStorageImages = 8;
    pProperties->limits.maxPerStageResources = 128;
    pProperties->limits.maxVertexInputAttributes = 32;
    pProperties->limits.maxVertexInputBindings = 32;
    pProperties->limits.maxVertexInputAttributeOffset = 2047;
    pProperties->limits.maxVertexInputBindingStride = 2048;
    pProperties->limits.maxVertexOutputComponents = 64;
    pProperties->limits.maxFragmentInputComponents = 64;
    pProperties->limits.maxFragmentOutputAttachments = 8;
    pProperties->limits.maxFragmentDualSrcAttachments = 1;
    pProperties->limits.maxColorAttachments = 8;
    pProperties->limits.maxViewports = 16;
    pProperties->limits.maxViewportDimensions[0] = 16384;
    pProperties->limits.maxViewportDimensions[1] = 16384;
    pProperties->limits.viewportBoundsRange[0] = -32768.0f;
    pProperties->limits.viewportBoundsRange[1] = 32767.0f;
    pProperties->limits.pointSizeRange[0] = 1.0f;
    pProperties->limits.pointSizeRange[1] = 189.0f;
    pProperties->limits.lineWidthRange[0] = 1.0f;
    pProperties->limits.lineWidthRange[1] = 8.0f;
    pProperties->limits.pointSizeGranularity = 0.125f;
    pProperties->limits.lineWidthGranularity = 0.125f;
    pProperties->limits.strictLines = VK_TRUE;
    pProperties->limits.standardSampleLocations = VK_TRUE;
    pProperties->limits.minMemoryMapAlignment = 64;
}

static void VKAPI_CALL mock_vkGetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures* pFeatures) {
    (void)physicalDevice;
    memset(pFeatures, 0, sizeof(*pFeatures));
    pFeatures->robustBufferAccess = VK_TRUE;
    pFeatures->fullDrawIndexUint32 = VK_TRUE;
    pFeatures->imageCubeArray = VK_TRUE;
    pFeatures->independentBlend = VK_TRUE;
    pFeatures->geometryShader = VK_TRUE;
    pFeatures->tessellationShader = VK_TRUE;
    pFeatures->sampleRateShading = VK_TRUE;
    pFeatures->dualSrcBlend = VK_TRUE;
    pFeatures->logicOp = VK_TRUE;
    pFeatures->depthClamp = VK_TRUE;
    pFeatures->depthBiasClamp = VK_TRUE;
    pFeatures->fillModeNonSolid = VK_TRUE;
    pFeatures->depthBounds = VK_TRUE;
    pFeatures->wideLines = VK_TRUE;
    pFeatures->largePoints = VK_TRUE;
    pFeatures->alphaToOne = VK_TRUE;
    pFeatures->multiViewport = VK_TRUE;
    pFeatures->samplerAnisotropy = VK_TRUE;
    pFeatures->textureCompressionBC = VK_TRUE;
    pFeatures->occlusionQueryPrecise = VK_TRUE;
}

static void VKAPI_CALL mock_vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties* pMemoryProperties) {
    (void)physicalDevice;
    memset(pMemoryProperties, 0, sizeof(*pMemoryProperties));
    pMemoryProperties->memoryTypeCount = 3;

    /* Type 0: Device Local (VRAM) */
    pMemoryProperties->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    pMemoryProperties->memoryTypes[0].heapIndex = 0;

    /* Type 1: Host Visible + Host Coherent (Staging / Dynamic) */
    pMemoryProperties->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    pMemoryProperties->memoryTypes[1].heapIndex = 1;

    /* Type 2: Host Visible + Host Cached */
    pMemoryProperties->memoryTypes[2].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    pMemoryProperties->memoryTypes[2].heapIndex = 1;

    pMemoryProperties->memoryHeapCount = 2;
    pMemoryProperties->memoryHeaps[0].size = 4ULL * 1024 * 1024 * 1024; /* 4GB VRAM */
    pMemoryProperties->memoryHeaps[0].flags = 1; /* VK_MEMORY_HEAP_DEVICE_LOCAL_BIT */
    pMemoryProperties->memoryHeaps[1].size = 8ULL * 1024 * 1024 * 1024; /* 8GB Host RAM */
    pMemoryProperties->memoryHeaps[1].flags = 0;
}

static void VKAPI_CALL mock_vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice, uint32_t* pCount, VkQueueFamilyProperties* pProperties) {
    (void)physicalDevice;
    if (!pProperties) {
        *pCount = 1;
        return;
    }
    if (*pCount > 0) {
        pProperties[0].queueFlags = 0x00000001 | 0x00000002 | 0x00000004; /* Graphics | Compute | Transfer */
        pProperties[0].queueCount = 1;
        pProperties[0].timestampValidBits = 64;
        pProperties[0].minImageTransferGranularity.width = 1;
        pProperties[0].minImageTransferGranularity.height = 1;
        pProperties[0].minImageTransferGranularity.depth = 1;
        *pCount = 1;
    }
}

static VkResult VKAPI_CALL mock_vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    (void)physicalDevice; (void)pCreateInfo; (void)pAllocator;
    MockDevice_T *dev = (MockDevice_T*)malloc(sizeof(MockDevice_T));
    dev->magic = 0x564C4B4E; /* 'VLKN' */
    *pDevice = (VkDevice)dev;
    return VK_SUCCESS;
}

static void VKAPI_CALL mock_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    (void)pAllocator;
    if (device) free((void*)device);
}

static void VKAPI_CALL mock_vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue) {
    (void)device; (void)queueFamilyIndex; (void)queueIndex;
    *pQueue = (VkQueue)(uintptr_t)0x3001;
}

static std::vector<MockCommandBuffer_T*> s_mockCmdBuffers;

static VkResult VKAPI_CALL mock_vkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkCommandPool* pCommandPool) {
    (void)device; (void)pCreateInfo; (void)pAllocator;
    static uint64_t s_poolId = 0x4000;
    *pCommandPool = reinterpret_cast<VkCommandPool>((uintptr_t)++s_poolId);
    return VK_SUCCESS;
}

static void VKAPI_CALL mock_vkDestroyCommandPool(VkDevice device, VkCommandPool commandPool, const VkAllocationCallbacks* pAllocator) {
    (void)device; (void)pAllocator;
    for (auto it = s_mockCmdBuffers.begin(); it != s_mockCmdBuffers.end(); ) {
        if ((*it)->pool == commandPool) {
            free(*it);
            it = s_mockCmdBuffers.erase(it);
        } else {
            ++it;
        }
    }
}

static VkResult VKAPI_CALL mock_vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* pAllocateInfo, VkCommandBuffer* pCommandBuffers) {
    (void)device;
    for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i) {
        MockCommandBuffer_T *cb = (MockCommandBuffer_T*)calloc(1, sizeof(MockCommandBuffer_T));
        cb->pool = pAllocateInfo->commandPool;
        s_mockCmdBuffers.push_back(cb);
        pCommandBuffers[i] = (VkCommandBuffer)cb;
    }
    return VK_SUCCESS;
}

static void VKAPI_CALL mock_vkFreeCommandBuffers(VkDevice device, VkCommandPool commandPool, uint32_t count, const VkCommandBuffer* pCommandBuffers) {
    (void)device; (void)commandPool;
    for (uint32_t i = 0; i < count; ++i) {
        MockCommandBuffer_T *cb = (MockCommandBuffer_T*)pCommandBuffers[i];
        if (cb) {
            for (auto it = s_mockCmdBuffers.begin(); it != s_mockCmdBuffers.end(); ++it) {
                if (*it == cb) {
                    s_mockCmdBuffers.erase(it);
                    break;
                }
            }
            free(cb);
        }
    }
}

static VkResult VKAPI_CALL mock_vkBeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo* pBeginInfo) {
    (void)pBeginInfo;
    MockCommandBuffer_T *cb = (MockCommandBuffer_T*)commandBuffer;
    if (cb) {
        cb->recording = true;
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL mock_vkEndCommandBuffer(VkCommandBuffer commandBuffer) {
    MockCommandBuffer_T *cb = (MockCommandBuffer_T*)commandBuffer;
    if (cb) {
        cb->recording = false;
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL mock_vkResetCommandBuffer(VkCommandBuffer commandBuffer, VkFlags flags) {
    (void)flags;
    MockCommandBuffer_T *cb = (MockCommandBuffer_T*)commandBuffer;
    if (cb) {
        cb->drawCount = 0;
        cb->clearCount = 0;
        cb->pipelineCount = 0;
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL mock_vkCreateImage(VkDevice device, const VkImageCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkImage* pImage) {
    (void)device; (void)pAllocator;
    static uint64_t s_imgId = 0x5000;
    MockImage_T *img = (MockImage_T*)calloc(1, sizeof(MockImage_T));
    img->imageType = pCreateInfo->imageType;
    img->format = pCreateInfo->format;
    img->extent = pCreateInfo->extent;
    img->mipLevels = pCreateInfo->mipLevels;
    img->arrayLayers = pCreateInfo->arrayLayers;
    img->usage = pCreateInfo->usage;
    (void)s_imgId;
    *pImage = (VkImage)(uintptr_t)img;
    return VK_SUCCESS;
}

static void VKAPI_CALL mock_vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* pAllocator) {
    (void)device; (void)pAllocator;
    if (image) free((void*)(uintptr_t)image);
}

static size_t mock_format_bpp(VkFormat fmt) {
    switch (fmt) {
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R8_SNORM:
        case VK_FORMAT_R8_USCALED:
        case VK_FORMAT_R8_SSCALED:
        case VK_FORMAT_R8_UINT:
        case VK_FORMAT_R8_SINT:
        case VK_FORMAT_S8_UINT:
            return 1;

        case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
        case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
        case VK_FORMAT_R5G6B5_UNORM_PACK16:
        case VK_FORMAT_B5G6R5_UNORM_PACK16:
        case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R8G8_SNORM:
        case VK_FORMAT_R8G8_UINT:
        case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R16_UNORM:
        case VK_FORMAT_R16_SNORM:
        case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R16_SINT:
        case VK_FORMAT_R16_SFLOAT:
        case VK_FORMAT_D16_UNORM:
            return 2;

        case VK_FORMAT_R8G8B8_UNORM:
        case VK_FORMAT_B8G8R8_UNORM:
            return 3;

        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R16G16_UNORM:
        case VK_FORMAT_R16G16_SNORM:
        case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_R32_UINT:
        case VK_FORMAT_R32_SINT:
        case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT:
            return 4;

        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R32G32_UINT:
        case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R32G32_SFLOAT:
            return 8;

        case VK_FORMAT_R32G32B32_SFLOAT:
            return 12;

        case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return 16;

        default:
            return 4;
    }
}

static void VKAPI_CALL mock_vkGetImageMemoryRequirements(VkDevice device, VkImage image, VkMemoryRequirements* pReqs) {
    (void)device;
    MockImage_T *img = (MockImage_T*)(uintptr_t)image;
    pReqs->alignment = 256;
    pReqs->memoryTypeBits = 0x7; /* Types 0, 1, 2 */
    if (img) {
        VkDeviceSize bpp = mock_format_bpp(img->format);
        uint32_t layers = img->arrayLayers ? img->arrayLayers : 1;
        pReqs->size = (VkDeviceSize)img->extent.width * img->extent.height * (img->extent.depth ? img->extent.depth : 1) * bpp * layers * 2;
        if (pReqs->size < 256) pReqs->size = 256;
    } else {
        pReqs->size = 65536;
    }
}

static VkResult VKAPI_CALL mock_vkCreateImageView(VkDevice device, const VkImageViewCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkImageView* pView) {
    (void)device; (void)pCreateInfo; (void)pAllocator;
    static uint64_t s_viewId = 0x6000;
    *pView = reinterpret_cast<VkImageView>((uintptr_t)++s_viewId);
    return VK_SUCCESS;
}

static void VKAPI_CALL mock_vkDestroyImageView(VkDevice device, VkImageView imageView, const VkAllocationCallbacks* pAllocator) {
    (void)device; (void)imageView; (void)pAllocator;
}

static VkResult VKAPI_CALL mock_vkCreateBuffer(VkDevice device, const VkBufferCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkBuffer* pBuffer) {
    (void)device; (void)pAllocator;
    MockBuffer_T *buf = (MockBuffer_T*)calloc(1, sizeof(MockBuffer_T));
    buf->size = pCreateInfo->size;
    buf->usage = pCreateInfo->usage;
    *pBuffer = (VkBuffer)(uintptr_t)buf;
    return VK_SUCCESS;
}

static void VKAPI_CALL mock_vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* pAllocator) {
    (void)device; (void)pAllocator;
    if (buffer) free((void*)(uintptr_t)buffer);
}

static void VKAPI_CALL mock_vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer, VkMemoryRequirements* pReqs) {
    (void)device;
    MockBuffer_T *buf = (MockBuffer_T*)(uintptr_t)buffer;
    pReqs->alignment = 64;
    pReqs->memoryTypeBits = 0x7;
    pReqs->size = buf ? buf->size : 65536;
}

static VkResult VKAPI_CALL mock_vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* pAllocateInfo, const VkAllocationCallbacks* pAllocator, VkDeviceMemory* pMemory) {
    (void)device; (void)pAllocator;
    MockMemory_T *mem = (MockMemory_T*)calloc(1, sizeof(MockMemory_T));
    mem->size = pAllocateInfo->allocationSize;
    mem->memoryTypeIndex = pAllocateInfo->memoryTypeIndex;
    mem->data = calloc(1, mem->size);
    *pMemory = (VkDeviceMemory)(uintptr_t)mem;
    return VK_SUCCESS;
}

static void VKAPI_CALL mock_vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* pAllocator) {
    (void)device; (void)pAllocator;
    MockMemory_T *mem = (MockMemory_T*)(uintptr_t)memory;
    if (mem) {
        if (mem->data) free(mem->data);
        free(mem);
    }
}

static VkResult VKAPI_CALL mock_vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
    (void)device;
    MockBuffer_T *buf = (MockBuffer_T*)(uintptr_t)buffer;
    MockMemory_T *mem = (MockMemory_T*)(uintptr_t)memory;
    if (buf && mem) {
        buf->memory = mem->data;
        buf->memoryOffset = memoryOffset;
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL mock_vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
    (void)device;
    MockImage_T *img = (MockImage_T*)(uintptr_t)image;
    MockMemory_T *mem = (MockMemory_T*)(uintptr_t)memory;
    if (img && mem) {
        img->memory = mem->data;
        img->memoryOffset = memoryOffset;
        img->memorySize = mem->size;
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL mock_vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkFlags flags, void** ppData) {
    (void)device; (void)flags; (void)size;
    MockMemory_T *mem = (MockMemory_T*)(uintptr_t)memory;
    if (mem && mem->data) {
        *ppData = (uint8_t*)mem->data + offset;
        return VK_SUCCESS;
    }
    return VK_ERROR_MEMORY_MAP_FAILED;
}

static void VKAPI_CALL mock_vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
    (void)device; (void)memory;
}

static VkResult VKAPI_CALL mock_vkCreateSampler(VkDevice device, const VkSamplerCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSampler* pSampler) {
    (void)device; (void)pCreateInfo; (void)pAllocator;
    static uint64_t s_samplerId = 0x7000;
    *pSampler = reinterpret_cast<VkSampler>((uintptr_t)++s_samplerId);
    return VK_SUCCESS;
}

static void VKAPI_CALL mock_vkDestroySampler(VkDevice device, VkSampler sampler, const VkAllocationCallbacks* pAllocator) {
    (void)device; (void)sampler; (void)pAllocator;
}

static VkResult VKAPI_CALL mock_vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkShaderModule* pShaderModule) {
    (void)device; (void)pCreateInfo; (void)pAllocator;
    static uint64_t s_smId = 0x8000;
    *pShaderModule = reinterpret_cast<VkShaderModule>((uintptr_t)++s_smId);
    return VK_SUCCESS;
}

static void VKAPI_CALL mock_vkDestroyShaderModule(VkDevice device, VkShaderModule shaderModule, const VkAllocationCallbacks* pAllocator) {
    (void)device; (void)shaderModule; (void)pAllocator;
}

static VkResult VKAPI_CALL mock_vkCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkPipelineLayout* pLayout) {
    (void)device; (void)pCreateInfo; (void)pAllocator;
    static uint64_t s_layoutId = 0x9000;
    *pLayout = reinterpret_cast<VkPipelineLayout>((uintptr_t)++s_layoutId);
    return VK_SUCCESS;
}

static void VKAPI_CALL mock_vkDestroyPipelineLayout(VkDevice device, VkPipelineLayout layout, const VkAllocationCallbacks* pAllocator) {
    (void)device; (void)layout; (void)pAllocator;
}

static VkResult VKAPI_CALL mock_vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass) {
    (void)device; (void)pCreateInfo; (void)pAllocator;
    static uint64_t s_rpId = 0xA000;
    *pRenderPass = reinterpret_cast<VkRenderPass>((uintptr_t)++s_rpId);
    return VK_SUCCESS;
}

static void VKAPI_CALL mock_vkDestroyRenderPass(VkDevice device, VkRenderPass renderPass, const VkAllocationCallbacks* pAllocator) {
    (void)device; (void)renderPass; (void)pAllocator;
}

static VkResult VKAPI_CALL mock_vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkFramebuffer* pFramebuffer) {
    (void)device; (void)pCreateInfo; (void)pAllocator;
    static uint64_t s_fbId = 0xB000;
    *pFramebuffer = reinterpret_cast<VkFramebuffer>((uintptr_t)++s_fbId);
    return VK_SUCCESS;
}

static void VKAPI_CALL mock_vkDestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks* pAllocator) {
    (void)device; (void)framebuffer; (void)pAllocator;
}

static VkResult VKAPI_CALL mock_vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count, const VkGraphicsPipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines) {
    (void)device; (void)cache; (void)pCreateInfos; (void)pAllocator;
    static uint64_t s_pipeId = 0xC000;
    for (uint32_t i = 0; i < count; ++i) {
        pPipelines[i] = reinterpret_cast<VkPipeline>((uintptr_t)++s_pipeId);
    }
    return VK_SUCCESS;
}

static void VKAPI_CALL mock_vkDestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* pAllocator) {
    (void)device; (void)pipeline; (void)pAllocator;
}

static void VKAPI_CALL mock_vkCmdBeginRenderPass(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* pBegin, VkSubpassContents contents) {
    (void)commandBuffer; (void)pBegin; (void)contents;
}

static void VKAPI_CALL mock_vkCmdEndRenderPass(VkCommandBuffer commandBuffer) {
    (void)commandBuffer;
}

static void VKAPI_CALL mock_vkCmdBindPipeline(VkCommandBuffer commandBuffer, VkPipelineBindPoint bindPoint, VkPipeline pipeline) {
    (void)commandBuffer; (void)bindPoint; (void)pipeline;
    MockCommandBuffer_T *cb = (MockCommandBuffer_T*)commandBuffer;
    if (cb) cb->pipelineCount++;
}

static void VKAPI_CALL mock_vkCmdSetViewport(VkCommandBuffer commandBuffer, uint32_t first, uint32_t count, const VkViewport* pViewports) {
    (void)commandBuffer; (void)first; (void)count; (void)pViewports;
}

static void VKAPI_CALL mock_vkCmdSetScissor(VkCommandBuffer commandBuffer, uint32_t first, uint32_t count, const VkRect2D* pScissors) {
    (void)commandBuffer; (void)first; (void)count; (void)pScissors;
}

static void VKAPI_CALL mock_vkCmdBindVertexBuffers(VkCommandBuffer commandBuffer, uint32_t first, uint32_t count, const VkBuffer* pBuffers, const VkDeviceSize* pOffsets) {
    (void)commandBuffer; (void)first; (void)count; (void)pBuffers; (void)pOffsets;
}

static void VKAPI_CALL mock_vkCmdBindIndexBuffer(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkIndexType indexType) {
    (void)commandBuffer; (void)buffer; (void)offset; (void)indexType;
}

static void VKAPI_CALL mock_vkCmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) {
    (void)vertexCount; (void)instanceCount; (void)firstVertex; (void)firstInstance;
    MockCommandBuffer_T *cb = (MockCommandBuffer_T*)commandBuffer;
    if (cb) cb->drawCount++;
}

static void VKAPI_CALL mock_vkCmdDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    (void)indexCount; (void)instanceCount; (void)firstIndex; (void)vertexOffset; (void)firstInstance;
    MockCommandBuffer_T *cb = (MockCommandBuffer_T*)commandBuffer;
    if (cb) cb->drawCount++;
}

static void VKAPI_CALL mock_vkCmdClearAttachments(VkCommandBuffer commandBuffer, uint32_t count, const VkClearAttachment* pAttachments, uint32_t rectCount, const VkClearRect* pRects) {
    (void)count; (void)pAttachments; (void)rectCount; (void)pRects;
    MockCommandBuffer_T *cb = (MockCommandBuffer_T*)commandBuffer;
    if (cb) cb->clearCount++;
}

static void VKAPI_CALL mock_vkCmdPipelineBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage, VkDependencyFlags depFlags, uint32_t memCount, const VkMemoryBarrier* pMem, uint32_t bufCount, const VkBufferMemoryBarrier* pBuf, uint32_t imgCount, const VkImageMemoryBarrier* pImg) {
    (void)commandBuffer; (void)srcStage; (void)dstStage; (void)depFlags; (void)memCount; (void)pMem; (void)bufCount; (void)pBuf; (void)imgCount; (void)pImg;
}

static void VKAPI_CALL mock_vkCmdCopyBuffer(VkCommandBuffer commandBuffer, VkBuffer src, VkBuffer dst, uint32_t count, const VkBufferCopy* pRegions) {
    (void)commandBuffer;
    MockBuffer_T *s = (MockBuffer_T*)(uintptr_t)src;
    MockBuffer_T *d = (MockBuffer_T*)(uintptr_t)dst;
    if (s && d && s->memory && d->memory) {
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t *srcPtr = (uint8_t*)s->memory + s->memoryOffset + pRegions[i].srcOffset;
            uint8_t *dstPtr = (uint8_t*)d->memory + d->memoryOffset + pRegions[i].dstOffset;
            VkDeviceSize copySize = pRegions[i].size;
            if (s->memoryOffset + pRegions[i].srcOffset + copySize > s->size) {
                copySize = (s->size > s->memoryOffset + pRegions[i].srcOffset) ? (s->size - (s->memoryOffset + pRegions[i].srcOffset)) : 0;
            }
            if (d->memoryOffset + pRegions[i].dstOffset + copySize > d->size) {
                copySize = (d->size > d->memoryOffset + pRegions[i].dstOffset) ? (d->size - (d->memoryOffset + pRegions[i].dstOffset)) : 0;
            }
            if (copySize > 0) {
                memcpy(dstPtr, srcPtr, (size_t)copySize);
            }
        }
    }
}

static void VKAPI_CALL mock_vkCmdCopyBufferToImage(VkCommandBuffer commandBuffer, VkBuffer src, VkImage dst, VkImageLayout layout, uint32_t count, const VkBufferImageCopy* pRegions) {
    (void)commandBuffer; (void)layout;
    MockBuffer_T *s = (MockBuffer_T*)(uintptr_t)src;
    MockImage_T *d = (MockImage_T*)(uintptr_t)dst;
    if (s && d && s->memory && d->memory) {
        size_t bpp = mock_format_bpp(d->format);
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t *srcPtr = (uint8_t*)s->memory + s->memoryOffset + pRegions[i].bufferOffset;
            uint8_t *dstPtr = (uint8_t*)d->memory + d->memoryOffset;
            VkDeviceSize copySize = (VkDeviceSize)pRegions[i].imageExtent.width * pRegions[i].imageExtent.height * (pRegions[i].imageExtent.depth ? pRegions[i].imageExtent.depth : 1) * bpp;
            if (s->memoryOffset + pRegions[i].bufferOffset + copySize > s->size) {
                copySize = (s->size > s->memoryOffset + pRegions[i].bufferOffset) ? (s->size - (s->memoryOffset + pRegions[i].bufferOffset)) : 0;
            }
            if (d->memorySize > 0 && d->memoryOffset + copySize > d->memorySize) {
                copySize = (d->memorySize > d->memoryOffset) ? (d->memorySize - d->memoryOffset) : 0;
            }
            if (copySize > 0) {
                memcpy(dstPtr, srcPtr, (size_t)copySize);
            }
        }
    }
}

static void VKAPI_CALL mock_vkCmdCopyImageToBuffer(VkCommandBuffer commandBuffer, VkImage src, VkImageLayout layout, VkBuffer dst, uint32_t count, const VkBufferImageCopy* pRegions) {
    (void)commandBuffer; (void)layout;
    MockImage_T *s = (MockImage_T*)(uintptr_t)src;
    MockBuffer_T *d = (MockBuffer_T*)(uintptr_t)dst;
    if (s && d && s->memory && d->memory) {
        size_t bpp = mock_format_bpp(s->format);
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t *srcPtr = (uint8_t*)s->memory + s->memoryOffset;
            uint8_t *dstPtr = (uint8_t*)d->memory + d->memoryOffset + pRegions[i].bufferOffset;
            VkDeviceSize copySize = (VkDeviceSize)pRegions[i].imageExtent.width * pRegions[i].imageExtent.height * (pRegions[i].imageExtent.depth ? pRegions[i].imageExtent.depth : 1) * bpp;
            if (d->memoryOffset + pRegions[i].bufferOffset + copySize > d->size) {
                copySize = (d->size > d->memoryOffset + pRegions[i].bufferOffset) ? (d->size - (d->memoryOffset + pRegions[i].bufferOffset)) : 0;
            }
            if (s->memorySize > 0 && s->memoryOffset + copySize > s->memorySize) {
                copySize = (s->memorySize > s->memoryOffset) ? (s->memorySize - s->memoryOffset) : 0;
            }
            if (copySize > 0) {
                memcpy(dstPtr, srcPtr, (size_t)copySize);
            }
        }
    }
}

static void VKAPI_CALL mock_vkCmdCopyImage(VkCommandBuffer commandBuffer, VkImage src, VkImageLayout srcLayout, VkImage dst, VkImageLayout dstLayout, uint32_t count, const VkImageCopy* pRegions) {
    (void)commandBuffer; (void)srcLayout; (void)dstLayout;
    MockImage_T *s = (MockImage_T*)(uintptr_t)src;
    MockImage_T *d = (MockImage_T*)(uintptr_t)dst;
    if (s && d && s->memory && d->memory) {
        size_t bpp = mock_format_bpp(s->format);
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t *srcPtr = (uint8_t*)s->memory + s->memoryOffset;
            uint8_t *dstPtr = (uint8_t*)d->memory + d->memoryOffset;
            VkDeviceSize copySize = (VkDeviceSize)pRegions[i].extent.width * pRegions[i].extent.height * (pRegions[i].extent.depth ? pRegions[i].extent.depth : 1) * bpp;
            if (s->memorySize > 0 && s->memoryOffset + copySize > s->memorySize) {
                copySize = (s->memorySize > s->memoryOffset) ? (s->memorySize - s->memoryOffset) : 0;
            }
            if (d->memorySize > 0 && d->memoryOffset + copySize > d->memorySize) {
                copySize = (d->memorySize > d->memoryOffset) ? (d->memorySize - d->memoryOffset) : 0;
            }
            if (copySize > 0) {
                memcpy(dstPtr, srcPtr, (size_t)copySize);
            }
        }
    }
}

static void VKAPI_CALL mock_vkCmdBlitImage(VkCommandBuffer commandBuffer, VkImage src, VkImageLayout srcLayout, VkImage dst, VkImageLayout dstLayout, uint32_t count, const VkImageBlit* pRegions, VkFilter filter) {
    (void)commandBuffer; (void)srcLayout; (void)dstLayout; (void)filter;
    MockImage_T *s = (MockImage_T*)(uintptr_t)src;
    MockImage_T *d = (MockImage_T*)(uintptr_t)dst;
    if (s && d && s->memory && d->memory) {
        size_t bpp = mock_format_bpp(d->format);
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t *srcPtr = (uint8_t*)s->memory + s->memoryOffset;
            uint8_t *dstPtr = (uint8_t*)d->memory + d->memoryOffset;
            uint32_t w = pRegions[i].dstOffsets[1].x - pRegions[i].dstOffsets[0].x;
            uint32_t h = pRegions[i].dstOffsets[1].y - pRegions[i].dstOffsets[0].y;
            VkDeviceSize copySize = (VkDeviceSize)w * h * bpp;
            if (s->memorySize > 0 && s->memoryOffset + copySize > s->memorySize) {
                copySize = (s->memorySize > s->memoryOffset) ? (s->memorySize - s->memoryOffset) : 0;
            }
            if (d->memorySize > 0 && d->memoryOffset + copySize > d->memorySize) {
                copySize = (d->memorySize > d->memoryOffset) ? (d->memorySize - d->memoryOffset) : 0;
            }
            if (copySize > 0) {
                memcpy(dstPtr, srcPtr, (size_t)copySize);
            }
        }
    }
}

struct MockQueryPool_T {
    VkQueryType type;
    uint32_t count;
    std::vector<uint64_t> results;
    std::vector<bool> ready;
};

static VkResult VKAPI_CALL mock_vkCreateQueryPool(VkDevice device, const VkQueryPoolCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkQueryPool* pQueryPool) {
    (void)device; (void)pAllocator;
    if (!pCreateInfo || !pQueryPool) return VK_ERROR_INITIALIZATION_FAILED;
    MockQueryPool_T *qp = new MockQueryPool_T();
    qp->type = pCreateInfo->queryType;
    qp->count = pCreateInfo->queryCount;
    qp->results.resize(qp->count, 0);
    qp->ready.resize(qp->count, false);
    *pQueryPool = (VkQueryPool)(uintptr_t)qp;
    return VK_SUCCESS;
}

static void VKAPI_CALL mock_vkDestroyQueryPool(VkDevice device, VkQueryPool queryPool, const VkAllocationCallbacks* pAllocator) {
    (void)device; (void)pAllocator;
    MockQueryPool_T *qp = (MockQueryPool_T*)(uintptr_t)queryPool;
    delete qp;
}

static void VKAPI_CALL mock_vkCmdBeginQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query, VkQueryControlFlags flags) {
    (void)commandBuffer; (void)flags;
    MockQueryPool_T *qp = (MockQueryPool_T*)(uintptr_t)queryPool;
    if (qp && query < qp->count) {
        qp->ready[query] = false;
        qp->results[query] = 0;
    }
}

static void VKAPI_CALL mock_vkCmdEndQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query) {
    (void)commandBuffer;
    MockQueryPool_T *qp = (MockQueryPool_T*)(uintptr_t)queryPool;
    if (qp && query < qp->count) {
        qp->ready[query] = true;
        qp->results[query] = 1920 * 1080;
    }
}

static void VKAPI_CALL mock_vkCmdResetQueryPool(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount) {
    (void)commandBuffer;
    MockQueryPool_T *qp = (MockQueryPool_T*)(uintptr_t)queryPool;
    if (qp) {
        for (uint32_t i = firstQuery; i < firstQuery + queryCount && i < qp->count; ++i) {
            qp->ready[i] = false;
            qp->results[i] = 0;
        }
    }
}

static VkResult VKAPI_CALL mock_vkGetQueryPoolResults(VkDevice device, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount, size_t dataSize, void* pData, VkDeviceSize stride, VkQueryResultFlags flags) {
    (void)device; (void)stride;
    MockQueryPool_T *qp = (MockQueryPool_T*)(uintptr_t)queryPool;
    if (!qp || !pData) return VK_ERROR_INITIALIZATION_FAILED;
    bool is64 = (flags & VK_QUERY_RESULT_64_BIT) != 0;

    for (uint32_t i = 0; i < queryCount && (firstQuery + i) < qp->count; ++i) {
        uint32_t idx = firstQuery + i;
        uint64_t val = qp->results[idx];
        if (is64) {
            if (dataSize >= sizeof(uint64_t) * (i + 1)) {
                reinterpret_cast<uint64_t*>(pData)[i] = val;
            }
        } else {
            if (dataSize >= sizeof(uint32_t) * (i + 1)) {
                reinterpret_cast<uint32_t*>(pData)[i] = (uint32_t)val;
            }
        }
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL mock_vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* pSubmits, VkFence fence) {
    (void)queue; (void)count; (void)pSubmits; (void)fence;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL mock_vkQueueWaitIdle(VkQueue queue) {
    (void)queue;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL mock_vkDeviceWaitIdle(VkDevice device) {
    (void)device;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL mock_vkEnumerateInstanceLayerProperties(uint32_t* pCount, VkLayerProperties* pProps) {
    (void)pProps;
    if (pCount) *pCount = 0;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL mock_vkEnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pCount, VkExtensionProperties* pProps) {
    (void)pLayerName; (void)pProps;
    if (pCount) *pCount = 0;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL mock_vkCreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDescriptorSetLayout* pSetLayout) {
    (void)device; (void)pCreateInfo; (void)pAllocator;
    static uint64_t s_dsLayoutId = 0x5000;
    *pSetLayout = reinterpret_cast<VkDescriptorSetLayout>((uintptr_t)++s_dsLayoutId);
    return VK_SUCCESS;
}

static void VKAPI_CALL mock_vkDestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout setLayout, const VkAllocationCallbacks* pAllocator) {
    (void)device; (void)setLayout; (void)pAllocator;
}

static VkResult VKAPI_CALL mock_vkCreateDescriptorPool(VkDevice device, const VkDescriptorPoolCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDescriptorPool* pPool) {
    (void)device; (void)pCreateInfo; (void)pAllocator;
    static uint64_t s_dpId = 0x6000;
    *pPool = reinterpret_cast<VkDescriptorPool>((uintptr_t)++s_dpId);
    return VK_SUCCESS;
}

static void VKAPI_CALL mock_vkDestroyDescriptorPool(VkDevice device, VkDescriptorPool pool, const VkAllocationCallbacks* pAllocator) {
    (void)device; (void)pool; (void)pAllocator;
}

static VkResult VKAPI_CALL mock_vkAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo* pAllocateInfo, VkDescriptorSet* pDescriptorSets) {
    (void)device;
    static uint64_t s_dsId = 0x7000;
    if (pAllocateInfo && pDescriptorSets) {
        for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; ++i) {
            pDescriptorSets[i] = reinterpret_cast<VkDescriptorSet>((uintptr_t)++s_dsId);
        }
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL mock_vkFreeDescriptorSets(VkDevice device, VkDescriptorPool pool, uint32_t descriptorSetCount, const VkDescriptorSet* pDescriptorSets) {
    (void)device; (void)pool; (void)descriptorSetCount; (void)pDescriptorSets;
    return VK_SUCCESS;
}

static void VKAPI_CALL mock_vkUpdateDescriptorSets(VkDevice device, uint32_t descriptorWriteCount, const VkWriteDescriptorSet* pDescriptorWrites, uint32_t descriptorCopyCount, const VkCopyDescriptorSet* pDescriptorCopies) {
    (void)device; (void)descriptorWriteCount; (void)pDescriptorWrites; (void)descriptorCopyCount; (void)pDescriptorCopies;
}

static void VKAPI_CALL mock_vkCmdBindDescriptorSets(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout, uint32_t firstSet, uint32_t descriptorSetCount, const VkDescriptorSet* pDescriptorSets, uint32_t dynamicOffsetCount, const uint32_t* pDynamicOffsets) {
    (void)commandBuffer; (void)pipelineBindPoint; (void)layout; (void)firstSet; (void)descriptorSetCount; (void)pDescriptorSets; (void)dynamicOffsetCount; (void)pDynamicOffsets;
}

static void VKAPI_CALL mock_vkCmdPushConstants(VkCommandBuffer commandBuffer, VkPipelineLayout layout, VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size, const void* pValues) {
    (void)commandBuffer; (void)layout; (void)stageFlags; (void)offset; (void)size; (void)pValues;
}

static void populate_mock_table(VlknDispatchTable *t) {
    t->isMock = true;
    t->libHandle = NULL;

    t->vkEnumerateInstanceLayerProperties = mock_vkEnumerateInstanceLayerProperties;
    t->vkEnumerateInstanceExtensionProperties = mock_vkEnumerateInstanceExtensionProperties;
    t->vkCreateInstance = mock_vkCreateInstance;
    t->vkDestroyInstance = mock_vkDestroyInstance;
    t->vkEnumeratePhysicalDevices = mock_vkEnumeratePhysicalDevices;
    t->vkGetPhysicalDeviceProperties = mock_vkGetPhysicalDeviceProperties;
    t->vkGetPhysicalDeviceFeatures = mock_vkGetPhysicalDeviceFeatures;
    t->vkGetPhysicalDeviceMemoryProperties = mock_vkGetPhysicalDeviceMemoryProperties;
    t->vkGetPhysicalDeviceQueueFamilyProperties = mock_vkGetPhysicalDeviceQueueFamilyProperties;
    t->vkCreateDevice = mock_vkCreateDevice;
    t->vkDestroyDevice = mock_vkDestroyDevice;
    t->vkGetDeviceQueue = mock_vkGetDeviceQueue;

    t->vkCreateDescriptorSetLayout = mock_vkCreateDescriptorSetLayout;
    t->vkDestroyDescriptorSetLayout = mock_vkDestroyDescriptorSetLayout;
    t->vkCreateDescriptorPool = mock_vkCreateDescriptorPool;
    t->vkDestroyDescriptorPool = mock_vkDestroyDescriptorPool;
    t->vkAllocateDescriptorSets = mock_vkAllocateDescriptorSets;
    t->vkFreeDescriptorSets = mock_vkFreeDescriptorSets;
    t->vkUpdateDescriptorSets = mock_vkUpdateDescriptorSets;
    t->vkCmdBindDescriptorSets = mock_vkCmdBindDescriptorSets;
    t->vkCmdPushConstants = mock_vkCmdPushConstants;

    t->vkCreateCommandPool = mock_vkCreateCommandPool;
    t->vkDestroyCommandPool = mock_vkDestroyCommandPool;
    t->vkAllocateCommandBuffers = mock_vkAllocateCommandBuffers;
    t->vkFreeCommandBuffers = mock_vkFreeCommandBuffers;
    t->vkBeginCommandBuffer = mock_vkBeginCommandBuffer;
    t->vkEndCommandBuffer = mock_vkEndCommandBuffer;
    t->vkResetCommandBuffer = mock_vkResetCommandBuffer;

    t->vkCreateImage = mock_vkCreateImage;
    t->vkDestroyImage = mock_vkDestroyImage;
    t->vkGetImageMemoryRequirements = mock_vkGetImageMemoryRequirements;
    t->vkCreateImageView = mock_vkCreateImageView;
    t->vkDestroyImageView = mock_vkDestroyImageView;

    t->vkCreateBuffer = mock_vkCreateBuffer;
    t->vkDestroyBuffer = mock_vkDestroyBuffer;
    t->vkGetBufferMemoryRequirements = mock_vkGetBufferMemoryRequirements;

    t->vkAllocateMemory = mock_vkAllocateMemory;
    t->vkFreeMemory = mock_vkFreeMemory;
    t->vkBindBufferMemory = mock_vkBindBufferMemory;
    t->vkBindImageMemory = mock_vkBindImageMemory;
    t->vkMapMemory = mock_vkMapMemory;
    t->vkUnmapMemory = mock_vkUnmapMemory;

    t->vkCreateSampler = mock_vkCreateSampler;
    t->vkDestroySampler = mock_vkDestroySampler;

    t->vkCreateShaderModule = mock_vkCreateShaderModule;
    t->vkDestroyShaderModule = mock_vkDestroyShaderModule;
    t->vkCreatePipelineLayout = mock_vkCreatePipelineLayout;
    t->vkDestroyPipelineLayout = mock_vkDestroyPipelineLayout;
    t->vkCreateRenderPass = mock_vkCreateRenderPass;
    t->vkDestroyRenderPass = mock_vkDestroyRenderPass;
    t->vkCreateFramebuffer = mock_vkCreateFramebuffer;
    t->vkDestroyFramebuffer = mock_vkDestroyFramebuffer;
    t->vkCreateGraphicsPipelines = mock_vkCreateGraphicsPipelines;
    t->vkDestroyPipeline = mock_vkDestroyPipeline;

    t->vkCmdBeginRenderPass = mock_vkCmdBeginRenderPass;
    t->vkCmdEndRenderPass = mock_vkCmdEndRenderPass;
    t->vkCmdBindPipeline = mock_vkCmdBindPipeline;
    t->vkCmdSetViewport = mock_vkCmdSetViewport;
    t->vkCmdSetScissor = mock_vkCmdSetScissor;
    t->vkCmdBindVertexBuffers = mock_vkCmdBindVertexBuffers;
    t->vkCmdBindIndexBuffer = mock_vkCmdBindIndexBuffer;
    t->vkCmdDraw = mock_vkCmdDraw;
    t->vkCmdDrawIndexed = mock_vkCmdDrawIndexed;
    t->vkCmdClearAttachments = mock_vkCmdClearAttachments;
    t->vkCmdPipelineBarrier = mock_vkCmdPipelineBarrier;
    t->vkCmdCopyBuffer = mock_vkCmdCopyBuffer;
    t->vkCmdCopyBufferToImage = mock_vkCmdCopyBufferToImage;
    t->vkCmdCopyImageToBuffer = mock_vkCmdCopyImageToBuffer;
    t->vkCmdCopyImage = mock_vkCmdCopyImage;
    t->vkCmdBlitImage = mock_vkCmdBlitImage;

    t->vkCreateQueryPool = mock_vkCreateQueryPool;
    t->vkDestroyQueryPool = mock_vkDestroyQueryPool;
    t->vkCmdBeginQuery = mock_vkCmdBeginQuery;
    t->vkCmdEndQuery = mock_vkCmdEndQuery;
    t->vkCmdResetQueryPool = mock_vkCmdResetQueryPool;
    t->vkGetQueryPoolResults = mock_vkGetQueryPoolResults;

    t->vkQueueSubmit = mock_vkQueueSubmit;
    t->vkQueueWaitIdle = mock_vkQueueWaitIdle;
    t->vkDeviceWaitIdle = mock_vkDeviceWaitIdle;
}

bool vlkn_dispatch_init(VlknDispatchTable *table, bool forceMock) {
    memset(table, 0, sizeof(*table));
    if (forceMock) {
        populate_mock_table(table);
        return true;
    }

    /* Auto-configure layer path if present in project */
    if (!getenv("VK_LAYER_PATH")) {
        if (access("/home/sull/Projects/SVGA3=VLKN/layers", F_OK) == 0) {
            setenv("VK_LAYER_PATH", "/home/sull/Projects/SVGA3=VLKN/layers:/home/sull/Projects/SVGA3=VLKN/layers_ext/usr/share/vulkan/explicit_layer.d", 0);
        }
    }

    void *handle = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        handle = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    }

    if (!handle) {
        /* Real Vulkan requested but library not found - FAIL */
        return false;
    }

    table->libHandle = handle;
    table->vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(handle, "vkGetInstanceProcAddr");
    if (!table->vkGetInstanceProcAddr) {
        dlclose(handle);
        table->libHandle = NULL;
        return false;
    }

    /* Load initial global functions (instance == VK_NULL_HANDLE) */
    table->vkEnumerateInstanceLayerProperties = (PFN_vkEnumerateInstanceLayerProperties)table->vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
    table->vkEnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties)table->vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    table->vkCreateInstance = (PFN_vkCreateInstance)table->vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");

    table->isMock = false;
    return true;
}

bool vlkn_dispatch_init_instance(VlknDispatchTable *table, VkInstance instance) {
    if (table->isMock) return true;
    if (!instance || !table->vkGetInstanceProcAddr) return false;

    table->vkDestroyInstance = (PFN_vkDestroyInstance)table->vkGetInstanceProcAddr(instance, "vkDestroyInstance");
    table->vkEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)table->vkGetInstanceProcAddr(instance, "vkEnumeratePhysicalDevices");
    table->vkGetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)table->vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties");
    table->vkGetPhysicalDeviceFeatures = (PFN_vkGetPhysicalDeviceFeatures)table->vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures");
    table->vkGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)table->vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceMemoryProperties");
    table->vkGetPhysicalDeviceQueueFamilyProperties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)table->vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    table->vkCreateDevice = (PFN_vkCreateDevice)table->vkGetInstanceProcAddr(instance, "vkCreateDevice");

    table->vkCreateDebugUtilsMessengerEXT = (PFN_vkCreateDebugUtilsMessengerEXT)table->vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    table->vkDestroyDebugUtilsMessengerEXT = (PFN_vkDestroyDebugUtilsMessengerEXT)table->vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");

    return true;
}

bool vlkn_dispatch_init_device(VlknDispatchTable *table, VkInstance instance, VkDevice device) {
    if (table->isMock) return true;

#define LOAD_DEV(fn) table->fn = (PFN_##fn)table->vkGetDeviceProcAddr(device, #fn); \
    if (!table->fn) table->fn = (PFN_##fn)table->vkGetInstanceProcAddr(instance, #fn);

    table->vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)table->vkGetInstanceProcAddr(instance, "vkGetDeviceProcAddr");
    table->vkDestroyDevice = (PFN_vkDestroyDevice)table->vkGetInstanceProcAddr(instance, "vkDestroyDevice");
    table->vkGetDeviceQueue = (PFN_vkGetDeviceQueue)table->vkGetInstanceProcAddr(instance, "vkGetDeviceQueue");

    /* Debug utils extensions (may be null if not enabled) */
    table->vkCreateDebugUtilsMessengerEXT = (PFN_vkCreateDebugUtilsMessengerEXT)table->vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    table->vkDestroyDebugUtilsMessengerEXT = (PFN_vkDestroyDebugUtilsMessengerEXT)table->vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");

    LOAD_DEV(vkCreateCommandPool);
    LOAD_DEV(vkDestroyCommandPool);
    LOAD_DEV(vkAllocateCommandBuffers);
    LOAD_DEV(vkFreeCommandBuffers);
    LOAD_DEV(vkBeginCommandBuffer);
    LOAD_DEV(vkEndCommandBuffer);
    LOAD_DEV(vkResetCommandBuffer);

    LOAD_DEV(vkCreateImage);
    LOAD_DEV(vkDestroyImage);
    LOAD_DEV(vkGetImageMemoryRequirements);
    LOAD_DEV(vkCreateImageView);
    LOAD_DEV(vkDestroyImageView);

    LOAD_DEV(vkCreateBuffer);
    LOAD_DEV(vkDestroyBuffer);
    LOAD_DEV(vkGetBufferMemoryRequirements);

    LOAD_DEV(vkAllocateMemory);
    LOAD_DEV(vkFreeMemory);
    LOAD_DEV(vkBindBufferMemory);
    LOAD_DEV(vkBindImageMemory);
    LOAD_DEV(vkMapMemory);
    LOAD_DEV(vkUnmapMemory);

    LOAD_DEV(vkCreateSampler);
    LOAD_DEV(vkDestroySampler);

    LOAD_DEV(vkCreateShaderModule);
    LOAD_DEV(vkDestroyShaderModule);
    LOAD_DEV(vkCreatePipelineLayout);
    LOAD_DEV(vkDestroyPipelineLayout);
    LOAD_DEV(vkCreateRenderPass);
    LOAD_DEV(vkDestroyRenderPass);
    LOAD_DEV(vkCreateFramebuffer);
    LOAD_DEV(vkDestroyFramebuffer);
    LOAD_DEV(vkCreateGraphicsPipelines);
    LOAD_DEV(vkDestroyPipeline);

    LOAD_DEV(vkCreateDescriptorSetLayout);
    LOAD_DEV(vkDestroyDescriptorSetLayout);
    LOAD_DEV(vkCreateDescriptorPool);
    LOAD_DEV(vkDestroyDescriptorPool);
    LOAD_DEV(vkAllocateDescriptorSets);
    LOAD_DEV(vkFreeDescriptorSets);
    LOAD_DEV(vkUpdateDescriptorSets);
    LOAD_DEV(vkCmdBindDescriptorSets);
    LOAD_DEV(vkCmdPushConstants);

    LOAD_DEV(vkCmdBeginRenderPass);
    LOAD_DEV(vkCmdEndRenderPass);
    LOAD_DEV(vkCmdBindPipeline);
    LOAD_DEV(vkCmdSetViewport);
    LOAD_DEV(vkCmdSetScissor);
    LOAD_DEV(vkCmdBindVertexBuffers);
    LOAD_DEV(vkCmdBindIndexBuffer);
    LOAD_DEV(vkCmdDraw);
    LOAD_DEV(vkCmdDrawIndexed);
    LOAD_DEV(vkCmdClearAttachments);
    LOAD_DEV(vkCmdPipelineBarrier);
    LOAD_DEV(vkCmdCopyBuffer);
    LOAD_DEV(vkCmdCopyBufferToImage);
    LOAD_DEV(vkCmdCopyImageToBuffer);
    LOAD_DEV(vkCmdCopyImage);
    LOAD_DEV(vkCmdBlitImage);

    LOAD_DEV(vkCreateQueryPool);
    LOAD_DEV(vkDestroyQueryPool);
    LOAD_DEV(vkCmdBeginQuery);
    LOAD_DEV(vkCmdEndQuery);
    LOAD_DEV(vkCmdResetQueryPool);
    LOAD_DEV(vkGetQueryPoolResults);

    LOAD_DEV(vkQueueSubmit);
    LOAD_DEV(vkQueueWaitIdle);
    LOAD_DEV(vkDeviceWaitIdle);

#undef LOAD_DEV
    return true;
}

void vlkn_dispatch_cleanup(VlknDispatchTable *table) {
    if (table->libHandle) {
        dlclose(table->libHandle);
        table->libHandle = NULL;
    }
}
