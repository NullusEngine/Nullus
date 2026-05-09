#pragma once

#include <memory>

#include "RenderDef.h"

namespace NLS::Render::RHI
{
	class RHIDevice;
	class UploadContext;
}

namespace NLS::Render::Settings
{
	struct DriverSettings;
}

namespace NLS::Render::Backend
{
	// Unified factory: creates RHIDevice directly from settings
	NLS_RENDER_API std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateRhiDevice(const NLS::Render::Settings::DriverSettings& settings);
	NLS_RENDER_API std::shared_ptr<NLS::Render::RHI::UploadContext> CreateUploadContextForRhiDevice(
		const std::shared_ptr<NLS::Render::RHI::RHIDevice>& device);
}
