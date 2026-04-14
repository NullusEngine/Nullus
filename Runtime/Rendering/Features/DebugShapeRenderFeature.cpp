#include "Rendering/Features/DebugShapeRenderFeature.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/Utils/Conversions.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ServiceLocator.h"

namespace NLS::Render::Features
{
DebugShapeRenderFeature::DebugShapeRenderFeature(Core::CompositeRenderer& p_renderer)
	: ARenderFeature(p_renderer)
{
	std::vector<Geometry::Vertex> vertices;
	vertices.push_back
	({
		0, 0, 0,
		0, 0,
		0, 0, 0,
		0, 0, 0,
		0, 0, 0
	});
	vertices.push_back
	({
		0, 0, 0,
		0, 0,
		0, 0, 0,
		0, 0, 0,
		0, 0, 0
	});

	m_lineMesh = new Resources::Mesh(vertices, { 0, 1 }, 0);

	if (::NLS::Core::ServiceLocator::Contains<::NLS::Core::ResourceManagement::ShaderManager>())
	{
		m_lineShader = ::NLS::Core::ServiceLocator::Get<::NLS::Core::ResourceManagement::ShaderManager>()[":Shaders/DebugLine.hlsl"];
	}
	else
	{
		m_lineShader = Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/DebugLine.hlsl");
		m_ownsLineShader = m_lineShader != nullptr;
	}

	if (m_lineShader != nullptr)
	{
		m_lineMaterial = std::make_unique<Material>();
		m_lineMaterial->SetShader(m_lineShader);
		m_lineMaterial->SetBlendable(false);
		m_lineMaterial->SetDepthTest(true);
		m_lineMaterial->SetBackfaceCulling(false);
		m_lineMaterial->SetFrontfaceCulling(false);
	}
}

DebugShapeRenderFeature::~DebugShapeRenderFeature()
{
	delete m_lineMesh;
	if (m_ownsLineShader)
		Resources::Loaders::ShaderLoader::Destroy(m_lineShader);
}

void DebugShapeRenderFeature::OnBeginFrame(const Data::FrameDescriptor& p_frameDescriptor)
{
	SetViewProjection(
		p_frameDescriptor.camera->GetProjectionMatrix() *
		p_frameDescriptor.camera->GetViewMatrix()
	);
}

void DebugShapeRenderFeature::SetViewProjection(const Maths::Matrix4& p_viewProjection)
{
	(void)p_viewProjection;
}

void DebugShapeRenderFeature::DrawLine(
	Data::PipelineState p_pso,
	const Maths::Vector3& p_start,
	const Maths::Vector3& p_end,
	const Maths::Vector3& p_color,
	float p_lineWidth
)
{
	if (!m_lineMaterial)
		return;

	m_lineMaterial->Set("u_Start", p_start);
	m_lineMaterial->Set("u_End", p_end);
	m_lineMaterial->Set("u_Color", p_color);

	p_pso.rasterizationMode = Settings::ERasterizationMode::LINE;
	p_pso.lineWidthPow2 = Utils::Conversions::FloatToPow2(p_lineWidth);
	p_pso.depthWriting = false;

	Entities::Drawable drawable;
	drawable.material = m_lineMaterial.get();
	drawable.mesh = m_lineMesh;
	drawable.stateMask = m_lineMaterial->GenerateStateMask();
	drawable.primitiveMode = Settings::EPrimitiveMode::LINES;

	m_renderer.DrawEntity(p_pso, drawable);
}

void DebugShapeRenderFeature::DrawBox(
	Data::PipelineState p_pso,
	const Maths::Vector3& p_position,
	const Maths::Quaternion& p_rotation,
	const Maths::Vector3& p_size,
	const Maths::Vector3& p_color,
	float p_lineWidth
)
{
	DrawLine(p_pso, p_position + p_rotation * Maths::Vector3{ -p_size.x, -p_size.y, -p_size.z }, p_position + p_rotation * Maths::Vector3{ -p_size.x, -p_size.y, +p_size.z }, p_color, p_lineWidth);
	DrawLine(p_pso, p_position + p_rotation * Maths::Vector3{ -p_size.x, p_size.y, -p_size.z }, p_position + p_rotation * Maths::Vector3{ -p_size.x, +p_size.y, +p_size.z }, p_color, p_lineWidth);
	DrawLine(p_pso, p_position + p_rotation * Maths::Vector3{ -p_size.x, -p_size.y, -p_size.z }, p_position + p_rotation * Maths::Vector3{ -p_size.x, +p_size.y, -p_size.z }, p_color, p_lineWidth);
	DrawLine(p_pso, p_position + p_rotation * Maths::Vector3{ -p_size.x, -p_size.y, +p_size.z }, p_position + p_rotation * Maths::Vector3{ -p_size.x, +p_size.y, +p_size.z }, p_color, p_lineWidth);
	DrawLine(p_pso, p_position + p_rotation * Maths::Vector3{ +p_size.x, -p_size.y, -p_size.z }, p_position + p_rotation * Maths::Vector3{ +p_size.x, -p_size.y, +p_size.z }, p_color, p_lineWidth);
	DrawLine(p_pso, p_position + p_rotation * Maths::Vector3{ +p_size.x, p_size.y, -p_size.z }, p_position + p_rotation * Maths::Vector3{ +p_size.x, +p_size.y, +p_size.z }, p_color, p_lineWidth);
	DrawLine(p_pso, p_position + p_rotation * Maths::Vector3{ +p_size.x, -p_size.y, -p_size.z }, p_position + p_rotation * Maths::Vector3{ +p_size.x, +p_size.y, -p_size.z }, p_color, p_lineWidth);
	DrawLine(p_pso, p_position + p_rotation * Maths::Vector3{ +p_size.x, -p_size.y, +p_size.z }, p_position + p_rotation * Maths::Vector3{ +p_size.x, +p_size.y, +p_size.z }, p_color, p_lineWidth);
	DrawLine(p_pso, p_position + p_rotation * Maths::Vector3{ -p_size.x, -p_size.y, -p_size.z }, p_position + p_rotation * Maths::Vector3{ +p_size.x, -p_size.y, -p_size.z }, p_color, p_lineWidth);
	DrawLine(p_pso, p_position + p_rotation * Maths::Vector3{ -p_size.x, +p_size.y, -p_size.z }, p_position + p_rotation * Maths::Vector3{ +p_size.x, +p_size.y, -p_size.z }, p_color, p_lineWidth);
	DrawLine(p_pso, p_position + p_rotation * Maths::Vector3{ -p_size.x, -p_size.y, +p_size.z }, p_position + p_rotation * Maths::Vector3{ +p_size.x, -p_size.y, +p_size.z }, p_color, p_lineWidth);
	DrawLine(p_pso, p_position + p_rotation * Maths::Vector3{ -p_size.x, +p_size.y, +p_size.z }, p_position + p_rotation * Maths::Vector3{ +p_size.x, +p_size.y, +p_size.z }, p_color, p_lineWidth);
}

void DebugShapeRenderFeature::DrawSphere(Data::PipelineState p_pso, const Maths::Vector3& p_position, const Maths::Quaternion& p_rotation, float p_radius, const Maths::Vector3& p_color, float p_lineWidth)
{
	if (!std::isinf(p_radius))
	{
		for (float i = 0; i <= 360.0f; i += 10.0f)
		{
			DrawLine(p_pso, p_position + p_rotation * (Maths::Vector3{ cos(i * (3.14f / 180.0f)), sin(i * (3.14f / 180.0f)), 0.f } *p_radius), p_position + p_rotation * (Maths::Vector3{ cos((i + 10.0f) * (3.14f / 180.0f)), sin((i + 10.0f) * (3.14f / 180.0f)), 0.f } *p_radius), p_color, p_lineWidth);
			DrawLine(p_pso, p_position + p_rotation * (Maths::Vector3{ 0.f, sin(i * (3.14f / 180.0f)), cos(i * (3.14f / 180.0f)) } *p_radius), p_position + p_rotation * (Maths::Vector3{ 0.f, sin((i + 10.0f) * (3.14f / 180.0f)), cos((i + 10.0f) * (3.14f / 180.0f)) } *p_radius), p_color, p_lineWidth);
			DrawLine(p_pso, p_position + p_rotation * (Maths::Vector3{ cos(i * (3.14f / 180.0f)), 0.f, sin(i * (3.14f / 180.0f)) } *p_radius), p_position + p_rotation * (Maths::Vector3{ cos((i + 10.0f) * (3.14f / 180.0f)), 0.f, sin((i + 10.0f) * (3.14f / 180.0f)) } *p_radius), p_color, p_lineWidth);
		}
	}
}

void DebugShapeRenderFeature::DrawCapsule(Data::PipelineState p_pso, const Maths::Vector3& p_position, const Maths::Quaternion& p_rotation, float p_radius, float p_height, const Maths::Vector3& p_color, float p_lineWidth)
{
	if (!std::isinf(p_radius))
	{
		float halfHeight = p_height / 2;

		Maths::Vector3 hVec = { 0.0f, halfHeight, 0.0f };

		for (float i = 0; i < 360.0f; i += 10.0f)
		{
			DrawLine(p_pso, p_position + p_rotation * (hVec + Maths::Vector3{ cos(i * (3.14f / 180.0f)), 0.f, sin(i * (3.14f / 180.0f)) } *p_radius), p_position + p_rotation * (hVec + Maths::Vector3{ cos((i + 10.0f) * (3.14f / 180.0f)), 0.f, sin((i + 10.0f) * (3.14f / 180.0f)) } *p_radius), p_color, p_lineWidth);
			DrawLine(p_pso, p_position + p_rotation * (-hVec + Maths::Vector3{ cos(i * (3.14f / 180.0f)), 0.f, sin(i * (3.14f / 180.0f)) } *p_radius), p_position + p_rotation * (-hVec + Maths::Vector3{ cos((i + 10.0f) * (3.14f / 180.0f)), 0.f, sin((i + 10.0f) * (3.14f / 180.0f)) } *p_radius), p_color, p_lineWidth);

			if (i < 180.f)
			{
				DrawLine(p_pso, p_position + p_rotation * (hVec + Maths::Vector3{ cos(i * (3.14f / 180.0f)), sin(i * (3.14f / 180.0f)), 0.f } *p_radius), p_position + p_rotation * (hVec + Maths::Vector3{ cos((i + 10.0f) * (3.14f / 180.0f)), sin((i + 10.0f) * (3.14f / 180.0f)), 0.f } *p_radius), p_color, p_lineWidth);
				DrawLine(p_pso, p_position + p_rotation * (hVec + Maths::Vector3{ 0.f, sin(i * (3.14f / 180.0f)), cos(i * (3.14f / 180.0f)) } *p_radius), p_position + p_rotation * (hVec + Maths::Vector3{ 0.f, sin((i + 10.0f) * (3.14f / 180.0f)), cos((i + 10.0f) * (3.14f / 180.0f)) } *p_radius), p_color, p_lineWidth);
			}
			else
			{
				DrawLine(p_pso, p_position + p_rotation * (-hVec + Maths::Vector3{ cos(i * (3.14f / 180.0f)), sin(i * (3.14f / 180.0f)), 0.f } *p_radius), p_position + p_rotation * (-hVec + Maths::Vector3{ cos((i + 10.0f) * (3.14f / 180.0f)), sin((i + 10.0f) * (3.14f / 180.0f)), 0.f } *p_radius), p_color, p_lineWidth);
				DrawLine(p_pso, p_position + p_rotation * (-hVec + Maths::Vector3{ 0.f, sin(i * (3.14f / 180.0f)), cos(i * (3.14f / 180.0f)) } *p_radius), p_position + p_rotation * (-hVec + Maths::Vector3{ 0.f, sin((i + 10.0f) * (3.14f / 180.0f)), cos((i + 10.0f) * (3.14f / 180.0f)) } *p_radius), p_color, p_lineWidth);
			}
		}

		DrawLine(p_pso, p_position + p_rotation * (Maths::Vector3{ -p_radius, -halfHeight, 0.f }), p_position + p_rotation * (Maths::Vector3{ -p_radius, +halfHeight, 0.f }), p_color, p_lineWidth);
		DrawLine(p_pso, p_position + p_rotation * (Maths::Vector3{ p_radius, -halfHeight, 0.f }), p_position + p_rotation * (Maths::Vector3{ p_radius, +halfHeight, 0.f }), p_color, p_lineWidth);
		DrawLine(p_pso, p_position + p_rotation * (Maths::Vector3{ 0.f, -halfHeight, -p_radius }), p_position + p_rotation * (Maths::Vector3{ 0.f, +halfHeight, -p_radius }), p_color, p_lineWidth);
		DrawLine(p_pso, p_position + p_rotation * (Maths::Vector3{ 0.f, -halfHeight, p_radius }), p_position + p_rotation * (Maths::Vector3{ 0.f, +halfHeight, p_radius }), p_color, p_lineWidth);
	}
}
}
