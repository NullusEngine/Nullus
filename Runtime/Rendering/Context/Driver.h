#pragma once

#include <string>
#include <array>
#include <memory>

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
#include "Rendering/RHI/RHITypes.h"

#include <Math/Vector4.h>
#include "RenderDef.h"
class DriverImpl;

namespace NLS::Render::RHI
{
	class IRenderDevice;
}

namespace NLS::Render::Resources
{
	class IMesh;
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

    uint32_t CreateFramebuffer();

    void DestroyFramebuffer(uint32_t framebufferId);

    /**
     * Bind a framebuffer by raw backend identifier. Use 0 for the backbuffer.
     */
    void BindFramebuffer(uint32_t framebufferId);

    void AttachFramebufferColorTexture(uint32_t framebufferId, uint32_t textureId, uint32_t attachmentIndex);

    void AttachFramebufferDepthStencilTexture(uint32_t framebufferId, uint32_t textureId);

    void SetFramebufferDrawBufferCount(uint32_t framebufferId, uint32_t colorAttachmentCount);

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
	bool IsBackendReady() const;
	bool CreateSwapchain(const ::NLS::Render::RHI::SwapchainDesc& desc);
	void DestroySwapchain();
	void ResizeSwapchain(uint32_t width, uint32_t height);
	void PresentSwapchain();

    void SetPolygonMode(Settings::ERasterizationMode mode);

	::NLS::Render::RHI::IRenderDevice& GetRenderDevice();
	const ::NLS::Render::RHI::IRenderDevice& GetRenderDevice() const;

private:
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
};
} // namespace NLS::Render::Context
