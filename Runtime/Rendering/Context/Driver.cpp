#include <Debug/Assertion.h>
#include <Debug/Logger.h>
#include <Core/ServiceLocator.h>
#include <UI/UIManager.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <vector>
#include <Math/Vector4.h>

#include "Rendering/Data/PipelineState.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/RHI/Backends/RHIDeviceFactory.h"
#include "Rendering/RHI/Backends/RHIDeviceFactory.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHISwapchain.h"
#include "Rendering/RHI/RHITypes.h"
#include "Rendering/RHI/Utils/DescriptorAllocator/DescriptorAllocator.h"
#include "Rendering/RHI/Utils/PipelineCache/PipelineCache.h"
#include "Rendering/RHI/Utils/ResourceStateTracker/ResourceStateTracker.h"
#include "Rendering/RHI/Utils/UploadContext/UploadContext.h"
#include "Rendering/FrameGraph/FrameGraphExecutionContext.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/SwapchainResizePolicy.h"
#include "Rendering/RHI/Core/RHICommandList.h"
#include "Rendering/RHI/Core/RHICommandListExecutor.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Rendering/Tooling/RenderDocCaptureController.h"
#include "Rendering/Resources/IMesh.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Utils/Conversions.h"

namespace NLS::Render::Context
{
class DriverImpl
{
public:
    Render::Data::PipelineState defaultPipelineState;
    Render::Data::PipelineState pipelineState;
    std::shared_ptr<Render::RHI::RHIDevice> explicitDevice;
    std::unique_ptr<Render::RHI::RHICommandList> commandList;
    std::unique_ptr<Render::RHI::IRHICommandListExecutor> commandExecutor;
    std::shared_ptr<Render::RHI::RHISwapchain> explicitSwapchain;
    std::shared_ptr<Render::RHI::PipelineCache> pipelineCache;
    std::vector<Render::RHI::RHIFrameContext> frameContexts;
    std::unique_ptr<Render::Tooling::RenderDocCaptureController> renderDocCaptureController;
    size_t currentFrameIndex = 0;
    bool explicitFrameActive = false;
    bool skipNextExplicitPresent = false;
    bool hasPendingSwapchainResize = false;
    bool hasLoggedOnDemandPresentAcquire = false;
    uint32_t pendingSwapchainWidth = 0;
    uint32_t pendingSwapchainHeight = 0;
    std::chrono::steady_clock::time_point lastSwapchainResizeRequestTime{};
    void* uiRenderFinishedSemaphore = nullptr; // Set by UI before SubmitUIRendering, used by Present
};

namespace
{
    Render::RHI::RHIFrameContext* GetActiveFrameContext(DriverImpl& impl)
    {
        if (impl.frameContexts.empty() || !impl.explicitFrameActive)
            return nullptr;

        return &impl.frameContexts[impl.currentFrameIndex % impl.frameContexts.size()];
    }

    const Render::RHI::RHIFrameContext* GetActiveFrameContext(const DriverImpl& impl)
    {
        if (impl.frameContexts.empty() || !impl.explicitFrameActive)
            return nullptr;

        return &impl.frameContexts[impl.currentFrameIndex % impl.frameContexts.size()];
    }

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

    bool AllowsCompatibilityImmediateMaterialDrawFallback(const Render::RHI::NativeBackendType backend)
    {
        switch (backend)
        {
        case Render::RHI::NativeBackendType::DX12:
        case Render::RHI::NativeBackendType::Vulkan:
        case Render::RHI::NativeBackendType::Metal:
            return false;
        case Render::RHI::NativeBackendType::None:
        case Render::RHI::NativeBackendType::OpenGL:
        case Render::RHI::NativeBackendType::DX11:
        default:
            return true;
        }
    }

    bool ShouldLogExplicitDrawDiagnostics()
    {
        static const bool enabled = []()
        {
            if (const char* value = std::getenv("NLS_LOG_RENDER_DRAW_PATH"); value != nullptr)
                return std::strcmp(value, "1") == 0 || _stricmp(value, "true") == 0;
            return false;
        }();
        return enabled;
    }

}

Driver* TryGetLocatedDriver()
{
    return NLS::Core::ServiceLocator::Contains<Driver>()
        ? &NLS::Core::ServiceLocator::Get<Driver>()
        : nullptr;
}

Driver& RequireLocatedDriver(const std::string_view ownerName)
{
    if (auto* driver = TryGetLocatedDriver(); driver != nullptr)
        return *driver;

    const auto message = ownerName.empty()
        ? std::string("Rendering operation requires an initialized Driver.")
        : std::string(ownerName) + " requires an initialized Driver.";
    NLS_ASSERT(false, message.c_str());
    return *static_cast<Driver*>(nullptr);
}

std::optional<Render::Settings::EGraphicsBackend> TryGetLocatedActiveGraphicsBackend()
{
    if (const auto* driver = TryGetLocatedDriver(); driver != nullptr)
        return driver->GetActiveGraphicsBackend();

    return std::nullopt;
}

bool DriverRendererAccess::HasExplicitRHI(const Driver& driver)
{
	return driver.m_impl->explicitDevice != nullptr;
}

std::shared_ptr<Render::RHI::RHIDevice> DriverRendererAccess::GetExplicitDevice(const Driver& driver)
{
	return driver.m_impl->explicitDevice;
}

void DriverRendererAccess::BeginExplicitFrame(Driver& driver, const bool acquireSwapchainImage)
{
	NLS_ASSERT(driver.m_impl->explicitDevice != nullptr, "Explicit RHI is not enabled for this driver.");
	NLS_ASSERT(!driver.m_impl->frameContexts.empty(), "Explicit RHI frame contexts were not initialized.");
	NLS_ASSERT(!driver.m_impl->explicitFrameActive, "Cannot begin a new explicit frame while another one is still active.");

	auto& frameContext = driver.m_impl->frameContexts[driver.m_impl->currentFrameIndex % driver.m_impl->frameContexts.size()];
	frameContext.frameIndex = static_cast<uint32_t>(driver.m_impl->currentFrameIndex);
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
		frameContext.descriptorAllocator->BeginFrame(driver.m_impl->currentFrameIndex);
	if (frameContext.uploadContext != nullptr)
		frameContext.uploadContext->BeginFrame(driver.m_impl->currentFrameIndex);

	frameContext.hasAcquiredSwapchainImage = false;
	frameContext.uploadBytesReserved = 0;
	frameContext.swapchainBackbufferView = nullptr;
	// Acquire swapchain image only when the caller is recording directly into the swapchain.
	// Offscreen frames (e.g. editor scene/game panels) should not consume swapchain images here;
	// PresentSwapchain() performs on-demand acquisition when needed.
	if (acquireSwapchainImage && driver.m_impl->explicitSwapchain != nullptr)
	{
		const auto acquiredImage = driver.m_impl->explicitSwapchain->AcquireNextImage(
			frameContext.imageAcquiredSemaphore,
			frameContext.frameFence);
		frameContext.hasAcquiredSwapchainImage = acquiredImage.has_value();
		frameContext.swapchainImageIndex = acquiredImage.has_value() ? acquiredImage->imageIndex : 0u;
		if (acquiredImage.has_value())
		{
			frameContext.swapchainBackbufferView = driver.m_impl->explicitSwapchain->GetBackbufferView(frameContext.swapchainImageIndex);
		}
	}

	if (frameContext.commandBuffer != nullptr)
		frameContext.commandBuffer->Begin();

	if (driver.m_impl->renderDocCaptureController != nullptr)
		driver.m_impl->renderDocCaptureController->OnPreFrame();

	driver.m_impl->skipNextExplicitPresent = false;
	driver.m_impl->explicitFrameActive = true;
}

void DriverRendererAccess::EndExplicitFrame(Driver& driver, const bool presentSwapchain)
{
	if (driver.m_impl->explicitDevice == nullptr || driver.m_impl->frameContexts.empty() || !driver.m_impl->explicitFrameActive)
		return;

	auto& frameContext = driver.m_impl->frameContexts[driver.m_impl->currentFrameIndex % driver.m_impl->frameContexts.size()];
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

	auto queue = driver.m_impl->explicitDevice->GetQueue(Render::RHI::QueueType::Graphics);
	if (queue != nullptr)
	{
		queue->Submit(submitDesc);
		if (presentSwapchain && frameContext.hasAcquiredSwapchainImage && driver.m_impl->explicitSwapchain != nullptr)
		{
			if (driver.m_impl->renderDocCaptureController != nullptr)
				driver.m_impl->renderDocCaptureController->OnPrePresent();

			Render::RHI::RHIPresentDesc presentDesc;
			presentDesc.swapchain = driver.m_impl->explicitSwapchain;
			presentDesc.imageIndex = frameContext.swapchainImageIndex;
			if (frameContext.renderFinishedSemaphore != nullptr)
				presentDesc.waitSemaphores.push_back(frameContext.renderFinishedSemaphore);
			queue->Present(presentDesc);
			if (driver.m_impl->renderDocCaptureController != nullptr)
				driver.m_impl->renderDocCaptureController->OnPostPresent();
			driver.m_impl->skipNextExplicitPresent = true;
			driver.ApplyPendingSwapchainResize();
		}
	}
	if (frameContext.descriptorAllocator != nullptr)
		frameContext.descriptorAllocator->EndFrame(driver.m_impl->currentFrameIndex);
	if (frameContext.uploadContext != nullptr)
		frameContext.uploadContext->EndFrame(driver.m_impl->currentFrameIndex);

	driver.m_impl->currentFrameIndex = (driver.m_impl->currentFrameIndex + 1) % driver.m_impl->frameContexts.size();
	driver.m_impl->explicitFrameActive = false;
}

void DriverRendererAccess::SetViewport(
	Driver& driver,
	const uint32_t /*x*/,
	const uint32_t /*y*/,
	const uint32_t /*width*/,
	const uint32_t /*height*/)
{
	// Formal RHI: viewport is set via RHICommandBuffer::SetViewport
	// This is a no-op when using explicit RHI since command buffer handles it
	NLS_ASSERT(driver.m_impl->explicitDevice != nullptr, "Driver requires explicitDevice for all backends");
}

Render::Data::PipelineState DriverRendererAccess::CreatePipelineState(const Driver& driver)
{
	return driver.m_impl->defaultPipelineState;
}

bool DriverRendererAccess::SupportsEditorPickingReadback(const Driver& driver)
{
	NLS_ASSERT(driver.m_impl->explicitDevice != nullptr, "Driver requires explicitDevice for all backends");
	return Render::Settings::SupportsEditorPickingReadback(driver.m_impl->explicitDevice->GetCapabilities());
}

bool DriverRendererAccess::SupportsFramebufferReadback(const Driver& driver)
{
	NLS_ASSERT(driver.m_impl->explicitDevice != nullptr, "Driver requires explicitDevice for all backends");
	return driver.m_impl->explicitDevice->GetCapabilities().supportsFramebufferReadback;
}

Render::FrameGraph::FrameGraphExecutionContext DriverRendererAccess::CreateFrameGraphExecutionContext(const Driver& driver)
{
	const auto* frameContext = GetActiveFrameContext(*driver.m_impl);
	return {
		const_cast<Driver&>(driver),
		driver.m_impl->explicitDevice.get(),
		frameContext != nullptr ? frameContext->commandBuffer.get() : nullptr,
		const_cast<Render::RHI::RHIFrameContext*>(frameContext)
	};
}

std::shared_ptr<Render::RHI::RHICommandBuffer> DriverRendererAccess::GetActiveExplicitCommandBuffer(const Driver& driver)
{
	const auto* frameContext = GetActiveFrameContext(*driver.m_impl);
	return frameContext != nullptr ? frameContext->commandBuffer : nullptr;
}

std::shared_ptr<Render::RHI::RHITextureView> DriverRendererAccess::GetSwapchainBackbufferView(const Driver& driver)
{
	const auto* frameContext = GetActiveFrameContext(*driver.m_impl);
	if (frameContext == nullptr)
		return nullptr;
	return frameContext->swapchainBackbufferView;
}

void DriverRendererAccess::TransitionTextureToShaderRead(
	Driver& driver,
	const std::shared_ptr<Render::RHI::RHITexture>& texture)
{
	if (texture == nullptr)
		return;

	auto* frameContext = GetActiveFrameContext(*driver.m_impl);
	if (frameContext == nullptr || frameContext->commandBuffer == nullptr || frameContext->resourceStateTracker == nullptr)
		return;

	Render::RHI::RHISubresourceRange fullRange;
	fullRange.baseMipLevel = 0u;
	fullRange.mipLevelCount = texture->GetDesc().mipLevels;
	fullRange.baseArrayLayer = 0u;
	fullRange.arrayLayerCount = texture->GetDesc().arrayLayers;

	Render::RHI::RHIBarrierDesc requestedBarriers;
	requestedBarriers.textureBarriers.push_back({
		texture,
		Render::RHI::ResourceState::Unknown,
		Render::RHI::ResourceState::ShaderRead,
		fullRange,
		Render::RHI::PipelineStageMask::AllCommands,
		Render::RHI::PipelineStageMask::AllGraphics | Render::RHI::PipelineStageMask::ComputeShader,
		Render::RHI::AccessMask::MemoryRead | Render::RHI::AccessMask::MemoryWrite,
		Render::RHI::AccessMask::ShaderRead
	});

	auto resolvedBarriers = frameContext->resourceStateTracker->BuildTransitionBarriers(
		requestedBarriers.bufferBarriers,
		requestedBarriers.textureBarriers);
	if (resolvedBarriers.bufferBarriers.empty() && resolvedBarriers.textureBarriers.empty())
		return;

	frameContext->commandBuffer->Barrier(resolvedBarriers);
	frameContext->resourceStateTracker->Commit(resolvedBarriers);
}

std::shared_ptr<Render::RHI::RHIBindingLayout> DriverRendererAccess::CreateExplicitBindingLayout(
	const Driver& driver,
	const Render::RHI::RHIBindingLayoutDesc& desc)
{
	return driver.m_impl->explicitDevice != nullptr ? driver.m_impl->explicitDevice->CreateBindingLayout(desc) : nullptr;
}

std::shared_ptr<Render::RHI::RHIBindingSet> DriverRendererAccess::CreateExplicitBindingSet(
	const Driver& driver,
	const Render::RHI::RHIBindingSetDesc& desc)
{
	return driver.m_impl->explicitDevice != nullptr ? driver.m_impl->explicitDevice->CreateBindingSet(desc) : nullptr;
}

void DriverRendererAccess::ReadPixels(
	const Driver& /*driver*/,
	const uint32_t /*x*/,
	const uint32_t /*y*/,
	const uint32_t /*width*/,
	const uint32_t /*height*/,
	const Settings::EPixelDataFormat /*format*/,
	const Settings::EPixelDataType /*type*/,
	void* /*data*/)
{
	// Formal RHI: ReadPixels requires staging buffer + copy + wait + map
	// This is not yet implemented in Formal RHI command buffer
	// ReadPixels is only used by Editor for picking - silently skip for now
	NLS_LOG_WARNING("DriverRendererAccess::ReadPixels: Formal RHI ReadPixels not yet implemented");
}

void DriverRendererAccess::Clear(
	Driver& /*driver*/,
	const bool /*colorBuffer*/,
	const bool /*depthBuffer*/,
	const bool /*stencilBuffer*/,
	const Maths::Vector4& /*color*/)
{
	// Formal RHI: clear is handled via BeginRenderPass with LoadOp::Clear
	// This is a no-op when using explicit RHI since render pass handles it
	NLS_ASSERT(false, "Clear should not be called with explicitDevice - use BeginRenderPass with LoadOp::Clear");
}

void DriverRendererAccess::BindDefaultCompatibilityFramebuffer(Driver& /*driver*/)
{
	// Formal RHI: framebuffer binding is handled by BeginRenderPass
	// This is a no-op when using Formal RHI or any modern path
	// All backends now use CreateRhiDevice which creates Formal RHI directly
	// The legacy IRenderDevice path is no longer supported in Driver
	return;
}

bool DriverRendererAccess::SubmitMaterialDraw(
	Driver& driver,
	const Render::Resources::Material& material,
	Render::Data::PipelineState pipelineState,
	const Render::Resources::MaterialPipelineStateOverrides& overrides,
	const Render::Resources::IMesh& mesh,
	const Settings::EPrimitiveMode primitiveMode,
	const Settings::EComparaisonAlgorithm depthCompare,
	const uint32_t instances,
	const bool allowExplicitRecording)
{
	return driver.SubmitMaterialDraw(
		material,
		pipelineState,
		overrides,
		mesh,
		primitiveMode,
		depthCompare,
		instances,
		allowExplicitRecording);
}

Render::RHI::NativeRenderDeviceInfo DriverUIAccess::GetNativeDeviceInfo(const Driver& driver)
{
	// Use explicit device - it has its own UI resources (renderPass, descriptorPool)
	// created via CreateUIResources() in the constructor
	NLS_ASSERT(driver.m_impl->explicitDevice != nullptr, "Driver requires explicitDevice for all backends");
	return driver.m_impl->explicitDevice->GetNativeDeviceInfo();
}

void* DriverUIAccess::GetUITextureHandle(
    const Driver& driver,
    const std::shared_ptr<Render::RHI::RHITextureView>& textureView)
{
    // Use the native shader resource view directly from the texture view
    if (textureView == nullptr)
        return nullptr;

    Render::RHI::NativeHandle nativeHandle = textureView->GetNativeShaderResourceView();
    if (nativeHandle.IsValid() && nativeHandle.backend == Render::RHI::BackendType::OpenGL)
        return nativeHandle.handle;

    // Formal RHI path - should work for all backends
    NLS_ASSERT(driver.m_impl->explicitDevice != nullptr, "Driver requires explicitDevice for all backends");
    NLS_LOG_WARNING("GetUITextureHandle: formal RHI path did not return valid handle");
    return nullptr;
}

bool DriverUIAccess::PrepareUIRender(Driver& driver)
{
	// Use formal RHI device for UI render preparation
	return driver.m_impl->explicitDevice->PrepareUIRender();
}

void DriverUIAccess::ReleaseUITextureHandles(Driver& driver)
{
	// Use formal RHI device for UI texture handle release
	driver.m_impl->explicitDevice->ReleaseUITextureHandles();
}

void DriverUIAccess::PresentSwapchain(Driver& driver)
{
	driver.PresentSwapchain();
}

void DriverUIAccess::SetPolygonMode(Driver& driver, const Settings::ERasterizationMode mode)
{
	driver.SetPolygonMode(mode);
}

bool DriverUIAccess::IsRenderDocAvailable(const Driver& driver)
{
	return driver.m_impl->renderDocCaptureController != nullptr &&
		driver.m_impl->renderDocCaptureController->IsAvailable();
}

bool DriverUIAccess::QueueRenderDocCapture(Driver& driver, const std::string& label)
{
	return driver.m_impl->renderDocCaptureController != nullptr &&
		driver.m_impl->renderDocCaptureController->QueueCapture(label);
}

bool DriverUIAccess::OpenLatestRenderDocCapture(const Driver& driver)
{
	return driver.m_impl->renderDocCaptureController != nullptr &&
		driver.m_impl->renderDocCaptureController->OpenLatestCapture();
}

std::string DriverUIAccess::GetRenderDocCaptureDirectory(const Driver& driver)
{
	return driver.m_impl->renderDocCaptureController != nullptr
		? driver.m_impl->renderDocCaptureController->GetCaptureDirectory()
		: std::string{};
}

bool DriverUIAccess::GetRenderDocAutoOpenEnabled(const Driver& driver)
{
	return driver.m_impl->renderDocCaptureController != nullptr &&
		driver.m_impl->renderDocCaptureController->GetAutoOpenReplayUI();
}

void DriverUIAccess::SetRenderDocAutoOpenEnabled(Driver& driver, const bool enabled)
{
	if (driver.m_impl->renderDocCaptureController != nullptr)
		driver.m_impl->renderDocCaptureController->SetAutoOpenReplayUI(enabled);
}

void DriverUIAccess::SetRenderDocEnabled(Driver& driver, const bool enabled)
{
	if (driver.m_impl->renderDocCaptureController != nullptr)
		driver.m_impl->renderDocCaptureController->SetEnabled(enabled);
}

bool DriverUIAccess::IsRenderDocEnabled(const Driver& driver)
{
	return driver.m_impl->renderDocCaptureController != nullptr &&
		driver.m_impl->renderDocCaptureController->IsEnabled();
}

void* DriverUIAccess::GetRenderFinishedSemaphore(Driver& driver)
{
	if (driver.m_impl->frameContexts.empty())
		return nullptr;
	auto& frameContext = driver.m_impl->frameContexts[driver.m_impl->currentFrameIndex % driver.m_impl->frameContexts.size()];
	if (frameContext.renderFinishedSemaphore == nullptr)
		return nullptr;
	return frameContext.renderFinishedSemaphore->GetNativeSemaphoreHandle();
}

void DriverUIAccess::SetUISignalSemaphore(Driver& driver, void* semaphore)
{
	driver.m_impl->uiRenderFinishedSemaphore = semaphore;
}

void DriverTestAccess::SetExplicitDevice(Driver& driver, std::shared_ptr<Render::RHI::RHIDevice> explicitDevice)
{
	driver.m_impl->explicitDevice = std::move(explicitDevice);
}

Render::RHI::RHIFrameContext& DriverTestAccess::EnsureFrameContext(Driver& driver, const size_t index)
{
	if (driver.m_impl->frameContexts.size() <= index)
		driver.m_impl->frameContexts.resize(index + 1u);
	return driver.m_impl->frameContexts[index];
}

const Render::RHI::RHIFrameContext* DriverTestAccess::PeekFrameContext(const Driver& driver, const size_t index)
{
	return index < driver.m_impl->frameContexts.size() ? &driver.m_impl->frameContexts[index] : nullptr;
}

void DriverTestAccess::SetExplicitFrameActive(Driver& driver, const bool active)
{
	driver.m_impl->explicitFrameActive = active;
}

Driver::Driver(const Render::Settings::DriverSettings& p_driverSettings)
    : m_impl(std::make_unique<DriverImpl>())
{
	m_impl->renderDocCaptureController = std::make_unique<Render::Tooling::RenderDocCaptureController>(p_driverSettings.renderDoc);

	// All backends use CreateRhiDevice for direct Tier A device creation
	// This creates the Formal RHI device without any IRenderDevice dependency
	m_impl->explicitDevice = Render::Backend::CreateRhiDevice(p_driverSettings);
	if (m_impl->explicitDevice == nullptr)
	{
		NLS_LOG_WARNING(
			std::string("Driver: failed to create explicit RHI device for backend: ") +
			Render::Settings::ToString(p_driverSettings.graphicsBackend) +
			", continuing without device");
	}

	// Set up pipeline state
	if (p_driverSettings.defaultPipelineState)
	{
		m_impl->defaultPipelineState = p_driverSettings.defaultPipelineState.value();
	}
	m_impl->pipelineState = m_impl->defaultPipelineState;

	// Setup RenderDoc if available
	if (m_impl->renderDocCaptureController != nullptr)
	{
		const auto nativeInfo = GetNativeDeviceInfo();
		m_impl->renderDocCaptureController->SetResolvedBackendName(ToRenderDocBackendName(nativeInfo.backend));
		m_impl->renderDocCaptureController->SetCaptureTarget(nativeInfo);
	}

	if (m_impl->explicitDevice != nullptr)
	{
		m_impl->pipelineCache = Render::RHI::CreateDefaultPipelineCache();
		// Map NativeBackendType to ERHIBackend for executor creation
		auto nativeBackend = GetNativeDeviceInfo().backend;
		NLS::Render::RHI::ERHIBackend executorBackend;
		switch (nativeBackend)
		{
		case NLS::Render::RHI::NativeBackendType::DX12:   executorBackend = NLS::Render::RHI::ERHIBackend::DX12; break;
		case NLS::Render::RHI::NativeBackendType::Vulkan: executorBackend = NLS::Render::RHI::ERHIBackend::Vulkan; break;
		case NLS::Render::RHI::NativeBackendType::DX11:   executorBackend = NLS::Render::RHI::ERHIBackend::DX11; break;
		case NLS::Render::RHI::NativeBackendType::OpenGL: executorBackend = NLS::Render::RHI::ERHIBackend::OpenGL; break;
		case NLS::Render::RHI::NativeBackendType::Metal:  executorBackend = NLS::Render::RHI::ERHIBackend::Metal; break;
		default:                        executorBackend = NLS::Render::RHI::ERHIBackend::Null; break;
		}
		auto nativeInfo = GetNativeDeviceInfo();
		m_impl->commandExecutor = Render::RHI::CreateCommandListExecutor(executorBackend, nativeInfo);
		m_impl->commandList = std::make_unique<Render::RHI::DefaultRHICommandList>();
		const auto frameCount = std::max<uint32_t>(1u, p_driverSettings.framesInFlight);
		m_impl->frameContexts.reserve(frameCount);
		for (uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
		{
			Render::RHI::RHIFrameContext frameContext;
			frameContext.frameIndex = frameIndex;
			frameContext.commandPool = m_impl->explicitDevice->CreateCommandPool(Render::RHI::QueueType::Graphics, "FrameCommandPool" + std::to_string(frameIndex));
			NLS_ASSERT(frameContext.commandPool != nullptr, "Failed to create command pool for explicit RHI");
			frameContext.commandBuffer = frameContext.commandPool->CreateCommandBuffer("FrameCommandBuffer" + std::to_string(frameIndex));
			NLS_ASSERT(frameContext.commandBuffer != nullptr, "Failed to create command buffer for explicit RHI");
			frameContext.frameFence = m_impl->explicitDevice->CreateFence("FrameFence" + std::to_string(frameIndex));
			NLS_ASSERT(frameContext.frameFence != nullptr, "Failed to create fence for explicit RHI");
			frameContext.imageAcquiredSemaphore = m_impl->explicitDevice->CreateSemaphore("FrameAcquire" + std::to_string(frameIndex));
			NLS_ASSERT(frameContext.imageAcquiredSemaphore != nullptr, "Failed to create semaphore for explicit RHI");
			frameContext.renderFinishedSemaphore = m_impl->explicitDevice->CreateSemaphore("FramePresent" + std::to_string(frameIndex));
			NLS_ASSERT(frameContext.renderFinishedSemaphore != nullptr, "Failed to create semaphore for explicit RHI");
			frameContext.resourceStateTracker = Render::RHI::CreateDefaultResourceStateTracker();
			frameContext.descriptorAllocator = Render::RHI::CreateDefaultDescriptorAllocator();
			frameContext.uploadContext = Render::RHI::CreateDefaultUploadContext();
			m_impl->frameContexts.push_back(std::move(frameContext));
		}
	}
}

Driver::~Driver() = default;

bool Driver::SubmitMaterialDraw(
    const Render::Resources::Material& material,
    Data::PipelineState pipelineState,
    const Render::Resources::MaterialPipelineStateOverrides& overrides,
    const Resources::IMesh& mesh,
    Settings::EPrimitiveMode primitiveMode,
    Settings::EComparaisonAlgorithm depthCompare,
    uint32_t instances,
    bool allowExplicitRecording)
{
    // Note: Formal RHI recording is now handled directly by renderers.
    // This method is deprecated and should not be called.
    // All rendering goes through ABaseRenderer which uses direct Formal RHI command recording.
    NLS_LOG_WARNING("[Driver] SubmitMaterialDraw is deprecated - renderer uses direct Formal RHI recording");
    return true;
}

void Driver::SetPipelineState(Render::Data::PipelineState p_state)
{
	// Formal RHI path uses RHIGraphicsPipeline bound directly via commandBuffer->BindGraphicsPipeline()
	// This method is deprecated - only updates internal state tracking
	m_impl->pipelineState = p_state;
}

Render::Settings::RuntimeBackendFallbackDecision Driver::EvaluateEditorMainRuntimeFallback(
	const Render::Settings::EGraphicsBackend requestedBackend) const
{
	const Render::RHI::RHIDeviceCapabilities capabilities = m_impl->explicitDevice != nullptr
		? m_impl->explicitDevice->GetCapabilities()
		: Render::RHI::RHIDeviceCapabilities{};
	return Render::Settings::EvaluateEditorMainRuntimeFallback(requestedBackend, capabilities);
}

Render::Settings::RuntimeBackendFallbackDecision Driver::EvaluateGameMainRuntimeFallback(
	const Render::Settings::EGraphicsBackend requestedBackend) const
{
	const Render::RHI::RHIDeviceCapabilities capabilities = m_impl->explicitDevice != nullptr
		? m_impl->explicitDevice->GetCapabilities()
		: Render::RHI::RHIDeviceCapabilities{};
	return Render::Settings::EvaluateGameMainRuntimeFallback(requestedBackend, capabilities);
}

std::optional<std::string> Driver::GetEditorPickingReadbackWarning() const
{
	const Render::RHI::RHIDeviceCapabilities capabilities = m_impl->explicitDevice != nullptr
		? m_impl->explicitDevice->GetCapabilities()
		: Render::RHI::RHIDeviceCapabilities{};
	return Render::Settings::GetEditorPickingReadbackWarning(capabilities);
}

Render::Settings::EGraphicsBackend Driver::GetActiveGraphicsBackend() const
{
	if (m_impl->explicitDevice == nullptr)
		return Render::Settings::EGraphicsBackend::NONE;

	switch (m_impl->explicitDevice->GetNativeDeviceInfo().backend)
	{
	case Render::RHI::NativeBackendType::DX11: return Render::Settings::EGraphicsBackend::DX11;
	case Render::RHI::NativeBackendType::DX12: return Render::Settings::EGraphicsBackend::DX12;
	case Render::RHI::NativeBackendType::Vulkan: return Render::Settings::EGraphicsBackend::VULKAN;
	case Render::RHI::NativeBackendType::OpenGL: return Render::Settings::EGraphicsBackend::OPENGL;
	case Render::RHI::NativeBackendType::Metal: return Render::Settings::EGraphicsBackend::METAL;
	case Render::RHI::NativeBackendType::None:
	default:
		return Render::Settings::EGraphicsBackend::NONE;
	}
}

Render::RHI::NativeRenderDeviceInfo Driver::GetNativeDeviceInfo() const
{
	if (m_impl->explicitDevice != nullptr)
	{
		return m_impl->explicitDevice->GetNativeDeviceInfo();
	}
	return {};
}

bool Driver::CreatePlatformSwapchain(
	void* platformWindow,
	void* nativeWindowHandle,
	const uint32_t width,
	const uint32_t height,
	const bool vsync,
	const uint32_t imageCount)
{
	Render::RHI::SwapchainDesc desc;
	desc.platformWindow = platformWindow;
	desc.nativeWindowHandle = nativeWindowHandle;
	desc.width = width;
	desc.height = height;
	desc.vsync = vsync;
	desc.imageCount = imageCount;
	return CreateSwapchain(desc);
}

void Driver::ResizePlatformSwapchain(const uint32_t width, const uint32_t height)
{
	if (width == 0u || height == 0u)
		return;
	ResizeSwapchain(width, height);

	// Interactive window-edge resizing should update the swapchain before the next
	// frame is rendered. Otherwise DXGI can stretch the previous backbuffer to the
	// new client rect for one frame, which shows up as UI stretching/ghosting.
	if (!m_impl->explicitFrameActive)
		ApplyPendingSwapchainResize();
}

bool Driver::CreateSwapchain(const Render::RHI::SwapchainDesc& desc)
{
	m_impl->explicitSwapchain = m_impl->explicitDevice->CreateSwapchain(desc);
	NLS_ASSERT(m_impl->explicitSwapchain != nullptr, "Failed to create swapchain for explicit RHI");
	// RenderDoc targets the explicit device's native info
	if (m_impl->renderDocCaptureController != nullptr)
		m_impl->renderDocCaptureController->SetCaptureTarget(m_impl->explicitDevice->GetNativeDeviceInfo());
	return m_impl->explicitSwapchain != nullptr;
}

void Driver::DestroySwapchain()
{
	m_impl->explicitSwapchain.reset();
}

void Driver::ResizeSwapchain(uint32_t width, uint32_t height)
{
	if (width == 0u || height == 0u)
		return;

	m_impl->pendingSwapchainWidth = width;
	m_impl->pendingSwapchainHeight = height;
	m_impl->hasPendingSwapchainResize = true;
	m_impl->lastSwapchainResizeRequestTime = std::chrono::steady_clock::now();
}

void Driver::PresentSwapchain()
{
	if (m_impl->renderDocCaptureController != nullptr)
		m_impl->renderDocCaptureController->OnPrePresent();

	if (m_impl->skipNextExplicitPresent)
	{
		m_impl->skipNextExplicitPresent = false;
		if (m_impl->renderDocCaptureController != nullptr)
			m_impl->renderDocCaptureController->OnPostPresent();
		return;
	}

	NLS_ASSERT(m_impl->explicitSwapchain != nullptr, "PresentSwapchain requires explicitSwapchain");
	if (m_impl->explicitFrameActive)
	{
		NLS_ASSERT(false, "PresentSwapchain() must not be called while an explicit frame is still recording.");
		return;
	}

	// Get the current frame's swapchain image index
	// If hasAcquiredSwapchainImage is false (e.g., BeginExplicitFrame was skipped or used outputBuffer != nullptr),
	// we need to acquire the swapchain image now before presenting
	uint32_t presentImageIndex = 0;
	bool needsAcquire = true;
	if (!m_impl->frameContexts.empty())
	{
		auto& frameContext = m_impl->frameContexts[m_impl->currentFrameIndex % m_impl->frameContexts.size()];
		presentImageIndex = frameContext.swapchainImageIndex;
		needsAcquire = !frameContext.hasAcquiredSwapchainImage;
	}

	// For DX12 and other backends that require explicit acquisition, acquire now if not already done
	if (needsAcquire && m_impl->explicitSwapchain != nullptr)
	{
		if (!m_impl->hasLoggedOnDemandPresentAcquire)
		{
			NLS_LOG_WARNING("Driver::PresentSwapchain: acquiring swapchain image on demand because no explicit frame acquisition happened.");
			m_impl->hasLoggedOnDemandPresentAcquire = true;
		}
		auto acquiredImage = m_impl->explicitSwapchain->AcquireNextImage(nullptr, nullptr);
		if (acquiredImage.has_value())
		{
			presentImageIndex = acquiredImage->imageIndex;
		}
	}

	Render::RHI::RHIPresentDesc presentDesc;
	presentDesc.swapchain = m_impl->explicitSwapchain;
	presentDesc.imageIndex = presentImageIndex;
	// Add UI render finished semaphore to wait semaphores
	// This ensures UI rendering completes before presenting
	if (m_impl->uiRenderFinishedSemaphore != nullptr)
	{
		presentDesc.uiSignalSemaphore = m_impl->uiRenderFinishedSemaphore;
	}
	m_impl->explicitDevice->GetQueue(Render::RHI::QueueType::Graphics)->Present(presentDesc);
	if (m_impl->renderDocCaptureController != nullptr)
		m_impl->renderDocCaptureController->OnPostPresent();
	ApplyPendingSwapchainResize();
}

void Driver::ApplyPendingSwapchainResize()
{
	if (!m_impl->hasPendingSwapchainResize)
		return;

	if (!ShouldApplyPendingSwapchainResize(
		std::chrono::steady_clock::now() - m_impl->lastSwapchainResizeRequestTime))
		return;

	const uint32_t width = m_impl->pendingSwapchainWidth;
	const uint32_t height = m_impl->pendingSwapchainHeight;
	m_impl->hasPendingSwapchainResize = false;

	for (auto& frameContext : m_impl->frameContexts)
	{
		if (frameContext.frameFence != nullptr)
			frameContext.frameFence->Wait();
	}

	if (NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>())
	{
		NLS_SERVICE(NLS::UI::UIManager).NotifySwapchainWillResize();
	}

	if (m_impl->explicitSwapchain != nullptr)
	{
		m_impl->explicitSwapchain->Resize(width, height);
	}
}

void Driver::SetPolygonMode(Settings::ERasterizationMode mode)
{
	m_impl->defaultPipelineState.rasterizationMode = mode;
}

}
