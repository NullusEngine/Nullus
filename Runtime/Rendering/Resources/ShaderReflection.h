#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Rendering/Resources/UniformType.h"
#include "Rendering/ShaderCompiler/ShaderCompilationTypes.h"
#include "RenderDef.h"

namespace NLS::Render::Resources
{
	enum class NLS_RENDER_API ShaderResourceKind : uint8_t
	{
		Value,
		SampledTexture,
		Sampler,
		UniformBuffer,
		StructuredBuffer,
		StorageBuffer
	};

	struct NLS_RENDER_API ShaderCBufferMemberDesc
	{
		std::string name;
		UniformType type = UniformType::UNIFORM_FLOAT;
		uint32_t byteOffset = 0;
		uint32_t byteSize = 0;
		uint32_t arraySize = 1;
	};

	struct NLS_RENDER_API ShaderConstantBufferDesc
	{
		std::string name;
		ShaderCompiler::ShaderStage stage = ShaderCompiler::ShaderStage::Vertex;
		uint32_t bindingSpace = 0;
		uint32_t bindingIndex = 0;
		uint32_t byteSize = 0;
		std::vector<ShaderCBufferMemberDesc> members;
	};

	struct NLS_RENDER_API ShaderPropertyDesc
	{
		std::string name;
		UniformType type = UniformType::UNIFORM_FLOAT;
		ShaderResourceKind kind = ShaderResourceKind::Value;
		ShaderCompiler::ShaderStage stage = ShaderCompiler::ShaderStage::Vertex;
		uint32_t bindingSpace = 0;
		uint32_t bindingIndex = 0;
		int32_t location = -1;
		int32_t arraySize = 1;
		uint32_t byteOffset = 0;
		uint32_t byteSize = 0;
		std::string parentConstantBuffer;
	};

	struct NLS_RENDER_API ShaderReflection
	{
		std::vector<ShaderPropertyDesc> properties;
		std::vector<ShaderConstantBufferDesc> constantBuffers;
	};
}
