#include "Rendering/Debug/DebugDrawPass.h"

#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ServiceLocator.h"
#include "Rendering/Core/CompositeRenderer.h"
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
        m_primitiveShader = Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/DebugPrimitive.hlsl");
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

    for (const auto& primitive : m_renderer.GetDebugDrawService()->CollectVisiblePrimitives())
        RenderPrimitive(primitive.get(), pso);
}

void DebugDrawPass::RenderPrimitive(const DebugDrawPrimitive& primitive, PipelineState pso)
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
