#pragma once

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Rendering/Core/IRenderer.h"
#include "Rendering/Core/RenderClearValues.h"
#include "Rendering/Core/RendererStats.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/Data/DrawableObjectDescriptor.h"
#include "Rendering/Data/FrameInfo.h"
#include "Rendering/Data/PipelineState.h"
#include "Rendering/FrameGraph/FrameGraphExecutionContext.h"
#include "Rendering/Resources/IMesh.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIEnums.h"
#include "Rendering/Entities/Drawable.h"
#include "Rendering/Settings/EPixelDataFormat.h"
#include "Rendering/Settings/EPixelDataType.h"
#include <Math/Vector4.h>
#include "RenderDef.h"

namespace NLS::Render::Buffers
{
    class Framebuffer;
    class MultiFramebuffer;
    class UniformBuffer;
}

namespace NLS::Render::RHI
{
    struct RHIBindingLayoutDesc;
    struct RHIBindingSetDesc;
    class RHIBindingLayout;
    class RHIBindingSet;
    class RHIGraphicsPipeline;
    class RHIDevice;
    class RHICommandBuffer;
    class RHITextureView;
    struct RHIFrameContext;
    class RHIMesh;
}

namespace NLS::Render::Context
{
    struct FrameSnapshot;
}

namespace NLS::Render::Resources
{
    struct MaterialPipelineStateOverrides;
}

namespace NLS::Render::Core
{
/**
 * A simple base renderer that doesn't handle any object binding, but provide a strong base for other renderers
 * to implement their own logic.
 */
class NLS_RENDER_API ABaseRenderer : public IRenderer
{
public:
    using PipelineState = Data::PipelineState;
    using Texture2D = Resources::Texture2D;

    struct ExplicitUniformBufferBindingDesc
    {
        uint32_t set = 0u;
        uint32_t registerSpace = 0u;
        uint32_t binding = 0u;
        uint64_t range = 0u;
        RHI::ShaderStageMask stageMask = RHI::ShaderStageMask::AllGraphics;
        std::string entryName;
        std::string layoutDebugName;
        std::string setDebugName;
        std::string snapshotDebugName;
    };

    ABaseRenderer(Context::Driver& p_driver);
    virtual ~ABaseRenderer();

    virtual void BeginFrame(const Data::FrameDescriptor& p_frameDescriptor);
    virtual void EndFrame();
    static bool TryBeginGlobalFrameForBackgroundPreview();
    static void EndGlobalFrameForBackgroundPreview();
    virtual void BeginFrameForBackgroundPreview(const Data::FrameDescriptor& p_frameDescriptor);

    const Data::FrameDescriptor& GetFrameDescriptor() const;
    Context::Driver& GetDriver();
    const Context::Driver& GetDriver() const;
    PipelineState CreatePipelineState() const;
    bool IsDrawing() const;
    bool IsFrameActive() const;
    bool SupportsEditorPickingReadback() const;
    bool HasActiveReadbackSource() const;
    void SetNextFramePostSubmitTextureReadback(Context::PostSubmitTextureReadbackRequest request);
    bool WasLastThreadedFramePublished() const;
    bool IsRenderCoordinateInBounds(uint32_t p_x, uint32_t p_y) const;
    std::shared_ptr<NLS::Render::RHI::RHIBindingSet> CreateExplicitUniformBufferBindingSet(
        const NLS::Render::Buffers::UniformBuffer& buffer,
        const ExplicitUniformBufferBindingDesc& desc) const;
    bool BeginRecordedRenderPass(
        NLS::Render::Buffers::Framebuffer* p_framebuffer,
        uint16_t p_width,
        uint16_t p_height,
        bool p_clearColor,
        bool p_clearDepth,
        bool p_clearStencil,
        const Maths::Vector4& p_clearValue = Core::DefaultOpaqueClearColor());
    bool BeginRecordedRenderPass(
        NLS::Render::Buffers::MultiFramebuffer* p_framebuffer,
        uint16_t p_width,
        uint16_t p_height,
        bool p_clearColor,
        bool p_clearDepth,
        bool p_clearStencil,
        const Maths::Vector4& p_clearValue = Core::DefaultOpaqueClearColor());
    bool BeginRecordedRenderPass(
        NLS::Render::Buffers::MultiFramebuffer* p_framebuffer,
        uint16_t p_width,
        uint16_t p_height,
        bool p_clearColor,
        bool p_clearDepth,
        bool p_clearStencil,
        bool p_writesDepthStencilAttachment,
        const Maths::Vector4& p_clearValue = Core::DefaultOpaqueClearColor());
    bool BeginOutputRenderPass(
        uint16_t p_width,
        uint16_t p_height,
        bool p_clearColor,
        bool p_clearDepth,
        bool p_clearStencil,
        const Maths::Vector4& p_clearValue = Core::DefaultOpaqueClearColor());
    bool BeginOutputRenderPass(
        uint16_t p_width,
        uint16_t p_height,
        bool p_clearColor,
        bool p_clearDepth,
        bool p_clearStencil,
        const std::shared_ptr<RHI::RHITextureView>& p_depthStencilView,
        bool p_writesDepthStencilAttachment,
        const Maths::Vector4& p_clearValue = Core::DefaultOpaqueClearColor());
    void EndRecordedRenderPass();
    void EndOutputRenderPass(bool p_startedRecordedPass);

    void ReadPixels(
        uint32_t p_x,
        uint32_t p_y,
        uint32_t p_width,
        uint32_t p_height,
        Settings::EPixelDataFormat p_format,
        Settings::EPixelDataType p_type,
        void* p_data) const;
    RHI::RHIReadbackResult ReadPixelsChecked(
        uint32_t p_x,
        uint32_t p_y,
        uint32_t p_width,
        uint32_t p_height,
        Settings::EPixelDataFormat p_format,
        Settings::EPixelDataType p_type,
        void* p_data) const;
    RHI::RHIReadbackResult BeginReadPixels(
        uint32_t p_x,
        uint32_t p_y,
        uint32_t p_width,
        uint32_t p_height,
        Settings::EPixelDataFormat p_format,
        Settings::EPixelDataType p_type,
        void* p_data) const;

    virtual void Clear(
        bool p_colorBuffer,
        bool p_depthBuffer,
        bool p_stencilBuffer,
        const Maths::Vector4& p_color = Maths::Vector4::Zero);

    virtual void DrawEntity(
        PipelineState p_pso,
        const Entities::Drawable& p_drawable,
        std::string_view lightMode = "Forward");
    void DrawEntity(
        const Entities::Drawable& p_drawable,
        Resources::MaterialPipelineStateOverrides pipelineOverrides = {},
        Settings::EComparaisonAlgorithm depthCompareOverride = Settings::EComparaisonAlgorithm::LESS,
        std::string_view lightMode = "Forward");

    /**
     * Record a draw call using Formal RHI directly to the command buffer.
     * This gives the renderer direct control over command recording on the
     * explicit RHI path.
     * Returns true if the draw was recorded using Formal RHI, false otherwise.
     */
    bool RecordDrawExplicit(
        const Entities::Drawable& p_drawable,
        Resources::MaterialPipelineStateOverrides pipelineOverrides = {},
        Settings::EComparaisonAlgorithm depthCompareOverride = Settings::EComparaisonAlgorithm::LESS,
        std::string_view lightMode = "Forward");
    uint64_t GetExplicitUniformBindingSetCreationCount() const;
    uint64_t GetExplicitUniformSnapshotBufferCreationCount() const;

protected:
    virtual std::optional<NLS::Render::Context::FrameSnapshot> BuildFrameSnapshot(
        const Data::FrameDescriptor& frameDescriptor) const;
    virtual NLS::Render::Context::PreparedRenderSceneBuilder BuildPreparedRenderSceneBuilder(
        const NLS::Render::Context::FrameSnapshot& snapshot) const;
    void SetPendingFrameSnapshot(
        NLS::Render::Context::FrameSnapshot snapshot);
    void SetPendingPreparedRenderSceneBuilder(
        NLS::Render::Context::PreparedRenderSceneBuilder renderSceneBuilder);
    virtual bool TryPublishThreadedFrame();
    virtual void OnThreadedFramePublished(uint64_t publishedFrameId);
    virtual void OnThreadedFramePublishFailed();
    virtual RendererStats* GetMutableRendererStats() const;
    struct PreparedRecordedDraw
    {
        std::shared_ptr<RHI::RHICommandBuffer> commandBuffer;
        std::shared_ptr<RHI::RHIGraphicsPipeline> pipeline;
        std::shared_ptr<RHI::RHIBindingSet> frameBindingSet;
        std::shared_ptr<RHI::RHIBindingSet> objectBindingSet;
        std::shared_ptr<RHI::RHIBindingSet> passBindingSet;
        std::shared_ptr<RHI::RHIBindingSet> materialBindingSet;
        std::shared_ptr<RHI::RHIMesh> mesh;
        uint32_t instanceCount = 0u;
        uint32_t vertexStart = 0u;
        uint32_t vertexCount = 0u;
        Data::ObjectDrawConstants objectConstants;
        bool usesObjectIndex = false;
    };

    virtual bool PrepareRecordedDraw(
        PipelineState p_pso,
        const Entities::Drawable& p_drawable,
        PreparedRecordedDraw& outDraw) const;
    virtual bool PrepareRecordedDraw(
        PipelineState p_pso,
        const Entities::Drawable& p_drawable,
        std::string_view lightMode,
        PreparedRecordedDraw& outDraw) const;
    virtual bool PrepareRecordedDraw(
        const Entities::Drawable& p_drawable,
        Resources::MaterialPipelineStateOverrides pipelineOverrides,
        Settings::EComparaisonAlgorithm depthCompareOverride,
        PreparedRecordedDraw& outDraw) const;
    virtual bool PrepareRecordedDraw(
        const Entities::Drawable& p_drawable,
        Resources::MaterialPipelineStateOverrides pipelineOverrides,
        Settings::EComparaisonAlgorithm depthCompareOverride,
        std::string_view lightMode,
        PreparedRecordedDraw& outDraw) const;
    bool PrepareRecordedDraw(
        const Entities::Drawable& p_drawable,
        Resources::Material& effectiveMaterial,
        Resources::MaterialPipelineStateOverrides pipelineOverrides,
        Settings::EComparaisonAlgorithm depthCompareOverride,
        std::string_view lightMode,
        PreparedRecordedDraw& outDraw) const;
    virtual void BindPreparedGraphicsPipeline(const PreparedRecordedDraw& preparedDraw) const;
    virtual void BindPreparedMaterialBindingSet(const PreparedRecordedDraw& preparedDraw) const;
    virtual void SubmitPreparedDraw(const PreparedRecordedDraw& preparedDraw) const;
    bool QueueThreadedRecordedDraw(const PreparedRecordedDraw& preparedDraw);
    void ClearPreparedRecordedDrawStaticBaseCache() const;
#if defined(NLS_ENABLE_TEST_HOOKS)
    size_t GetPreparedRecordedDrawStaticBaseCacheSizeForTesting() const;
    size_t GetPreparedRecordedDrawStaticBaseStableIndexSizeForTesting() const;
    static size_t GetPreparedRecordedDrawStaticBaseCacheMaxEntriesForTesting();
    static uint64_t GetPreparedRecordedDrawStaticBaseCacheMaxFrameAgeForTesting();
    static size_t GetPreparedRecordedDrawStaticBaseCacheAgeSweepBudgetForTesting();
    size_t AdvancePreparedRecordedDrawStaticBaseCacheForTesting(uint64_t frameCount) const;
#endif
    NLS::Render::FrameGraph::FrameGraphExecutionContext CreateFrameGraphExecutionContext() const;
    std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> GetActiveExplicitCommandBuffer() const;
    std::shared_ptr<NLS::Render::RHI::RHIDevice> GetExplicitDevice() const;
    void SetActivePreparedPassBindingSet(const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& bindingSet);
    void CaptureRecordedPassAttachmentViews(const NLS::Render::RHI::RHIRenderPassDesc& renderPassDesc);
    bool BeginSingleTargetRenderPass(
        NLS::Render::Buffers::Framebuffer* p_framebuffer,
        uint16_t p_width,
        uint16_t p_height,
        bool p_clearColor,
        bool p_clearDepth,
        bool p_clearStencil,
        const std::shared_ptr<RHI::RHITextureView>& p_depthStencilViewOverride,
        bool p_writesDepthStencilAttachment,
        const Maths::Vector4& p_clearValue);

    Data::FrameDescriptor m_frameDescriptor;
    Context::Driver& m_driver;
    Texture2D* m_emptyTexture;
    PipelineState m_basePipelineState;
    bool m_isDrawing;
    bool m_frameActive = false;
    bool m_globalFrameReleaseDeferred = false;
    bool m_recordedRenderPassActive = false;
    std::shared_ptr<NLS::Render::RHI::RHIBindingSet> m_activePreparedPassBindingSet;
    std::optional<Context::FrameSnapshot> m_pendingFrameSnapshot;
    Context::PreparedRenderSceneBuilder m_pendingPreparedRenderSceneBuilder;
    std::vector<Context::PostSubmitTextureReadbackRequest> m_nextFramePostSubmitTextureReadbacks;
    bool m_lastThreadedFramePublished = false;
    std::vector<Context::RecordedDrawCommandInput> m_threadedRecordedDrawCommands;
    std::vector<std::shared_ptr<NLS::Render::RHI::RHITextureView>> m_activeRecordedPassColorViews;
    std::shared_ptr<NLS::Render::RHI::RHITextureView> m_activeRecordedPassDepthStencilView;

private:
    void BeginFrameInternal(const Data::FrameDescriptor& p_frameDescriptor, bool globalFrameAlreadyAcquired);

    struct PreparedRecordedDrawStaticBaseCacheKey
    {
        uint64_t deviceIdentity = 0u;
        RHI::NativeBackendType backend = RHI::NativeBackendType::None;
        uint64_t materialInstanceId = 0u;
        uint64_t materialParameterRevision = 0u;
        uint64_t materialRenderStateRevision = 0u;
        uint64_t materialBindingRevision = 0u;
        uint64_t materialShaderInstanceId = 0u;
        uint64_t materialShaderGeneration = 0u;
        uint64_t meshInstanceId = 0u;
        uint64_t meshContentRevision = 0u;
        uintptr_t passBindingSetAddress = 0u;
        Settings::EPrimitiveMode primitiveMode = Settings::EPrimitiveMode::TRIANGLES;
        Settings::EComparaisonAlgorithm depthCompareOverride = Settings::EComparaisonAlgorithm::LESS;
        Data::PipelineState pipelineState {};
        Resources::MaterialPipelineStateOverrides pipelineOverrides {};

        bool operator==(const PreparedRecordedDrawStaticBaseCacheKey& rhs) const;
    };
    struct PreparedRecordedDrawStaticBaseCacheKeyHash
    {
        size_t operator()(const PreparedRecordedDrawStaticBaseCacheKey& key) const;
    };
    struct PreparedRecordedDrawStaticBaseStableKey
    {
        uint64_t deviceIdentity = 0u;
        RHI::NativeBackendType backend = RHI::NativeBackendType::None;
        uint64_t materialInstanceId = 0u;
        uint64_t materialShaderInstanceId = 0u;
        uint64_t meshInstanceId = 0u;
        uintptr_t passBindingSetAddress = 0u;
        Settings::EPrimitiveMode primitiveMode = Settings::EPrimitiveMode::TRIANGLES;
        Settings::EComparaisonAlgorithm depthCompareOverride = Settings::EComparaisonAlgorithm::LESS;
        Data::PipelineState pipelineState {};
        Resources::MaterialPipelineStateOverrides pipelineOverrides {};

        bool operator==(const PreparedRecordedDrawStaticBaseStableKey& rhs) const;
    };
    struct PreparedRecordedDrawStaticBaseStableKeyHash
    {
        size_t operator()(const PreparedRecordedDrawStaticBaseStableKey& key) const;
    };
    struct PreparedRecordedDrawStaticBase
    {
        std::shared_ptr<RHI::RHIGraphicsPipeline> pipeline;
        std::shared_ptr<RHI::RHIBindingSet> materialBindingSet;
        std::shared_ptr<RHI::RHIBindingSet> passBindingSet;
        std::shared_ptr<RHI::RHIMesh> mesh;
        int gpuInstances = 0;
        uint64_t lastUsedFrame = 0u;
        std::list<PreparedRecordedDrawStaticBaseCacheKey>::iterator lruIterator {};
        bool lruLinked = false;
    };

    bool ResolvePreparedRecordedDrawStaticBase(
        const char* preparationPath,
        const Entities::Drawable& drawable,
        Resources::Material& material,
        const Resources::MaterialPipelineStateOverrides& pipelineOverrides,
        Settings::EComparaisonAlgorithm depthCompareOverride,
        const Data::PipelineState& pipelineState,
        std::string_view lightMode,
        PreparedRecordedDrawStaticBase& outBase) const;
    void EraseStalePreparedRecordedDrawStaticBaseEntries(
        const PreparedRecordedDrawStaticBaseCacheKey& currentKey) const;
    void ErasePreparedRecordedDrawStaticBaseEntry(
        const PreparedRecordedDrawStaticBaseCacheKey& key) const;
    void IndexPreparedRecordedDrawStaticBaseEntry(
        const PreparedRecordedDrawStaticBaseCacheKey& key) const;
    void LinkPreparedRecordedDrawStaticBaseEntry(
        const PreparedRecordedDrawStaticBaseCacheKey& key,
        PreparedRecordedDrawStaticBase& entry) const;
    void TouchPreparedRecordedDrawStaticBaseEntry(PreparedRecordedDrawStaticBase& entry) const;
    void TrimPreparedRecordedDrawStaticBaseCache(bool includeFrameAgeSweep) const;
    void InvalidateExplicitDeviceDependentCachesIfNeeded() const;
    static PreparedRecordedDrawStaticBaseStableKey BuildPreparedRecordedDrawStaticBaseStableKey(
        const PreparedRecordedDrawStaticBaseCacheKey& key);
    static PreparedRecordedDrawStaticBaseCacheKey BuildPreparedRecordedDrawStaticBaseCacheKey(
        const Entities::Drawable& drawable,
        const Resources::Material& material,
        const std::shared_ptr<RHI::RHIDevice>& device,
        const Resources::MaterialPipelineStateOverrides& pipelineOverrides,
        Settings::EComparaisonAlgorithm depthCompareOverride,
        const Data::PipelineState& pipelineState,
        const std::shared_ptr<RHI::RHIBindingSet>& passBindingSet,
        const Resources::Shader* effectiveShader);
    static void PopulatePreparedRecordedDrawFromStaticBase(
        PreparedRecordedDraw& outDraw,
        std::shared_ptr<RHI::RHICommandBuffer> commandBuffer,
        const Entities::Drawable& drawable,
        const PreparedRecordedDrawStaticBase& staticBase);

    static std::atomic_bool s_isDrawing;
    mutable std::unordered_map<
        PreparedRecordedDrawStaticBaseCacheKey,
        PreparedRecordedDrawStaticBase,
        PreparedRecordedDrawStaticBaseCacheKeyHash> m_preparedRecordedDrawStaticBaseCache;
    mutable std::unordered_map<
        PreparedRecordedDrawStaticBaseStableKey,
        PreparedRecordedDrawStaticBaseCacheKey,
        PreparedRecordedDrawStaticBaseStableKeyHash> m_preparedRecordedDrawStaticBaseStableIndex;
    mutable std::list<PreparedRecordedDrawStaticBaseCacheKey> m_preparedRecordedDrawStaticBaseLruKeys;
    mutable uint64_t m_preparedRecordedDrawStaticBaseCacheFrame = 0u;
    mutable uint64_t m_cachedExplicitDeviceIdentity = 0u;
    mutable RHI::NativeBackendType m_cachedExplicitDeviceBackend = RHI::NativeBackendType::None;
    mutable std::unordered_map<std::string, std::weak_ptr<NLS::Render::RHI::RHIBindingLayout>> m_explicitUniformBindingLayouts;
    mutable uint64_t m_explicitUniformBindingSetCreationCount = 0u;
    mutable uint64_t m_explicitUniformSnapshotBufferCreationCount = 0u;
};
}
