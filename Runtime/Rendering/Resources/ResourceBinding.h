#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Rendering/RHI/RHITypes.h"
#include "Rendering/Resources/ShaderReflection.h"
#include "RenderDef.h"

namespace NLS::Render::Resources
{
	class Texture;
}

namespace NLS::Render::RHI
{
	class IRHIResource;
	class IRHITexture;
	class IRHIBuffer;
	class RHITexture;
	class RHIBuffer;
}

namespace NLS::Render::Resources
{
	struct NLS_RENDER_API ResourceBindingDesc
	{
		std::string name;
		ShaderResourceKind kind = ShaderResourceKind::Value;
		UniformType type = UniformType::UNIFORM_FLOAT;
		uint32_t bindingSpace = 0;
		uint32_t bindingIndex = 0;
		int32_t slot = -1;
	};

	struct NLS_RENDER_API ResourceBindingLayout
	{
		std::vector<ResourceBindingDesc> bindings;
	};

	struct NLS_RENDER_API ResourceBindingEntry
	{
		std::string name;
		ShaderResourceKind kind = ShaderResourceKind::Value;
		uint32_t bindingSpace = 0;
		uint32_t bindingIndex = 0;
		int32_t slot = -1;
		const RHI::IRHIResource* resource = nullptr;
		const RHI::IRHITexture* textureResource = nullptr;
		const RHI::IRHIBuffer* bufferResource = nullptr;
		std::shared_ptr<RHI::RHITexture> textureHandle;
		std::shared_ptr<RHI::RHIBuffer> bufferHandle;
		const Texture* texture = nullptr;
		RHI::SamplerDesc sampler{};
		bool hasSampler = false;
	};
}
