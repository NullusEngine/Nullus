#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Rendering/RenderDef.h"
#include "Rendering/ShaderCompiler/ShaderCompilationTypes.h"

namespace NLS::Render::ShaderCompiler
{
	struct NLS_RENDER_API ShaderVariantKey
	{
		std::string assetPath;
		ShaderStage stage = ShaderStage::Vertex;
		std::vector<ShaderMacroDefinition> macros;

		bool operator==(const ShaderVariantKey& other) const;
		bool operator!=(const ShaderVariantKey& other) const;

		std::string ToString() const;
	};
}
