#pragma once

#include <memory>
#include <string>

#include "Rendering/RHI/RHITypes.h"
#include "Rendering/Settings/DriverSettings.h"
#include "RenderDef.h"

namespace NLS::Render::Tooling
{
	NLS_RENDER_API void* ResolveRenderDocCaptureDevice(const ::NLS::Render::RHI::NativeRenderDeviceInfo& nativeInfo);

	class NLS_RENDER_API RenderDocCaptureController final
	{
	public:
		explicit RenderDocCaptureController(Settings::RenderDocSettings settings);
		~RenderDocCaptureController();

		bool IsAvailable() const;
		bool QueueCapture(const std::string& label = {});
		bool StartCapture();
		bool EndCapture();
		void OnPreFrame();
		void OnPrePresent();
		void OnPostPresent();

		std::string GetLatestCapturePath() const;
		std::string GetCaptureDirectory() const;
		bool OpenLatestCapture() const;

		bool GetAutoOpenReplayUI() const;
		void SetAutoOpenReplayUI(bool enabled);
		void SetResolvedBackendName(const std::string& backendName);
		void SetCaptureTarget(const ::NLS::Render::RHI::NativeRenderDeviceInfo& nativeInfo);

	private:
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};
}
