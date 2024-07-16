#include <functional>

#include "Rendering/Core/ABaseRenderer.h"
#include "Rendering/Resources/Loaders/TextureLoader.h"
#include <Debug/Assertion.h>
std::atomic_bool NLS::Render::Core::ABaseRenderer::s_isDrawing{ false };

const NLS::Render::Entities::Camera kDefaultCamera;

NLS::Render::Core::ABaseRenderer::ABaseRenderer(Context::Driver& p_driver) : 
	m_driver(p_driver),
	m_isDrawing(false),
	m_emptyTexture(NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(255, 255, 255, 255))
{
}

NLS::Render::Core::ABaseRenderer::~ABaseRenderer()
{
	NLS::Render::Resources::Loaders::TextureLoader::Destroy(m_emptyTexture);
}

void NLS::Render::Core::ABaseRenderer::BeginFrame(const Data::FrameDescriptor& p_frameDescriptor)
{
	NLS_ASSERT(!s_isDrawing, "Cannot call BeginFrame() when previous frame hasn't finished.");
	NLS_ASSERT(p_frameDescriptor.IsValid(), "Invalid FrameDescriptor!");

	m_frameDescriptor = p_frameDescriptor;

	if (p_frameDescriptor.outputBuffer)
	{
		p_frameDescriptor.outputBuffer->Bind();
	}

	m_basePipelineState = m_driver.CreatePipelineState();
	m_driver.SetViewport(0, 0, p_frameDescriptor.renderWidth, p_frameDescriptor.renderHeight);

	Clear(
		p_frameDescriptor.camera->GetClearColorBuffer(),
		p_frameDescriptor.camera->GetClearDepthBuffer(),
		p_frameDescriptor.camera->GetClearStencilBuffer(),
		p_frameDescriptor.camera->GetClearColor()
	);

	p_frameDescriptor.camera->CacheMatrices(p_frameDescriptor.renderWidth, p_frameDescriptor.renderHeight);

	m_isDrawing = true;
	s_isDrawing.store(true);
}

void NLS::Render::Core::ABaseRenderer::EndFrame()
{
	NLS_ASSERT(s_isDrawing, "Cannot call EndFrame() before calling BeginFrame()");

	if (m_frameDescriptor.outputBuffer)
	{
		m_frameDescriptor.outputBuffer->Unbind();
	}

	m_isDrawing = false;
	s_isDrawing.store(false);
}

const NLS::Render::Data::FrameDescriptor& NLS::Render::Core::ABaseRenderer::GetFrameDescriptor() const
{
	NLS_ASSERT(m_isDrawing, "Cannot call GetFrameDescriptor() outside of a frame");
	return m_frameDescriptor;
}

NLS::Render::Data::PipelineState NLS::Render::Core::ABaseRenderer::CreatePipelineState() const
{
	return m_basePipelineState;
}

bool NLS::Render::Core::ABaseRenderer::IsDrawing() const
{
	return m_isDrawing;
}

void NLS::Render::Core::ABaseRenderer::ReadPixels(
	uint32_t p_x,
	uint32_t p_y,
	uint32_t p_width,
	uint32_t p_height,
	Settings::EPixelDataFormat p_format,
	Settings::EPixelDataType p_type,
	void* p_data
) const
{
	return m_driver.ReadPixels(p_x, p_y, p_width, p_height, p_format, p_type, p_data);
}

void NLS::Render::Core::ABaseRenderer::Clear(
	bool p_colorBuffer,
	bool p_depthBuffer,
	bool p_stencilBuffer,
	const NLS::Maths::Vector4& p_color
)
{
	m_driver.Clear(p_colorBuffer, p_depthBuffer, p_stencilBuffer, p_color);
}

void NLS::Render::Core::ABaseRenderer::DrawEntity(
	NLS::Render::Data::PipelineState p_pso,
	const Entities::Drawable& p_drawable
)
{
	auto material = p_drawable.material;
	auto mesh = p_drawable.mesh;

	const auto gpuInstances = material->GetGPUInstances();

	if (mesh && gpuInstances > 0)
	{
		p_pso.depthWriting = p_drawable.stateMask.depthWriting;
		p_pso.colorWriting.mask = p_drawable.stateMask.colorWriting ? 0xFF : 0x00;
		p_pso.blending = p_drawable.stateMask.blendable;
		p_pso.culling = p_drawable.stateMask.frontfaceCulling || p_drawable.stateMask.backfaceCulling;
		p_pso.depthTest = p_drawable.stateMask.depthTest;

		if (p_pso.culling)
		{
			if (p_drawable.stateMask.backfaceCulling && p_drawable.stateMask.frontfaceCulling)
			{
				p_pso.cullFace = Settings::ECullFace::FRONT_AND_BACK;
			}
			else
			{
				p_pso.cullFace =
					p_drawable.stateMask.backfaceCulling ?
					Settings::ECullFace::BACK :
					Settings::ECullFace::FRONT;
			}
		}

		material->Bind(m_emptyTexture);
		m_driver.Draw(p_pso, *mesh, p_drawable.primitiveMode, gpuInstances);
		material->UnBind();
	}
}
