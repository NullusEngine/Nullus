#pragma once

#include <cstddef>
#include <cstdint>

namespace NLS::Render::Data
{
	constexpr size_t kSceneOcclusionPrimitivePacketScreenMinXOffset = 0u;
	constexpr size_t kSceneOcclusionPrimitivePacketScreenMinYOffset = kSceneOcclusionPrimitivePacketScreenMinXOffset + sizeof(float);
	constexpr size_t kSceneOcclusionPrimitivePacketScreenMaxXOffset = kSceneOcclusionPrimitivePacketScreenMinYOffset + sizeof(float);
	constexpr size_t kSceneOcclusionPrimitivePacketScreenMaxYOffset = kSceneOcclusionPrimitivePacketScreenMaxXOffset + sizeof(float);
	constexpr size_t kSceneOcclusionPrimitivePacketNearestDepthOffset = kSceneOcclusionPrimitivePacketScreenMaxYOffset + sizeof(float);
	constexpr size_t kSceneOcclusionPrimitivePacketFlagsOffset = kSceneOcclusionPrimitivePacketNearestDepthOffset + sizeof(float);
	constexpr uint32_t kSceneOcclusionPrimitivePacketFloatCount = 5u;
	constexpr uint32_t kSceneOcclusionPrimitivePacketFlagCount = 1u;
	constexpr size_t kSceneOcclusionPrimitivePacketStride =
		static_cast<size_t>(kSceneOcclusionPrimitivePacketFloatCount) * sizeof(float) +
		static_cast<size_t>(kSceneOcclusionPrimitivePacketFlagCount) * sizeof(uint32_t);
	static_assert(kSceneOcclusionPrimitivePacketFlagsOffset + sizeof(uint32_t) == kSceneOcclusionPrimitivePacketStride);
}
