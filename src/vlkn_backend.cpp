/*
 * SVGA3=VLKN - Vulkan Backend Device & Memory Management Implementation
 */

#include "vlkn_backend.h"
#include <cstring>
#include <cstdio>
#include <iostream>

extern "C" void log_msg(const char *fmt, ...);

namespace svga3_vlkn {

static VKAPI_ATTR VkBool32 VKAPI_CALL vlkn_debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData)
{
    (void)type;
    VlknBackend *backend = static_cast<VlknBackend*>(pUserData);
    bool isError = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    std::string msg = (pCallbackData && pCallbackData->pMessage) ? pCallbackData->pMessage : "Unknown validation message";
    if (backend) {
        backend->addValidationMessage(isError, msg);
    }
    fprintf(stderr, "[Vulkan Validation %s] %s\n", isError ? "ERROR" : "WARN", msg.c_str());
    return VK_FALSE;
}

VlknBackend::VlknBackend()
    : m_instance(VK_NULL_HANDLE)
    , m_physicalDevice(VK_NULL_HANDLE)
    , m_device(VK_NULL_HANDLE)
    , m_queue(VK_NULL_HANDLE)
    , m_queueFamilyIndex(0)
    , m_cmdPool(VK_NULL_HANDLE)
    , m_cmdBuffer(VK_NULL_HANDLE)
    , m_cmdBufferRecording(false)
    , m_cmdBufferPending(false)
    , m_completedSubmissionSerial(0)
    , m_descriptorPool(VK_NULL_HANDLE)
    , m_debugMessenger(VK_NULL_HANDLE)
    , m_validationErrors(0)
    , m_validationWarnings(0)
    , m_stagingBuffer(VK_NULL_HANDLE)
    , m_stagingMemory(VK_NULL_HANDLE)
    , m_stagingMapped(nullptr)
    , m_stagingSize(0)
    , m_fallbackBuffer(VK_NULL_HANDLE)
    , m_fallbackMemory(VK_NULL_HANDLE)
    , m_fallbackSize(0)
{
    memset(&m_dispatch, 0, sizeof(m_dispatch));
    memset(&m_props, 0, sizeof(m_props));
    memset(&m_features, 0, sizeof(m_features));
    memset(&m_memProps, 0, sizeof(m_memProps));
}

VlknBackend::~VlknBackend() {
    shutdown();
}

void VlknBackend::addValidationMessage(bool isError, const std::string &msg) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (isError) {
        m_validationErrors++;
    } else {
        m_validationWarnings++;
    }
    m_validationMessages.push_back(msg);
}

Svga3VlknStatus VlknBackend::init(const Svga3VlknConfig *config) {
    bool forceMock = config ? config->forceMockBackend : false;
    if (!vlkn_dispatch_init(&m_dispatch, forceMock)) {
        return SVGA3_VLKN_ERROR_VULKAN_INIT_FAILED;
    }

    Svga3VlknStatus st = initInstance(config);
    if (st != SVGA3_VLKN_SUCCESS) {
        /* NO fallback to mock when real Vulkan is requested! */
        return st;
    }

    st = selectPhysicalDevice(config);
    if (st != SVGA3_VLKN_SUCCESS) return st;

    st = initDevice(config);
    if (st != SVGA3_VLKN_SUCCESS) return st;

    size_t stagingSize = (config && config->stagingBufferSize > 0) ? config->stagingBufferSize : (16 * 1024 * 1024);
    st = initStagingBuffer(stagingSize);
    if (st != SVGA3_VLKN_SUCCESS) return st;

    st = initFallbackBuffer(1024 * 1024); /* 1MB static fallback vertex/index buffer */
    if (st != SVGA3_VLKN_SUCCESS) return st;

    return SVGA3_VLKN_SUCCESS;
}

void VlknBackend::shutdown() {
    waitIdle();

    for (auto &rp : m_renderPasses) {
        if (rp.renderPass) {
            m_dispatch.vkDestroyRenderPass(m_device, rp.renderPass, nullptr);
        }
    }
    m_renderPasses.clear();

    if (m_descriptorPool && m_device) {
        m_dispatch.vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }

    if (m_stagingMapped && m_stagingMemory) {
        m_dispatch.vkUnmapMemory(m_device, m_stagingMemory);
        m_stagingMapped = nullptr;
    }
    if (m_stagingBuffer) {
        m_dispatch.vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
        m_stagingBuffer = VK_NULL_HANDLE;
    }
    if (m_stagingMemory) {
        m_dispatch.vkFreeMemory(m_device, m_stagingMemory, nullptr);
        m_stagingMemory = VK_NULL_HANDLE;
    }

    if (m_fallbackBuffer) {
        m_dispatch.vkDestroyBuffer(m_device, m_fallbackBuffer, nullptr);
        m_fallbackBuffer = VK_NULL_HANDLE;
    }
    if (m_fallbackMemory) {
        m_dispatch.vkFreeMemory(m_device, m_fallbackMemory, nullptr);
        m_fallbackMemory = VK_NULL_HANDLE;
    }
    m_fallbackSize = 0;

    if (m_cmdPool) {
        m_dispatch.vkDestroyCommandPool(m_device, m_cmdPool, nullptr);
        m_cmdPool = VK_NULL_HANDLE;
        m_cmdBuffer = VK_NULL_HANDLE;
        m_cmdBufferRecording = false;
    }

    if (m_device) {
        m_dispatch.vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }

    if (m_debugMessenger && m_instance && m_dispatch.vkDestroyDebugUtilsMessengerEXT) {
        m_dispatch.vkDestroyDebugUtilsMessengerEXT(m_instance, m_debugMessenger, nullptr);
        m_debugMessenger = VK_NULL_HANDLE;
    }

    if (m_instance) {
        m_dispatch.vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }

    vlkn_dispatch_cleanup(&m_dispatch);
}

Svga3VlknStatus VlknBackend::waitIdle() {
    if (m_device && m_dispatch.vkDeviceWaitIdle) {
        VkResult res = m_dispatch.vkDeviceWaitIdle(m_device);
        if (res != VK_SUCCESS) {
            log_msg("[libqemu_svga3d] vkDeviceWaitIdle error: %d\n", res);
            return SVGA3_VLKN_ERROR_DEVICE_LOST;
        }
        if (m_cmdBufferPending) {
            ++m_completedSubmissionSerial;
            m_cmdBufferPending = false;
            if (m_cmdBuffer && m_dispatch.vkResetCommandBuffer) {
                res = m_dispatch.vkResetCommandBuffer(m_cmdBuffer, 0);
                if (res != VK_SUCCESS) {
                    log_msg("[libqemu_svga3d] vkResetCommandBuffer error after idle: %d\n", res);
                    return SVGA3_VLKN_ERROR_DEVICE_LOST;
                }
            }
        }
    } else if (m_device) {
        return SVGA3_VLKN_ERROR_DEVICE_LOST;
    }
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknBackend::initInstance(const Svga3VlknConfig *config) {
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = (config && config->appName) ? config->appName : "SVGA3=VLKN Engine";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "SVGA3=VLKN";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = (config && config->apiVersion) ? config->apiVersion : VK_API_VERSION_1_1;

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    std::vector<const char*> layers;
    std::vector<const char*> extensions;
    VkDebugUtilsMessengerCreateInfoEXT debugInfo = {};

    if (config && config->enableValidationLayers && !m_dispatch.isMock) {
        /* Check layer availability */
        uint32_t layerCount = 0;
        if (m_dispatch.vkEnumerateInstanceLayerProperties) {
            m_dispatch.vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
            if (layerCount > 0) {
                std::vector<VkLayerProperties> availableLayers(layerCount);
                m_dispatch.vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());
                for (const auto &l : availableLayers) {
                    if (strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                        layers.push_back("VK_LAYER_KHRONOS_validation");
                        break;
                    }
                }
            }
        }

        /* Check extension availability for debug utils */
        uint32_t extCount = 0;
        if (m_dispatch.vkEnumerateInstanceExtensionProperties) {
            m_dispatch.vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
            if (extCount > 0) {
                std::vector<VkExtensionProperties> availableExts(extCount);
                m_dispatch.vkEnumerateInstanceExtensionProperties(nullptr, &extCount, availableExts.data());
                for (const auto &e : availableExts) {
                    if (strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
                        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                        break;
                    }
                }
            }
        }

        if (!extensions.empty()) {
            debugInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            debugInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debugInfo.pfnUserCallback = vlkn_debug_callback;
            debugInfo.pUserData = this;
            createInfo.pNext = &debugInfo;
        }
    }

    if (!layers.empty()) {
        createInfo.enabledLayerCount = (uint32_t)layers.size();
        createInfo.ppEnabledLayerNames = layers.data();
    }
    if (!extensions.empty()) {
        createInfo.enabledExtensionCount = (uint32_t)extensions.size();
        createInfo.ppEnabledExtensionNames = extensions.data();
    }

    VkResult res = m_dispatch.vkCreateInstance(&createInfo, nullptr, &m_instance);
    if (res != VK_SUCCESS) {
        return SVGA3_VLKN_ERROR_VULKAN_INIT_FAILED;
    }

    if (!vlkn_dispatch_init_instance(&m_dispatch, m_instance)) {
        log_msg("[libqemu_svga3d] vlkn_dispatch_init_instance failed\n");
        return SVGA3_VLKN_ERROR_VULKAN_INIT_FAILED;
    }

    if (config && config->enableValidationLayers && !m_dispatch.isMock && !extensions.empty()) {
        PFN_vkCreateDebugUtilsMessengerEXT pfnCreateDebugUtils =
            (PFN_vkCreateDebugUtilsMessengerEXT)m_dispatch.vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT");
        if (pfnCreateDebugUtils) {
            pfnCreateDebugUtils(m_instance, &debugInfo, nullptr, &m_debugMessenger);
        }
    }

    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknBackend::selectPhysicalDevice(const Svga3VlknConfig *config) {
    uint32_t deviceCount = 0;
    m_dispatch.vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        return SVGA3_VLKN_ERROR_VULKAN_INIT_FAILED;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    m_dispatch.vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

    /* Score and select best physical device:
     * Discrete GPU > Integrated GPU > Virtual GPU > Other > CPU */
    int bestScore = -1;
    m_physicalDevice = devices[0];
    for (uint32_t i = 0; i < deviceCount; ++i) {
        VkPhysicalDeviceProperties props;
        m_dispatch.vkGetPhysicalDeviceProperties(devices[i], &props);
        int score = 0;
        if (props.deviceType == 2 /* DISCRETE_GPU */) {
            score = (config && config->preferIntegratedGpu) ? 800 : 1000;
        } else if (props.deviceType == 1 /* INTEGRATED_GPU */) {
            score = (config && config->preferIntegratedGpu) ? 1000 : 900;
        } else if (props.deviceType == 3 /* VIRTUAL_GPU */) {
            score = 500;
        } else if (props.deviceType == 0 /* OTHER */) {
            score = 100;
        } else if (props.deviceType == 4 /* CPU */) {
            score = 10;
        }
        if (score > bestScore) {
            bestScore = score;
            m_physicalDevice = devices[i];
        }
    }

    m_dispatch.vkGetPhysicalDeviceProperties(m_physicalDevice, &m_props);
    m_dispatch.vkGetPhysicalDeviceFeatures(m_physicalDevice, &m_features);
    m_dispatch.vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &m_memProps);

    log_msg("[libqemu_svga3d] Selected Vulkan device: %s (type=%d, API=%u.%u.%u, driver=0x%x)\n",
            m_props.deviceName, m_props.deviceType,
            (m_props.apiVersion >> 22) & 0x7F,
            (m_props.apiVersion >> 12) & 0x3FF,
            m_props.apiVersion & 0xFFF,
            m_props.driverVersion);

    /* Find graphics queue family */
    uint32_t queueFamilyCount = 0;
    m_dispatch.vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    m_dispatch.vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, queueFamilies.data());

    bool foundGraphicsQueue = false;
    m_queueFamilyIndex = 0;
    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        if (queueFamilies[i].queueFlags & 0x00000001 /* GRAPHICS_BIT */) {
            m_queueFamilyIndex = i;
            foundGraphicsQueue = true;
            break;
        }
    }
    if (!foundGraphicsQueue) {
        log_msg("[libqemu_svga3d] No graphics queue family found on selected device\n");
        return SVGA3_VLKN_ERROR_VULKAN_INIT_FAILED;
    }

    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknBackend::initDevice(const Svga3VlknConfig *config) {
    (void)config;
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo = {};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = m_queueFamilyIndex;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    VkPhysicalDeviceFeatures enabledFeatures = {};
    enabledFeatures.samplerAnisotropy = m_features.samplerAnisotropy;
    enabledFeatures.textureCompressionBC = m_features.textureCompressionBC;
    enabledFeatures.depthClamp = m_features.depthClamp;
    enabledFeatures.depthBiasClamp = m_features.depthBiasClamp;
    enabledFeatures.fillModeNonSolid = m_features.fillModeNonSolid;
    enabledFeatures.wideLines = m_features.wideLines;

    VkDeviceCreateInfo deviceCreateInfo = {};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.pEnabledFeatures = &enabledFeatures;

    VkResult res = m_dispatch.vkCreateDevice(m_physicalDevice, &deviceCreateInfo, nullptr, &m_device);
    if (res != VK_SUCCESS) {
        return SVGA3_VLKN_ERROR_VULKAN_INIT_FAILED;
    }

    if (!vlkn_dispatch_init_device(&m_dispatch, m_instance, m_device)) {
        log_msg("[libqemu_svga3d] vlkn_dispatch_init_device failed: missing device entry points\n");
        return SVGA3_VLKN_ERROR_VULKAN_INIT_FAILED;
    }

    m_dispatch.vkGetDeviceQueue(m_device, m_queueFamilyIndex, 0, &m_queue);

    /* Create Command Pool */
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = m_queueFamilyIndex;
    poolInfo.flags = 0x00000002; /* VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT */

    res = m_dispatch.vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_cmdPool);
    if (res != VK_SUCCESS) {
        return SVGA3_VLKN_ERROR_VULKAN_INIT_FAILED;
    }

    /* Allocate initial Command Buffer */
    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_cmdPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    res = m_dispatch.vkAllocateCommandBuffers(m_device, &allocInfo, &m_cmdBuffer);
    if (res != VK_SUCCESS) {
        return SVGA3_VLKN_ERROR_VULKAN_INIT_FAILED;
    }

    /* Create Descriptor Pool for shader uniform buffers and texture samplers */
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1024 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2048 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024 * 8 }
    };
    VkDescriptorPoolCreateInfo descPoolInfo = {};
    descPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descPoolInfo.maxSets = 1024;
    descPoolInfo.poolSizeCount = 3;
    descPoolInfo.pPoolSizes = poolSizes;
    descPoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

    res = m_dispatch.vkCreateDescriptorPool(m_device, &descPoolInfo, nullptr, &m_descriptorPool);
    if (res != VK_SUCCESS) {
        return SVGA3_VLKN_ERROR_VULKAN_INIT_FAILED;
    }

    return SVGA3_VLKN_SUCCESS;
}

int VlknBackend::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    for (uint32_t i = 0; i < m_memProps.memoryTypeCount; ++i) {
        if ((typeFilter & (1 << i)) &&
            (m_memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return (int)i;
        }
    }
    /* No fallback: returning a type without the requested properties (e.g.
     * device-local memory for a HOST_VISIBLE|HOST_COHERENT request) would
     * silently produce unmappable or incoherent allocations. Callers must
     * treat -1 as "no suitable memory type" and fail the allocation. */
    return -1;
}

Svga3VlknStatus VlknBackend::allocateMemory(VkDeviceSize size, uint32_t memoryTypeIndex, VkDeviceMemory *outMemory) {
    /* A memory type index is only meaningful below memoryTypeCount. The
     * bug in issue #10 passed findMemoryType()'s -1 failure return here,
     * where it wrapped to 0xFFFFFFFF. Individual call sites guard their
     * findMemoryType() results, and this central check makes the whole
     * class of failure impossible regardless of caller. */
    if (memoryTypeIndex >= m_memProps.memoryTypeCount) {
        log_msg("[libqemu_svga3d] allocateMemory: invalid memory type index %u (type count %u)\n",
                memoryTypeIndex, m_memProps.memoryTypeCount);
        return SVGA3_VLKN_ERROR_INVALID_PARAM;
    }

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;

    VkResult res = m_dispatch.vkAllocateMemory(m_device, &allocInfo, nullptr, outMemory);
    if (res != VK_SUCCESS) {
        return SVGA3_VLKN_ERROR_OUT_OF_MEMORY;
    }
    return SVGA3_VLKN_SUCCESS;
}

void VlknBackend::freeMemory(VkDeviceMemory memory) {
    if (memory) {
        m_dispatch.vkFreeMemory(m_device, memory, nullptr);
    }
}

Svga3VlknStatus VlknBackend::createBuffer(VkDeviceSize size,
                                         VkBufferUsageFlags usage,
                                         VkMemoryPropertyFlags properties,
                                         VkBuffer *outBuffer,
                                         VkDeviceMemory *outMemory)
{
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult res = m_dispatch.vkCreateBuffer(m_device, &bufferInfo, nullptr, outBuffer);
    if (res != VK_SUCCESS) {
        return SVGA3_VLKN_ERROR_OUT_OF_MEMORY;
    }

    VkMemoryRequirements memReqs;
    m_dispatch.vkGetBufferMemoryRequirements(m_device, *outBuffer, &memReqs);

    int memType = findMemoryType(memReqs.memoryTypeBits, properties);
    if (memType < 0) {
        log_msg("[libqemu_svga3d] createBuffer: no memory type satisfies the requested properties\n");
        m_dispatch.vkDestroyBuffer(m_device, *outBuffer, nullptr);
        *outBuffer = VK_NULL_HANDLE;
        return SVGA3_VLKN_ERROR_OUT_OF_MEMORY;
    }
    Svga3VlknStatus st = allocateMemory(memReqs.size, (uint32_t)memType, outMemory);
    if (st != SVGA3_VLKN_SUCCESS) {
        m_dispatch.vkDestroyBuffer(m_device, *outBuffer, nullptr);
        *outBuffer = VK_NULL_HANDLE;
        return st;
    }

    VkResult bindRes = m_dispatch.vkBindBufferMemory(m_device, *outBuffer, *outMemory, 0);
    if (bindRes != VK_SUCCESS) {
        log_msg("[libqemu_svga3d] createBuffer: vkBindBufferMemory failed (%d)\n", bindRes);
        m_dispatch.vkDestroyBuffer(m_device, *outBuffer, nullptr);
        *outBuffer = VK_NULL_HANDLE;
        m_dispatch.vkFreeMemory(m_device, *outMemory, nullptr);
        *outMemory = VK_NULL_HANDLE;
        return SVGA3_VLKN_ERROR_OUT_OF_MEMORY;
    }
    return SVGA3_VLKN_SUCCESS;
}

void VlknBackend::destroyBuffer(VkBuffer buffer, VkDeviceMemory memory) {
    if (buffer) m_dispatch.vkDestroyBuffer(m_device, buffer, nullptr);
    if (memory) m_dispatch.vkFreeMemory(m_device, memory, nullptr);
}

Svga3VlknStatus VlknBackend::initStagingBuffer(size_t size) {
    m_stagingSize = size;
    Svga3VlknStatus st = createBuffer(size,
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                      &m_stagingBuffer,
                                      &m_stagingMemory);
    if (st != SVGA3_VLKN_SUCCESS) return st;

    VkResult res = m_dispatch.vkMapMemory(m_device, m_stagingMemory, 0, size, 0, &m_stagingMapped);
    if (res != VK_SUCCESS) {
        return SVGA3_VLKN_ERROR_OUT_OF_MEMORY;
    }
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknBackend::initFallbackBuffer(size_t size) {
    m_fallbackSize = size;
    Svga3VlknStatus st = createBuffer(size,
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                      &m_fallbackBuffer,
                                      &m_fallbackMemory);
    if (st != SVGA3_VLKN_SUCCESS) return st;

    void *mapped = nullptr;
    VkResult res = m_dispatch.vkMapMemory(m_device, m_fallbackMemory, 0, size, 0, &mapped);
    if (res != VK_SUCCESS) {
        return SVGA3_VLKN_ERROR_OUT_OF_MEMORY;
    }

    /* Fill buffer with 1.0f float values (0x3F800000) so any attribute read produces 1.0f */
    uint32_t *words = static_cast<uint32_t*>(mapped);
    size_t numWords = size / sizeof(uint32_t);
    for (size_t i = 0; i < numWords; ++i) {
        words[i] = 0x3F800000;
    }
    m_dispatch.vkUnmapMemory(m_device, m_fallbackMemory);
    return SVGA3_VLKN_SUCCESS;
}

Svga3VlknStatus VlknBackend::uploadToBuffer(VkBuffer dstBuffer, VkDeviceSize dstOffset, const void *srcData, VkDeviceSize size) {
    std::lock_guard<std::mutex> lock(m_stagingMutex);
    if (size > m_stagingSize) {
        /* Allocate a temporary staging buffer for large transfers */
        VkBuffer tempBuffer;
        VkDeviceMemory tempMemory;
        Svga3VlknStatus st = createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                          &tempBuffer, &tempMemory);
        if (st != SVGA3_VLKN_SUCCESS) return st;

        void *mapped = nullptr;
        VkResult mapRes = m_dispatch.vkMapMemory(m_device, tempMemory, 0, size, 0, &mapped);
        if (mapRes != VK_SUCCESS || !mapped) {
            log_msg("[libqemu_svga3d] uploadToBuffer: vkMapMemory failed (%d)\n", mapRes);
            destroyBuffer(tempBuffer, tempMemory);
            return SVGA3_VLKN_ERROR_OUT_OF_MEMORY;
        }
        memcpy(mapped, srcData, (size_t)size);
        m_dispatch.vkUnmapMemory(m_device, tempMemory);

        VkCommandBuffer cb = getActiveCommandBuffer();
        VkBufferCopy region = {};
        region.srcOffset = 0;
        region.dstOffset = dstOffset;
        region.size = size;
        m_dispatch.vkCmdCopyBuffer(cb, tempBuffer, dstBuffer, 1, &region);
        Svga3VlknStatus flushSt = flushCommandBuffer();

        destroyBuffer(tempBuffer, tempMemory);
        return flushSt;
    }

    memcpy(m_stagingMapped, srcData, (size_t)size);

    VkCommandBuffer cb = getActiveCommandBuffer();
    VkBufferCopy region = {};
    region.srcOffset = 0;
    region.dstOffset = dstOffset;
    region.size = size;
    m_dispatch.vkCmdCopyBuffer(cb, m_stagingBuffer, dstBuffer, 1, &region);
    return flushCommandBuffer();
}

Svga3VlknStatus VlknBackend::downloadFromBuffer(void *dstData, VkBuffer srcBuffer, VkDeviceSize srcOffset, VkDeviceSize size) {
    std::lock_guard<std::mutex> lock(m_stagingMutex);
    if (size > m_stagingSize) {
        /* Allocate a temporary staging buffer for large transfers (mirrors uploadToBuffer) */
        VkBuffer tempBuffer;
        VkDeviceMemory tempMemory;
        Svga3VlknStatus st = createBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                          &tempBuffer, &tempMemory);
        if (st != SVGA3_VLKN_SUCCESS) return st;

        VkCommandBuffer cb = getActiveCommandBuffer();
        VkBufferCopy region = {};
        region.srcOffset = srcOffset;
        region.dstOffset = 0;
        region.size = size;
        m_dispatch.vkCmdCopyBuffer(cb, srcBuffer, tempBuffer, 1, &region);
        st = flushCommandBuffer();
        if (st != SVGA3_VLKN_SUCCESS) {
            destroyBuffer(tempBuffer, tempMemory);
            return st;
        }

        void *mapped = nullptr;
        VkResult mapRes = m_dispatch.vkMapMemory(m_device, tempMemory, 0, size, 0, &mapped);
        if (mapRes != VK_SUCCESS || !mapped) {
            log_msg("[libqemu_svga3d] downloadFromBuffer: vkMapMemory failed (%d)\n", mapRes);
            destroyBuffer(tempBuffer, tempMemory);
            return SVGA3_VLKN_ERROR_OUT_OF_MEMORY;
        }
        memcpy(dstData, mapped, (size_t)size);
        m_dispatch.vkUnmapMemory(m_device, tempMemory);

        destroyBuffer(tempBuffer, tempMemory);
        return SVGA3_VLKN_SUCCESS;
    }

    VkCommandBuffer cb = getActiveCommandBuffer();
    VkBufferCopy region = {};
    region.srcOffset = srcOffset;
    region.dstOffset = 0;
    region.size = size;

    m_dispatch.vkCmdCopyBuffer(cb, srcBuffer, m_stagingBuffer, 1, &region);
    Svga3VlknStatus st = flushCommandBuffer();
    if (st != SVGA3_VLKN_SUCCESS) return st;

    memcpy(dstData, m_stagingMapped, (size_t)size);
    return SVGA3_VLKN_SUCCESS;
}

VkCommandBuffer VlknBackend::getActiveCommandBuffer() {
    if (!m_cmdBufferRecording) {
        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        m_dispatch.vkBeginCommandBuffer(m_cmdBuffer, &beginInfo);
        m_cmdBufferRecording = true;
    }
    return m_cmdBuffer;
}

Svga3VlknStatus VlknBackend::flushCommandBuffer() {
    /* A prior submit may have succeeded even when its queue wait failed. Do
     * not reset or reuse that command buffer until device idle confirms it. */
    if (m_cmdBufferPending) {
        Svga3VlknStatus idleStatus = waitIdle();
        if (idleStatus != SVGA3_VLKN_SUCCESS) return idleStatus;
    }
    if (!m_cmdBufferRecording) return SVGA3_VLKN_SUCCESS;

    /* Hook runs without the flush holding any backend lock, but transfer
     * callers may hold m_stagingMutex (non-recursive) across this whole
     * function — see the threading contract in vlkn_backend.h. The hook
     * must not call transfer APIs. */
    if (m_preFlushHook) {
        auto hook = m_preFlushHook;
        m_preFlushHook = nullptr;
        hook();
        m_preFlushHook = hook;
        if (!m_cmdBufferRecording) return SVGA3_VLKN_SUCCESS;
    }

    VkResult res = m_dispatch.vkEndCommandBuffer(m_cmdBuffer);
    if (res != VK_SUCCESS) {
        log_msg("[libqemu_svga3d] vkEndCommandBuffer error: %d\n", res);
        m_cmdBufferRecording = false;
        VkResult resetRes = m_dispatch.vkResetCommandBuffer(m_cmdBuffer, 0);
        if (resetRes != VK_SUCCESS)
            log_msg("[libqemu_svga3d] vkResetCommandBuffer error after end failure: %d\n", resetRes);
        return SVGA3_VLKN_ERROR_DEVICE_LOST;
    }
    m_cmdBufferRecording = false;

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_cmdBuffer;

    res = m_dispatch.vkQueueSubmit(m_queue, 1, &submitInfo, VK_NULL_HANDLE);
    if (res != VK_SUCCESS) {
        log_msg("[libqemu_svga3d] vkQueueSubmit error: %d\n", res);
        VkResult resetRes = m_dispatch.vkResetCommandBuffer(m_cmdBuffer, 0);
        if (resetRes != VK_SUCCESS)
            log_msg("[libqemu_svga3d] vkResetCommandBuffer error after submit failure: %d\n", resetRes);
        return SVGA3_VLKN_ERROR_DEVICE_LOST;
    }
    m_cmdBufferPending = true;
    res = m_dispatch.vkQueueWaitIdle(m_queue);
    if (res != VK_SUCCESS) {
        log_msg("[libqemu_svga3d] vkQueueWaitIdle error: %d\n", res);
        /* The submit was accepted. Keep its buffer pending until waitIdle()
         * establishes completion; resetting it here would race the GPU. */
        return SVGA3_VLKN_ERROR_DEVICE_LOST;
    }
    ++m_completedSubmissionSerial;
    m_cmdBufferPending = false;
    res = m_dispatch.vkResetCommandBuffer(m_cmdBuffer, 0);
    if (res != VK_SUCCESS) {
        log_msg("[libqemu_svga3d] vkResetCommandBuffer error: %d\n", res);
        return SVGA3_VLKN_ERROR_DEVICE_LOST;
    }

    return SVGA3_VLKN_SUCCESS;
}

VkRenderPass VlknBackend::getOrCreateRenderPass(VkFormat colorFormat, VkFormat depthFormat) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto &entry : m_renderPasses) {
        if (entry.colorFormat == colorFormat && entry.depthFormat == depthFormat) {
            return entry.renderPass;
        }
    }

    VkAttachmentDescription attachments[2] = {};
    uint32_t attachmentCount = 0;

    VkAttachmentReference colorRef = {};
    colorRef.attachment = VK_ATTACHMENT_UNUSED;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    if (colorFormat != VK_FORMAT_UNDEFINED) {
        attachments[attachmentCount].format = colorFormat;
        attachments[attachmentCount].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[attachmentCount].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[attachmentCount].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[attachmentCount].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[attachmentCount].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[attachmentCount].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments[attachmentCount].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        colorRef.attachment = attachmentCount;
        attachmentCount++;
    }

    VkAttachmentReference depthRef = {};
    depthRef.attachment = VK_ATTACHMENT_UNUSED;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    if (depthFormat != VK_FORMAT_UNDEFINED) {
        attachments[attachmentCount].format = depthFormat;
        attachments[attachmentCount].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[attachmentCount].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[attachmentCount].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        bool hasStencil = (depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ||
                           depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                           depthFormat == VK_FORMAT_D16_UNORM_S8_UINT);
        attachments[attachmentCount].stencilLoadOp = hasStencil ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[attachmentCount].stencilStoreOp = hasStencil ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[attachmentCount].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments[attachmentCount].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        depthRef.attachment = attachmentCount;
        attachmentCount++;
    }

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (colorRef.attachment != VK_ATTACHMENT_UNUSED) {
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
    }
    if (depthRef.attachment != VK_ATTACHMENT_UNUSED) {
        subpass.pDepthStencilAttachment = &depthRef;
    }

    VkRenderPassCreateInfo rpInfo = {};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = attachmentCount;
    rpInfo.pAttachments = attachments;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;

    VkRenderPass rp = VK_NULL_HANDLE;
    VkResult res = m_dispatch.vkCreateRenderPass(m_device, &rpInfo, nullptr, &rp);
    if (res != VK_SUCCESS) {
        log_msg("[libqemu_svga3d] getOrCreateRenderPass: vkCreateRenderPass failed (%d)\n", res);
        return VK_NULL_HANDLE;
    }

    m_renderPasses.push_back({ colorFormat, depthFormat, rp });
    return rp;
}

} // namespace svga3_vlkn
