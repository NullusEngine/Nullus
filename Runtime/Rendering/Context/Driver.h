#pragma once

#include <string>
#include <array>
#include <chrono>
#include <memory>
#include <vector>

#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/ERenderingCapability.h"
#include "Rendering/Settings/EPrimitiveMode.h"
#include "Rendering/Settings/ERasterizationMode.h"
#include "Rendering/Settings/EComparaisonAlgorithm.h"
#include "Rendering/Settings/EOperation.h"
#include "Rendering/Settings/ECullFace.h"
#include "Rendering/Settings/ECullingOptions.h"
#include "Rendering/Settings/EPixelDataFormat.h"
#include "Rendering/Settings/EPixelDataType.h"
#include "Rendering/Data/PipelineState.h"
#include "Rendering/RHI/IRHIResource.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Utils/PipelineCache/PipelineCache.h"
#include "Rendering/RHI/GraphicsPipelineDesc.h"
#include "Rendering/RHI/RHITypes.h"

#include <Math/Vector4.h>
#include "RenderDef.h"
class DriverImpl;

namespace NLS::Render::RHI
{
	class IRenderDevice;
}

namespace NLS::Render::Tooling
{
	class RenderDocCaptureController;
}

namespace NLS::Render::Resources
{
	class IMesh;
	class BindingSetInstance;
}

namespace NLS::Render::Context
{
/**
 * Handles the lifecycle of the underlying graphics context
 */
class NLS_RENDER_API Driver final
{
public:
    /**
     * Creates the driver
     * @param p_driverSettings
     */
    Driver(const Settings::DriverSettings& p_driverSettings);

    /**
     * Destroy the driver
     */
    ~Driver();

    /**
     * Set the viewport
     * @param p_x
     * @param p_y
     * @param p_width
     * @param p_height
     */
    void SetViewport(
        uint32_t p_x,
        uint32_t p_y,
        uint32_t p_width,
        uint32_t p_height);

	std::shared_ptr<::NLS::Render::RHI::IRHITexture> CreateTextureResource(::NLS::Render::RHI::TextureDimension dimension = ::NLS::Render::RHI::TextureDimension::Texture2D);
	uint32_t CreateTexture();
	void DestroyTexture(uint32_t textureId);
	void BindTexture(::NLS::Render::RHI::TextureDimension dimension, uint32_t textureId);
	void ActivateTexture(uint32_t slot);
	void SetupTexture(const ::NLS::Render::RHI::TextureDesc& desc, const void* data);
	void GenerateTextureMipmap(::NLS::Render::RHI::TextureDimension dimension);

	std::shared_ptr<::NLS::Render::RHI::IRHIBuffer> CreateBufferResource(::NLS::Render::RHI::BufferType type);
	uint32_t CreateBuffer();
	void DestroyBuffer(uint32_t bufferId);
	void BindBuffer(::NLS::Render::RHI::BufferType type, uint32_t bufferId);
	void BindBufferBase(::NLS::Render::RHI::BufferType type, uint32_t bindingPoint, uint32_t bufferId);
	void SetBufferData(::NLS::Render::RHI::BufferType type, size_t size, const void* data, ::NLS::Render::RHI::BufferUsage usage);
	void SetBufferSubData(::NLS::Render::RHI::BufferType type, size_t offset, size_t size, const void* data);
	void* GetUITextureHandle(uint32_t textureId) const;
	void ReleaseUITextureHandles();
    bool PrepareUIRender();

    uint32_t CreateFramebuffer();
    uint32_t CreateFramebuffer(const ::NLS::Render::RHI::FramebufferDesc& desc);

    void DestroyFramebuffer(uint32_t framebufferId);

    /**
     * Bind a framebuffer by raw backend identifier. Use 0 for the backbuffer.
     */
    void BindFramebuffer(uint32_t framebufferId);

    void AttachFramebufferColorTexture(uint32_t framebufferId, uint32_t textureId, uint32_t attachmentIndex);

    void AttachFramebufferDepthStencilTexture(uint32_t framebufferId, uint32_t textureId);

    void SetFramebufferDrawBufferCount(uint32_t framebufferId, uint32_t colorAttachmentCount);
    void ConfigureFramebuffer(uint32_t framebufferId, const ::NLS::Render::RHI::FramebufferDesc& desc);

    /**
     * Blit the depth buffer from one framebuffer to another.
     */
    void BlitDepth(uint32_t sourceFramebufferId, uint32_t destinationFramebufferId, uint32_t width, uint32_t height);

    /**
     * Clear the screen using the previously defined clear color (With Renderer::SetClearColor()) or by
     * using the OpenGL default one.
     * @param p_colorBuffer
     * @param p_depthBuffer
     * @param p_stencilBuffer
     * @param p_color
     */
    void Clear(
        bool p_colorBuffer,
        bool p_depthBuffer,
        bool p_stencilBuffer,
        const Maths::Vector4& p_color = Maths::Vector4(0, 0, 0, 0));

    /**
     * Read a block of pixels from the currently bound framebuffer (or backbuffer).
     * @param p_x
     * @param p_y
     * @param p_width
     * @param p_height
     * @param p_format
     * @param p_type
     * @param p_data
     */
    void ReadPixels(
        uint32_t p_x,
        uint32_t p_y,
        uint32_t p_width,
        uint32_t p_height,
        Settings::EPixelDataFormat p_format,
        Settings::EPixelDataType p_type,
        void* p_data) const;

    /**
     * Draw a mesh
     * @param p_pso
     * @param p_mesh
     * @param p_primitiveMode
     * @param p_instances
     */
	void Draw(
        ::NLS::Render::Data::PipelineState p_pso,
        const ::NLS::Render::Resources::IMesh& p_mesh,
        Settings::EPrimitiveMode p_primitiveMode = Settings::EPrimitiveMode::TRIANGLES,
        uint32_t p_instances = 1);
	void BindGraphicsPipeline(const ::NLS::Render::RHI::GraphicsPipelineDesc& pipelineDesc, const ::NLS::Render::Resources::BindingSetInstance* bindingSet);

    /**
     * Create a pipeline state from the default state
     */
    Data::PipelineState CreatePipelineState() const;

    /**
     * Returns the vendor
     */
    std::string_view GetVendor() const;

    /**
     * Returns details about the current rendering hardware
     */
    std::string_view GetHardware() const;

    /**
     * Returns the current graphics API version
     */
    std::string_view GetVersion() const;

    /**
     * Returns the current shading language version
     */
    std::string_view GetShadingLanguageVersion() const;

	::NLS::Render::RHI::RHIDeviceCapabilities GetCapabilities() const;
	::NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const;
	bool IsBackendReady() const;
	bool SupportsCurrentSceneRenderer() const;
	bool IsRenderDocAvailable() const;
	bool QueueRenderDocCapture(const std::string& label = {});
	bool StartRenderDocCapture();
	bool EndRenderDocCapture();
	std::string GetLatestRenderDocCapturePath() const;
	std::string GetRenderDocCaptureDirectory() const;
	bool OpenLatestRenderDocCapture();
	bool GetRenderDocAutoOpenEnabled() const;
	void SetRenderDocAutoOpenEnabled(bool enabled);
	bool CreateSwapchain(const ::NLS::Render::RHI::SwapchainDesc& desc);
	void DestroySwapchain();
	void ResizeSwapchain(uint32_t width, uint32_t height);
	void PresentSwapchain();
	bool HasExplicitRHI() const;
	::NLS::Render::RHI::RHIFrameContext& BeginExplicitFrame(bool acquireSwapchainImage = true);
	void EndExplicitFrame(bool presentSwapchain = true);
	::NLS::Render::RHI::RHIFrameContext* GetCurrentExplicitFrameContext();
	const ::NLS::Render::RHI::RHIFrameContext* GetCurrentExplicitFrameContext() const;
	std::shared_ptr<::NLS::Render::RHI::RHIDevice> GetExplicitDevice() const;
	std::shared_ptr<::NLS::Render::RHI::RHISwapchain> GetExplicitSwapchain() const;

    void SetPolygonMode(Settings::ERasterizationMode mode);

private:
    void ApplyPendingSwapchainResize();
    void SetPipelineState(::NLS::Render::Data::PipelineState p_state);
    void ResetPipelineState();

private:
    std::string m_vendor;
    std::string m_hardware;
    std::string m_version;
    std::string m_shadingLanguageVersion;
    ::NLS::Render::Data::PipelineState m_defaultPipelineState;
    ::NLS::Render::Data::PipelineState m_pipelineState;
	std::unique_ptr<::NLS::Render::RHI::IRenderDevice> m_renderDevice;
	std::shared_ptr<::NLS::Render::RHI::RHIDevice> m_explicitDevice;
	std::shared_ptr<::NLS::Render::RHI::RHISwapchain> m_explicitSwapchain;
	std::shared_ptr<::NLS::Render::RHI::PipelineCache> m_pipelineCache;
	std::vector<::NLS::Render::RHI::RHIFrameContext> m_frameContexts;
	std::unique_ptr<::NLS::Render::Tooling::RenderDocCaptureController> m_renderDocCaptureController;
	size_t m_currentFrameIndex = 0;
	bool m_explicitFrameActive = false;
	bool m_skipNextExplicitPresent = false;
    bool m_hasPendingSwapchainResize = false;
    uint32_t m_pendingSwapchainWidth = 0;
    uint32_t m_pendingSwapchainHeight = 0;
    std::chrono::steady_clock::time_point m_lastSwapchainResizeRequestTime{};
};
} // namespace NLS::Render::Context
