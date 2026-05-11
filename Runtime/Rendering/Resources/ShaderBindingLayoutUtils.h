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

	enum class ShaderParameterGroupKind : uint8_t
	{
		Frame,
		Material,
		Object,
		Pass
	};

	struct ShaderParameterBindingContract
	{
		std::string name;
		RHI::BindingType type = RHI::BindingType::UniformBuffer;
		uint32_t descriptorSet = 0u;
		uint32_t registerSpace = 0u;
		uint32_t binding = 0u;
		uint32_t count = 1u;
		RHI::ShaderStageMask stageMask = RHI::ShaderStageMask::AllGraphics;
		bool required = true;
	};

	struct ShaderParameterGroupContract
	{
		std::string debugName;
		ShaderParameterGroupKind groupKind = ShaderParameterGroupKind::Frame;
		uint32_t descriptorSet = 0u;
		uint32_t registerSpace = 0u;
		bool required = true;
		std::vector<ShaderParameterBindingContract> parameters;
	};

	struct ShaderParameterBindingResourceState
	{
		std::string name;
		ShaderParameterGroupKind groupKind = ShaderParameterGroupKind::Frame;
		uint32_t descriptorSet = 0u;
		RHI::BindingType type = RHI::BindingType::UniformBuffer;
		uint32_t binding = 0u;
		bool hasResource = true;
		uint64_t resourceVersion = 0u;
		uint64_t requiredVersion = 0u;
	};

	NLS_RENDER_API ShaderBindingValidationResult ValidateShaderBindingReflection(
		const ShaderReflection& reflection);

	NLS_RENDER_API std::vector<RHI::RHIBindingLayoutDesc> BuildExplicitBindingLayoutDescsBySet(
		const ShaderReflection& reflection,
		std::string_view debugNamePrefix);

	NLS_RENDER_API std::vector<ShaderParameterGroupContract> BuildShaderParameterGroupContracts(
		const ShaderReflection& reflection,
		std::string_view debugNamePrefix);

	NLS_RENDER_API ShaderBindingValidationResult ValidateShaderParameterGroupResources(
		const std::vector<ShaderParameterGroupContract>& groups,
		const std::vector<ShaderParameterBindingResourceState>& resources);
}
