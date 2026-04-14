#include <Debug/Assertion.h>
#include <Debug/Logger.h>
#include <Core/ServiceLocator.h>
#include <UI/UIManager.h>
#include <algorithm>
#include <fstream>
#include <filesystem>

#include "Rendering/RHI/Backends/ExplicitDeviceFactory.h"
#include "Rendering/RHI/Backends/RenderDeviceFactory.h"
#include "Rendering/RHI/Utils/DescriptorAllocator/DescriptorAllocator.h"
#include "Rendering/RHI/Utils/PipelineCache/PipelineCache.h"
#include "Rendering/RHI/Utils/ResourceStateTracker/ResourceStateTracker.h"
#include "Rendering/RHI/Utils/UploadContext/UploadContext.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/RHI/IRenderDevice.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Rendering/Tooling/RenderDocCaptureController.h"
#include "Rendering/Resources/IMesh.h"
#include "Rendering/Utils/Conversions.h"

namespace NLS::Render::Context
{
namespace
{
	const char* ToRenderDocBackendName(const Render::RHI::NativeBackendType backend)
	{
		switch (backend)
		{
		case Render::RHI::NativeBackendType::Vulkan: return "Vulkan";
		case Render::RHI::NativeBackendType::DX12: return "DX12";
		case Render::RHI::NativeBackendType::Metal: return "Metal";
		case Render::RHI::NativeBackendType::OpenGL: return "OpenGL";
		case Render::RHI::NativeBackendType::None:
		default:
			return "Unknown";
		}
	}
}

Driver::Driver(const Render::Settings::DriverSettings& p_driverSettings)
{
	m_renderDocCaptureController = std::make_unique<Render::Tooling::RenderDocCaptureController>(p_driverSettings.renderDoc);

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
	if (m_renderDocCaptureController != nullptr)
	{
		const auto nativeInfo = m_renderDevice->GetNativeDeviceInfo();
		m_renderDocCaptureController->SetResolvedBackendName(ToRenderDocBackendName(nativeInfo.backend));
		m_renderDocCaptureController->SetCaptureTarget(nativeInfo);
	}

	if (p_driverSettings.enableExplicitRHI)
	{
		m_explicitDevice = Render::Backend::CreateExplicitDevice(*m_renderDevice);
		if (m_explicitDevice != nullptr)
		{
			m_pipelineCache = Render::RHI::CreateDefaultPipelineCache();
			const auto frameCount = std::max<uint32_t>(1u, p_driverSettings.framesInFlight);
			m_frameContexts.reserve(frameCount);
			for (uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
			{
				Render::RHI::RHIFrameContext frameContext;
				frameContext.frameIndex = frameIndex;
				frameContext.commandPool = m_explicitDevice->CreateCommandPool(Render::RHI::QueueType::Graphics, "FrameCommandPool" + std::to_string(frameIndex));
				frameContext.commandBuffer = frameContext.commandPool != nullptr
					? frameContext.commandPool->CreateCommandBuffer("FrameCommandBuffer" + std::to_string(frameIndex))
					: nullptr;
				frameContext.frameFence = m_explicitDevice->CreateFence("FrameFence" + std::to_string(frameIndex));
				frameContext.imageAcquiredSemaphore = m_explicitDevice->CreateSemaphore("FrameAcquire" + std::to_string(frameIndex));
				frameContext.renderFinishedSemaphore = m_explicitDevice->CreateSemaphore("FramePresent" + std::to_string(frameIndex));
				frameContext.resourceStateTracker = Render::RHI::CreateDefaultResourceStateTracker();
				frameContext.descriptorAllocator = Render::RHI::CreateDefaultDescriptorAllocator();
				frameContext.uploadContext = Render::RHI::CreateDefaultUploadContext();
				m_frameContexts.push_back(std::move(frameContext));
			}
		}
	}
}

Driver::~Driver() = default;

void Driver::SetViewport(uint32_t p_x, uint32_t p_y, uint32_t p_width, uint32_t p_height)
{
	m_renderDevice->SetViewport(p_x, p_y, p_width, p_height);
}

std::shared_ptr<Render::RHI::IRHITexture> Driver::CreateTextureResource(const Render::RHI::TextureDimension dimension)
{
	return m_renderDevice->CreateTextureResource(dimension);
}

uint32_t Driver::CreateTexture()
{
	return m_renderDevice->CreateTexture();
}

void Driver::DestroyTexture(uint32_t textureId)
{
	m_renderDevice->DestroyTexture(textureId);
}

void Driver::BindTexture(const Render::RHI::TextureDimension dimension, uint32_t textureId)
{
	m_renderDevice->BindTexture(dimension, textureId);
}

void Driver::ActivateTexture(uint32_t slot)
{
	m_renderDevice->ActivateTexture(slot);
}

void Driver::SetupTexture(const Render::RHI::TextureDesc& desc, const void* data)
{
	m_renderDevice->SetupTexture(desc, data);
}

void Driver::GenerateTextureMipmap(const Render::RHI::TextureDimension dimension)
{
	m_renderDevice->GenerateTextureMipmap(dimension);
}

std::shared_ptr<Render::RHI::IRHIBuffer> Driver::CreateBufferResource(const Render::RHI::BufferType type)
{
	return m_renderDevice->CreateBufferResource(type);
}

uint32_t Driver::CreateBuffer()
{
	return m_renderDevice->CreateBuffer();
}

void Driver::DestroyBuffer(uint32_t bufferId)
{
	m_renderDevice->DestroyBuffer(bufferId);
}

void Driver::BindBuffer(const Render::RHI::BufferType type, uint32_t bufferId)
{
	m_renderDevice->BindBuffer(type, bufferId);
}

void Driver::BindBufferBase(const Render::RHI::BufferType type, uint32_t bindingPoint, uint32_t bufferId)
{
	m_renderDevice->BindBufferBase(type, bindingPoint, bufferId);
}

void Driver::SetBufferData(const Render::RHI::BufferType type, size_t size, const void* data, const Render::RHI::BufferUsage usage)
{
	m_renderDevice->SetBufferData(type, size, data, usage);
}

void Driver::SetBufferSubData(const Render::RHI::BufferType type, size_t offset, size_t size, const void* data)
{
	m_renderDevice->SetBufferSubData(type, offset, size, data);
}

void* Driver::GetUITextureHandle(uint32_t textureId) const
{
	return m_renderDevice->GetUITextureHandle(textureId);
}

void Driver::ReleaseUITextureHandles()
{
	m_renderDevice->ReleaseUITextureHandles();
}

bool Driver::PrepareUIRender()
{
	return m_renderDevice->PrepareUIRender();
}

uint32_t Driver::CreateFramebuffer()
{
	return m_renderDevice->CreateFramebuffer();
}

uint32_t Driver::CreateFramebuffer(const Render::RHI::FramebufferDesc& desc)
{
	return m_renderDevice->CreateFramebuffer(desc);
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

void Driver::ConfigureFramebuffer(uint32_t framebufferId, const Render::RHI::FramebufferDesc& desc)
{
	m_renderDevice->ConfigureFramebuffer(framebufferId, desc);
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
		m_renderDevice->SetClearColor(p_color.x, p_color.y, p_color.z, p_color.w);

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

	const auto vertexBufferView = p_mesh.GetVertexBufferView();
	if (vertexBufferView.buffer == nullptr || vertexBufferView.bufferId == 0 || p_mesh.GetVertexCount() == 0)
		return;

	SetPipelineState(p_pso);

	p_mesh.Bind();

	const auto indexBufferView = p_mesh.GetIndexBufferView();
	if (indexBufferView.has_value() && p_mesh.GetIndexCount() > 0)
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

void Driver::BindGraphicsPipeline(const Render::RHI::GraphicsPipelineDesc& pipelineDesc, const Render::Resources::BindingSetInstance* bindingSet)
{
	m_renderDevice->BindGraphicsPipeline(pipelineDesc, bindingSet);
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

Render::RHI::NativeRenderDeviceInfo Driver::GetNativeDeviceInfo() const
{
	return m_renderDevice->GetNativeDeviceInfo();
}

bool Driver::IsBackendReady() const
{
	return m_renderDevice->IsBackendReady();
}

bool Driver::SupportsCurrentSceneRenderer() const
{
	return m_renderDevice->GetCapabilities().supportsCurrentSceneRenderer;
}

bool Driver::IsRenderDocAvailable() const
{
	return m_renderDocCaptureController != nullptr && m_renderDocCaptureController->IsAvailable();
}

bool Driver::QueueRenderDocCapture(const std::string& label)
{
	return m_renderDocCaptureController != nullptr && m_renderDocCaptureController->QueueCapture(label);
}

bool Driver::StartRenderDocCapture()
{
	return m_renderDocCaptureController != nullptr && m_renderDocCaptureController->StartCapture();
}

bool Driver::EndRenderDocCapture()
{
	return m_renderDocCaptureController != nullptr && m_renderDocCaptureController->EndCapture();
}

std::string Driver::GetLatestRenderDocCapturePath() const
{
	return m_renderDocCaptureController != nullptr
		? m_renderDocCaptureController->GetLatestCapturePath()
		: std::string{};
}

std::string Driver::GetRenderDocCaptureDirectory() const
{
	return m_renderDocCaptureController != nullptr
		? m_renderDocCaptureController->GetCaptureDirectory()
		: std::string{};
}

bool Driver::OpenLatestRenderDocCapture()
{
	return m_renderDocCaptureController != nullptr && m_renderDocCaptureController->OpenLatestCapture();
}

bool Driver::GetRenderDocAutoOpenEnabled() const
{
	return m_renderDocCaptureController != nullptr && m_renderDocCaptureController->GetAutoOpenReplayUI();
}

void Driver::SetRenderDocAutoOpenEnabled(bool enabled)
{
	if (m_renderDocCaptureController != nullptr)
		m_renderDocCaptureController->SetAutoOpenReplayUI(enabled);
}

bool Driver::CreateSwapchain(const Render::RHI::SwapchainDesc& desc)
{
	if (m_explicitDevice != nullptr)
	{
		m_explicitSwapchain = m_explicitDevice->CreateSwapchain(desc);
		if (m_renderDocCaptureController != nullptr)
			m_renderDocCaptureController->SetCaptureTarget(m_renderDevice->GetNativeDeviceInfo());
		return m_explicitSwapchain != nullptr;
	}

	const bool created = m_renderDevice->CreateSwapchain(desc);
	if (created && m_renderDocCaptureController != nullptr)
		m_renderDocCaptureController->SetCaptureTarget(m_renderDevice->GetNativeDeviceInfo());
	return created;
}

void Driver::DestroySwapchain()
{
	m_explicitSwapchain.reset();
	if (m_explicitDevice == nullptr)
		m_renderDevice->DestroySwapchain();
}

void Driver::ResizeSwapchain(uint32_t width, uint32_t height)
{
	if (width == 0u || height == 0u)
		return;

	m_pendingSwapchainWidth = width;
	m_pendingSwapchainHeight = height;
	m_hasPendingSwapchainResize = true;
	m_lastSwapchainResizeRequestTime = std::chrono::steady_clock::now();
}

void Driver::ResizePlatformSwapchain(uint32_t width, uint32_t height)
{
	if (width == 0u || height == 0u)
		return;

	ResizeSwapchain(width, height);

	if (!m_explicitFrameActive)
		ApplyPendingSwapchainResize();
}

void Driver::PresentSwapchain()
{
	if (m_renderDocCaptureController != nullptr)
		m_renderDocCaptureController->OnPrePresent();

	if (m_explicitDevice != nullptr && m_skipNextExplicitPresent)
	{
		m_skipNextExplicitPresent = false;
		if (m_renderDocCaptureController != nullptr)
			m_renderDocCaptureController->OnPostPresent();
		return;
	}

	if (m_explicitDevice != nullptr && m_explicitSwapchain != nullptr)
	{
		if (m_explicitFrameActive)
		{
			NLS_ASSERT(false, "PresentSwapchain() must not be called while an explicit frame is still recording.");
			return;
		}

		m_explicitDevice->GetQueue(Render::RHI::QueueType::Graphics)->Present({ m_explicitSwapchain, 0, {} });
		if (m_renderDocCaptureController != nullptr)
			m_renderDocCaptureController->OnPostPresent();
		ApplyPendingSwapchainResize();
		return;
	}

	m_renderDevice->PresentSwapchain();
	if (m_renderDocCaptureController != nullptr)
		m_renderDocCaptureController->OnPostPresent();
	ApplyPendingSwapchainResize();
}

bool Driver::HasExplicitRHI() const
{
	return m_explicitDevice != nullptr;
}

Render::RHI::RHIFrameContext& Driver::BeginExplicitFrame(bool acquireSwapchainImage)
{
	NLS_ASSERT(m_explicitDevice != nullptr, "Explicit RHI is not enabled for this driver.");
	NLS_ASSERT(!m_frameContexts.empty(), "Explicit RHI frame contexts were not initialized.");
	NLS_ASSERT(!m_explicitFrameActive, "Cannot begin a new explicit frame while another one is still active.");

	auto& frameContext = m_frameContexts[m_currentFrameIndex % m_frameContexts.size()];
	frameContext.frameIndex = static_cast<uint32_t>(m_currentFrameIndex);
	if (frameContext.frameFence != nullptr)
		frameContext.frameFence->Wait();
	if (frameContext.frameFence != nullptr)
		frameContext.frameFence->Reset();
	if (frameContext.commandPool != nullptr)
		frameContext.commandPool->Reset();
	if (frameContext.commandBuffer != nullptr)
		frameContext.commandBuffer->Reset();
	if (frameContext.resourceStateTracker != nullptr)
		frameContext.resourceStateTracker->Reset();
	if (frameContext.descriptorAllocator != nullptr)
		frameContext.descriptorAllocator->BeginFrame(m_currentFrameIndex);
	if (frameContext.uploadContext != nullptr)
		frameContext.uploadContext->BeginFrame(m_currentFrameIndex);

	frameContext.hasAcquiredSwapchainImage = false;
	frameContext.uploadBytesReserved = 0;
	if (acquireSwapchainImage && m_explicitSwapchain != nullptr)
	{
		const auto acquiredImage = m_explicitSwapchain->AcquireNextImage(frameContext.imageAcquiredSemaphore, frameContext.frameFence);
		frameContext.hasAcquiredSwapchainImage = acquiredImage.has_value();
		frameContext.swapchainImageIndex = acquiredImage.has_value() ? acquiredImage->imageIndex : 0u;
	}

	if (frameContext.commandBuffer != nullptr)
		frameContext.commandBuffer->Begin();

	if (m_renderDocCaptureController != nullptr)
		m_renderDocCaptureController->OnPreFrame();

	m_skipNextExplicitPresent = false;
	m_explicitFrameActive = true;
	return frameContext;
}

void Driver::EndExplicitFrame(bool presentSwapchain)
{
	if (m_explicitDevice == nullptr || m_frameContexts.empty() || !m_explicitFrameActive)
		return;

	auto& frameContext = m_frameContexts[m_currentFrameIndex % m_frameContexts.size()];
	if (frameContext.commandBuffer != nullptr && frameContext.commandBuffer->IsRecording())
		frameContext.commandBuffer->End();

	Render::RHI::RHISubmitDesc submitDesc;
	if (frameContext.commandBuffer != nullptr)
		submitDesc.commandBuffers.push_back(frameContext.commandBuffer);
	if (frameContext.hasAcquiredSwapchainImage && frameContext.imageAcquiredSemaphore != nullptr)
		submitDesc.waitSemaphores.push_back(frameContext.imageAcquiredSemaphore);
	if (presentSwapchain && frameContext.renderFinishedSemaphore != nullptr)
		submitDesc.signalSemaphores.push_back(frameContext.renderFinishedSemaphore);
	submitDesc.signalFence = frameContext.frameFence;

	auto queue = m_explicitDevice->GetQueue(Render::RHI::QueueType::Graphics);
	if (queue != nullptr)
	{
		queue->Submit(submitDesc);
		if (presentSwapchain && frameContext.hasAcquiredSwapchainImage && m_explicitSwapchain != nullptr)
		{
			if (m_renderDocCaptureController != nullptr)
				m_renderDocCaptureController->OnPrePresent();

			Render::RHI::RHIPresentDesc presentDesc;
			presentDesc.swapchain = m_explicitSwapchain;
			presentDesc.imageIndex = frameContext.swapchainImageIndex;
			if (frameContext.renderFinishedSemaphore != nullptr)
				presentDesc.waitSemaphores.push_back(frameContext.renderFinishedSemaphore);
			queue->Present(presentDesc);
			if (m_renderDocCaptureController != nullptr)
				m_renderDocCaptureController->OnPostPresent();
			m_skipNextExplicitPresent = true;
			ApplyPendingSwapchainResize();
		}
	}
	if (frameContext.descriptorAllocator != nullptr)
		frameContext.descriptorAllocator->EndFrame(m_currentFrameIndex);
	if (frameContext.uploadContext != nullptr)
		frameContext.uploadContext->EndFrame(m_currentFrameIndex);

	m_currentFrameIndex = (m_currentFrameIndex + 1) % m_frameContexts.size();
	m_explicitFrameActive = false;
}

void Driver::ApplyPendingSwapchainResize()
{
	if (!m_hasPendingSwapchainResize)
		return;

	constexpr auto kResizeDebounce = std::chrono::milliseconds(750);
	if (std::chrono::steady_clock::now() - m_lastSwapchainResizeRequestTime < kResizeDebounce)
		return;

	const uint32_t width = m_pendingSwapchainWidth;
	const uint32_t height = m_pendingSwapchainHeight;
	m_hasPendingSwapchainResize = false;

	for (auto& frameContext : m_frameContexts)
	{
		if (frameContext.frameFence != nullptr)
			frameContext.frameFence->Wait();
	}

	if (NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>())
	{
		NLS_SERVICE(NLS::UI::UIManager).NotifySwapchainWillResize();
	}

	if (m_explicitSwapchain != nullptr)
	{
		m_explicitSwapchain->Resize(width, height);
		return;
	}

	if (m_renderDevice != nullptr)
		m_renderDevice->ResizeSwapchain(width, height);
}

const Render::RHI::RHIFrameContext* Driver::GetCurrentExplicitFrameContext() const
{
	if (m_frameContexts.empty() || !m_explicitFrameActive)
		return nullptr;

	return &m_frameContexts[m_currentFrameIndex % m_frameContexts.size()];
}

Render::RHI::RHIFrameContext* Driver::GetCurrentExplicitFrameContext()
{
	if (m_frameContexts.empty() || !m_explicitFrameActive)
		return nullptr;

	return &m_frameContexts[m_currentFrameIndex % m_frameContexts.size()];
}

std::shared_ptr<Render::RHI::RHIDevice> Driver::GetExplicitDevice() const
{
	return m_explicitDevice;
}

std::shared_ptr<Render::RHI::RHISwapchain> Driver::GetExplicitSwapchain() const
{
	return m_explicitSwapchain;
}

void Driver::SetPolygonMode(Settings::ERasterizationMode mode)
{
	m_defaultPipelineState.rasterizationMode = mode;
}

}
