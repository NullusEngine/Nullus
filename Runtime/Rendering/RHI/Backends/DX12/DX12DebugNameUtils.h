#pragma once

#include <string>
#include <string_view>

#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/RHI/Core/RHIResource.h"

namespace NLS::Render::RHI::DX12
{
	NLS_RENDER_API std::string BuildDX12BufferDebugLabel(const RHIBufferDesc& desc);
	NLS_RENDER_API std::string BuildDX12TextureDebugLabel(const RHITextureDesc& desc);
	NLS_RENDER_API std::string BuildDX12TextureViewDebugLabel(const RHITextureViewDesc& desc, std::string_view textureDebugName);
	NLS_RENDER_API std::string BuildDX12GraphicsPipelineDebugLabel(const RHIGraphicsPipelineDesc& desc, std::string_view stableCacheKey);
	NLS_RENDER_API std::string BuildDX12ComputePipelineDebugLabel(const RHIComputePipelineDesc& desc, std::string_view stableCacheKey);
}
