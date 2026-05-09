#include "Rendering/RHI/Backends/DX12/DX12DebugNameUtils.h"

#include <sstream>

#include "Rendering/RHI/RHITypes.h"

namespace NLS::Render::RHI::DX12
{
	namespace
	{
		std::string TextureDimensionToLabel(TextureDimension dimension)
		{
			switch (dimension)
			{
			case TextureDimension::Texture1D: return "Texture1D";
			case TextureDimension::Texture2D: return "Texture2D";
			case TextureDimension::Texture2DArray: return "Texture2DArray";
			case TextureDimension::Texture3D: return "Texture3D";
			case TextureDimension::TextureCube: return "TextureCube";
			case TextureDimension::TextureCubeArray: return "TextureCubeArray";
			default: return "Texture";
			}
		}

		std::string TextureFormatToLabel(TextureFormat format)
		{
			switch (format)
			{
			case TextureFormat::R8: return "R8";
			case TextureFormat::RG8: return "RG8";
			case TextureFormat::RGB8: return "RGB8";
			case TextureFormat::RGBA8: return "RGBA8";
			case TextureFormat::R16F: return "R16F";
			case TextureFormat::RG16F: return "RG16F";
			case TextureFormat::RGBA16F: return "RGBA16F";
			case TextureFormat::R32F: return "R32F";
			case TextureFormat::RG32F: return "RG32F";
			case TextureFormat::RGBA32F: return "RGBA32F";
			case TextureFormat::Depth24Stencil8: return "Depth24Stencil8";
			case TextureFormat::Depth32F: return "Depth32F";
			default: return "UnknownFormat";
			}
		}

		std::string BufferUsageToLabel(BufferUsageFlags usage)
		{
			std::string label;
			const auto append = [&label](std::string_view value)
			{
				if (!label.empty())
					label += "|";
				label += value;
			};

			if (HasBufferUsage(usage, BufferUsageFlags::Vertex))
				append("Vertex");
			if (HasBufferUsage(usage, BufferUsageFlags::Index))
				append("Index");
			if (HasBufferUsage(usage, BufferUsageFlags::Uniform))
				append("Uniform");
			if (HasBufferUsage(usage, BufferUsageFlags::Storage))
				append("Storage");
			if (HasBufferUsage(usage, BufferUsageFlags::CopySrc))
				append("CopySrc");
			if (HasBufferUsage(usage, BufferUsageFlags::CopyDst))
				append("CopyDst");
			if (label.empty())
				label = "None";
			return label;
		}

		std::string ShaderDebugName(const std::shared_ptr<RHIShaderModule>& shader)
		{
			if (shader == nullptr)
				return "none";

			const auto name = shader->GetDebugName();
			if (!name.empty())
				return std::string(name);

			const auto& desc = shader->GetDesc();
			if (!desc.debugName.empty())
				return desc.debugName;
			return desc.entryPoint.empty() ? "unnamed" : desc.entryPoint;
		}

		std::string SemanticNameOrFallback(std::string_view name, std::string_view fallback)
		{
			return name.empty() ? std::string(fallback) : std::string(name);
		}
	}

	std::string BuildDX12BufferDebugLabel(const RHIBufferDesc& desc)
	{
		std::ostringstream stream;
		stream << "DX12 Buffer";
		stream << " \"" << SemanticNameOrFallback(desc.debugName, "UnnamedBuffer") << "\"";
		stream << " " << desc.size << "B";
		stream << " usage=" << BufferUsageToLabel(desc.usage);
		return stream.str();
	}

	std::string BuildDX12TextureDebugLabel(const RHITextureDesc& desc)
	{
		std::ostringstream stream;
		stream << "DX12 " << TextureDimensionToLabel(desc.dimension);
		stream << " \"" << SemanticNameOrFallback(desc.debugName, "UnnamedTexture") << "\"";
		stream << " " << desc.extent.width << "x" << desc.extent.height << "x" << desc.extent.depth;
		stream << " " << TextureFormatToLabel(desc.format);
		stream << " mips=" << desc.mipLevels;
		stream << " layers=" << GetTextureLayerCount(desc.dimension, desc.arrayLayers);
		if (desc.sampleCount > 1u)
			stream << " MSAAx" << desc.sampleCount;
		return stream.str();
	}

	std::string BuildDX12TextureViewDebugLabel(const RHITextureViewDesc& desc, std::string_view textureDebugName)
	{
		std::ostringstream stream;
		stream << "DX12 TextureView";
		stream << " \"" << SemanticNameOrFallback(desc.debugName, "UnnamedTextureView") << "\"";
		if (!textureDebugName.empty())
			stream << " of \"" << textureDebugName << "\"";
		stream << " " << TextureFormatToLabel(desc.format);
		return stream.str();
	}

	std::string BuildDX12GraphicsPipelineDebugLabel(const RHIGraphicsPipelineDesc& desc, std::string_view stableCacheKey)
	{
		std::ostringstream stream;
		stream << "DX12 GraphicsPSO";
		stream << " \"" << SemanticNameOrFallback(desc.debugName, "UnnamedGraphicsPipeline") << "\"";
		stream << " VS=" << ShaderDebugName(desc.vertexShader);
		stream << " PS=" << ShaderDebugName(desc.fragmentShader);
		stream << " RTs=" << desc.renderTargetLayout.colorFormats.size();
		if (desc.renderTargetLayout.hasDepth)
			stream << " Depth=" << TextureFormatToLabel(desc.renderTargetLayout.depthFormat);
		if (!stableCacheKey.empty())
			stream << " key=" << stableCacheKey;
		return stream.str();
	}

	std::string BuildDX12ComputePipelineDebugLabel(const RHIComputePipelineDesc& desc, std::string_view stableCacheKey)
	{
		std::ostringstream stream;
		stream << "DX12 ComputePSO";
		stream << " \"" << SemanticNameOrFallback(desc.debugName, "UnnamedComputePipeline") << "\"";
		stream << " CS=" << ShaderDebugName(desc.computeShader);
		if (!stableCacheKey.empty())
			stream << " key=" << stableCacheKey;
		return stream.str();
	}
}
