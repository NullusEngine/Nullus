#include <Debug/Assertion.h>
#include <Debug/Logger.h>

#include "Rendering/Backend/RenderDeviceFactory.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/RHI/IRenderDevice.h"
#include "Rendering/Resources/IMesh.h"
#include "Rendering/Utils/Conversions.h"

namespace NLS::Render::Context
{
Driver::Driver(const Render::Settings::DriverSettings& p_driverSettings)
{
	m_renderDevice = Render::Backend::CreateRenderDevice(p_driverSettings.graphicsBackend);
	NLS_ASSERT(m_renderDevice != nullptr, "Failed to create render device!");

	auto initialPipelineState = m_renderDevice->Init(p_driverSettings);
	NLS_ASSERT(initialPipelineState.has_value(), "Failed to initialized driver!");

	if (p_driverSettings.defaultPipelineState)
	{
		m_defaultPipelineState = p_driverSettings.defaultPipelineState.value();
	}

	m_pipelineState = initialPipelineState.value();
	SetPipelineState(m_defaultPipelineState);

	m_vendor = m_renderDevice->GetVendor();
	m_hardware = m_renderDevice->GetHardware();
	m_version = m_renderDevice->GetVersion();
	m_shadingLanguageVersion = m_renderDevice->GetShadingLanguageVersion();
}

Driver::~Driver() = default;

void Driver::SetViewport(uint32_t p_x, uint32_t p_y, uint32_t p_width, uint32_t p_height)
{
	m_renderDevice->SetViewport(p_x, p_y, p_width, p_height);
}

uint32_t Driver::CreateFramebuffer()
{
	return m_renderDevice->CreateFramebuffer();
}

void Driver::DestroyFramebuffer(uint32_t framebufferId)
{
	m_renderDevice->DestroyFramebuffer(framebufferId);
}

void Driver::BindFramebuffer(uint32_t framebufferId)
{
	m_renderDevice->BindFramebuffer(framebufferId);
}

void Driver::AttachFramebufferColorTexture(uint32_t framebufferId, uint32_t textureId, uint32_t attachmentIndex)
{
	m_renderDevice->AttachFramebufferColorTexture(framebufferId, textureId, attachmentIndex);
}

void Driver::AttachFramebufferDepthStencilTexture(uint32_t framebufferId, uint32_t textureId)
{
	m_renderDevice->AttachFramebufferDepthStencilTexture(framebufferId, textureId);
}

void Driver::SetFramebufferDrawBufferCount(uint32_t framebufferId, uint32_t colorAttachmentCount)
{
	m_renderDevice->SetFramebufferDrawBufferCount(framebufferId, colorAttachmentCount);
}

void Driver::BlitDepth(uint32_t sourceFramebufferId, uint32_t destinationFramebufferId, uint32_t width, uint32_t height)
{
	m_renderDevice->BlitDepth(sourceFramebufferId, destinationFramebufferId, width, height);
}

void Driver::Clear(
	bool p_colorBuffer,
	bool p_depthBuffer,
	bool p_stencilBuffer,
	const Maths::Vector4& p_color
)
{
	if (p_colorBuffer)
	{
		m_renderDevice->SetClearColor(p_color.x, p_color.y, p_color.z, p_color.w);
	}

	auto pso = CreatePipelineState();

	if (p_stencilBuffer)
	{
		pso.stencilWriteMask = ~0;
	}

	pso.scissorTest = false;

	SetPipelineState(pso);

	m_renderDevice->Clear(p_colorBuffer, p_depthBuffer, p_stencilBuffer);
}

void Driver::ReadPixels(
	uint32_t p_x,
	uint32_t p_y,
	uint32_t p_width,
	uint32_t p_height,
	Render::Settings::EPixelDataFormat p_format,
	Render::Settings::EPixelDataType p_type,
	void* p_data
) const
{
	m_renderDevice->ReadPixels(p_x, p_y, p_width, p_height, p_format, p_type, p_data);
}

void Driver::Draw(
	Data::PipelineState p_pso,
	const Resources::IMesh& p_mesh,
	Settings::EPrimitiveMode p_primitiveMode,
	uint32_t p_instances
)
{
	if (p_instances == 0)
		return;

	SetPipelineState(p_pso);

	p_mesh.Bind();

	if (p_mesh.GetIndexCount() > 0)
	{
		if (p_instances == 1)
		{
			m_renderDevice->DrawElements(p_primitiveMode, p_mesh.GetIndexCount());
		}
		else
		{
			m_renderDevice->DrawElementsInstanced(p_primitiveMode, p_mesh.GetIndexCount(), p_instances);
		}
	}
	else
	{
		if (p_instances == 1)
		{
			m_renderDevice->DrawArrays(p_primitiveMode, p_mesh.GetVertexCount());
		}
		else
		{
			m_renderDevice->DrawArraysInstanced(p_primitiveMode, p_mesh.GetVertexCount(), p_instances);
		}
	}

	p_mesh.Unbind();
}

void Driver::SetPipelineState(Render::Data::PipelineState p_state)
{
	using namespace Render::Settings;

	if (p_state.bits != m_pipelineState.bits)
	{
		auto& i = p_state;
		auto& c = m_pipelineState;

		if (i.rasterizationMode != c.rasterizationMode) m_renderDevice->SetRasterizationMode(i.rasterizationMode);
		if (i.lineWidthPow2 != c.lineWidthPow2) m_renderDevice->SetRasterizationLinesWidth(Utils::Conversions::Pow2toFloat(i.lineWidthPow2));

		if (i.colorWriting.mask != c.colorWriting.mask) m_renderDevice->SetColorWriting(i.colorWriting.r, i.colorWriting.g, i.colorWriting.b, i.colorWriting.a);
		if (i.depthWriting != c.depthWriting) m_renderDevice->SetDepthWriting(i.depthWriting);

		if (i.blending != c.blending) m_renderDevice->SetCapability(ERenderingCapability::BLEND, i.blending);
		if (i.culling != c.culling) m_renderDevice->SetCapability(ERenderingCapability::CULL_FACE, i.culling);
		if (i.dither != c.dither) m_renderDevice->SetCapability(ERenderingCapability::DITHER, i.dither);
		if (i.polygonOffsetFill != c.polygonOffsetFill) m_renderDevice->SetCapability(ERenderingCapability::POLYGON_OFFSET_FILL, i.polygonOffsetFill);
		if (i.sampleAlphaToCoverage != c.sampleAlphaToCoverage) m_renderDevice->SetCapability(ERenderingCapability::SAMPLE_ALPHA_TO_COVERAGE, i.sampleAlphaToCoverage);
		if (i.depthTest != c.depthTest) m_renderDevice->SetCapability(ERenderingCapability::DEPTH_TEST, i.depthTest);
		if (i.scissorTest != c.scissorTest) m_renderDevice->SetCapability(ERenderingCapability::SCISSOR_TEST, i.scissorTest);
		if (i.stencilTest != c.stencilTest) m_renderDevice->SetCapability(ERenderingCapability::STENCIL_TEST, i.stencilTest);
		if (i.multisample != c.multisample) m_renderDevice->SetCapability(ERenderingCapability::MULTISAMPLE, i.multisample);

		if (i.stencilFuncOp != c.stencilFuncOp ||
			i.stencilFuncRef != c.stencilFuncRef ||
			i.stencilFuncMask != c.stencilFuncMask)
		{
			m_renderDevice->SetStencilAlgorithm(i.stencilFuncOp, i.stencilFuncRef, i.stencilFuncMask);
		}

		if (i.stencilWriteMask != c.stencilWriteMask) m_renderDevice->SetStencilMask(i.stencilWriteMask);
		if (i.stencilOpFail != c.stencilOpFail || i.depthOpFail != c.depthOpFail || i.bothOpFail != c.bothOpFail) m_renderDevice->SetStencilOperations(i.stencilOpFail, i.depthOpFail, i.bothOpFail);

		if (i.depthFunc != c.depthFunc) m_renderDevice->SetDepthAlgorithm(i.depthFunc);
		if (i.cullFace != c.cullFace) m_renderDevice->SetCullFace(i.cullFace);

		m_pipelineState = p_state;
	}
}

void Driver::ResetPipelineState()
{
	SetPipelineState(m_defaultPipelineState);
}

Render::Data::PipelineState Driver::CreatePipelineState() const
{
	return m_defaultPipelineState;
}

std::string_view Driver::GetVendor() const
{
	return m_vendor;
}

std::string_view Driver::GetHardware() const
{
	return m_hardware;
}

std::string_view Driver::GetVersion() const
{
	return m_version;
}

std::string_view Driver::GetShadingLanguageVersion() const
{
	return m_shadingLanguageVersion;
}

Render::RHI::RHIDeviceCapabilities Driver::GetCapabilities() const
{
	return m_renderDevice->GetCapabilities();
}

bool Driver::IsBackendReady() const
{
	return m_renderDevice->IsBackendReady();
}

bool Driver::CreateSwapchain(const Render::RHI::SwapchainDesc& desc)
{
	return m_renderDevice->CreateSwapchain(desc);
}

void Driver::DestroySwapchain()
{
	m_renderDevice->DestroySwapchain();
}

void Driver::ResizeSwapchain(uint32_t width, uint32_t height)
{
	m_renderDevice->ResizeSwapchain(width, height);
}

void Driver::PresentSwapchain()
{
	m_renderDevice->PresentSwapchain();
}

void Driver::SetPolygonMode(Settings::ERasterizationMode mode)
{
	m_defaultPipelineState.rasterizationMode = mode;
}

Render::RHI::IRenderDevice& Driver::GetRenderDevice()
{
	return *m_renderDevice;
}

const Render::RHI::IRenderDevice& Driver::GetRenderDevice() const
{
	return *m_renderDevice;
}
}
