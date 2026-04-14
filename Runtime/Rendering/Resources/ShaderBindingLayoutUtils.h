#pragma once

#include <string_view>
#include <vector>

#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/Resources/ShaderReflection.h"

namespace NLS::Render::Resources
{
	NLS_RENDER_API std::vector<RHI::RHIBindingLayoutDesc> BuildExplicitBindingLayoutDescsBySet(
		const ShaderReflection& reflection,
		std::string_view debugNamePrefix);
}
