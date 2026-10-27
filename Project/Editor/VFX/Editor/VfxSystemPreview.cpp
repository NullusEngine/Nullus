#include "VfxSystemPreview.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>
#include <ServiceLocator.h>
#include <UI/UIManager.h>
#include <VFX/ParticleRenderer.h>
#include <VFX/SystemRenderProxy.h>
#include <VFXCompiler/VfxShaderCompiler.h>
#include <Rendering/Context/RenderCommandPipe.h>
#include <Rendering/Context/RhiCommandListExecutor.h>
#include <Base/TaskGraph/TaskGraph.h>
#include <Jobs/BackgroundJobQueue.h>
#include <Jobs/JobSystem.h>

namespace NLS::Editor::VFXTools
{
namespace
{
namespace rhi = Render::RHI;
namespace context = Render::Context;
context::RenderCommandEvent CompilePreviewAsync(std::function<void()> compile)
{
    const auto* graph = context::RenderCommandDispatcher::GetTaskGraph();
    if (graph && graph->IsStarted())
        return context::RenderCommandDispatcher::EnqueueAsync(std::move(compile));

    if (Base::Jobs::IsJobSystemInitialized())
    {
        // A single render lane still has a background IO/compiler lane. Never
        // join shader compilation into a frame: it can take seconds on a miss.
        struct Work
        {
            std::function<void()> compile;
            context::RenderCommandEvent completion;
        };
        auto work = std::make_unique<Work>(); work->compile = std::move(compile);
        const auto completion = work->completion;
        Base::Jobs::BackgroundJobDesc desc;
        desc.userData = work.get(); desc.cancelUserData = work.get();
        desc.debugName = "VfxPreview::CompileShaders";
        desc.function = [](void* data)
        {
            std::unique_ptr<Work> work(static_cast<Work*>(data));
            try { work->compile(); work->completion.Signal(); }
            catch (const std::exception& error) { work->completion.Fail(error.what()); }
            catch (...) { work->completion.Fail("Particle preview compiler failed"); }
        };
        desc.cancelFunction = [](void* data)
        {
            std::unique_ptr<Work> work(static_cast<Work*>(data));
            work->completion.Cancel("Particle preview compilation was cancelled");
        };
        if (Base::Jobs::ScheduleBackgroundJob(desc).id != 0) (void)work.release();
        else completion.Cancel("Particle preview compiler is shutting down");
        return completion;
    }

    // Standalone render tests/tools without an application JobSystem retain
    // their explicit serial drain contract. The Editor initializes JobSystem.
    auto completion = context::RenderCommandDispatcher::EnqueueAsync(std::move(compile));
    context::RenderCommandDispatcher::MergeAsync(completion,std::chrono::seconds(60));
    return completion;
}
void Retire(const std::shared_ptr<rhi::RHITextureView>& view)
{
    if (view && NLS::Core::ServiceLocator::Contains<UI::UIManager>())
        NLS::Core::ServiceLocator::Get<UI::UIManager>().RetireTextureViewHandle(view);
}
std::shared_ptr<const NLS::VFX::SystemFrame> Rebind(const NLS::VFX::SystemFrame& source, const std::shared_ptr<const NLS::VFX::CompiledSystem>& program)
{
    auto result = std::make_shared<NLS::VFX::SystemFrame>();
    result->system = program; result->cpu = source.cpu; result->cpuBounds = source.cpuBounds; result->parameters = source.parameters;
    result->sequence = source.sequence; result->resetRevision = source.resetRevision; result->clearRevision = source.clearRevision;
    result->simulationAge = source.simulationAge; result->simulationSteps = source.simulationSteps;
    result->deltaTime = source.deltaTime; result->age = source.age; result->spawning = source.spawning; result->paused = source.paused; return result;
}
struct DrawResources
{
    std::shared_ptr<rhi::RHIDevice> device;
    std::shared_ptr<rhi::RHICommandPool> pool;
    std::shared_ptr<rhi::RHICommandBuffer> commands;
    std::shared_ptr<rhi::RHITextureView> color, depth;
    std::shared_ptr<rhi::RHIFence> fence;
    std::shared_ptr<const NLS::VFX::SystemRenderFrame> frame;
    std::vector<std::shared_ptr<const NLS::VFX::SpriteDrawResources>> draws;
    std::optional<context::RecordedDrawCommandInput> grid;
    std::optional<std::array<context::RecordedDrawCommandInput,3>> axes;
};
bool SameInput(const VfxSystemPreviewInput& a,const VfxSystemPreviewInput& b)
{
    if (a.showGrid != b.showGrid || a.gridShaders != b.gridShaders || a.gridEye != b.gridEye || a.gridColor != b.gridColor) return false;
    if (a.showOriginAxis != b.showOriginAxis || a.axesShaders != b.axesShaders) return false;
    if (a.width != b.width || a.height != b.height || a.isolatedEmitter != b.isolatedEmitter || a.background != b.background || a.materials != b.materials ||
        a.camera.viewProjection != b.camera.viewProjection || a.camera.localToWorld != b.camera.localToWorld || a.camera.cameraRight != b.camera.cameraRight ||
        a.camera.cameraUp != b.camera.cameraUp || a.camera.cameraForward != b.camera.cameraForward || a.meshes.size() != b.meshes.size()) return false;
    for (size_t emitter = 0; emitter < a.meshes.size(); ++emitter)
    {
        if (a.meshes[emitter].size() != b.meshes[emitter].size()) return false;
        for (size_t slot = 0; slot < a.meshes[emitter].size(); ++slot)
        {
            const auto& left = a.meshes[emitter][slot]; const auto& right = b.meshes[emitter][slot];
            if (!left || !right) { if (left != right) return false; continue; }
            if (left->firstLod != right->firstLod || left->bounds != right->bounds || left->levels.size() != right->levels.size()) return false;
            for (size_t lod = 0; lod < left->levels.size(); ++lod)
                if (left->levels[lod].mesh != right->levels[lod].mesh || left->levels[lod].screenSize != right->levels[lod].screenSize) return false;
        }
    }
    return true;
}
}
struct VfxSystemPreview::State
{
    std::atomic_bool cancelled{false};
    std::mutex mutex;
    std::shared_ptr<rhi::RHIDevice> device;
    std::shared_ptr<const NLS::VFX::CompiledSystem> source;
    std::shared_ptr<NLS::VFX::CompiledSystem> program;
    std::unique_ptr<NLS::VFX::SystemRenderProxy> proxy;
    std::vector<std::unique_ptr<NLS::VFX::ParticleRenderer>> renderers;
    Rendering::EditorGridRenderer gridRenderer;
    context::RenderCommandEvent compilation, render;
    bool hasRender = false;
    std::vector<std::shared_ptr<const NLS::VFX::SystemFrame>> pending;
    std::shared_ptr<const NLS::VFX::SystemFrame> lastFrame;
    std::shared_ptr<const NLS::VFX::SystemFrame> lastSource;
    std::shared_ptr<rhi::RHITextureView> output;
    std::shared_ptr<rhi::RHIFence> fence;
    std::string error;
    std::optional<VfxSystemPreviewInput> lastInput;
};
VfxSystemPreview::VfxSystemPreview() = default;
VfxSystemPreview::~VfxSystemPreview() { Clear(); }
void VfxSystemPreview::Clear()
{
    if (m_state) m_state->cancelled = true;
    m_state.reset(); Retire(m_display); m_display.reset();
}
bool VfxSystemPreview::CanAdvance() const
{
    if (!m_state || !m_state->compilation.IsComplete() || !m_state->compilation.Succeeded()) return false;
    if (m_state->hasRender && (!m_state->render.IsComplete() || !m_state->render.Succeeded())) return false;
    std::lock_guard lock(m_state->mutex);
    return m_state->error.empty() && (!m_state->fence || m_state->fence->IsSignaled());
}
void VfxSystemPreview::Update(std::shared_ptr<rhi::RHIDevice> device, std::shared_ptr<const NLS::VFX::CompiledSystem> system,
    std::vector<std::shared_ptr<const NLS::VFX::SystemFrame>> frames, const std::filesystem::path& cacheRoot, VfxSystemPreviewInput input)
{
    if (!device || !system || !input.width || !input.height || input.width > 4096 || input.height > 4096) return;
    if (!m_state || m_state->source != system || m_state->device != device)
    {
        if (m_state) m_state->cancelled = true;
        auto state = std::make_shared<State>(); state->device = device; state->source = system; state->program = std::make_shared<NLS::VFX::CompiledSystem>(*system);
        m_state = state;
        state->compilation = CompilePreviewAsync([state,cacheRoot]
        {
            if (state->cancelled) return;
            const auto backend = state->device->GetAdapter()->GetBackendType(); std::vector<NLS::VFX::Diagnostic> diagnostics;
            try
            {
                std::string error;
                if (!NLS::VFX::ValidatePlatformPrograms(*state->program,backend,error) &&
                    !NLS::VFX::CompilePlatformPrograms(*state->program,std::span(&backend,1),cacheRoot,diagnostics,&state->cancelled))
                    throw std::runtime_error(diagnostics.empty() ? "Particle shader compilation failed" : diagnostics.front().message);
            }
            catch (const std::exception& exception) { std::lock_guard lock(state->mutex); state->error = exception.what(); }
        });
    }
    auto state = m_state;
    for (auto& frame : frames) if (frame && frame->system == system) state->pending.push_back(std::move(frame));
    if (!CanAdvance() || (state->pending.empty() && !state->lastFrame)) return;
    if (state->pending.empty() && state->lastInput && SameInput(*state->lastInput,input)) return;
    state->lastInput = input;
    auto pending = std::exchange(state->pending,{});
    state->hasRender = true;
    state->render = context::EnqueueRenderCommand<struct VfxPreviewRender>([state,pending=std::move(pending),input=std::move(input)]
    {
        if (state->cancelled) return;
        try
        {
            std::string error;
            const auto backend = state->device->GetAdapter()->GetBackendType();
            const auto* shaders = state->program->FindPlatform(backend);
            if (!state->proxy)
            {
                state->proxy = std::make_unique<NLS::VFX::SystemRenderProxy>(state->program);
                rhi::RHIRenderTargetLayoutDesc targets; targets.colorFormats = {rhi::TextureFormat::RGBA8}; targets.hasDepth = true; targets.depthFormat = rhi::TextureFormat::Depth32F;
                for (size_t i = 0; i < state->program->emitters.size(); ++i)
                {
                    const auto& settings = state->program->asset.emitters[i]; const auto& shader = shaders->emitters[i];
                    auto renderer = std::make_unique<NLS::VFX::ParticleRenderer>();
                    if (!renderer->Initialize(state->device,settings.attributes,shader.spriteVertex,shader.spriteFragment,targets,error,
                        settings.sortByViewDepth ? &shader.sort : nullptr,&shader.visibility,&shader.simulation.bounds,settings.sprite,
                        settings.renderer == NLS::VFX::RendererType::Mesh ? &settings.mesh : nullptr)) throw std::runtime_error(error);
                    state->renderers.push_back(std::move(renderer));
                }
            }
            std::shared_ptr<const NLS::VFX::SystemRenderFrame> prepared;
            for (const auto& source : pending)
            {
                if (state->cancelled) return;
                // Fixed ticks can publish the same immutable source several
                // times while accumulating host time. Reuse its render cache.
                if (state->lastSource != source)
                {
                    state->lastSource = source;
                    state->lastFrame = Rebind(*source,state->program);
                }
                prepared = state->proxy->Prepare(state->device,state->lastFrame,error);
                if (!prepared) throw std::runtime_error(error);
            }
            if (!prepared && state->lastFrame) prepared = state->proxy->Prepare(state->device,state->lastFrame,error);
            if (!prepared) throw std::runtime_error(error);
            auto resources = std::make_shared<DrawResources>(); resources->device = state->device; resources->frame = prepared;
            resources->pool = state->device->CreateCommandPool(rhi::QueueType::Graphics,"VfxPreview");
            if (resources->pool) resources->commands = resources->pool->CreateCommandBuffer("VfxPreview");
            if (!resources->commands) throw std::runtime_error("Preview command allocation failed");
            rhi::RHITextureDesc desc; desc.extent = {input.width,input.height,1}; desc.format = rhi::TextureFormat::RGBA8;
            desc.usage = rhi::TextureUsageFlags::Sampled|rhi::TextureUsageFlags::ColorAttachment|rhi::TextureUsageFlags::CopySrc;
            auto color = state->device->CreateTexture(desc); rhi::RHITextureViewDesc view; view.format = desc.format;
            if (color) resources->color = state->device->CreateTextureView(color,view);
            desc.format = rhi::TextureFormat::Depth32F; desc.usage = rhi::TextureUsageFlags::DepthStencilAttachment|rhi::TextureUsageFlags::Sampled;
            auto depth = state->device->CreateTexture(desc); view.format = desc.format;
            if (depth) resources->depth = state->device->CreateTextureView(depth,view);
            if (!resources->color || !resources->depth) throw std::runtime_error("Preview target allocation failed");
            auto& commands = *resources->commands; commands.Begin();
            rhi::RHIRenderPassDesc pass; pass.renderArea = {0,0,input.width,input.height}; pass.debugName = "VfxPreview";
            rhi::RHIRenderPassColorAttachmentDesc attachment; attachment.view = resources->color;
            attachment.clearValue = {input.background[0],input.background[1],input.background[2],input.background[3]}; pass.colorAttachments = {attachment};
            rhi::RHIRenderPassDepthStencilAttachmentDesc depthAttachment; depthAttachment.view = resources->depth; depthAttachment.clearValue.depth = 1;
            pass.depthStencilAttachment = depthAttachment; commands.BeginRenderPass(pass); commands.EndRenderPass();
            pass.colorAttachments[0].loadOp = rhi::LoadOp::Load; pass.depthStencilAttachment->depthLoadOp = rhi::LoadOp::Load;
            if (input.showGrid && input.gridShaders)
            {
                rhi::RHIRenderTargetLayoutDesc targets; targets.colorFormats = {rhi::TextureFormat::RGBA8};
                targets.hasDepth = true; targets.depthFormat = rhi::TextureFormat::Depth32F;
                Maths::Matrix4 matrix; std::copy(input.camera.viewProjection.begin(),input.camera.viewProjection.end(),std::begin(matrix.data));
                resources->grid = state->gridRenderer.Prepare(state->device,input.gridShaders,targets,matrix,input.gridEye,input.gridColor,error);
                if (!resources->grid) throw std::runtime_error(error);
                Rendering::EditorGridRenderer::Record(commands,*resources->grid,pass);
            }
            std::vector<size_t> order; for (size_t i = 0; i < prepared->emitters.size(); ++i)
                if (input.isolatedEmitter.empty() || state->program->asset.emitters[i].id == input.isolatedEmitter) order.push_back(i);
            const auto solid = [&](size_t i) { const auto blend = state->program->asset.emitters[i].sprite.blend; return blend == NLS::VFX::SpriteBlendMode::Opaque || blend == NLS::VFX::SpriteBlendMode::Masked; };
            const auto depthKey = [&](size_t i)
            {
                const auto& data = prepared->emitters[i]; const auto bounds = data->orderingBounds ? data->orderingBounds : data->cpuBounds;
                if (!bounds || bounds->Empty()) return 0.0f;
                float result = 0;
                for (size_t axis = 0; axis < 3; ++axis) result += (bounds->minimum[axis]+bounds->maximum[axis])*0.5f*input.camera.cameraForward[axis];
                return result;
            };
            std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b) { return solid(a) != solid(b) ? solid(a) : !solid(a) && depthKey(a) > depthKey(b); });
            for (const auto i : order)
            {
                NLS::VFX::SpriteDrawContext drawContext;
                if (i < input.materials.size() && !input.materials[i].empty()) { drawContext.material = input.materials[i][0]; drawContext.meshMaterials = input.materials[i]; }
                if (i < input.meshes.size()) drawContext.meshes = input.meshes[i];
                pass.depthStencilAttachment->readOnlyDepthStencil = !solid(i);
                if (!solid(i)) drawContext.sceneDepth = resources->depth;
                drawContext.depthProjection = {input.camera.viewProjection[10],input.camera.viewProjection[11],input.camera.viewProjection[14],input.camera.viewProjection[15]};
                auto camera = input.camera; camera.frustumCulling = state->program->asset.emitters[i].frustumCulling;
                auto draw = state->renderers[i]->Record(commands,*prepared->emitters[i],camera,pass,error,drawContext);
                if (!draw) throw std::runtime_error(error);
                resources->draws.push_back(std::move(draw));
            }
            if (input.showOriginAxis && input.axesShaders)
            {
                rhi::RHIRenderTargetLayoutDesc targets; targets.colorFormats = {rhi::TextureFormat::RGBA8};
                targets.hasDepth = true; targets.depthFormat = rhi::TextureFormat::Depth32F;
                Maths::Matrix4 matrix; std::copy(input.camera.viewProjection.begin(),input.camera.viewProjection.end(),std::begin(matrix.data));
                resources->axes = state->gridRenderer.PrepareAxes(state->device,input.axesShaders,targets,matrix,input.gridEye,error);
                if (!resources->axes) throw std::runtime_error(error);
                pass.depthStencilAttachment->readOnlyDepthStencil = true;
                for (const auto& draw : *resources->axes) Rendering::EditorGridRenderer::Record(commands,draw,pass);
            }
            rhi::RHIBarrierDesc barrier;
            barrier.textureBarriers = {{color,color->GetState(),rhi::ResourceState::ShaderRead,{},rhi::PipelineStageMask::RenderTarget,rhi::PipelineStageMask::FragmentShader,rhi::AccessMask::ColorAttachmentWrite,rhi::AccessMask::ShaderRead}};
            commands.Barrier(barrier); commands.End(); resources->fence = state->device->CreateFence("VfxPreviewComplete");
            if (!resources->fence) throw std::runtime_error("Preview fence allocation failed");
            resources->fence->Reset(); rhi::RHISubmitDesc submit; submit.commandBuffers = {resources->commands}; submit.signalFence = resources->fence;
            const auto accepted = context::RhiCommandListExecutor::Get().RunBackendOperation("VfxPreviewSubmit",[&] { return state->device->GetQueue(rhi::QueueType::Graphics)->SubmitChecked(submit); });
            if (accepted.Succeeded() || accepted.mayHaveQueuedGpuWork) context::RhiCommandListExecutor::Get().DeferRelease({resources});
            if (!accepted.Succeeded()) throw std::runtime_error(accepted.message);
            std::lock_guard lock(state->mutex); state->output = resources->color; state->fence = resources->fence;
        }
        catch (const std::exception& exception) { std::lock_guard lock(state->mutex); state->error = exception.what(); }
    });
}
std::shared_ptr<rhi::RHITextureView> VfxSystemPreview::View()
{
    if (m_state)
    {
        std::lock_guard lock(m_state->mutex);
        if (m_state->output && m_state->fence && m_state->fence->IsSignaled() && m_display != m_state->output)
        { Retire(m_display); m_display = m_state->output; }
    }
    return m_display;
}
std::string VfxSystemPreview::Status() const
{
    if (!m_state) return "Preview requires a rendering device";
    std::lock_guard lock(m_state->mutex);
    if (!m_state->error.empty()) return m_state->error;
    if (!m_state->compilation.IsComplete()) return "Compiling shaders...";
    if (!m_state->compilation.Succeeded()) return m_state->compilation.GetDiagnostic();
    return {};
}
}
