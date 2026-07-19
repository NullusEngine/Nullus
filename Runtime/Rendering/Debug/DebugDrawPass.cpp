#include "Rendering/Debug/DebugDrawPass.h"

#include <algorithm>

#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ServiceLocator.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Debug/DebugDrawService.h"
#include "Rendering/Geometry/BoundingSphere.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Utils/Conversions.h"

namespace NLS::Render::Debug
{
namespace
{
    std::vector<Geometry::Vertex> CreatePlaceholderVertices(const size_t count)
    {
        std::vector<Geometry::Vertex> vertices;
        vertices.reserve(count);

        for (size_t index = 0u; index < count; ++index)
        {
            vertices.push_back({
                0, 0, 0,
                0, 0,
                0, 0, 0,
                0, 0, 0,
                0, 0, 0
            });
        }

        return vertices;
    }

    Geometry::Vertex CreateLineVertex(const Maths::Vector3& position)
    {
        return {
            position.x, position.y, position.z,
            0.0f, 0.0f,
            0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f
        };
    }

    bool MatchesLineBatchStyle(const DebugDrawStyle& left, const DebugDrawStyle& right)
    {
        return left.color == right.color &&
            left.depthMode == right.depthMode &&
            left.lineWidth == right.lineWidth;
    }

    Geometry::BoundingSphere ComputeLineBatchBoundingSphere(const std::vector<Geometry::Vertex>& vertices)
    {
        if (vertices.empty())
            return {};

        Maths::Vector3 center = Maths::Vector3::Zero;
        for (const auto& vertex : vertices)
        {
            center.x += vertex.position[0];
            center.y += vertex.position[1];
            center.z += vertex.position[2];
        }

        const auto invVertexCount = 1.0f / static_cast<float>(vertices.size());
        center *= invVertexCount;

        float radius = 0.0f;
        for (const auto& vertex : vertices)
        {
            const Maths::Vector3 position{ vertex.position[0], vertex.position[1], vertex.position[2] };
            radius = std::max(radius, Maths::Vector3::Distance(center, position));
        }

        return { center, radius };
    }

    bool HaveSameLineVertexPositions(
        const std::vector<Geometry::Vertex>& left,
        const std::vector<Geometry::Vertex>& right)
    {
        if (left.size() != right.size())
            return false;

        for (size_t index = 0u; index < left.size(); ++index)
        {
            for (size_t component = 0u; component < 3u; ++component)
            {
                if (left[index].position[component] != right[index].position[component])
                    return false;
            }
        }
        return true;
    }

    void ConfigurePrimitiveMaterial(Resources::Material& material, Resources::Shader& shader)
    {
        material.SetShader(&shader);
        material.SetBlendable(false);
        material.SetDepthTest(true);
        material.SetBackfaceCulling(false);
        material.SetFrontfaceCulling(false);
    }
}

DebugDrawPass::DebugDrawPass(Core::CompositeRenderer& renderer)
    : ARenderPass(renderer)
{
    m_pointMesh = new Resources::Mesh(CreatePlaceholderVertices(1u), { 0 }, 0);
    m_lineMesh = new Resources::Mesh(CreatePlaceholderVertices(2u), { 0, 1 }, 0);
    m_triangleMesh = new Resources::Mesh(CreatePlaceholderVertices(3u), { 0, 1, 2 }, 0);

    if (::NLS::Core::ServiceLocator::Contains<::NLS::Core::ResourceManagement::ShaderManager>())
    {
        m_primitiveShader = ::NLS::Core::ServiceLocator::Get<::NLS::Core::ResourceManagement::ShaderManager>()[":Shaders/DebugPrimitive.hlsl"];
    }
    else
    {
        m_primitiveShader = Resources::Loaders::ShaderLoader::CreateBuiltInHlsl(
            "App/Assets/Engine/Shaders/DebugPrimitive.hlsl",
            ::NLS::Core::ResourceManagement::ShaderManager::ProjectAssetsRoot());
        m_ownsPrimitiveShader = m_primitiveShader != nullptr;
    }

    if (m_primitiveShader != nullptr)
    {
        m_primitiveMaterial = std::make_unique<Resources::Material>();
        ConfigurePrimitiveMaterial(*m_primitiveMaterial, *m_primitiveShader);
    }
}

DebugDrawPass::~DebugDrawPass()
{
    delete m_pointMesh;
    delete m_lineMesh;
    delete m_triangleMesh;

    if (m_ownsPrimitiveShader)
        Resources::Loaders::ShaderLoader::Destroy(m_primitiveShader);
}

void DebugDrawPass::Draw(PipelineState pso)
{
    if (!m_renderer.HasDebugDrawService())
        return;

    auto* debugDrawService = m_renderer.GetDebugDrawService();
    const auto contentRevision = debugDrawService->GetContentRevision();
    const bool canReuseCollectedCommands =
        m_collectedPrimitiveService == debugDrawService &&
        m_collectedContentRevision == contentRevision;
    if (!canReuseCollectedCommands)
    {
        ++m_commandBuildCount;
        m_lineBatchVertices.clear();
        m_drawCommands.clear();

        LineBatch lineBatch;
        auto flushLineBatch = [&]()
        {
            if (!lineBatch.segments.empty())
                RenderLineBatch(lineBatch, pso);
            lineBatch.segments.clear();
        };

        debugDrawService->CollectVisiblePrimitives(m_visiblePrimitives);
        for (const auto& primitiveRef : m_visiblePrimitives)
        {
            const auto& primitive = primitiveRef.get();
            if (primitive.type != DebugDrawPrimitiveType::Line)
            {
                flushLineBatch();
                RenderPrimitive(primitive, pso);
                continue;
            }

            const auto& style = primitive.options.style;
            if (!lineBatch.segments.empty() && !MatchesLineBatchStyle(lineBatch.style, style))
                flushLineBatch();

            if (lineBatch.segments.empty())
                lineBatch.style = style;

            lineBatch.segments.push_back({ primitive.points[0], primitive.points[1] });
        }

        flushLineBatch();
        m_collectedPrimitiveService = debugDrawService;
        m_collectedContentRevision = contentRevision;
    }

    RenderCollectedCommands(pso);
}

void DebugDrawPass::RenderPrimitive(const DebugDrawPrimitive& primitive, PipelineState pso)
{
    (void)pso;
    m_drawCommands.push_back({ DrawCommandType::Primitive, primitive, {} });
}

void DebugDrawPass::RenderLineBatch(const LineBatch& batch, PipelineState pso)
{
    if (!m_primitiveMaterial || batch.segments.empty())
        return;

    (void)pso;
    const auto vertexStart = static_cast<uint32_t>(m_lineBatchVertices.size());

    for (const auto& segment : batch.segments)
    {
        m_lineBatchVertices.push_back(CreateLineVertex(segment.start));
        m_lineBatchVertices.push_back(CreateLineVertex(segment.end));
    }

    DrawCommand command;
    command.type = DrawCommandType::Line;
    command.line = {
        batch.style,
        vertexStart,
        static_cast<uint32_t>(batch.segments.size() * 2u)
    };
    m_drawCommands.push_back(command);
}

void DebugDrawPass::RenderCollectedCommands(PipelineState pso)
{
    if (!m_primitiveMaterial || m_drawCommands.empty())
        return;

    auto* debugDrawService = m_renderer.GetDebugDrawService();
    Resources::Mesh* lineMesh = nullptr;
    if (!m_lineBatchVertices.empty())
    {
        const auto contentRevision = debugDrawService->GetContentRevision();
        const bool canReuseUploadedMesh =
            m_uploadedLineService == debugDrawService &&
            (m_uploadedLineContentRevision == contentRevision ||
                HaveSameLineVertexPositions(m_uploadedLineVertices, m_lineBatchVertices)) &&
            m_uploadedLineMesh != nullptr;
        if (canReuseUploadedMesh)
        {
            lineMesh = m_uploadedLineMesh;
        }
        else
        {
            lineMesh = UploadLineVertices(ComputeLineBatchBoundingSphere(m_lineBatchVertices));
            m_uploadedLineService = debugDrawService;
            m_uploadedLineMesh = lineMesh;
            m_uploadedLineVertices = m_lineBatchVertices;
        }
        m_uploadedLineContentRevision = contentRevision;
    }
    else
    {
        m_uploadedLineService = debugDrawService;
        m_uploadedLineMesh = nullptr;
        m_uploadedLineVertices.clear();
        m_uploadedLineContentRevision = debugDrawService->GetContentRevision();
    }

    size_t lineCommandIndex = 0u;
    for (const auto& command : m_drawCommands)
    {
        if (command.type == DrawCommandType::Primitive)
        {
            RenderPrimitiveNow(command.primitive, pso);
            continue;
        }

        if (lineMesh != nullptr)
        {
            if (auto* material = ResolveLineMaterial(lineCommandIndex, command.line.style); material != nullptr)
                RenderLineCommand(command.line, *lineMesh, *material, pso);
        }
        ++lineCommandIndex;
    }
}

void DebugDrawPass::RenderPrimitiveNow(const DebugDrawPrimitive& primitive, PipelineState pso)
{
    switch (primitive.type)
    {
    case DebugDrawPrimitiveType::Point:
        RenderPoint(primitive, pso);
        break;

    case DebugDrawPrimitiveType::Line:
        RenderLine(primitive, pso);
        break;

    case DebugDrawPrimitiveType::Triangle:
        RenderTriangle(primitive, pso);
        break;
    }
}

Resources::Material* DebugDrawPass::ResolveLineMaterial(
    const size_t lineCommandIndex,
    const DebugDrawStyle& style)
{
    if (m_primitiveShader == nullptr)
        return nullptr;

    if (m_cachedLineMaterials.size() <= lineCommandIndex)
        m_cachedLineMaterials.resize(lineCommandIndex + 1u);

    auto& cached = m_cachedLineMaterials[lineCommandIndex];
    if (cached.material == nullptr)
    {
        cached.material = std::make_unique<Resources::Material>();
        ConfigurePrimitiveMaterial(*cached.material, *m_primitiveShader);
        cached.material->Set("u_Point0", Maths::Vector3::Zero);
        cached.material->Set("u_Point1", Maths::Vector3::Zero);
        cached.material->Set("u_Point2", Maths::Vector3::Zero);
        cached.material->Set("u_UseVertexPosition", 1);
    }

    if (!cached.hasStyle || !MatchesLineBatchStyle(cached.style, style))
    {
        cached.material->Set("u_Color", style.color);
        cached.style = style;
        cached.hasStyle = true;
    }

    return cached.material.get();
}

void DebugDrawPass::RenderLineCommand(
    const LineDrawCommand& command,
    Resources::Mesh& mesh,
    Resources::Material& material,
    PipelineState pso)
{

    auto effectivePso = pso;
    effectivePso.depthWriting = false;
    effectivePso.rasterizationMode = Settings::ERasterizationMode::LINE;

    effectivePso.depthTest = command.style.depthMode == DebugDrawDepthMode::DepthTest;
    effectivePso.lineWidthPow2 = Utils::Conversions::FloatToPow2(command.style.lineWidth);

    Entities::Drawable drawable;
    drawable.material = &material;
    drawable.mesh = &mesh;
    drawable.stateMask = material.GenerateStateMask();
    drawable.primitiveMode = Settings::EPrimitiveMode::LINES;
    drawable.vertexStart = command.vertexStart;
    drawable.vertexCount = command.vertexCount;

    m_renderer.DrawEntity(effectivePso, drawable);
}

Resources::Mesh* DebugDrawPass::UploadLineVertices(const Geometry::BoundingSphere& boundingSphere)
{
    ++m_lineMeshUploadCount;
    const auto slotCount = std::max(
        kMinLineMeshSlotCount,
        Render::Context::DriverRendererAccess::GetFrameContextSlotCount(m_renderer.GetDriver()) + 1u);
    if (m_lineMeshSlots.size() < slotCount)
        m_lineMeshSlots.resize(slotCount);

    auto& slot = m_lineMeshSlots[m_nextLineMeshSlot];
    m_nextLineMeshSlot = (m_nextLineMeshSlot + 1u) % m_lineMeshSlots.size();

    const auto vertexCount = static_cast<uint32_t>(m_lineBatchVertices.size());
    if (slot.mesh == nullptr || slot.capacity < vertexCount)
    {
        slot.mesh = std::make_unique<Resources::Mesh>(
            m_lineBatchVertices,
            std::vector<uint32_t>{},
            0u,
            Resources::MeshBufferUploadMode::CpuToGpu,
            boundingSphere);
        slot.capacity = vertexCount;
        return slot.mesh.get();
    }

    if (!slot.mesh->UpdateVertices(m_lineBatchVertices, boundingSphere))
    {
        auto reloadVertices = m_lineBatchVertices;
        reloadVertices.resize(slot.capacity);
        slot.mesh->Reload(
            reloadVertices,
            std::vector<uint32_t>{},
            0u,
            Resources::MeshBufferUploadMode::CpuToGpu,
            boundingSphere);
    }
    return slot.mesh.get();
}

#if defined(NLS_ENABLE_TEST_HOOKS)
const Resources::Mesh* DebugDrawPassTestAccess::GetLineMeshSlotMesh(const DebugDrawPass& pass, const size_t slotIndex)
{
    if (slotIndex >= pass.m_lineMeshSlots.size())
        return nullptr;

    return pass.m_lineMeshSlots[slotIndex].mesh.get();
}

uint32_t DebugDrawPassTestAccess::GetLineMeshSlotCapacity(const DebugDrawPass& pass, const size_t slotIndex)
{
    if (slotIndex >= pass.m_lineMeshSlots.size())
        return 0u;

    return pass.m_lineMeshSlots[slotIndex].capacity;
}

uint64_t DebugDrawPassTestAccess::GetCommandBuildCount(const DebugDrawPass& pass)
{
    return pass.m_commandBuildCount;
}

uint64_t DebugDrawPassTestAccess::GetLineMeshUploadCount(const DebugDrawPass& pass)
{
    return pass.m_lineMeshUploadCount;
}

size_t DebugDrawPassTestAccess::GetCachedLineMaterialCount(const DebugDrawPass& pass)
{
    return pass.m_cachedLineMaterials.size();
}

uint64_t DebugDrawPassTestAccess::GetCachedLineMaterialParameterRevision(
    const DebugDrawPass& pass,
    const size_t index)
{
    if (index >= pass.m_cachedLineMaterials.size() ||
        pass.m_cachedLineMaterials[index].material == nullptr)
    {
        return 0u;
    }

    return pass.m_cachedLineMaterials[index].material->GetParameterRevision();
}
#endif

void DebugDrawPass::RenderPoint(const DebugDrawPrimitive& primitive, PipelineState pso)
{
    if (m_pointMesh != nullptr)
        RenderWithMesh(primitive, *m_pointMesh, Settings::EPrimitiveMode::POINTS, pso);
}

void DebugDrawPass::RenderLine(const DebugDrawPrimitive& primitive, PipelineState pso)
{
    if (m_lineMesh != nullptr)
        RenderWithMesh(primitive, *m_lineMesh, Settings::EPrimitiveMode::LINES, pso);
}

void DebugDrawPass::RenderTriangle(const DebugDrawPrimitive& primitive, PipelineState pso)
{
    if (m_triangleMesh != nullptr)
        RenderWithMesh(primitive, *m_triangleMesh, Settings::EPrimitiveMode::TRIANGLES, pso);
}

void DebugDrawPass::RenderWithMesh(
    const DebugDrawPrimitive& primitive,
    Resources::Mesh& mesh,
    const Settings::EPrimitiveMode primitiveMode,
    PipelineState pso)
{
    if (!m_primitiveMaterial)
        return;

    m_primitiveMaterial->Set("u_Point0", primitive.points[0]);
    m_primitiveMaterial->Set("u_Point1", primitive.points[1]);
    m_primitiveMaterial->Set("u_Point2", primitive.points[2]);
    m_primitiveMaterial->Set("u_Color", primitive.options.style.color);
    m_primitiveMaterial->Set("u_UseVertexPosition", 0);

    auto effectivePso = pso;
    effectivePso.depthWriting = false;
    effectivePso.depthTest = primitive.options.style.depthMode == DebugDrawDepthMode::DepthTest;

    switch (primitive.type)
    {
    case DebugDrawPrimitiveType::Point:
        effectivePso.rasterizationMode = Settings::ERasterizationMode::POINT;
        break;

    case DebugDrawPrimitiveType::Line:
        effectivePso.rasterizationMode = Settings::ERasterizationMode::LINE;
        effectivePso.lineWidthPow2 = Utils::Conversions::FloatToPow2(primitive.options.style.lineWidth);
        break;

    case DebugDrawPrimitiveType::Triangle:
        effectivePso.rasterizationMode =
            primitive.options.style.fillMode == DebugDrawFillMode::Wireframe
                ? Settings::ERasterizationMode::LINE
                : Settings::ERasterizationMode::FILL;
        effectivePso.lineWidthPow2 = Utils::Conversions::FloatToPow2(primitive.options.style.lineWidth);
        break;
    }

    Entities::Drawable drawable;
    drawable.material = m_primitiveMaterial.get();
    drawable.mesh = &mesh;
    drawable.stateMask = m_primitiveMaterial->GenerateStateMask();
    drawable.primitiveMode = primitiveMode;

    m_renderer.DrawEntity(effectivePso, drawable);
}
}
