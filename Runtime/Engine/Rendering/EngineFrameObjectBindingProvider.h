#pragma once

#include <chrono>
#include <memory>
#include <cstddef>
#include <atomic>
#include <array>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include <Rendering/Buffers/UniformBuffer.h>
#include <Rendering/Context/ThreadedRenderingLifecycle.h>
#include <Rendering/Core/FrameObjectBindingProvider.h>
#include <Rendering/RHI/Core/RHIBinding.h>
#include <Rendering/RHI/Core/RHIResource.h>

#include "EngineDef.h"

namespace NLS::Render::Resources
{
class Shader;
}

namespace NLS::Render::RHI
{
class RHIDevice;
}

namespace NLS::Render::Data
{
struct DrawableObjectDescriptor;
}

namespace NLS::Engine::Rendering
{
class NLS_ENGINE_API EngineFrameObjectBindingProvider final : public NLS::Render::Core::FrameObjectBindingProvider
{
public:
    explicit EngineFrameObjectBindingProvider(NLS::Render::Core::CompositeRenderer& renderer);
    bool TryReservePreparedFrameResourcesUntil(
        std::chrono::steady_clock::time_point retirementDeadline);
    void PrepareRenderScenePackage(
        const NLS::Render::Context::FrameSnapshot& snapshot,
        NLS::Render::Context::RenderScenePackage& package) const;

    /**
     * Binds every provider capture entry point to one frame slot. Delayed paths
     * that capture a frame on a different thread than the frame that produced the
     * draws must bind the producing slot before touching those entry points, so
     * two frames in flight never share binding-set scratch state. The owner must
     * still guarantee that a single slot is never captured concurrently.
     */
    void BindCaptureFrameSlot(size_t slotIndex) override;
    void UnbindCaptureFrameSlot() override;
    std::function<void(size_t)> FreezeCaptureFrame() override;
#if defined(NLS_ENABLE_TEST_HOOKS)
    std::optional<size_t> GetBoundCaptureFrameSlotForTesting() const;
#endif
#if defined(NLS_ENABLE_TEST_HOOKS)
    struct ObjectDataWorkCountsForTesting
    {
        uint64_t validityScanCount = 0u;
        uint64_t memcmpCount = 0u;
        uint64_t transposeCount = 0u;
        uint64_t uploadCount = 0u;
    };

    uint64_t GetIndexedObjectDataShaderSupportQueryCountForTesting() const;
    uint64_t GetLegacyObjectBufferWriteCountForTesting() const;
    ObjectDataWorkCountsForTesting GetObjectDataWorkCountsForTesting() const;
    void SetActiveObjectDataSlotIndexForTesting(size_t slotIndex);
    void ResetObjectDataSlotForTesting(size_t slotIndex);
#endif

protected:
    PreparationState& ActivePreparationState() const override;
    void OnBeginFrame(const NLS::Render::Data::FrameDescriptor& frameDescriptor) override;
    void OnEndFrame() override;
    bool OnTryReservePreparedFrameResources() override;
    void OnReleaseReservedPreparedFrameResources() override;
    bool OnHasReservedPreparedFrameResources() const override;
    bool OnPrepareDraw(PipelineState& pso, const NLS::Render::Entities::Drawable& drawable) override;
    void OnPrepareExplicitDraw(
        NLS::Render::RHI::RHICommandBuffer& commandBuffer,
        PipelineState& pso,
        const NLS::Render::Entities::Drawable& drawable) override;
    bool OnCaptureFrameBindingSet(
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& outBindingSet) override;
    bool OnCapturePreparedObjectBindingSet(
        PipelineState& pso,
        const NLS::Render::Entities::Drawable& drawable,
        PreparedBindingSets& outBindings) override;
    bool OnCapturePreparedBindingSets(
        PipelineState& pso,
        const NLS::Render::Entities::Drawable& drawable,
        PreparedBindingSets& outBindings) override;

private:
    /**
     * Per-frame-slot binding capture scratch. Everything a prepared draw needs in
     * order to be captured lives here instead of in provider-wide members, so a
     * later frame's game-thread work cannot clobber an earlier frame that is still
     * being captured.
     */
    struct FrameCaptureState
    {
        PreparationState preparation;
        std::unique_ptr<NLS::Render::Buffers::UniformBuffer> frameConstants;
        std::unique_ptr<NLS::Render::Buffers::UniformBuffer> objectConstantsBuffer;
        std::unique_ptr<NLS::Render::Buffers::UniformBuffer> objectConstantsAltBuffer;
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> frameBindingSet;
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> legacyObjectBindingSet;
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> deferredFrameBindingSet;
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> deferredLegacyObjectBindingSet;
        NLS::Render::Data::ObjectDrawConstants currentDrawObjectConstants;
        uint64_t lastFrameGeneration = 0u;
        bool frameBindingSetDirty = true;
        bool legacyObjectBindingSetDirty = true;
        bool currentDrawUsesIndexedObjectData = false;
        bool currentDrawRequiresIndexedObjectData = false;
        bool currentDrawUsesInstanceObjectIndirection = false;
        bool currentDrawPrepared = true;
        bool useAltObjectBuffer = false;
    };

    static void ResetCaptureStateBindings(FrameCaptureState& captureState);
    void ActivateCaptureStateForCurrentFrame(FrameCaptureState& captureState);
    FrameCaptureState& CaptureStateForSlot(size_t slotIndex);
    FrameCaptureState* TryResolveActiveCaptureState(
        std::optional<std::chrono::steady_clock::time_point> retirementDeadline = std::nullopt);
    FrameCaptureState& ActiveCaptureState();
    FrameCaptureState* TryGetActiveCaptureState();

    void RefreshExplicitFrameBindingSet();
    void RefreshExplicitLegacyObjectBindingSet();
    struct ObjectDataFrameSlot;
    std::optional<size_t> ResolveActiveObjectDataSlotIndex(
        std::optional<std::chrono::steady_clock::time_point> retirementDeadline = std::nullopt);
    ObjectDataFrameSlot* ResolveActiveObjectDataSlot();
    void ReleaseStalePreparedObjectDataSlotReservation();
    void RetireIdleObjectDataSlots();
    void ResetObjectDataSlot(ObjectDataFrameSlot& slot);
    void InvalidateObjectDataDeviceCachesIfNeeded();
    bool EnsureObjectDataBufferCapacity(ObjectDataFrameSlot& slot, uint32_t objectIndex);
    bool EnsureIndirectionBufferCapacity(ObjectDataFrameSlot& slot, uint32_t lastIndex);
    std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> ResolveObjectDataBindingLayout(
        const bool usesInstanceIndirection,
        uint64_t deviceIdentity);
    std::shared_ptr<NLS::Render::RHI::RHIBindingSet> RefreshExplicitIndexedObjectBindingSet(ObjectDataFrameSlot& slot);
    void OnDeferredReset();
    bool TryPrepareIndexedObjectData(
        const NLS::Render::Entities::Drawable& drawable,
        const NLS::Render::Data::DrawableObjectDescriptor& descriptor,
        FrameCaptureState& captureState,
        uint32_t* preparedObjectIndex = nullptr);
    bool PreparedShaderRequiresIndexedObjectData() const;
    bool ShaderRequiresIndexedObjectData(const NLS::Render::Resources::Shader& shader) const;
    bool PreparedShaderUsesInstanceObjectIndirection() const;
    bool ShaderUsesInstanceObjectIndirection(const NLS::Render::Resources::Shader& shader) const;

    std::chrono::high_resolution_clock::time_point m_startTime;
    std::unique_ptr<NLS::Render::Buffers::UniformBuffer> m_engineBuffer;
    std::unique_ptr<NLS::Render::Buffers::UniformBuffer> m_hlslFrameBuffer;
    std::unique_ptr<NLS::Render::Buffers::UniformBuffer> m_hlslObjectBuffer;
    std::unique_ptr<NLS::Render::Buffers::UniformBuffer> m_hlslObjectBufferAlt;
    std::shared_ptr<NLS::Render::RHI::RHIDevice> m_explicitDevice;
    std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> m_objectDataBindingLayout;
    uint64_t m_objectDataBindingLayoutDeviceIdentity = 0u;
    std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> m_legacyObjectDataBindingLayout;
    uint64_t m_legacyObjectDataBindingLayoutDeviceIdentity = 0u;
    uint64_t m_cachedObjectDataDeviceIdentity = 0u;
    bool m_preparedFrameObjectDataSlotUnavailable = false;
    bool m_preparedFrameHasObjectDataSlot = false;
    bool m_preparedFrameObjectDataSlotReserved = false;
    size_t m_preparedFrameObjectDataSlotIndex = 0u;
    size_t m_activeObjectDataSlotIndex = 0u;

    // Slot entries are heap-stable because capture can run while another frame
    // grows the container for a later slot.
    std::vector<std::unique_ptr<FrameCaptureState>> m_frameCaptureStates;
    mutable std::mutex m_frameCaptureStatesMutex;
    FrameCaptureState m_fallbackCaptureState;
    std::atomic<uint64_t> m_frameGeneration = 0u;
    std::atomic_bool m_pendingFrameConstantsDirty = true;
    std::array<uint8_t, sizeof(Maths::Matrix4) * 2u + sizeof(Maths::Vector3) + sizeof(float)> m_frameConstantData{};

    struct ObjectDataFrameSlot
    {
        struct RevisionMetadata
        {
            NLS::Render::Data::StaticDrawSceneIdentity stableSceneIdentity;
            uint64_t transformRevision = 0u;
            uint32_t objectIndex = NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex;
            uint32_t objectCount = 0u;
            bool valid = false;
        };

        std::shared_ptr<NLS::Render::RHI::RHIBuffer> buffer;
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> indirectBuffer;
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> bindingSet;
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> deferredBindingSet;
        uint64_t deviceIdentity = 0u;
        std::vector<Maths::Matrix4> objectDataShadow;
        std::vector<Maths::Matrix4> objectDataSourceShadow;
        std::vector<uint8_t> objectDataSourceValid;
        std::vector<uint32_t> objectDataIndirectShadow;
        std::vector<RevisionMetadata> objectDataRevisionMetadata;
        std::vector<uint8_t> objectDataPhysicalSlotOccupancy;
        std::vector<uint32_t> objectDataExplicitLogicalOwnerByPhysicalSlot;
        std::unordered_map<uint32_t, uint32_t> explicitObjectPhysicalSlots;
        uint32_t nextTransientObjectIndex = 0u;
        uint32_t nextTransientIndirectionIndex = 0u;
        size_t capacity = 0u;
        size_t indirectionCapacity = 0u;
        uint32_t idleFrameCount = 0u;
        bool bindingSetDirty = true;
        bool bindingSetUsesInstanceIndirection = false;
        bool usedThisFrame = false;
    };

    std::vector<ObjectDataFrameSlot> m_objectDataSlots;
    std::vector<Maths::Matrix4> m_objectDataTransposeScratch;
    mutable std::unordered_map<uint64_t, std::pair<uint64_t, bool>>
        m_indexedObjectDataShaderSupportCache;
    mutable std::unordered_map<uint64_t, std::pair<uint64_t, bool>>
        m_instanceObjectIndirectionShaderSupportCache;
    mutable uint64_t m_indexedObjectDataShaderSupportQueryCount = 0u;
#if defined(NLS_ENABLE_TEST_HOOKS)
    uint64_t m_legacyObjectBufferWriteCount = 0u;
    ObjectDataWorkCountsForTesting m_objectDataWorkCountsForTesting;
    std::optional<size_t> m_activeObjectDataSlotIndexForTesting;
#endif
};
}
