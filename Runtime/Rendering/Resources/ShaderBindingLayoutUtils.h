#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/Resources/ShaderReflection.h"

namespace NLS::Render::Resources
{
	enum class ShaderBindingValidationSeverity : uint8_t
	{
		Info,
		Warning,
		Error
	};

	struct ShaderBindingValidationDiagnostic
	{
		ShaderBindingValidationSeverity severity = ShaderBindingValidationSeverity::Info;
		std::string message;
	};

	struct ShaderBindingValidationResult
	{
		std::vector<ShaderBindingValidationDiagnostic> diagnostics;

		bool HasErrors() const
		{
			for (const auto& diagnostic : diagnostics)
			{
				if (diagnostic.severity == ShaderBindingValidationSeverity::Error)
					return true;
			}
			return false;
		}
	};

	NLS_RENDER_API ShaderBindingValidationResult ValidateShaderBindingReflection(
		const ShaderReflection& reflection);

	NLS_RENDER_API std::vector<RHI::RHIBindingLayoutDesc> BuildExplicitBindingLayoutDescsBySet(
		const ShaderReflection& reflection,
		std::string_view debugNamePrefix);
}
