#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "RenderDef.h"

namespace NLS::Render::Data
{
    struct PipelineState;
}

namespace NLS::Render::Settings
{
    struct DriverSettings;
    enum class EGraphicsBackend : uint8_t;
    struct RuntimeBackendReadinessDecision;
    enum class ERasterizationMode : uint8_t;
}

namespace NLS::Render::RHI
{
    struct SwapchainDesc;
    struct NativeRenderDeviceInfo;
}

namespace NLS::Render::Resources
{
}

namespace NLS::Render::Context
{
class DriverImpl;
struct RenderThreadCoordinator;
struct RhiThreadCoordinator;
class ThreadedRenderingLifecycle;
enum class RhiSubmissionAttribution : uint8_t;
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

    Settings::RuntimeBackendReadinessDecision EvaluateEditorMainRuntimeReadiness(Settings::EGraphicsBackend requestedBackend) const;
    Settings::RuntimeBackendReadinessDecision EvaluateGameMainRuntimeReadiness(Settings::EGraphicsBackend requestedBackend) const;
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
    void ShutdownThreadedRendering();

    // Set a callback to be invoked before swapchain resize.
    // This allows the application layer (e.g., Editor) to notify UI about impending resize
    // without requiring Driver to directly depend on UIManager.
    using SwapchainWillResizeCallback = std::function<void()>;
    void SetSwapchainWillResizeCallback(SwapchainWillResizeCallback callback);

private:
    friend struct DriverRendererAccess;
    friend struct DriverTestAccess;
    friend struct DriverUIAccess;
    friend struct RenderThreadCoordinator;
    friend struct RhiThreadCoordinator;

	bool CreateSwapchain(const ::NLS::Render::RHI::SwapchainDesc& desc);
	void DestroySwapchain();
	void ResizeSwapchain(uint32_t width, uint32_t height);
    void PresentSwapchain();
    void SetPolygonMode(Settings::ERasterizationMode mode);
    void StartThreadedRenderingWorkers();
    void StopThreadedRenderingWorkers();
    bool IsThreadedRenderingEnabled() const;
    void ShutdownRhiResources();

    void ApplyPendingSwapchainResize();
    void SetPipelineState(::NLS::Render::Data::PipelineState p_state);
    std::unique_ptr<DriverImpl> m_impl;
};
} // namespace NLS::Render::Context
