/*
 * SVGA3=VLKN - Vulkan Backend Device & Memory Management
 */

#ifndef ___VLKN_BACKEND_H___
#define ___VLKN_BACKEND_H___

#include "svga3_vlkn.h"
#include "vlkn_dispatch.h"
#include "vlkn_resource_budgets.h"
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <functional>

namespace svga3_vlkn {

/* Threading and command-buffer contract (issue #13).
 *
 * The backend owns a SINGLE global command buffer (m_cmdBuffer) and a single
 * queue (m_queue). Every flush ends with vkQueueWaitIdle, so all recorded
 * work — transfers and draws alike — is fully serialized: correctness over
 * throughput. There is no separate transfer queue and no fence-based overlap.
 *
 * VlknBackend methods are NOT thread-safe as a whole. Callers must
 * externally serialize device-level operations. The internal mutexes protect
 * specific shared state only:
 *   - m_stagingMutex guards the staging buffer contents AND the bump
 *     allocator offset. Upload paths bump-allocate staging ranges and record
 *     their copies without flushing; the flush is deferred until a range
 *     would be reused (bump overflow forces a flush first), a reader runs
 *     (present / blit readback / guest readback / SVGA_CMD_FENCE), or an
 *     explicit sync point is reached. Every readback still submits before
 *     the host touches the data, so pixel order is unchanged. There is no
 *     second queue and no fence-based overlap: one queue, one command
 *     buffer, correctness over throughput.
 *   - m_mutex guards the render-pass cache.
 * Neither mutex protects the command-buffer recording state machine
 * (m_cmdBufferRecording / m_cmdBufferPending); concurrent
 * flushCommandBuffer() calls from two threads would corrupt it.
 *
 * Pre-flush hook: flushCommandBuffer() invokes m_preFlushHook exactly once
 * per flush using a copy/null/restore pattern, so the hook cannot observe a
 * stale registration. The hook runs with no backend lock held by the flush
 * itself, BUT the caller may hold m_stagingMutex (see above), which is a
 * non-recursive mutex. The hook must therefore not call transfer APIs
 * (uploadToBuffer / downloadFromBuffer) — that would deadlock. The
 * production hook (svga3_device.cpp) only ends render passes, which take the
 * context manager's recursive mutex and record no transfers. */
class VlknBackend {
public:
    VlknBackend();
    ~VlknBackend();

    Svga3VlknStatus init(const Svga3VlknConfig *config);
    void shutdown();
    Svga3VlknStatus waitIdle();
    uint64_t completedSubmissionSerial() const { return m_completedSubmissionSerial; }

    /* Memory allocation helpers */
    /* Returns the memory type index, or -1 if no type in typeFilter has all
     * requested properties. Never falls back to a type lacking the properties. */
    int findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    Svga3VlknStatus allocateMemory(VkDeviceSize size, uint32_t memoryTypeIndex, VkDeviceMemory *outMemory);
    void freeMemory(VkDeviceMemory memory);

    /* Buffer creation helpers */
    Svga3VlknStatus createBuffer(VkDeviceSize size,
                                 VkBufferUsageFlags usage,
                                 VkMemoryPropertyFlags properties,
                                 VkBuffer *outBuffer,
                                 VkDeviceMemory *outMemory);
    void destroyBuffer(VkBuffer buffer, VkDeviceMemory memory);

    /* Staging buffer operations */
    Svga3VlknStatus uploadToBuffer(VkBuffer dstBuffer, VkDeviceSize dstOffset, const void *srcData, VkDeviceSize size);
    Svga3VlknStatus downloadFromBuffer(void *dstData, VkBuffer srcBuffer, VkDeviceSize srcOffset, VkDeviceSize size);

    /* Command buffer management */
    VkCommandBuffer getActiveCommandBuffer();
    Svga3VlknStatus flushCommandBuffer();
    bool cmdBufferRecording() const { return m_cmdBufferRecording; }
    uint64_t recordingSerial() const { return m_recordingSerial; }
    void retireBuffer(VkBuffer buffer, VkDeviceMemory memory, void *mapped, uint64_t serial, size_t budgetBytes);
    void cleanupRetiredBuffers(bool forceAll = false);

    /* Render pass execution with state tracking and safety guard */
    bool isRenderPassActive() const { return m_renderPassActive; }
    void cmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *pBegin, VkSubpassContents contents);
    void cmdEndRenderPass(VkCommandBuffer cb);

    /* Accessors */
    VlknDispatchTable& dispatch() { return m_dispatch; }
    VkInstance instance() const { return m_instance; }
    VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
    VkDevice device() const { return m_device; }
    VkQueue queue() const { return m_queue; }
    uint32_t queueFamilyIndex() const { return m_queueFamilyIndex; }
    const VkPhysicalDeviceProperties& properties() const { return m_props; }
    const VkPhysicalDeviceFeatures& features() const { return m_features; }
    const VkPhysicalDeviceMemoryProperties& memoryProperties() const { return m_memProps; }
    VkDescriptorPool descriptorPool() const { return m_descriptorPool; }

    /* Validation & Driver Info */
    uint32_t validationErrors() const { return m_validationErrors; }
    uint32_t validationWarnings() const { return m_validationWarnings; }
    const std::vector<std::string>& validationMessages() const { return m_validationMessages; }
    void addValidationMessage(bool isError, const std::string &msg);

    const char* driverName() const { return m_props.deviceName; }
    uint32_t apiVersion() const { return m_props.apiVersion; }
    uint32_t driverVersion() const { return m_props.driverVersion; }
    uint32_t vendorId() const { return m_props.vendorID; }
    uint32_t deviceId() const { return m_props.deviceID; }
    bool isSoftware() const { return m_props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU; }

    /* Device-wide aggregate resource budgets (guest-exhaustion guard).
     * Shared by the surface and context managers; see
     * vlkn_resource_budgets.h. */
    VlknResourceBudgets& resourceBudgets() { return m_budgets; }
    const VlknResourceBudgets& resourceBudgets() const { return m_budgets; }

    /* Default / shared RenderPass for color + depth/stencil */
    VkRenderPass getOrCreateRenderPass(VkFormat colorFormat, VkFormat depthFormat);

    /* Staging Buffer Accessors */
    VkBuffer stagingBuffer() const { return m_stagingBuffer; }
    void* stagingMapped() const { return m_stagingMapped; }
    size_t stagingSize() const { return m_stagingSize; }
    std::mutex& stagingMutex() { return m_stagingMutex; }

    /* Staging bump allocator. The caller must hold stagingMutex(). Returns
     * the byte offset of a `size`-byte range, or kStagingAllocFailed when the
     * range does not fit in the remaining space. On failure the caller must
     * flushCommandBuffer() (which retires all pending ranges and resets the
     * bump) and retry once; if it still fails the transfer is larger than the
     * staging buffer and needs a temporary buffer. Returned offsets satisfy
     * optimalBufferCopyOffsetAlignment, so they are valid for both
     * vkCmdCopyBuffer and vkCmdCopyBufferToImage/vkCmdCopyImageToBuffer. */
    static constexpr VkDeviceSize kStagingAllocFailed = ~VkDeviceSize(0);
    VkDeviceSize stagingAlloc(VkDeviceSize size);
    void* stagingMappedAt(VkDeviceSize offset) {
        return static_cast<uint8_t*>(m_stagingMapped) + offset;
    }

    /* Fallback Buffer Accessors (always-valid dummy buffer for missing vertex/index inputs) */
    VkBuffer fallbackBuffer() const { return m_fallbackBuffer; }
    size_t fallbackSize() const { return m_fallbackSize; }

    /* Pre-flush hook (e.g. to end any active render passes before ending command buffer) */
    void setPreFlushHook(std::function<void()> hook) { m_preFlushHook = std::move(hook); }

private:
    Svga3VlknStatus initInstance(const Svga3VlknConfig *config);
    Svga3VlknStatus selectPhysicalDevice(const Svga3VlknConfig *config);
    Svga3VlknStatus initDevice(const Svga3VlknConfig *config);
    Svga3VlknStatus initStagingBuffer(size_t size);
    Svga3VlknStatus initFallbackBuffer(size_t size);

    VlknDispatchTable m_dispatch;
    VkInstance m_instance;
    VkPhysicalDevice m_physicalDevice;
    VkDevice m_device;
    VkQueue m_queue;
    uint32_t m_queueFamilyIndex;

    VkPhysicalDeviceProperties m_props;
    VkPhysicalDeviceFeatures m_features;
    VkPhysicalDeviceMemoryProperties m_memProps;

    VkCommandPool m_cmdPool;
    VkCommandBuffer m_cmdBuffer;
    bool m_cmdBufferRecording;
    bool m_cmdBufferPending;
    bool m_renderPassActive;
    uint64_t m_completedSubmissionSerial;
    uint64_t m_recordingSerial;

    struct RetiredBuffer {
        VkBuffer buffer;
        VkDeviceMemory memory;
        void *mapped;
        uint64_t serial;
        size_t budgetBytes;
    };
    std::vector<RetiredBuffer> m_retiredBuffers;

    VkDescriptorPool m_descriptorPool;

    /* Validation */
    VkDebugUtilsMessengerEXT m_debugMessenger;
    uint32_t m_validationErrors;
    uint32_t m_validationWarnings;
    std::vector<std::string> m_validationMessages;

    std::function<void()> m_preFlushHook;

    /* Staging buffer */
    VkBuffer m_stagingBuffer;
    VkDeviceMemory m_stagingMemory;
    void *m_stagingMapped;
    size_t m_stagingSize;
    std::mutex m_stagingMutex;
    /* Bump offset into the staging buffer. Only touched while holding
     * m_stagingMutex. Reset to 0 whenever flushCommandBuffer() establishes
     * device idle, which retires all recorded staging ranges. */
    VkDeviceSize m_stagingBump;

    /* Fallback buffer */
    VkBuffer m_fallbackBuffer;
    VkDeviceMemory m_fallbackMemory;
    size_t m_fallbackSize;

    /* Default RenderPass cache */
    struct RenderPassEntry {
        VkFormat colorFormat;
        VkFormat depthFormat;
        VkRenderPass renderPass;
    };
    std::vector<RenderPassEntry> m_renderPasses;
    std::mutex m_mutex;

    /* Aggregate device resource budgets (see vlkn_resource_budgets.h). */
    VlknResourceBudgets m_budgets;
};

} // namespace svga3_vlkn

#endif /* ___VLKN_BACKEND_H___ */
