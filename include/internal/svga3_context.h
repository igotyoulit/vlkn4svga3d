/*
 * SVGA3=VLKN - SVGA3D Context & Vulkan Pipeline State Tracker
 */

#ifndef ___SVGA3_CONTEXT_H___
#define ___SVGA3_CONTEXT_H___

#include "svga3_vlkn.h"
#include "vlkn_backend.h"
#include "svga3_surface.h"
#include <unordered_map>
#include <vector>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>

namespace svga3_vlkn {

/* Max stages and render targets supported by SVGA3D */
constexpr uint32_t SVGA3_MAX_RENDER_TARGETS = 4;
constexpr uint32_t SVGA3_MAX_TEXTURE_STAGES = 8;
constexpr uint32_t SVGA3_MAX_VERTEX_DECLS   = 32;

struct RenderTargetBinding {
    uint32_t sid;
    uint32_t face;
    uint32_t mipmap;
};

struct TextureStageState {
    uint32_t sid;
    uint32_t addressU;
    uint32_t addressV;
    uint32_t addressW;
    uint32_t minFilter;
    uint32_t magFilter;
    uint32_t mipFilter;
    uint32_t maxAnisotropy;
    float    mipLodBias;
    VkSampler sampler;
    bool     samplerDirty;
};

struct Svga3Shader {
    uint32_t shid;
    SVGA3dShaderType type;
    std::vector<uint32_t> bytecode;
    VkShaderModule module;
    uint32_t inputLocationMask;
};

struct ShaderConstantBank {
    float floatConsts[256][4];
    int32_t intConsts[16][4];
    uint32_t boolConsts;
};

struct PipelineKey {
    uint32_t topology;
    uint32_t fillMode;
    uint32_t cullMode;
    uint32_t depthTestEnable;
    uint32_t depthWriteEnable;
    uint32_t depthFunc;
    uint32_t blendEnable;
    uint32_t srcColorBlend;
    uint32_t dstColorBlend;
    uint32_t colorBlendEq;
    uint32_t srcAlphaBlend;
    uint32_t dstAlphaBlend;
    uint32_t alphaBlendEq;
    uint32_t stencilEnable;
    uint32_t stencilFunc;
    uint32_t colorWriteMask;
    uint32_t numVertexDecls;
    VkRenderPass renderPass;
    uint64_t vertexDeclHash;
    uint32_t boundVS;
    uint32_t boundPS;
    uint32_t ffTextureStage0;
    uint32_t pad;

    bool operator==(const PipelineKey &other) const {
        return memcmp(this, &other, sizeof(PipelineKey)) == 0;
    }
};

struct PipelineKeyHasher {
    size_t operator()(const PipelineKey &k) const {
        const uint64_t *ptr = reinterpret_cast<const uint64_t*>(&k);
        size_t h = 0xcbf29ce484222325ULL;
        for (size_t i = 0; i < sizeof(PipelineKey) / sizeof(uint64_t); ++i) {
            h = (h ^ ptr[i]) * 0x100000001b3ULL;
        }
        return h;
    }
};

class VlknContext {
public:
    VlknContext(VlknBackend *backend, VlknSurfaceManager *surfaceMgr, uint32_t cid);
    ~VlknContext();

    uint32_t cid() const { return m_cid; }

    /* Render States */
    Svga3VlknStatus setRenderState(SVGA3dRenderStateName state, uint32_t value);
    Svga3VlknStatus getRenderState(SVGA3dRenderStateName state, uint32_t *outValue) const;

    /* Render Targets */
    Svga3VlknStatus setRenderTarget(SVGA3dRenderTargetType type, uint32_t sid, uint32_t face, uint32_t mipmap);
    const RenderTargetBinding* getRenderTarget(SVGA3dRenderTargetType type) const;

    /* Texture Stages & Samplers */
    Svga3VlknStatus setTexture(uint32_t stage, uint32_t sid);
    Svga3VlknStatus setTextureStageState(uint32_t stage, SVGA3dTextureStateName name, uint32_t value);
    uint32_t getTexture(uint32_t stage) const {
        if (stage < SVGA3_MAX_TEXTURE_STAGES) return m_stages[stage].sid;
        return SVGA3D_INVALID_ID;
    }

    /* Viewport & Scissor */
    Svga3VlknStatus setViewport(const SVGA3dRect *rect);
    Svga3VlknStatus setScissorRect(const SVGA3dRect *rect);
    const VkViewport& getViewport() const { return m_viewport; }
    const VkRect2D& getScissor() const { return m_scissor; }

    /* Transforms & Geometry */
    Svga3VlknStatus setTransform(SVGA3dTransformType type, const float matrix[16]);
    Svga3VlknStatus getTransform(SVGA3dTransformType type, float outMatrix[16]) const;

    /* ZRange */
    Svga3VlknStatus setZRange(const SVGA3dZRange *zRange);
    const SVGA3dZRange& getZRange() const { return m_zRange; }

    /* Clip Planes */
    Svga3VlknStatus setClipPlane(uint32_t index, const float plane[4]);
    Svga3VlknStatus getClipPlane(uint32_t index, float outPlane[4]) const;

    /* Materials & Lights */
    Svga3VlknStatus setMaterial(SVGA3dFace face, const SVGA3dMaterial *mat);
    const SVGA3dMaterial* getMaterial(SVGA3dFace face) const;
    Svga3VlknStatus setLightData(uint32_t index, const SVGA3dLightData *data);
    Svga3VlknStatus setLightEnabled(uint32_t index, uint32_t enabled);
    bool isLightEnabled(uint32_t index) const;

    /* Shaders */
    Svga3VlknStatus defineShader(uint32_t shid, SVGA3dShaderType type, const uint32_t *bytecode, uint32_t numDwords);
    Svga3VlknStatus destroyShader(uint32_t shid, SVGA3dShaderType type);
    Svga3VlknStatus setShader(SVGA3dShaderType type, uint32_t shid);
    Svga3VlknStatus setShaderConst(uint32_t reg, SVGA3dShaderType type, SVGA3dShaderConstType ctype, const uint32_t values[4]);
    Svga3VlknStatus getShaderConst(uint32_t reg, SVGA3dShaderType type, SVGA3dShaderConstType ctype, uint32_t outValues[4]) const;
    uint32_t getBoundShader(SVGA3dShaderType type) const;
    bool hasShader(uint32_t shid, SVGA3dShaderType type) const;

    /* Clear */
    Svga3VlknStatus clear(SVGA3dClearFlag flags,
                          uint32_t colorRGBA,
                          float depth,
                          uint32_t stencil,
                          const SVGA3dRect *rects,
                          uint32_t numRects);

    /* Draw */
    Svga3VlknStatus draw(SVGA3dPrimitiveType primitiveType,
                         const SVGA3dVertexDecl *decls,
                         uint32_t numDecls,
                         const SVGA3dPrimitiveRange *ranges,
                         uint32_t numRanges);

    /* Occlusion / Hardware Queries */
    Svga3VlknStatus beginQuery(SVGA3dQueryType type);
    Svga3VlknStatus endQuery(SVGA3dQueryType type);
    Svga3VlknStatus waitForQuery(SVGA3dQueryType type, uint32_t *outResult);
    bool isQueryActive(SVGA3dQueryType type) const;

    /* Stats */
    uint64_t drawCount() const { return m_drawCount; }
    uint64_t vertexCount() const { return m_vertexCount; }
    uint64_t clearCount() const { return m_clearCount; }

    /* Window Surface Tracking */
    uint32_t lastDrawnWindowSid() const { return m_lastDrawnWindowSid; }
    bool hasDrawnToWindow() const { return m_hasDrawnToWindow; }
    void resetDrawnToWindow() { m_hasDrawnToWindow = false; }
    void markWindowDrawn(uint32_t sid) {
        m_lastDrawnWindowSid = sid;
        m_hasDrawnToWindow = true;
    }

    void endRenderPassIfActive();
    void invalidateSurface(uint32_t sid);

private:
    void initDefaultRenderStates();
    std::array<float, 16> getTransformOrDefault(SVGA3dTransformType type) const;
    VkSampler getOrCreateSampler(uint32_t stage);
    VkPipeline getOrCreatePipeline(SVGA3dPrimitiveType primitiveType,
                                   const SVGA3dVertexDecl *decls,
                                   uint32_t numDecls,
                                   VkRenderPass renderPass);
    Svga3VlknStatus ensureRenderPassActive();

    VlknBackend *m_backend;
    VlknSurfaceManager *m_surfaceMgr;
    uint32_t m_cid;

    /* State tables */
    std::unordered_map<uint32_t, uint32_t> m_renderStates;
    RenderTargetBinding m_renderTargets[SVGA3_MAX_RENDER_TARGETS];
    RenderTargetBinding m_depthStencilTarget;
    TextureStageState m_stages[SVGA3_MAX_TEXTURE_STAGES];

    VkViewport m_viewport;
    VkRect2D m_scissor;

    /* Fixed-function transforms and geometry */
    std::unordered_map<uint32_t, std::array<float, 16>> m_transforms;
    SVGA3dZRange m_zRange;
    struct ClipPlane {
        float plane[4];
        bool enabled;
    };
    ClipPlane m_clipPlanes[6];

    /* Material and Lighting */
    SVGA3dMaterial m_materials[2];
    struct LightEntry {
        SVGA3dLightData data;
        bool enabled;
    };
    LightEntry m_lights[8];

    /* Shader Subsystem */
    std::unordered_map<uint32_t, Svga3Shader> m_vertexShaders;
    std::unordered_map<uint32_t, Svga3Shader> m_pixelShaders;
    uint32_t m_boundVS;
    uint32_t m_boundPS;
    ShaderConstantBank m_vsConsts;
    ShaderConstantBank m_psConsts;

    /* Current Active Framebuffer / RenderPass */
    VkFramebuffer m_activeFramebuffer;
    VkRenderPass m_activeRenderPass;
    bool m_inRenderPass;

    /* Pipeline Cache */
    std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHasher> m_pipelineCache;
    VkPipelineLayout m_defaultPipelineLayout;
    VkShaderModule m_defaultVS;
    VkShaderModule m_defaultFS;
    VkShaderModule m_defaultFSTex;
    VkShaderModule m_defaultFSTexPure;

    /* Descriptors and Constant Buffers */
    VkDescriptorSetLayout m_descriptorSetLayout;
    VkDescriptorSet m_descriptorSet;
    bool m_descriptorSetInitialized;
    bool m_descriptorSetDirty;
    bool m_constantsDirty;
    bool m_vsConstsUploadedForFf;
    std::array<float, 16> m_lastFfMvp;
    VkBuffer m_vsConstBuffer;
    VkDeviceMemory m_vsConstMemory;
    VkBuffer m_psConstBuffer;
    VkDeviceMemory m_psConstMemory;

    /* Dummy 1x1 textures. Unbound stages and missing views sample white (1,1,1,1)
       so multitexture modulate operations preserve identity. */
    VkImage m_dummyImage;
    VkDeviceMemory m_dummyMemory;
    VkImageView m_dummyView;
    VkImage m_whiteImage;
    VkDeviceMemory m_whiteMemory;
    VkImageView m_whiteView;
    VkSampler m_dummySampler;

    /* Query Pool */
    VkQueryPool m_queryPool;
    bool m_queryActive;
    bool m_queryEnded;
    uint32_t m_lastQueryResult;

    /* Execution stats */
    uint64_t m_drawCount;
    uint64_t m_vertexCount;
    uint64_t m_clearCount;

    /* Window Surface Tracking */
    uint32_t m_lastDrawnWindowSid;
    bool m_hasDrawnToWindow;

    /* Dummy Vertex Buffer for Missing Vertex Attributes */
    VkBuffer m_dummyVb;
    VkDeviceMemory m_dummyVbMemory;
    uint32_t m_defaultVsInputMask;

    /* Cached Framebuffer Bindings */
    uint32_t m_fbColorSid;
    uint32_t m_fbColorMip;
    uint32_t m_fbColorFace;
    VkImageView m_fbColorView;
    uint32_t m_fbDepthSid;
    uint32_t m_fbDepthMip;
    uint32_t m_fbDepthFace;
    VkImageView m_fbDepthView;
    uint32_t m_fbWidth;
    uint32_t m_fbHeight;

    /* Cached Texture Stage ImageViews & Samplers */
    VkImageView m_boundImageViews[SVGA3_MAX_TEXTURE_STAGES];
    VkSampler m_boundSamplers[SVGA3_MAX_TEXTURE_STAGES];
};

class VlknContextManager {
public:
    VlknContextManager(VlknBackend *backend, VlknSurfaceManager *surfaceMgr);
    ~VlknContextManager();

    Svga3VlknStatus createContext(uint32_t cid);
    Svga3VlknStatus destroyContext(uint32_t cid);
    VlknContext* getContext(uint32_t cid);
    bool exists(uint32_t cid) const;
    void clear();
    size_t count() const;
    void endAllRenderPasses();
    void endAllRenderPassesExcept(uint32_t cid);
    std::vector<std::pair<uint32_t, uint32_t>> collectPendingWindowPresents();
    void invalidateSurface(uint32_t sid);

private:
    VlknBackend *m_backend;
    VlknSurfaceManager *m_surfaceMgr;
    std::unordered_map<uint32_t, std::unique_ptr<VlknContext>> m_contexts;
    mutable std::recursive_mutex m_mutex;
};

} // namespace svga3_vlkn

#endif /* ___SVGA3_CONTEXT_H___ */
