#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Rendering/RenderDef.h"

namespace NLS::Render::ShaderCompiler
{
	enum class NLS_RENDER_API ShaderSourceLanguage : uint8_t
	{
		HLSL,
		GLSL
	};

	enum class NLS_RENDER_API ShaderStage : uint8_t
	{
		Vertex,
		Pixel,
		Compute
	};

	enum class NLS_RENDER_API ShaderTargetPlatform : uint8_t
	{
		Unknown,
		DXIL,
		SPIRV,
		GLSL
	};

	enum class NLS_RENDER_API ShaderCompilationStatus : uint8_t
	{
		NotCompiled,
		Succeeded,
		Failed
	};

	struct NLS_RENDER_API ShaderMacroDefinition
	{
		std::string name;
		std::string value;
	};

	struct NLS_RENDER_API ShaderCompileOptions
	{
		ShaderSourceLanguage sourceLanguage = ShaderSourceLanguage::HLSL;
		ShaderTargetPlatform targetPlatform = ShaderTargetPlatform::Unknown;
		std::string entryPoint = "Main";
		std::string targetProfile;
		bool enableDebugInfo = false;
		bool treatWarningsAsErrors = false;
		std::vector<ShaderMacroDefinition> macros;
		std::vector<std::string> includeDirectories;
	};

	struct NLS_RENDER_API ShaderCompilationInput
	{
		std::string assetPath;
		ShaderStage stage = ShaderStage::Vertex;
		ShaderCompileOptions options;
	};

	struct NLS_RENDER_API ShaderCompilationOutput
	{
		ShaderCompilationStatus status = ShaderCompilationStatus::NotCompiled;
		std::vector<uint8_t> bytecode;
		std::string diagnostics;
		std::vector<std::string> dependencyPaths;
		std::string cacheKey;
		std::string artifactPath;
	};
}
