#include "Rendering/Features/DebugShapeRenderFeature.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/Utils/Conversions.h"

NLS::Rendering::Features::DebugShapeRenderFeature::DebugShapeRenderFeature(Core::CompositeRenderer& p_renderer)
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

	// TODO: Move these out of here, maybe we could have proper source files for these.
	std::string vertexShader = R"(
#version 430 core

uniform vec3 start;
uniform vec3 end;
uniform mat4 viewProjection;

void main()
{
	vec3 position = gl_VertexID == 0 ? start : end;
	gl_Position = viewProjection * vec4(position, 1.0);
}

)";

	std::string fragmentShader = R"(
#version 430 core

uniform vec3 color;

out vec4 FRAGMENT_COLOR;

void main()
{
	FRAGMENT_COLOR = vec4(color, 1.0);
}
)";

	m_lineShader = NLS::Rendering::Resources::Loaders::ShaderLoader::CreateFromSource(vertexShader, fragmentShader);
	m_lineMaterial = std::make_unique<NLS::Rendering::Data::Material>(m_lineShader);
}

NLS::Rendering::Features::DebugShapeRenderFeature::~DebugShapeRenderFeature()
{
	delete m_lineMesh;
	NLS::Rendering::Resources::Loaders::ShaderLoader::Destroy(m_lineShader);
}

void NLS::Rendering::Features::DebugShapeRenderFeature::OnBeginFrame(const Data::FrameDescriptor& p_frameDescriptor)
{
	SetViewProjection(
		p_frameDescriptor.camera->GetProjectionMatrix() *
		p_frameDescriptor.camera->GetViewMatrix()
	);
}

void NLS::Rendering::Features::DebugShapeRenderFeature::SetViewProjection(const Maths::Matrix4& p_viewProjection)
{
	m_lineShader->Bind();
	m_lineShader->SetUniformMat4("viewProjection", p_viewProjection);
	m_lineShader->Unbind();
}

void NLS::Rendering::Features::DebugShapeRenderFeature::DrawLine(
	NLS::Rendering::Data::PipelineState p_pso,
	const Maths::Vector3& p_start,
	const Maths::Vector3& p_end,
	const Maths::Vector3& p_color,
	float p_lineWidth
)
{
	m_lineMaterial->Set("start", p_start);
	m_lineMaterial->Set("end", p_end);
	m_lineMaterial->Set("color", p_color);

	p_pso.rasterizationMode = Settings::ERasterizationMode::LINE;
	p_pso.lineWidthPow2 = Utils::Conversions::FloatToPow2(p_lineWidth);

	NLS::Rendering::Entities::Drawable drawable;
	drawable.material = m_lineMaterial.get();
	drawable.mesh = m_lineMesh;
	drawable.stateMask = m_lineMaterial->GenerateStateMask();
	drawable.primitiveMode = Settings::EPrimitiveMode::LINES;

	m_renderer.DrawEntity(p_pso, drawable);

	m_lineShader->Unbind();
}

void NLS::Rendering::Features::DebugShapeRenderFeature::DrawBox(
	NLS::Rendering::Data::PipelineState p_pso,
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

void NLS::Rendering::Features::DebugShapeRenderFeature::DrawSphere(NLS::Rendering::Data::PipelineState p_pso, const Maths::Vector3& p_position, const Maths::Quaternion& p_rotation, float p_radius, const Maths::Vector3& p_color, float p_lineWidth)
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

void NLS::Rendering::Features::DebugShapeRenderFeature::DrawCapsule(NLS::Rendering::Data::PipelineState p_pso, const Maths::Vector3& p_position, const Maths::Quaternion& p_rotation, float p_radius, float p_height, const Maths::Vector3& p_color, float p_lineWidth)
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
