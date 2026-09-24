#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <mutex>
#include <functional>

#include "Rendering/Data/FrameDescriptor.h"
#include "Rendering/Data/DrawableObjectDescriptor.h"
#include "Rendering/Data/PipelineState.h"
#include "Rendering/Entities/Drawable.h"
#include "RenderDef.h"

namespace NLS::Render::RHI
{
    class RHICommandBuffer;
    class RHIBindingSet;
}

namespace NLS::Render::Resources
{
    class Material;
    class Shader;
}

namespace NLS::Render::Core
{
class CompositeRenderer;

class NLS_RENDER_API FrameObjectBindingProvider
{
public:
    using PipelineState = Data::PipelineState;
    struct PreparedBindingSets
    {
        std::shared_ptr<RHI::RHIBindingSet> frameBindingSet;
        std::shared_ptr<RHI::RHIBindingSet> objectBindingSet;
        Data::ObjectDrawConstants objectConstants;
        bool usesObjectIndex = false;
    };

    explicit FrameObjectBindingProvider(CompositeRenderer& renderer);
    virtual ~FrameObjectBindingProvider() = default;

    void BeginFrame(const Data::FrameDescriptor& frameDescriptor);
    void EndFrame();
    bool TryReservePreparedFrameResources();
    void ReleaseReservedPreparedFrameResources();
    bool HasReservedPreparedFrameResources() const;
    virtual void BindCaptureFrameSlot(size_t slotIndex);
    virtual void UnbindCaptureFrameSlot();
    // Called by the producing thread; invoke on the capture thread, then unbind.
    // The provider must outlive the returned callback and its capture scope.
    virtual std::function<void(size_t)> FreezeCaptureFrame();
    bool PrepareDraw(PipelineState& pso, const Entities::Drawable& drawable);
    bool PrepareDraw(
        PipelineState& pso,
        const Entities::Drawable& drawable,
        const Resources::Material& effectiveMaterial);
    bool PrepareDraw(
        PipelineState& pso,
        const Entities::Drawable& drawable,
        const Resources::Material& effectiveMaterial,
        const Resources::Shader& effectiveShader);
    void PrepareExplicitDraw(RHI::RHICommandBuffer& commandBuffer, PipelineState& pso, const Entities::Drawable& drawable);
    // Captures only the current frame constants binding; no drawable/object state is prepared.
    bool CaptureFrameBindingSet(std::shared_ptr<RHI::RHIBindingSet>& outBindingSet);
    // Captures the current drawable/object state without refreshing frame-constant bindings.
    // Legacy providers may also return a frame binding through PreparedBindingSets.
    bool CapturePreparedObjectBindingSet(
        PipelineState& pso,
        const Entities::Drawable& drawable,
        PreparedBindingSets& outBindings);
    bool CapturePreparedBindingSets(PipelineState& pso, const Entities::Drawable& drawable, PreparedBindingSets& outBindings);

    bool IsFramePrepared() const;
    bool IsObjectPrepared() const;
    uint64_t GetPreparedDrawCount() const;

protected:
    struct PreparationState
    {
        bool framePrepared = false;
        bool objectPrepared = false;
        uint64_t preparedDrawCount = 0u;
        const Resources::Material* preparedMaterial = nullptr;
        const Resources::Shader* preparedShader = nullptr;
    };
    virtual PreparationState& ActivePreparationState() const;
    // Public operations protect shared device/cache state. Slot retirement waits
    // must release this lock so pending captures can finish.
    mutable std::recursive_mutex m_stateMutex;
    mutable PreparationState m_preparationState;
    virtual void OnBeginFrame(const Data::FrameDescriptor& frameDescriptor);
    virtual void OnEndFrame();
    virtual bool OnTryReservePreparedFrameResources();
    virtual void OnReleaseReservedPreparedFrameResources();
    virtual bool OnHasReservedPreparedFrameResources() const;
    virtual bool OnPrepareDraw(PipelineState& pso, const Entities::Drawable& drawable);
    virtual void OnPrepareExplicitDraw(RHI::RHICommandBuffer& commandBuffer, PipelineState& pso, const Entities::Drawable& drawable);
    virtual bool OnCaptureFrameBindingSet(std::shared_ptr<RHI::RHIBindingSet>& outBindingSet);
    virtual bool OnCapturePreparedObjectBindingSet(
        PipelineState& pso,
        const Entities::Drawable& drawable,
        PreparedBindingSets& outBindings);
    virtual bool OnCapturePreparedBindingSets(PipelineState& pso, const Entities::Drawable& drawable, PreparedBindingSets& outBindings);

    const Resources::Material* GetPreparedMaterial() const;
    const Resources::Shader* GetPreparedShader() const;

    CompositeRenderer& m_renderer;

};
}
