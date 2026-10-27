#include "EditorGridRenderer.h"
#include <Rendering/Resources/Shader.h>
#include <Rendering/RHI/BindingPointMap.h>
#include <Rendering/RHI/Core/RHIMesh.h>
#include <Rendering/Geometry/Vertex.h>
#include <algorithm>
#include <stdexcept>

namespace NLS::Editor::Rendering
{
namespace rhi = Render::RHI;
namespace
{
class GridMesh final : public rhi::RHIMesh
{
public:
    bool axes = false;
    std::shared_ptr<rhi::RHIBuffer> vertices,indices;
    std::shared_ptr<rhi::RHIBuffer> GetVertexBuffer() const override { return vertices; }
    std::shared_ptr<rhi::RHIBuffer> GetIndexBuffer() const override { return indices; }
    uint32_t GetVertexCount() const override { return axes ? 2 : 4; }
    uint32_t GetIndexCount() const override { return axes ? 2 : 6; }
    uint32_t GetVertexStride() const override { return sizeof(Render::Geometry::Vertex); }
    rhi::IndexType GetIndexType() const override { return rhi::IndexType::UInt32; }
    Render::Settings::EPrimitiveMode GetPrimitiveMode() const override { return axes ? Render::Settings::EPrimitiveMode::LINES : Render::Settings::EPrimitiveMode::TRIANGLES; }
};
std::array<float,16> ShaderMatrix(const Maths::Matrix4& value)
{
    std::array<float,16> result{};
    for (size_t row = 0; row < 4; ++row) for (size_t column = 0; column < 4; ++column)
        result[column*4+row] = value.data[row*4+column];
    return result;
}
std::shared_ptr<rhi::RHIBuffer> UploadBytes(const std::shared_ptr<rhi::RHIDevice>& device,
    const void* data,size_t size,rhi::BufferUsageFlags usage)
{
    rhi::RHIBufferDesc desc; desc.size = size; desc.usage = usage; desc.memoryUsage = rhi::MemoryUsage::CPUToGPU;
    desc.debugName = "EditorGrid";
    rhi::RHIBufferUploadDesc upload; upload.data = data; upload.dataSize = size;
    return device->CreateBuffer(desc,upload);
}
template<class T> std::shared_ptr<rhi::RHIBuffer> Upload(const std::shared_ptr<rhi::RHIDevice>& device,
    const T& value,rhi::BufferUsageFlags usage)
{ return UploadBytes(device,&value,sizeof(value),usage); }
}
std::array<Render::Debug::DebugDrawPrimitive,3> BuildEditorOriginAxes(const Maths::Vector3& eye,float extent)
{
    std::array<Render::Debug::DebugDrawPrimitive,3> axes{};
    for (size_t axis = 0; axis < axes.size(); ++axis)
    {
        auto& line = axes[axis];
        line.type = Render::Debug::DebugDrawPrimitiveType::Line;
        line.options.category = Render::Debug::DebugDrawCategory::Grid;
        line.options.lifetime = Render::Debug::DebugDrawLifetime::Persistent();
        line.options.style.lineWidth = 1.0f;
        line.options.style.depthMode = Render::Debug::DebugDrawDepthMode::DepthTest;
        line.options.style.color = Maths::Vector3::Zero;
        line.options.style.color[axis] = 1;
        line.points[0][axis] = eye[axis]-extent;
        line.points[1][axis] = eye[axis]+extent;
    }
    return axes;
}
std::shared_ptr<const EditorGridShaders> CaptureEditorGridShaders(const Render::Resources::Shader& shader,rhi::NativeBackendType backend)
{
    namespace sc = Render::ShaderCompiler;
    const auto target = backend == rhi::NativeBackendType::DX12 ? sc::ShaderTargetPlatform::DXIL : sc::ShaderTargetPlatform::SPIRV;
    auto result = std::make_shared<EditorGridShaders>(); result->generation = shader.GetGeneration();
    for (const auto& artifact : shader.GetCompiledArtifacts())
    {
        if (artifact.targetPlatform != target || artifact.keywordHash != 0 || artifact.output.status != sc::ShaderCompilationStatus::Succeeded) continue;
        auto* module = artifact.stage == sc::ShaderStage::Vertex ? &result->vertex : artifact.stage == sc::ShaderStage::Pixel ? &result->fragment : nullptr;
        if (!module) continue;
        module->stage = artifact.stage == sc::ShaderStage::Vertex ? rhi::ShaderStage::Vertex : rhi::ShaderStage::Fragment;
        module->targetBackend = backend; module->entryPoint = artifact.entryPoint; module->bytecode = artifact.output.bytecode;
    }
    return result->vertex.bytecode.empty() || result->fragment.bytecode.empty() ? nullptr : result;
}
struct EditorGridRenderer::State
{
    std::shared_ptr<rhi::RHIDevice> device;
    std::shared_ptr<const EditorGridShaders> shaders;
    rhi::RHIRenderTargetLayoutDesc targets;
    std::shared_ptr<rhi::RHIGraphicsPipeline> pipeline;
    std::array<std::shared_ptr<rhi::RHIBindingLayout>,3> layouts;
    std::shared_ptr<GridMesh> mesh;
};
EditorGridRenderer::EditorGridRenderer() = default;
EditorGridRenderer::~EditorGridRenderer() = default;
std::optional<Render::Context::RecordedDrawCommandInput> EditorGridRenderer::Prepare(
    std::shared_ptr<rhi::RHIDevice> device,const std::shared_ptr<const EditorGridShaders>& shaders,
    const rhi::RHIRenderTargetLayoutDesc& targets,const Maths::Matrix4& viewProjection,
    const Maths::Vector3& eye,const Maths::Vector3& color,std::string& error)
{
    const std::array<float,4> tint{color.x,color.y,color.z,0};
    return PrepareHelper(m_state,false,std::move(device),shaders,targets,viewProjection,eye,std::as_bytes(std::span(&tint,1)),error);
}
std::optional<std::array<Render::Context::RecordedDrawCommandInput,3>> EditorGridRenderer::PrepareAxes(
    std::shared_ptr<rhi::RHIDevice> device,const std::shared_ptr<const EditorGridShaders>& shaders,
    const rhi::RHIRenderTargetLayoutDesc& targets,const Maths::Matrix4& viewProjection,const Maths::Vector3& eye,std::string& error)
{
    std::array<Render::Context::RecordedDrawCommandInput,3> draws;
    const auto axes = BuildEditorOriginAxes(eye);
    for (size_t index = 0; index < axes.size(); ++index)
    {
        const auto& axis = axes[index];
        const std::array<std::array<float,4>,4> material{{
            {axis.points[0].x,axis.points[0].y,axis.points[0].z,0},
            {axis.points[1].x,axis.points[1].y,axis.points[1].z,0},{0,0,0,0},
            {axis.options.style.color.x,axis.options.style.color.y,axis.options.style.color.z,0}}};
        auto draw = PrepareHelper(m_axesState,true,device,shaders,targets,viewProjection,eye,std::as_bytes(std::span(&material,1)),error);
        if (!draw) return std::nullopt;
        draws[index] = std::move(*draw);
    }
    return draws;
}
std::optional<Render::Context::RecordedDrawCommandInput> EditorGridRenderer::PrepareHelper(
    std::unique_ptr<State>& cache,bool axes,std::shared_ptr<rhi::RHIDevice> device,
    const std::shared_ptr<const EditorGridShaders>& shaders,const rhi::RHIRenderTargetLayoutDesc& targets,
    const Maths::Matrix4& viewProjection,const Maths::Vector3& eye,std::span<const std::byte> material,std::string& error)
{
    if (!device || !shaders || targets.colorFormats.size() != 1) { error = "Editor grid shader or render target is unavailable"; return std::nullopt; }
    const auto sameTargets = [](const auto& a,const auto& b)
    { return a.colorFormats == b.colorFormats && a.colorSpaces == b.colorSpaces && a.depthFormat == b.depthFormat && a.hasDepth == b.hasDepth && a.sampleCount == b.sampleCount; };
    if (!cache || cache->device != device || cache->shaders != shaders || !sameTargets(cache->targets,targets))
    {
        auto state = std::make_unique<State>(); state->device = device; state->shaders = shaders; state->targets = targets;
        rhi::RHIPipelineLayoutDesc pipelineLayout; pipelineLayout.bindingLayouts.resize(3);
        constexpr std::array<uint32_t,3> spaces{0,2,3};
        for (size_t index = 0; index < spaces.size(); ++index)
        {
            rhi::RHIBindingLayoutDesc layout;
            layout.entries.push_back({"EditorGridConstants",rhi::BindingType::UniformBuffer,static_cast<uint32_t>(index),0,1,rhi::ShaderStageMask::AllGraphics,spaces[index],0});
            state->layouts[index] = device->CreateBindingLayout(layout); pipelineLayout.bindingLayouts[index] = state->layouts[index];
            if (!state->layouts[index]) { error = "Editor grid binding layout creation failed"; return std::nullopt; }
        }
        rhi::RHIGraphicsPipelineDesc pipeline; pipeline.pipelineLayout = device->CreatePipelineLayout(pipelineLayout);
        pipeline.vertexShader = device->CreateShaderModule(shaders->vertex); pipeline.fragmentShader = device->CreateShaderModule(shaders->fragment);
        pipeline.renderTargetLayout = targets; pipeline.rasterState.cullEnabled = false;
        pipeline.depthStencilState.depthTest = targets.hasDepth; pipeline.depthStencilState.depthWrite = false;
        pipeline.blendState.enabled = !axes; rhi::RHIRenderTargetBlendStateDesc blend; blend.blendEnable = !axes;
        pipeline.blendState.renderTargets = {blend}; pipeline.debugName = axes ? "EditorOriginAxes" : "EditorGrid";
        pipeline.primitiveTopology = axes ? rhi::PrimitiveTopology::LineList : rhi::PrimitiveTopology::TriangleList;
        using Vertex = Render::Geometry::Vertex;
        pipeline.vertexBuffers = {{0,sizeof(Vertex),false}};
        pipeline.vertexAttributes = {
            {0,0,offsetof(Vertex,position),12,rhi::RHIVertexSemantic::Position,0},
            {1,0,offsetof(Vertex,texCoords),8,rhi::RHIVertexSemantic::TexCoord,0},
            {2,0,offsetof(Vertex,normals),12,rhi::RHIVertexSemantic::Normal,0},
            {3,0,offsetof(Vertex,tangent),12,rhi::RHIVertexSemantic::TexCoord,1},
            {4,0,offsetof(Vertex,bitangent),12,rhi::RHIVertexSemantic::TexCoord,2}};
        state->pipeline = device->CreateGraphicsPipeline(pipeline);
        constexpr std::array<float,12> positions{-1,0,-1,1,0,-1,1,0,1,-1,0,1};
        std::array<Vertex,4> vertices{};
        for (size_t index = 0; index < vertices.size(); ++index) std::copy_n(positions.data()+3*index,3,vertices[index].position);
        constexpr std::array<uint32_t,6> indices{0,1,2,0,2,3};
        state->mesh = std::make_shared<GridMesh>();
        state->mesh->axes = axes;
        state->mesh->vertices = Upload(device,vertices,rhi::BufferUsageFlags::Vertex);
        state->mesh->indices = Upload(device,indices,rhi::BufferUsageFlags::Index);
        if (!state->pipeline || !state->mesh->vertices || !state->mesh->indices) { error = "Editor grid pipeline creation failed"; return std::nullopt; }
        cache = std::move(state);
    }
    struct Frame { std::array<float,16> viewProjection; std::array<float,4> eye; std::array<float,16> noTranslation{}; };
    const Frame frame{ShaderMatrix(viewProjection),{eye.x,eye.y,eye.z,0},{}};
    const auto model = ShaderMatrix(Maths::Matrix4::Translation({eye.x,0,eye.z})*Maths::Matrix4::Scaling({5000,1,5000}));
    struct Owner { std::array<std::shared_ptr<rhi::RHIBuffer>,3> buffers; std::shared_ptr<rhi::RHIDevice> device; };
    auto owner = std::make_shared<Owner>(); owner->device = device;
    owner->buffers = {Upload(device,frame,rhi::BufferUsageFlags::Uniform),UploadBytes(device,material.data(),material.size(),rhi::BufferUsageFlags::Uniform),Upload(device,model,rhi::BufferUsageFlags::Uniform)};
    std::array<std::shared_ptr<rhi::RHIBindingSet>,3> sets;
    for (size_t index = 0; index < sets.size(); ++index)
    {
        if (!owner->buffers[index]) { error = "Editor grid constants allocation failed"; return std::nullopt; }
        rhi::RHIBindingSetDesc desc; desc.layout = cache->layouts[index];
        rhi::RHIBindingSetEntry entry; entry.type = rhi::BindingType::UniformBuffer; entry.buffer = owner->buffers[index];
        entry.bufferRange = entry.buffer->GetDesc().size; desc.entries = {entry}; sets[index] = device->CreateBindingSet(desc);
        if (!sets[index]) { error = "Editor grid constants binding failed"; return std::nullopt; }
    }
    Render::Context::RecordedDrawCommandInput draw; draw.pipeline = cache->pipeline; draw.mesh = cache->mesh;
    draw.frameBindingSet = sets[0]; draw.materialBindingSet = sets[1]; draw.objectBindingSet = sets[2];
    draw.instanceCount = 1; draw.vertexCount = cache->mesh->GetIndexCount(); draw.objectBindingImmutable = true; draw.resourceOwner = owner;
    error.clear(); return draw;
}
void EditorGridRenderer::Record(rhi::RHICommandBuffer& commands,const Render::Context::RecordedDrawCommandInput& draw,const rhi::RHIRenderPassDesc& pass)
{
    // These targets can already contain scene/preview pixels. Use the same
    // explicit attachment transitions as the particle renderer to preserve them.
    rhi::RHIBarrierDesc barriers;
    const auto transition = [&](const std::shared_ptr<rhi::RHITextureView>& view,rhi::ResourceState state,rhi::PipelineStageMask stages,rhi::AccessMask access)
    {
        barriers.textureBarriers.push_back({view->GetTexture(),view->GetTexture()->GetState(),state,view->GetDesc().subresourceRange,
            rhi::PipelineStageMask::AllCommands,stages,rhi::AccessMask::MemoryRead|rhi::AccessMask::MemoryWrite,access});
    };
    for (const auto& color : pass.colorAttachments)
        transition(color.view,rhi::ResourceState::RenderTarget,rhi::PipelineStageMask::RenderTarget,rhi::AccessMask::ColorAttachmentRead|rhi::AccessMask::ColorAttachmentWrite);
    if (pass.depthStencilAttachment)
        transition(pass.depthStencilAttachment->view,pass.depthStencilAttachment->readOnlyDepthStencil ? rhi::ResourceState::DepthRead : rhi::ResourceState::DepthWrite,
            rhi::PipelineStageMask::DepthStencil,pass.depthStencilAttachment->readOnlyDepthStencil ? rhi::AccessMask::DepthStencilRead : rhi::AccessMask::DepthStencilRead|rhi::AccessMask::DepthStencilWrite);
    const auto transitioned = commands.BarrierChecked(barriers);
    if (!transitioned.Succeeded()) throw std::runtime_error(transitioned.message);
    auto external = pass; external.attachmentsRequireExternalStateTransitions = true;
    commands.BeginRenderPass(external); commands.SetViewport({0,0,static_cast<float>(pass.renderArea.width),static_cast<float>(pass.renderArea.height),0,1});
    commands.SetScissor(pass.renderArea); commands.BindGraphicsPipeline(draw.pipeline);
    commands.BindBindingSet(rhi::BindingPointMap::kFrameDescriptorSet,draw.frameBindingSet);
    commands.BindBindingSet(rhi::BindingPointMap::kMaterialDescriptorSet,draw.materialBindingSet);
    commands.BindBindingSet(rhi::BindingPointMap::kObjectDescriptorSet,draw.objectBindingSet);
    commands.BindVertexBuffer(0,{draw.mesh->GetVertexBuffer(),0,draw.mesh->GetVertexStride()});
    commands.BindIndexBuffer({draw.mesh->GetIndexBuffer(),0,draw.mesh->GetIndexType()});
    commands.DrawIndexed(draw.vertexCount,draw.instanceCount,draw.vertexStart,0,0); commands.EndRenderPass();
}
}
