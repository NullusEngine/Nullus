#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "Rendering/Settings/EPrimitiveMode.h"
#include "Rendering/Settings/EComparaisonAlgorithm.h"
#include "RenderDef.h"

namespace NLS::Render::Data
{
    struct PipelineState;
}

namespace NLS::Render::Settings
{
    struct DriverSettings;
    enum class EGraphicsBackend : uint8_t;
    struct RuntimeBackendFallbackDecision;
    enum class ERasterizationMode : uint8_t;
}

namespace NLS::Render::RHI
{
    struct SwapchainDesc;
    struct NativeRenderDeviceInfo;
}

namespace NLS::Render::Resources
{
	class IMesh;
	class Material;
	struct MaterialPipelineStateOverrides;
}

namespace NLS::Render::Context
{
class DriverImpl;
struct DriverCompatibilityAccess;
struct DriverRendererAccess;
struct DriverTestAccess;
struct DriverUIAccess;

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

    bool SubmitMaterialDraw(
        const ::NLS::Render::Resources::Material& material,
        ::NLS::Render::Data::PipelineState pipelineState,
        const ::NLS::Render::Resources::MaterialPipelineStateOverrides& overrides,
        const ::NLS::Render::Resources::IMesh& mesh,
        Settings::EPrimitiveMode primitiveMode = Settings::EPrimitiveMode::TRIANGLES,
        Settings::EComparaisonAlgorithm depthCompare = Settings::EComparaisonAlgorithm::LESS,
        uint32_t instances = 1,
        bool allowExplicitRecording = true);

    Settings::RuntimeBackendFallbackDecision EvaluateEditorMainRuntimeFallback(Settings::EGraphicsBackend requestedBackend) const;
    Settings::RuntimeBackendFallbackDecision EvaluateGameMainRuntimeFallback(Settings::EGraphicsBackend requestedBackend) const;
    std::optional<std::string> GetEditorPickingReadbackWarning() const;
    Settings::EGraphicsBackend GetActiveGraphicsBackend() const;
    ::NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const;
	bool CreatePlatformSwapchain(
		void* platformWindow,
		void* nativeWindowHandle,
		uint32_t width,
		uint32_t height,
		bool vsync,
		uint32_t imageCount = 2u);
	void ResizePlatformSwapchain(uint32_t width, uint32_t height);

private:
    friend struct DriverCompatibilityAccess;
    friend struct DriverRendererAccess;
    friend struct DriverTestAccess;
    friend struct DriverUIAccess;

	bool CreateSwapchain(const ::NLS::Render::RHI::SwapchainDesc& desc);
	void DestroySwapchain();
	void ResizeSwapchain(uint32_t width, uint32_t height);
	void PresentSwapchain();
    void SetPolygonMode(Settings::ERasterizationMode mode);

    void ApplyPendingSwapchainResize();
    void SetPipelineState(::NLS::Render::Data::PipelineState p_state);
    std::unique_ptr<DriverImpl> m_impl;
};
} // namespace NLS::Render::Context
