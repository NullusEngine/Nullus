#pragma once

#include <memory>

#include "RenderDef.h"

namespace NLS::Render::RHI
{
	class RHIDevice;
}

namespace NLS::Render::Settings
{
	struct DriverSettings;
}

namespace NLS::Render::Backend
{
	// Unified factory: creates RHIDevice directly from settings
	NLS_RENDER_API std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateRhiDevice(const NLS::Render::Settings::DriverSettings& settings);
}
