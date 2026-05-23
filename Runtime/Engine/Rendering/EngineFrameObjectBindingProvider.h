#pragma once

#include <chrono>
#include <memory>
#include <cstddef>
#include <optional>
#include <vector>

#include <Rendering/Buffers/UniformBuffer.h>
#include <Rendering/Context/ThreadedRenderingLifecycle.h>
#include <Rendering/Core/FrameObjectBindingProvider.h>
#include <Rendering/RHI/Core/RHIBinding.h>
#include <Rendering/RHI/Core/RHIResource.h>

#include "EngineDef.h"

namespace NLS::Engine::Rendering
{
class NLS_ENGINE_API EngineFrameObjectBindingProvider final : public NLS::Render::Core::FrameObjectBindingProvider
{
public:
    explicit EngineFrameObjectBindingProvider(NLS::Render::Core::CompositeRenderer& renderer);
    void PrepareRenderScenePackage(
        const NLS::Render::Context::FrameSnapshot& snapshot,
        NLS::Render::Context::RenderScenePackage& package) const;

protected:
    void OnBeginFrame(const NLS::Render::Data::FrameDescriptor& frameDescriptor) override;
    void OnEndFrame() override;
    bool OnPrepareDraw(PipelineState& pso, const NLS::Render::Entities::Drawable& drawable) override;
    void OnPrepareExplicitDraw(
        NLS::Render::RHI::RHICommandBuffer& commandBuffer,
        PipelineState& pso,
        const NLS::Render::Entities::Drawable& drawable) override;
    bool OnCapturePreparedBindingSets(
        PipelineState& pso,
        const NLS::Render::Entities::Drawable& drawable,
        PreparedBindingSets& outBindings) override;

private:
    void RefreshExplicitFrameBindingSet();
    void RefreshExplicitObjectBindingSet();
    struct ObjectDataFrameSlot;
    std::optional<size_t> ResolveActiveObjectDataSlotIndex();
    ObjectDataFrameSlot* ResolveActiveObjectDataSlot();
    void ReleaseStalePreparedObjectDataSlotReservation();
    void RetireIdleObjectDataSlots();
    static void ResetObjectDataSlot(ObjectDataFrameSlot& slot);
    bool EnsureObjectDataBufferCapacity(ObjectDataFrameSlot& slot, uint32_t objectIndex);
    std::shared_ptr<NLS::Render::RHI::RHIBindingSet> RefreshExplicitIndexedObjectBindingSet(ObjectDataFrameSlot& slot);
    void OnDeferredReset();
    bool TryPrepareIndexedObjectData(const NLS::Render::Entities::Drawable& drawable, uint32_t* preparedObjectIndex = nullptr);
    bool DrawableRequiresIndexedObjectData(const NLS::Render::Entities::Drawable& drawable) const;

    std::chrono::high_resolution_clock::time_point m_startTime;
    std::unique_ptr<NLS::Render::Buffers::UniformBuffer> m_engineBuffer;
    std::unique_ptr<NLS::Render::Buffers::UniformBuffer> m_hlslFrameBuffer;
    std::unique_ptr<NLS::Render::Buffers::UniformBuffer> m_hlslObjectBuffer;
    std::unique_ptr<NLS::Render::Buffers::UniformBuffer> m_hlslObjectBufferAlt;
    std::shared_ptr<NLS::Render::RHI::RHIBindingSet> m_explicitFrameBindingSet;
    std::shared_ptr<NLS::Render::RHI::RHIBindingSet> m_explicitObjectBindingSet;
    std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> m_objectDataBindingLayout;
    bool m_explicitFrameBindingSetDirty = true;
    bool m_explicitObjectBindingSetDirty = true;
    bool m_currentDrawUsesIndexedObjectData = false;
    bool m_currentDrawRequiresIndexedObjectData = false;
    bool m_currentDrawPrepared = true;
    uint32_t m_currentDrawObjectIndex = 0u;
    bool m_preparedFrameHasObjectDataSlot = false;
    bool m_preparedFrameObjectDataSlotReserved = false;
    bool m_useAltObjectBuffer = false;
    size_t m_preparedFrameObjectDataSlotIndex = 0u;
    size_t m_activeObjectDataSlotIndex = 0u;

    struct ObjectDataFrameSlot
    {
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> buffer;
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> bindingSet;
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> deferredBindingSet;
        std::vector<Maths::Matrix4> objectDataShadow;
        size_t capacity = 0u;
        uint32_t idleFrameCount = 0u;
        bool bindingSetDirty = true;
        bool usedThisFrame = false;
    };

    std::shared_ptr<NLS::Render::RHI::RHIBindingSet> m_deferredFrameBindingSet;
    std::shared_ptr<NLS::Render::RHI::RHIBindingSet> m_deferredObjectBindingSet;
    std::vector<ObjectDataFrameSlot> m_objectDataSlots;
};
}
