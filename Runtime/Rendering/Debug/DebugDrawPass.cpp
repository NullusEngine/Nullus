#include "Rendering/Debug/DebugDrawPass.h"

#include <algorithm>

#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ServiceLocator.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Core/CompositeRenderer.h"
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
        m_primitiveMaterial->SetShader(m_primitiveShader);
        m_primitiveMaterial->SetBlendable(false);
        m_primitiveMaterial->SetDepthTest(true);
        m_primitiveMaterial->SetBackfaceCulling(false);
        m_primitiveMaterial->SetFrontfaceCulling(false);
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

    m_lineBatchVertices.clear();
    m_drawCommands.clear();

    LineBatch lineBatch;
    auto flushLineBatch = [&]()
    {
        if (!lineBatch.segments.empty())
            RenderLineBatch(lineBatch, pso);
        lineBatch.segments.clear();
    };

    const auto visiblePrimitives = m_renderer.GetDebugDrawService()->CollectVisiblePrimitives();
    for (const auto& primitiveRef : visiblePrimitives)
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
        {
            lineBatch.style = style;
        }

        lineBatch.segments.push_back({ primitive.points[0], primitive.points[1] });
    }

    flushLineBatch();
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

    Resources::Mesh* lineMesh = nullptr;
    if (!m_lineBatchVertices.empty())
        lineMesh = UploadLineVertices(ComputeLineBatchBoundingSphere(m_lineBatchVertices));

    for (const auto& command : m_drawCommands)
    {
        if (command.type == DrawCommandType::Primitive)
        {
            RenderPrimitiveNow(command.primitive, pso);
            continue;
        }

        if (lineMesh != nullptr)
            RenderLineCommand(command.line, *lineMesh, pso);
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

void DebugDrawPass::RenderLineCommand(const LineDrawCommand& command, Resources::Mesh& mesh, PipelineState pso)
{
    if (!m_primitiveMaterial)
        return;

    m_primitiveMaterial->Set("u_Point0", Maths::Vector3::Zero);
    m_primitiveMaterial->Set("u_Point1", Maths::Vector3::Zero);
    m_primitiveMaterial->Set("u_Point2", Maths::Vector3::Zero);
    m_primitiveMaterial->Set("u_UseVertexPosition", 1);

    auto effectivePso = pso;
    effectivePso.depthWriting = false;
    effectivePso.rasterizationMode = Settings::ERasterizationMode::LINE;
    m_primitiveMaterial->Set("u_Color", command.style.color);

    effectivePso.depthTest = command.style.depthMode == DebugDrawDepthMode::DepthTest;
    effectivePso.lineWidthPow2 = Utils::Conversions::FloatToPow2(command.style.lineWidth);

    Entities::Drawable drawable;
    drawable.material = m_primitiveMaterial.get();
    drawable.mesh = &mesh;
    drawable.stateMask = m_primitiveMaterial->GenerateStateMask();
    drawable.primitiveMode = Settings::EPrimitiveMode::LINES;
    drawable.vertexStart = command.vertexStart;
    drawable.vertexCount = command.vertexCount;

    m_renderer.DrawEntity(effectivePso, drawable);
}

Resources::Mesh* DebugDrawPass::UploadLineVertices(const Geometry::BoundingSphere& boundingSphere)
{
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
