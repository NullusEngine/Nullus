#include "Rendering/Resources/ShaderBindingLayoutUtils.h"

#include <algorithm>
#include <map>
#include <optional>
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
		const NLS::Render::RHI::ShaderStageMask stageMask,
		const uint32_t elementStride = 0u)
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
			existingEntry->elementStride = std::max(existingEntry->elementStride, elementStride);
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
			registerSpace,
			elementStride
		});
	}

	uint32_t ResolveResourceElementStride(
		const NLS::Render::RHI::BindingType bindingType,
		const uint32_t reflectedByteSize)
	{
		if (bindingType != NLS::Render::RHI::BindingType::StructuredBuffer &&
			bindingType != NLS::Render::RHI::BindingType::StorageBuffer)
		{
			return 0u;
		}

		return reflectedByteSize != 0u ? reflectedByteSize : sizeof(uint32_t);
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

	NLS::Render::Resources::ShaderParameterGroupKind ToShaderParameterGroupKind(const uint32_t registerSpace)
	{
		switch (registerSpace)
		{
		case NLS::Render::RHI::BindingPointMap::kFrameBindingSpace:
			return NLS::Render::Resources::ShaderParameterGroupKind::Frame;
		case NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace:
			return NLS::Render::Resources::ShaderParameterGroupKind::Material;
		case NLS::Render::RHI::BindingPointMap::kObjectBindingSpace:
			return NLS::Render::Resources::ShaderParameterGroupKind::Object;
		case NLS::Render::RHI::BindingPointMap::kPassBindingSpace:
		default:
			return NLS::Render::Resources::ShaderParameterGroupKind::Pass;
		}
	}

	uint32_t GetShaderParameterGroupRegisterSpace(const NLS::Render::Resources::ShaderParameterGroupKind groupKind)
	{
		switch (groupKind)
		{
		case NLS::Render::Resources::ShaderParameterGroupKind::Frame:
			return NLS::Render::RHI::BindingPointMap::kFrameBindingSpace;
		case NLS::Render::Resources::ShaderParameterGroupKind::Material:
			return NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace;
		case NLS::Render::Resources::ShaderParameterGroupKind::Object:
			return NLS::Render::RHI::BindingPointMap::kObjectBindingSpace;
		case NLS::Render::Resources::ShaderParameterGroupKind::Pass:
		default:
			return NLS::Render::RHI::BindingPointMap::kPassBindingSpace;
		}
	}

	const char* GetShaderParameterGroupName(const NLS::Render::Resources::ShaderParameterGroupKind groupKind)
	{
		switch (groupKind)
		{
		case NLS::Render::Resources::ShaderParameterGroupKind::Frame: return "Frame";
		case NLS::Render::Resources::ShaderParameterGroupKind::Material: return "Material";
		case NLS::Render::Resources::ShaderParameterGroupKind::Object: return "Object";
		case NLS::Render::Resources::ShaderParameterGroupKind::Pass:
		default:
			return "Pass";
		}
	}

	NLS::Render::Resources::ShaderParameterBindingContract MakeShaderParameterBindingContract(
		const std::string& name,
		const NLS::Render::RHI::BindingType bindingType,
		const uint32_t registerSpace,
		const uint32_t bindingIndex,
		const uint32_t count,
		const NLS::Render::RHI::ShaderStageMask stageMask,
		const uint32_t elementStride = 0u)
	{
		return {
			name,
			bindingType,
			NLS::Render::RHI::BindingPointMap::GetDescriptorSetIndex(registerSpace),
			registerSpace,
			bindingIndex,
			count,
			elementStride,
			stageMask,
			true
		};
	}

	bool IsRendererOwnedObjectIndexConstantBuffer(
		const NLS::Render::Resources::ShaderConstantBufferDesc& constantBuffer)
	{
		return constantBuffer.name == "ObjectIndexConstants" &&
			constantBuffer.bindingSpace == NLS::Render::RHI::BindingPointMap::kObjectBindingSpace &&
			constantBuffer.bindingIndex == 1u &&
			constantBuffer.byteSize >= sizeof(uint32_t) &&
			constantBuffer.byteSize <= 16u;
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
			if (IsRendererOwnedObjectIndexConstantBuffer(constantBuffer))
				continue;

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
			if (IsRendererOwnedObjectIndexConstantBuffer(constantBuffer))
				continue;

			const uint32_t setIndex = RHI::BindingPointMap::GetDescriptorSetIndex(constantBuffer.bindingSpace);
			UpsertBindingLayoutEntry(
				layoutDescs[setIndex],
				constantBuffer.name,
				RHI::BindingType::UniformBuffer,
				setIndex,
				constantBuffer.bindingSpace,
				constantBuffer.bindingIndex,
				1u,
				ToShaderStageMask(constantBuffer.stage),
				0u);
		}

		for (const auto& property : reflection.properties)
		{
			if (property.kind == ShaderResourceKind::Value)
				continue;

			const uint32_t setIndex = RHI::BindingPointMap::GetDescriptorSetIndex(property.bindingSpace);
			const auto bindingType = ToBindingType(property.kind);
			UpsertBindingLayoutEntry(
				layoutDescs[setIndex],
				property.name,
				bindingType,
				setIndex,
				property.bindingSpace,
				property.bindingIndex,
				property.arraySize > 0 ? static_cast<uint32_t>(property.arraySize) : 1u,
				ToShaderStageMask(property.stage),
				ResolveResourceElementStride(bindingType, property.byteSize));
		}

		return layoutDescs;
	}

	std::vector<ShaderParameterGroupContract> BuildShaderParameterGroupContracts(
		const ShaderReflection& reflection,
		std::string_view debugNamePrefix)
	{
		if (ValidateShaderBindingReflection(reflection).HasErrors())
			return {};

		std::vector<ShaderParameterGroupContract> groups;
		groups.reserve(4u);
		for (const auto groupKind : {
			ShaderParameterGroupKind::Frame,
			ShaderParameterGroupKind::Material,
			ShaderParameterGroupKind::Object,
			ShaderParameterGroupKind::Pass })
		{
			const uint32_t registerSpace = GetShaderParameterGroupRegisterSpace(groupKind);
			const uint32_t descriptorSet = RHI::BindingPointMap::GetDescriptorSetIndex(registerSpace);
			groups.push_back({
				debugNamePrefix.empty()
					? std::string(GetShaderParameterGroupName(groupKind)) + "ShaderParameters"
					: std::string(debugNamePrefix) + ":" + GetShaderParameterGroupName(groupKind) + "ShaderParameters",
				groupKind,
				descriptorSet,
				registerSpace,
				true,
				{}
			});
		}

		auto findGroup = [&groups](const ShaderParameterGroupKind groupKind) -> ShaderParameterGroupContract&
		{
			auto found = std::find_if(
				groups.begin(),
				groups.end(),
				[groupKind](const ShaderParameterGroupContract& group)
				{
					return group.groupKind == groupKind;
				});
			return *found;
		};

		for (const auto& constantBuffer : reflection.constantBuffers)
		{
			if (IsRendererOwnedObjectIndexConstantBuffer(constantBuffer))
				continue;

			auto& group = findGroup(ToShaderParameterGroupKind(constantBuffer.bindingSpace));
			group.parameters.push_back(MakeShaderParameterBindingContract(
				constantBuffer.name,
				RHI::BindingType::UniformBuffer,
				constantBuffer.bindingSpace,
				constantBuffer.bindingIndex,
				1u,
				ToShaderStageMask(constantBuffer.stage),
				0u));
		}

		for (const auto& property : reflection.properties)
		{
			if (property.kind == ShaderResourceKind::Value)
				continue;

			auto& group = findGroup(ToShaderParameterGroupKind(property.bindingSpace));
			const auto bindingType = ToBindingType(property.kind);
			group.parameters.push_back(MakeShaderParameterBindingContract(
				property.name,
				bindingType,
				property.bindingSpace,
				property.bindingIndex,
				property.arraySize > 0 ? static_cast<uint32_t>(property.arraySize) : 1u,
				ToShaderStageMask(property.stage),
				ResolveResourceElementStride(bindingType, property.byteSize)));
		}

		return groups;
	}

	ShaderBindingValidationResult ValidateShaderParameterGroupResources(
		const std::vector<ShaderParameterGroupContract>& groups,
		const std::vector<ShaderParameterBindingResourceState>& resources)
	{
		ShaderBindingValidationResult result;

		auto findResource = [&resources](const ShaderParameterGroupKind groupKind, const ShaderParameterBindingContract& parameter)
			-> std::optional<ShaderParameterBindingResourceState>
		{
			const auto found = std::find_if(
				resources.begin(),
				resources.end(),
				[groupKind, &parameter](const ShaderParameterBindingResourceState& resource)
				{
					return resource.groupKind == groupKind &&
						resource.descriptorSet == parameter.descriptorSet &&
						resource.binding == parameter.binding &&
						resource.type == parameter.type &&
						resource.name == parameter.name;
				});

			if (found == resources.end())
				return std::nullopt;

			return *found;
		};

		for (const auto& group : groups)
		{
			for (const auto& parameter : group.parameters)
			{
				if (!group.required || !parameter.required)
					continue;

				const auto resource = findResource(group.groupKind, parameter);
				if (!resource.has_value() || !resource->hasResource)
				{
					AddValidationError(
						result,
						"Shader parameter binding missing required resource \"" +
						parameter.name + "\" in " + GetShaderParameterGroupName(group.groupKind) + " group.");
					continue;
				}

				if (resource->resourceVersion < resource->requiredVersion)
				{
					AddValidationError(
						result,
						"Shader parameter binding stale resource \"" +
						parameter.name + "\" in " + GetShaderParameterGroupName(group.groupKind) +
						" group: resourceVersion " + std::to_string(resource->resourceVersion) +
						" is older than requiredVersion " + std::to_string(resource->requiredVersion) + ".");
				}
			}
		}

		return result;
	}
}
