/*
 * SVGA3=VLKN - Device-wide aggregate resource budgets
 *
 * Per-object caps (max surface dimensions, max shaders per context, ...)
 * still allow a malicious guest to exhaust host/device memory by creating
 * many objects that are each under the per-object cap. These device-wide
 * budgets bound the total bytes/modules a guest can have live at once.
 *
 * All check-and-reserve operations are serialized by an internal mutex so
 * a check-then-act race cannot overshoot the cap from two threads.
 */

#ifndef ___VLKN_RESOURCE_BUDGETS_H___
#define ___VLKN_RESOURCE_BUDGETS_H___

#include <cstdint>
#include <mutex>

namespace svga3_vlkn {

/* Aggregate caps. Sized to be generous for legitimate guests while still
 * bounding worst-case host memory consumption. */
constexpr uint64_t SVGA3_BUDGET_MAX_SURFACE_BYTES        = 1ULL * 1024 * 1024 * 1024; /* 1 GiB */
constexpr uint64_t SVGA3_BUDGET_MAX_SHADER_BYTECODE_BYTES = 256ULL * 1024 * 1024;     /* 256 MiB */
constexpr uint64_t SVGA3_BUDGET_MAX_SHADER_MODULES       = 8192;

class VlknResourceBudgets {
public:
    VlknResourceBudgets() = default;

    /* Reserve `bytes` of surface (image + buffer) memory. Returns false and
     * reserves nothing when the reservation would exceed the cap. */
    bool tryReserveSurfaceBytes(uint64_t bytes) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (bytes > m_maxSurfaceBytes) return false;
        if (m_surfaceBytes > m_maxSurfaceBytes - bytes) return false;
        m_surfaceBytes += bytes;
        return true;
    }

    /* Release a previous reservation. Saturates at zero rather than
     * wrapping: an accounting bug must not corrupt the budget. */
    void releaseSurfaceBytes(uint64_t bytes) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_surfaceBytes = (bytes >= m_surfaceBytes) ? 0 : m_surfaceBytes - bytes;
    }

    bool tryReserveShaderBytes(uint64_t bytes) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (bytes > m_maxShaderBytecodeBytes) return false;
        if (m_shaderBytecodeBytes > m_maxShaderBytecodeBytes - bytes) return false;
        m_shaderBytecodeBytes += bytes;
        return true;
    }

    void releaseShaderBytes(uint64_t bytes) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shaderBytecodeBytes = (bytes >= m_shaderBytecodeBytes) ? 0 : m_shaderBytecodeBytes - bytes;
    }

    bool tryReserveShaderModules(uint64_t count) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (count > m_maxShaderModules) return false;
        if (m_shaderModuleCount > m_maxShaderModules - count) return false;
        m_shaderModuleCount += count;
        return true;
    }

    void releaseShaderModules(uint64_t count) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shaderModuleCount = (count >= m_shaderModuleCount) ? 0 : m_shaderModuleCount - count;
    }

    /* Introspection for tests and monitoring. */
    uint64_t surfaceBytes() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_surfaceBytes;
    }
    uint64_t shaderBytecodeBytes() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_shaderBytecodeBytes;
    }
    uint64_t shaderModuleCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_shaderModuleCount;
    }
    uint64_t maxSurfaceBytes() const { return m_maxSurfaceBytes; }
    uint64_t maxShaderBytecodeBytes() const { return m_maxShaderBytecodeBytes; }
    uint64_t maxShaderModules() const { return m_maxShaderModules; }

    /* Test-only: shrink caps so stress tests don't need gigabytes. */
    void setMaxForTesting(uint64_t surfaceBytes, uint64_t shaderBytes, uint64_t modules) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_maxSurfaceBytes = surfaceBytes;
        m_maxShaderBytecodeBytes = shaderBytes;
        m_maxShaderModules = modules;
    }

private:
    mutable std::mutex m_mutex;
    uint64_t m_surfaceBytes = 0;
    uint64_t m_shaderBytecodeBytes = 0;
    uint64_t m_shaderModuleCount = 0;
    uint64_t m_maxSurfaceBytes = SVGA3_BUDGET_MAX_SURFACE_BYTES;
    uint64_t m_maxShaderBytecodeBytes = SVGA3_BUDGET_MAX_SHADER_BYTECODE_BYTES;
    uint64_t m_maxShaderModules = SVGA3_BUDGET_MAX_SHADER_MODULES;
};

} // namespace svga3_vlkn

#endif /* ___VLKN_RESOURCE_BUDGETS_H___ */
