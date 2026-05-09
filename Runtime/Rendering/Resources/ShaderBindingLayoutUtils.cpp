#include "Rendering/Resources/ShaderBindingLayoutUtils.h"

#include <algorithm>
#include <map>
#include <string>
#include <tuple>

#include "Rendering/RHI/BindingPointMap.h"

namespace
{
	NLS::Render::RHI::ShaderStageMask ToShaderStageMask(const NLS::Render::ShaderCompiler::ShaderStage stage)
	{
		switch (stage)
		{
		case NLS::Render::ShaderCompiler::ShaderStage::Vertex: return NLS::Render::RHI::ShaderStageMask::Vertex;
		case NLS::Render::ShaderCompiler::ShaderStage::Compute: return NLS::Render::RHI::ShaderStageMask::Compute;
		case NLS::Render::ShaderCompiler::ShaderStage::Pixel:
		default:
			return NLS::Render::RHI::ShaderStageMask::Fragment;
		}
	}

	NLS::Render::RHI::BindingType ToBindingType(const NLS::Render::Resources::ShaderResourceKind kind)
	{
		switch (kind)
		{
		case NLS::Render::Resources::ShaderResourceKind::UniformBuffer: return NLS::Render::RHI::BindingType::UniformBuffer;
		case NLS::Render::Resources::ShaderResourceKind::StructuredBuffer: return NLS::Render::RHI::BindingType::StructuredBuffer;
		case NLS::Render::Resources::ShaderResourceKind::StorageBuffer: return NLS::Render::RHI::BindingType::StorageBuffer;
		case NLS::Render::Resources::ShaderResourceKind::SampledTexture: return NLS::Render::RHI::BindingType::Texture;
		case NLS::Render::Resources::ShaderResourceKind::Sampler:
		default:
			return NLS::Render::RHI::BindingType::Sampler;
		}
	}

	void UpsertBindingLayoutEntry(
		NLS::Render::RHI::RHIBindingLayoutDesc& layoutDesc,
		const std::string& name,
		const NLS::Render::RHI::BindingType bindingType,
		const uint32_t setIndex,
		const uint32_t registerSpace,
		const uint32_t bindingIndex,
		const uint32_t count,
		const NLS::Render::RHI::ShaderStageMask stageMask)
	{
		auto existingEntry = std::find_if(
			layoutDesc.entries.begin(),
			layoutDesc.entries.end(),
			[&name, bindingType, setIndex, registerSpace, bindingIndex](const NLS::Render::RHI::RHIBindingLayoutEntry& entry)
			{
				return entry.name == name &&
					entry.type == bindingType &&
					entry.set == setIndex &&
					entry.registerSpace == registerSpace &&
					entry.binding == bindingIndex;
			});

		if (existingEntry != layoutDesc.entries.end())
		{
			existingEntry->count = std::max(existingEntry->count, count);
			existingEntry->stageMask = static_cast<NLS::Render::RHI::ShaderStageMask>(
				static_cast<uint32_t>(existingEntry->stageMask) | static_cast<uint32_t>(stageMask));
			return;
		}

		layoutDesc.entries.push_back({
			name,
			bindingType,
			setIndex,
			bindingIndex,
			count,
			stageMask,
			registerSpace
		});
	}

	std::string BindingCoordinate(
		const uint32_t registerSpace,
		const uint32_t bindingIndex)
	{
		return "space" + std::to_string(registerSpace) + "/binding" + std::to_string(bindingIndex);
	}

	void AddValidationError(
		NLS::Render::Resources::ShaderBindingValidationResult& result,
		std::string message)
	{
		result.diagnostics.push_back({
			NLS::Render::Resources::ShaderBindingValidationSeverity::Error,
			std::move(message)
		});
	}
}

namespace NLS::Render::Resources
{
	ShaderBindingValidationResult ValidateShaderBindingReflection(
		const ShaderReflection& reflection)
	{
		ShaderBindingValidationResult result;
		struct SeenBinding
		{
			std::string name;
			RHI::BindingType type = RHI::BindingType::UniformBuffer;
		};
		std::map<std::tuple<uint32_t, uint32_t, uint32_t, RHI::BindingType>, SeenBinding> seenBindings;

		auto validateBinding = [&result, &seenBindings](
			const std::string& name,
			const RHI::BindingType bindingType,
			const uint32_t registerSpace,
			const uint32_t bindingIndex,
			const int32_t arraySize)
		{
			if (name.empty())
			{
				AddValidationError(
					result,
					"Shader binding reflection contains an unnamed resource at " +
					BindingCoordinate(registerSpace, bindingIndex) + ".");
			}

			if (arraySize <= 0)
			{
				AddValidationError(
					result,
					"Shader binding \"" + name + "\" has invalid arraySize " +
					std::to_string(arraySize) + " at " +
					BindingCoordinate(registerSpace, bindingIndex) + ".");
			}

			const uint32_t setIndex = RHI::BindingPointMap::GetDescriptorSetIndex(registerSpace);
			const auto key = std::make_tuple(setIndex, registerSpace, bindingIndex, bindingType);
			if (const auto found = seenBindings.find(key); found != seenBindings.end())
			{
				if (found->second.name != name)
				{
					AddValidationError(
						result,
						"Shader binding reflection conflict at " +
						BindingCoordinate(registerSpace, bindingIndex) +
						": \"" + found->second.name + "\" and \"" + name +
						"\" map to the same descriptor slot.");
				}
				return;
			}

			seenBindings.emplace(key, SeenBinding{ name, bindingType });
		};

		for (const auto& constantBuffer : reflection.constantBuffers)
		{
			if (constantBuffer.byteSize == 0)
			{
				AddValidationError(
					result,
					"Shader constant buffer \"" + constantBuffer.name +
					"\" has zero byteSize at " +
					BindingCoordinate(constantBuffer.bindingSpace, constantBuffer.bindingIndex) + ".");
			}

			validateBinding(
				constantBuffer.name,
				RHI::BindingType::UniformBuffer,
				constantBuffer.bindingSpace,
				constantBuffer.bindingIndex,
				1);
		}

		for (const auto& property : reflection.properties)
		{
			if (property.kind == ShaderResourceKind::Value)
				continue;

			validateBinding(
				property.name,
				ToBindingType(property.kind),
				property.bindingSpace,
				property.bindingIndex,
				property.arraySize);
		}

		return result;
	}

	std::vector<RHI::RHIBindingLayoutDesc> BuildExplicitBindingLayoutDescsBySet(
		const ShaderReflection& reflection,
		std::string_view debugNamePrefix)
	{
		if (ValidateShaderBindingReflection(reflection).HasErrors())
			return {};

		uint32_t maxSetIndex = 0u;
		bool hasAnyBinding = false;

		for (const auto& constantBuffer : reflection.constantBuffers)
		{
			maxSetIndex = std::max(maxSetIndex, RHI::BindingPointMap::GetDescriptorSetIndex(constantBuffer.bindingSpace));
			hasAnyBinding = true;
		}

		for (const auto& property : reflection.properties)
		{
			if (property.kind == ShaderResourceKind::Value)
				continue;

			maxSetIndex = std::max(maxSetIndex, RHI::BindingPointMap::GetDescriptorSetIndex(property.bindingSpace));
			hasAnyBinding = true;
		}

		if (!hasAnyBinding)
			return {};

		std::vector<RHI::RHIBindingLayoutDesc> layoutDescs(maxSetIndex + 1u);
		for (uint32_t setIndex = 0u; setIndex < layoutDescs.size(); ++setIndex)
		{
			layoutDescs[setIndex].debugName = debugNamePrefix.empty()
				? "ExplicitSet" + std::to_string(setIndex) + "BindingLayout"
				: std::string(debugNamePrefix) + ":Set" + std::to_string(setIndex) + "BindingLayout";
		}

		for (const auto& constantBuffer : reflection.constantBuffers)
		{
			const uint32_t setIndex = RHI::BindingPointMap::GetDescriptorSetIndex(constantBuffer.bindingSpace);
			UpsertBindingLayoutEntry(
				layoutDescs[setIndex],
				constantBuffer.name,
				RHI::BindingType::UniformBuffer,
				setIndex,
				constantBuffer.bindingSpace,
				constantBuffer.bindingIndex,
				1u,
				ToShaderStageMask(constantBuffer.stage));
		}

		for (const auto& property : reflection.properties)
		{
			if (property.kind == ShaderResourceKind::Value)
				continue;

			const uint32_t setIndex = RHI::BindingPointMap::GetDescriptorSetIndex(property.bindingSpace);
			UpsertBindingLayoutEntry(
				layoutDescs[setIndex],
				property.name,
				ToBindingType(property.kind),
				setIndex,
				property.bindingSpace,
				property.bindingIndex,
				property.arraySize > 0 ? static_cast<uint32_t>(property.arraySize) : 1u,
				ToShaderStageMask(property.stage));
		}

		return layoutDescs;
	}
}
