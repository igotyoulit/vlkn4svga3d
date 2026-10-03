/*
 * SVGA3=VLKN - Vulkan Dispatch & Dynamic Loader
 */

#ifndef ___VLKN_DISPATCH_H___
#define ___VLKN_DISPATCH_H___

#include "vulkan/vulkan.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VlknDispatchTable {
    bool isMock;
    void *libHandle;

    /* Global / Instance pointers */
    PFN_vkGetInstanceProcAddr               vkGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr                  vkGetDeviceProcAddr;
    PFN_vkEnumerateInstanceLayerProperties   vkEnumerateInstanceLayerProperties;
    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance                     vkCreateInstance;
    PFN_vkDestroyInstance                    vkDestroyInstance;
    PFN_vkEnumeratePhysicalDevices           vkEnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties        vkGetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceFeatures          vkGetPhysicalDeviceFeatures;
    PFN_vkGetPhysicalDeviceMemoryProperties  vkGetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties;
    PFN_vkCreateDevice                       vkCreateDevice;
    PFN_vkDestroyDevice                      vkDestroyDevice;
    PFN_vkGetDeviceQueue                     vkGetDeviceQueue;

    /* Debug Utils */
    PFN_vkCreateDebugUtilsMessengerEXT       vkCreateDebugUtilsMessengerEXT;
    PFN_vkDestroyDebugUtilsMessengerEXT      vkDestroyDebugUtilsMessengerEXT;

    /* Descriptor Set & Layout management */
    PFN_vkCreateDescriptorSetLayout          vkCreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout         vkDestroyDescriptorSetLayout;
    PFN_vkCreateDescriptorPool               vkCreateDescriptorPool;
    PFN_vkDestroyDescriptorPool              vkDestroyDescriptorPool;
    PFN_vkAllocateDescriptorSets             vkAllocateDescriptorSets;
    PFN_vkFreeDescriptorSets                 vkFreeDescriptorSets;
    PFN_vkUpdateDescriptorSets               vkUpdateDescriptorSets;
    PFN_vkCmdBindDescriptorSets              vkCmdBindDescriptorSets;
    PFN_vkCmdPushConstants                   vkCmdPushConstants;

    /* Device pointers */
    PFN_vkCreateCommandPool                  vkCreateCommandPool;
    PFN_vkDestroyCommandPool                 vkDestroyCommandPool;
    PFN_vkAllocateCommandBuffers             vkAllocateCommandBuffers;
    PFN_vkFreeCommandBuffers                 vkFreeCommandBuffers;
    PFN_vkBeginCommandBuffer                 vkBeginCommandBuffer;
    PFN_vkEndCommandBuffer                   vkEndCommandBuffer;
    PFN_vkResetCommandBuffer                 vkResetCommandBuffer;

    PFN_vkCreateImage                        vkCreateImage;
    PFN_vkDestroyImage                       vkDestroyImage;
    PFN_vkGetImageMemoryRequirements         vkGetImageMemoryRequirements;
    PFN_vkCreateImageView                    vkCreateImageView;
    PFN_vkDestroyImageView                   vkDestroyImageView;

    PFN_vkCreateBuffer                       vkCreateBuffer;
    PFN_vkDestroyBuffer                      vkDestroyBuffer;
    PFN_vkGetBufferMemoryRequirements        vkGetBufferMemoryRequirements;

    PFN_vkAllocateMemory                     vkAllocateMemory;
    PFN_vkFreeMemory                         vkFreeMemory;
    PFN_vkBindBufferMemory                   vkBindBufferMemory;
    PFN_vkBindImageMemory                    vkBindImageMemory;
    PFN_vkMapMemory                          vkMapMemory;
    PFN_vkUnmapMemory                        vkUnmapMemory;

    PFN_vkCreateSampler                      vkCreateSampler;
    PFN_vkDestroySampler                     vkDestroySampler;

    PFN_vkCreateShaderModule                 vkCreateShaderModule;
    PFN_vkDestroyShaderModule                vkDestroyShaderModule;
    PFN_vkCreatePipelineLayout               vkCreatePipelineLayout;
    PFN_vkDestroyPipelineLayout              vkDestroyPipelineLayout;
    PFN_vkCreateRenderPass                   vkCreateRenderPass;
    PFN_vkDestroyRenderPass                  vkDestroyRenderPass;
    PFN_vkCreateFramebuffer                  vkCreateFramebuffer;
    PFN_vkDestroyFramebuffer                 vkDestroyFramebuffer;
    PFN_vkCreateGraphicsPipelines            vkCreateGraphicsPipelines;
    PFN_vkDestroyPipeline                    vkDestroyPipeline;

    PFN_vkCmdBeginRenderPass                 vkCmdBeginRenderPass;
    PFN_vkCmdEndRenderPass                   vkCmdEndRenderPass;
    PFN_vkCmdBindPipeline                    vkCmdBindPipeline;
    PFN_vkCmdSetViewport                     vkCmdSetViewport;
    PFN_vkCmdSetScissor                      vkCmdSetScissor;
    PFN_vkCmdBindVertexBuffers               vkCmdBindVertexBuffers;
    PFN_vkCmdBindIndexBuffer                 vkCmdBindIndexBuffer;
    PFN_vkCmdDraw                            vkCmdDraw;
    PFN_vkCmdDrawIndexed                     vkCmdDrawIndexed;
    PFN_vkCmdClearColorImage                 vkCmdClearColorImage;
    PFN_vkCmdClearAttachments                vkCmdClearAttachments;
    PFN_vkCmdPipelineBarrier                 vkCmdPipelineBarrier;
    PFN_vkCmdCopyBuffer                      vkCmdCopyBuffer;
    PFN_vkCmdCopyBufferToImage               vkCmdCopyBufferToImage;
    PFN_vkCmdCopyImageToBuffer               vkCmdCopyImageToBuffer;
    PFN_vkCmdCopyImage                       vkCmdCopyImage;
    PFN_vkCmdBlitImage                       vkCmdBlitImage;

    PFN_vkCreateQueryPool                    vkCreateQueryPool;
    PFN_vkDestroyQueryPool                   vkDestroyQueryPool;
    PFN_vkCmdBeginQuery                      vkCmdBeginQuery;
    PFN_vkCmdEndQuery                        vkCmdEndQuery;
    PFN_vkCmdResetQueryPool                  vkCmdResetQueryPool;
    PFN_vkGetQueryPoolResults                vkGetQueryPoolResults;

    PFN_vkQueueSubmit                        vkQueueSubmit;
    PFN_vkQueueWaitIdle                      vkQueueWaitIdle;
    PFN_vkDeviceWaitIdle                     vkDeviceWaitIdle;
} VlknDispatchTable;

/*
 * Load Vulkan dynamic library and populate the dispatch table.
 * If forceMock is true or libvulkan cannot be loaded, initializes the mock table.
 */
bool vlkn_dispatch_init(VlknDispatchTable *table, bool forceMock);

/*
 * Populate instance-level function pointers after vkCreateInstance
 */
bool vlkn_dispatch_init_instance(VlknDispatchTable *table, VkInstance instance);

/*
 * Populate device-level function pointers after vkCreateDevice
 */
bool vlkn_dispatch_init_device(VlknDispatchTable *table, VkInstance instance, VkDevice device);

/*
 * Unload library and cleanup
 */
void vlkn_dispatch_cleanup(VlknDispatchTable *table);

/*
 * Mock-backend test hooks. Only the mock entry points consult this state;
 * the real Vulkan loader path never touches it. Tests use these to inject
 * Vulkan failures and audit live mock objects, so error paths can prove
 * they neither leak resources nor proceed on invalid handles.
 */
void svga3_mock_reset_hooks(void);
void svga3_mock_fail_create_image_view(int count);
void svga3_mock_fail_bind_image_memory(int count);
void svga3_mock_set_image_memory_type_bits(uint32_t bits);
void svga3_mock_clear_image_memory_type_bits(void);
long svga3_mock_live_images(void);
long svga3_mock_live_memory(void);
long svga3_mock_live_views(void);
long svga3_mock_allocate_calls(void);
int  svga3_mock_saw_invalid_alloc_type(void);
long svga3_mock_unbound_image_uses(void);

#ifdef __cplusplus
}
#endif

#endif /* ___VLKN_DISPATCH_H___ */
