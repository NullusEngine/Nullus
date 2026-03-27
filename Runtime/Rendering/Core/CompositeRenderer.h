#pragma once

#include <typeindex>
#include <memory>

#include "Rendering/Core/ABaseRenderer.h"
#include "Rendering/Core/ARenderPass.h"
#include "Rendering/Features/ARenderFeature.h"
#include "Rendering/Data/Describable.h"
#include "Eventing/Event.h"
#include "RenderDef.h"

namespace NLS::Render::Core
{
/**
 * A renderer relying on composition to define rendering logic.
 */
class NLS_RENDER_API CompositeRenderer : public ABaseRenderer, public Data::Describable
{
public:
    using PipelineState = Data::PipelineState;

    NLS::Event<PipelineState&, const Entities::Drawable&> preDrawEntityEvent;
    NLS::Event<const Entities::Drawable&> postDrawEntityEvent;

    CompositeRenderer(Context::Driver& p_driver);
    CompositeRenderer(const CompositeRenderer&) = delete;
    CompositeRenderer& operator=(const CompositeRenderer&) = delete;

    virtual void BeginFrame(const Data::FrameDescriptor& p_frameDescriptor);
    virtual void DrawFrame();
    virtual void EndFrame() override;

    virtual void DrawEntity(
        PipelineState p_pso,
        const Entities::Drawable& p_drawable
    ) override;

    template<typename T, typename ... Args>
    T& AddFeature(Args&&... p_args);

    template<typename T>
    bool RemoveFeature();

    template<typename T>
    T& GetFeature() const;

    template<typename T>
    bool HasFeature() const;

    template<typename T, typename ... Args>
    T& AddPass(const std::string& p_name, uint32_t p_order, Args&&... p_args);

    template<typename T>
    T& GetPass(const std::string& p_name) const;

protected:
    bool CanRecordExplicitFrame() const override;
    void DrawRegisteredPasses(PipelineState pso);
    std::unordered_map<std::type_index, std::unique_ptr<Features::ARenderFeature>> m_features;
    std::multimap<uint32_t, std::pair<std::string, std::unique_ptr<Core::ARenderPass>>> m_passes;
};
}

#include "Rendering/Core/CompositeRenderer.inl"
