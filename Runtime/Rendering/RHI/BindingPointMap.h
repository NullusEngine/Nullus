#pragma once

#include <cstdint>

namespace NLS::Render::RHI
{
	namespace BindingPointMap
	{
		constexpr uint32_t kFrameBindingSpace = 0u;
		constexpr uint32_t kPassBindingSpace = 1u;
		constexpr uint32_t kMaterialBindingSpace = 2u;
		constexpr uint32_t kObjectBindingSpace = 3u;

		inline constexpr uint32_t GetDescriptorSetIndex(uint32_t bindingSpace)
		{
			switch (bindingSpace)
			{
			case kFrameBindingSpace: return 0u;
			case kMaterialBindingSpace: return 1u;
			case kObjectBindingSpace: return 2u;
			case kPassBindingSpace: return 3u;
			default: return bindingSpace;
			}
		}

		constexpr uint32_t kFrameDescriptorSet = GetDescriptorSetIndex(kFrameBindingSpace);
		constexpr uint32_t kMaterialDescriptorSet = GetDescriptorSetIndex(kMaterialBindingSpace);
		constexpr uint32_t kObjectDescriptorSet = GetDescriptorSetIndex(kObjectBindingSpace);
		constexpr uint32_t kPassDescriptorSet = GetDescriptorSetIndex(kPassBindingSpace);

		constexpr uint32_t kUniformBufferBase = 8u;
		constexpr uint32_t kUniformBufferSpaceStride = 4u;
		constexpr uint32_t kTextureSpaceStride = 16u;

		inline constexpr uint32_t GetUniformBufferBindingPoint(uint32_t bindingSpace, uint32_t bindingIndex)
		{
			return kUniformBufferBase + bindingSpace * kUniformBufferSpaceStride + bindingIndex;
		}

		inline constexpr uint32_t GetTextureBindingPoint(uint32_t bindingSpace, uint32_t bindingIndex)
		{
			return bindingSpace * kTextureSpaceStride + bindingIndex;
		}
	}
}
