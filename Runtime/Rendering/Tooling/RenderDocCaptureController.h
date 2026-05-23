#pragma once

#include <memory>
#include <string>
#include <cstdint>

#include "Rendering/RHI/RHITypes.h"
#include "Rendering/Settings/DriverSettings.h"
#include "RenderDef.h"

namespace NLS::Render::Tooling
{
	enum class RenderDocQueuedCaptureAction
	{
		None,
		WaitForFutureFrame,
		StartExplicitFrameCapture,
	};

	NLS_RENDER_API ::NLS::Render::RHI::NativeRenderDeviceHandle ResolveRenderDocCaptureDeviceHandle(
		const ::NLS::Render::RHI::NativeRenderDeviceInfo& nativeInfo);
	NLS_RENDER_API void* ResolveRenderDocCaptureDevice(const ::NLS::Render::RHI::NativeRenderDeviceInfo& nativeInfo);
	NLS_RENDER_API RenderDocQueuedCaptureAction ResolveRenderDocQueuedCapturePreFrameAction(
		bool available,
		bool captureQueued,
		bool frameWillPresent,
		uint32_t presentCountdown);

	class NLS_RENDER_API RenderDocCaptureController final
	{
	public:
		explicit RenderDocCaptureController(Settings::RenderDocSettings settings);
		~RenderDocCaptureController();

		bool IsAvailable() const;
		bool QueueCapture(const std::string& label = {});
		bool StartCapture();
		bool EndCapture();
		void OnPreFrame(bool frameWillPresent = true);
		void OnPrePresent();
		void OnPostPresent();

		std::string GetLatestCapturePath() const;
		std::string GetCaptureDirectory() const;
		bool OpenLatestCapture() const;

		bool GetAutoOpenReplayUI() const;
		void SetAutoOpenReplayUI(bool enabled);
		void SetResolvedBackendName(const std::string& backendName);
		void SetCaptureTarget(const ::NLS::Render::RHI::NativeRenderDeviceInfo& nativeInfo);

		// Dynamic enable/disable RenderDoc at runtime
		void SetEnabled(bool enabled);
		bool IsEnabled() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};
}
