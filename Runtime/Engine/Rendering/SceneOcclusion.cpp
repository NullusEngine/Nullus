#include "Rendering/SceneOcclusion.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#include <Math/Vector4.h>

#include "Rendering/Geometry/BoundingSphereUtils.h"
#include "Rendering/RHI/Core/RHIDevice.h"

namespace NLS::Engine::Rendering
{
	namespace
	{
		using NLS::Render::RHI::RHIDeviceCapabilities;
		using NLS::Render::RHI::RHIDeviceFeature;
		using NLS::Render::RHI::TextureFormatCapability;

		constexpr size_t kMaxHistoryKeysPerPrimitive = 32u;
		constexpr float kProjectionEpsilon = 0.00001f;

		void HashCombine(size_t& seed, const size_t value)
		{
			seed ^= value + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
		}

		void HashHandle(size_t& seed, const ScenePrimitiveHandle& handle)
		{
			HashCombine(seed, static_cast<size_t>(handle.sceneId));
			HashCombine(seed, static_cast<size_t>(handle.index));
			HashCombine(seed, static_cast<size_t>(handle.generation));
		}

		uint64_t ToNonZeroGeneration(const size_t hash)
		{
			const auto generation = static_cast<uint64_t>(hash);
			return generation != 0u ? generation : 1u;
		}

		void HashFloat(size_t& seed, const float value)
		{
			uint32_t bits = 0u;
			std::memcpy(&bits, &value, sizeof(bits));
			HashCombine(seed, static_cast<size_t>(bits));
		}

		uint64_t HashBoundsGeneration(const ScenePrimitiveSnapshotRecord& record)
		{
			size_t seed = 0u;
			HashFloat(seed, record.modelBounds.center.x);
			HashFloat(seed, record.modelBounds.center.y);
			HashFloat(seed, record.modelBounds.center.z);
			HashFloat(seed, record.modelBounds.size.x);
			HashFloat(seed, record.modelBounds.size.y);
			HashFloat(seed, record.modelBounds.size.z);
			return ToNonZeroGeneration(seed);
		}

		uint64_t HashTransformGeneration(const ScenePrimitiveSnapshotRecord& record)
		{
			size_t seed = 0u;
			for (const float value : record.worldMatrix.data)
				HashFloat(seed, value);
			return ToNonZeroGeneration(seed);
		}

		uint64_t HashRepresentationId(const ScenePrimitiveSnapshotRecord& record)
		{
			size_t seed = 0u;
			HashHandle(seed, record.handle);
			HashCombine(seed, record.lodGroup.has_value() ? static_cast<size_t>(record.lodGroup->index) : static_cast<size_t>(~0u));
			HashCombine(seed, record.hlodCluster.has_value() ? static_cast<size_t>(record.hlodCluster->index) : static_cast<size_t>(~0u));
			return ToNonZeroGeneration(seed);
		}

		uint64_t HashDepthWriteEligibilityGeneration(const ScenePrimitiveSnapshotRecord& record)
		{
			size_t seed = 0u;
			HashHandle(seed, record.handle);
			HashCombine(seed, record.depthWriteEligibleForOcclusion ? 1u : 0u);
			return ToNonZeroGeneration(seed);
		}

		bool SameViewIdentity(const SceneOcclusionHistoryKey& key, const SceneOcclusionHistoryKey& candidate)
		{
			return key.viewKey == candidate.viewKey &&
				key.viewCompatibilityHash == candidate.viewCompatibilityHash &&
				key.projectionHash == candidate.projectionHash &&
				key.jitterHash == candidate.jitterHash &&
				key.depthFormatKey == candidate.depthFormatKey &&
				key.viewportWidth == candidate.viewportWidth &&
				key.viewportHeight == candidate.viewportHeight;
		}

		bool SamePrimitiveIdentity(const SceneOcclusionHistoryKey& key, const SceneOcclusionHistoryKey& candidate)
		{
			return key.boundsGeneration == candidate.boundsGeneration &&
				key.transformGeneration == candidate.transformGeneration &&
				key.depthWriteEligibilityGeneration == candidate.depthWriteEligibilityGeneration;
		}

		bool ProjectToPacketSample(
			const SceneOcclusionPrimitivePacketBuildInput& input,
			const Maths::Vector3& worldPoint,
			float& screenX,
			float& screenY,
			float& depth)
		{
			const auto clip = input.viewProjection * Maths::Vector4(worldPoint, 1.0f);
			if (!std::isfinite(clip.w) || clip.w <= 0.0f)
				return false;

			const float ndcX = clip.x / clip.w;
			const float ndcY = clip.y / clip.w;
			const float ndcZ = clip.z / clip.w;
			if (!std::isfinite(ndcX) || !std::isfinite(ndcY) || !std::isfinite(ndcZ))
				return false;

			screenX = (ndcX * 0.5f + 0.5f) * static_cast<float>(input.viewportWidth);
			screenY = (0.5f - ndcY * 0.5f) * static_cast<float>(input.viewportHeight);
			depth = std::clamp(ndcZ, 0.0f, 1.0f);
			return true;
		}

		std::optional<SceneOcclusionPrimitivePacket> BuildPrimitivePacket(
			const SceneOcclusionPrimitivePacketBuildInput& input,
			const SceneOcclusionPrimitivePacketSource& source)
		{
			if (!source.primitive.depthWriteEligible || input.viewportWidth == 0u || input.viewportHeight == 0u)
				return std::nullopt;

			const auto modelCorners = NLS::Render::Geometry::BuildBoundsCorners(source.modelBounds);

			SceneOcclusionPrimitivePacket packet;
			packet.screenMinX = std::numeric_limits<float>::max();
			packet.screenMinY = std::numeric_limits<float>::max();
			packet.screenMaxX = std::numeric_limits<float>::lowest();
			packet.screenMaxY = std::numeric_limits<float>::lowest();
			packet.nearestDepth = 1.0f;
			packet.flags = 1u;

			for (const auto& modelCorner : modelCorners)
			{
				const auto sample = NLS::Render::Geometry::TransformPoint(source.worldMatrix, modelCorner);
				float screenX = 0.0f;
				float screenY = 0.0f;
				float depth = 1.0f;
				if (!ProjectToPacketSample(input, sample, screenX, screenY, depth))
					return std::nullopt;

				packet.screenMinX = std::min(packet.screenMinX, screenX);
				packet.screenMinY = std::min(packet.screenMinY, screenY);
				packet.screenMaxX = std::max(packet.screenMaxX, screenX);
				packet.screenMaxY = std::max(packet.screenMaxY, screenY);
				packet.nearestDepth = std::min(packet.nearestDepth, depth);
			}

			const float maxX = static_cast<float>(input.viewportWidth - 1u);
			const float maxY = static_cast<float>(input.viewportHeight - 1u);
			if (packet.screenMaxX < 0.0f ||
				packet.screenMaxY < 0.0f ||
				packet.screenMinX > maxX ||
				packet.screenMinY > maxY)
			{
				return std::nullopt;
			}
			packet.screenMinX = std::clamp(packet.screenMinX, 0.0f, maxX);
			packet.screenMinY = std::clamp(packet.screenMinY, 0.0f, maxY);
			packet.screenMaxX = std::clamp(packet.screenMaxX, 0.0f, maxX);
			packet.screenMaxY = std::clamp(packet.screenMaxY, 0.0f, maxY);
			return packet;
		}

		SceneOcclusionFallbackReason ClassifyMissingHistory(
			const SceneOcclusionHistoryKey& key,
			const std::vector<SceneOcclusionHistoryKey>& previousKeys)
		{
			for (const auto& previous : previousKeys)
			{
				if (!SameViewIdentity(key, previous))
					return SceneOcclusionFallbackReason::ViewChanged;
			}
			for (const auto& previous : previousKeys)
			{
				if (key.representationId != previous.representationId)
					return SceneOcclusionFallbackReason::RepresentationChanged;
			}
			for (const auto& previous : previousKeys)
			{
				if (!SamePrimitiveIdentity(key, previous))
					return SceneOcclusionFallbackReason::PrimitiveChanged;
			}
			return SceneOcclusionFallbackReason::MissingHistory;
		}

		bool ContainsHistoryKey(
			const std::vector<SceneOcclusionHistoryKey>& keys,
			const SceneOcclusionHistoryKey& key)
		{
			return std::find(keys.begin(), keys.end(), key) != keys.end();
		}

		template <typename OccludedFrameMap>
		void PruneHistoryKeys(
			std::vector<SceneOcclusionHistoryKey>& keys,
			OccludedFrameMap& occludedFrameByKey)
		{
			while (keys.size() > kMaxHistoryKeysPerPrimitive)
			{
				occludedFrameByKey.erase(keys.front());
				keys.erase(keys.begin());
			}
		}

		template <typename OccludedFrameMap>
		void RememberHistoryKey(
			std::vector<SceneOcclusionHistoryKey>& keys,
			OccludedFrameMap& occludedFrameByKey,
			const SceneOcclusionHistoryKey& key)
		{
			if (!ContainsHistoryKey(keys, key))
				keys.push_back(key);
			PruneHistoryKeys(keys, occludedFrameByKey);
		}

		SceneOcclusionCapabilitySupport Unsupported(
			const SceneOcclusionFallbackReason reason,
			std::string diagnostic)
		{
			SceneOcclusionCapabilitySupport support;
			support.backendSupported = false;
			support.fallbackReason = reason;
			support.diagnosticReason = std::move(diagnostic);
			return support;
		}

		SceneOcclusionCapabilitySupport Supported()
		{
			SceneOcclusionCapabilitySupport support;
			support.backendSupported = true;
			support.fallbackReason = SceneOcclusionFallbackReason::None;
			return support;
		}

		SceneOcclusionCapabilitySupport CheckFeature(
			const RHIDeviceCapabilities& capabilities,
			const RHIDeviceFeature feature,
			const SceneOcclusionFallbackReason reason,
			const char* defaultDiagnostic)
		{
			const auto state = capabilities.GetFeature(feature);
			if (state.supported)
				return Supported();
			return Unsupported(reason, state.reason.empty() ? std::string(defaultDiagnostic) : state.reason);
		}

		SceneOcclusionCapabilitySupport CheckTextureCapability(
			const TextureFormatCapability& capability,
			const SceneOcclusionFallbackReason reason,
			const char* defaultDiagnostic,
			const bool requireSampled,
			const bool requireStorage)
		{
			if ((!requireSampled || capability.sampled) && (!requireStorage || capability.storage))
				return Supported();
			return Unsupported(
				reason,
				capability.diagnosticReason.empty() ? std::string(defaultDiagnostic) : capability.diagnosticReason);
		}
	}

	size_t SceneOcclusionHistory::ScenePrimitiveHandleHash::operator()(const ScenePrimitiveHandle& handle) const noexcept
	{
		size_t seed = 0u;
		HashHandle(seed, handle);
		return seed;
	}

	size_t SceneOcclusionHistory::HistoryKeyHash::operator()(const SceneOcclusionHistoryKey& key) const noexcept
	{
		size_t seed = 0u;
		HashHandle(seed, key.handle);
		HashCombine(seed, static_cast<size_t>(key.viewKey));
		HashCombine(seed, static_cast<size_t>(key.viewCompatibilityHash));
		HashCombine(seed, static_cast<size_t>(key.projectionHash));
		HashCombine(seed, static_cast<size_t>(key.jitterHash));
		HashCombine(seed, static_cast<size_t>(key.depthFormatKey));
		HashCombine(seed, static_cast<size_t>(key.viewportWidth));
		HashCombine(seed, static_cast<size_t>(key.viewportHeight));
		HashCombine(seed, static_cast<size_t>(key.boundsGeneration));
		HashCombine(seed, static_cast<size_t>(key.transformGeneration));
		HashCombine(seed, static_cast<size_t>(key.representationId));
		HashCombine(seed, static_cast<size_t>(key.depthWriteEligibilityGeneration));
		return seed;
	}

	SceneOcclusionHistory::SceneOcclusionHistory() = default;

	SceneOcclusionHistory::~SceneOcclusionHistory() = default;

	void SceneOcclusionHistory::Clear()
	{
		m_occludedFrameByKey.clear();
		m_keysByHandle.clear();
	}

	void SceneOcclusionHistory::RecordOccluded(const SceneOcclusionHistoryKey& key, const uint64_t frameSerial)
	{
		auto& keys = m_keysByHandle[key.handle];
		m_occludedFrameByKey[key] = frameSerial;
		RememberHistoryKey(keys, m_occludedFrameByKey, key);
	}

	void SceneOcclusionHistory::RecordVisible(const SceneOcclusionHistoryKey& key)
	{
		m_occludedFrameByKey.erase(key);
		auto& keys = m_keysByHandle[key.handle];
		RememberHistoryKey(keys, m_occludedFrameByKey, key);
	}

	SceneOcclusionHistoryPruneStats SceneOcclusionHistory::PruneHandles(
		std::span<const ScenePrimitiveHandle> removedHandles)
	{
		SceneOcclusionHistoryPruneStats stats;
		stats.trackedHandleCountBefore = static_cast<uint64_t>(m_keysByHandle.size());
		stats.touchedHandleCount = static_cast<uint64_t>(removedHandles.size());

		for (const auto handle : removedHandles)
		{
			const auto found = m_keysByHandle.find(handle);
			if (found == m_keysByHandle.end())
				continue;

			stats.removedKeyCount += static_cast<uint64_t>(found->second.size());
			for (const auto& key : found->second)
				m_occludedFrameByKey.erase(key);
			m_keysByHandle.erase(found);
			++stats.removedHandleCount;
		}

		stats.trackedHandleCountAfter = static_cast<uint64_t>(m_keysByHandle.size());
		return stats;
	}

	SceneOcclusionHistoryPruneStats SceneOcclusionHistory::PruneByLiveHandleSweepForLifecycleFallback(
		std::span<const ScenePrimitiveHandle> liveHandles)
	{
		SceneOcclusionHistoryPruneStats stats;
		stats.trackedHandleCountBefore = static_cast<uint64_t>(m_keysByHandle.size());
		stats.touchedHandleCount =
			static_cast<uint64_t>(liveHandles.size()) + static_cast<uint64_t>(m_keysByHandle.size());

		std::unordered_set<ScenePrimitiveHandle, ScenePrimitiveHandleHash> liveHandleSet;
		liveHandleSet.reserve(liveHandles.size());
		for (const auto handle : liveHandles)
			liveHandleSet.insert(handle);

		for (auto iterator = m_keysByHandle.begin(); iterator != m_keysByHandle.end();)
		{
			if (liveHandleSet.find(iterator->first) != liveHandleSet.end())
			{
				++iterator;
				continue;
			}

			stats.removedKeyCount += static_cast<uint64_t>(iterator->second.size());
			for (const auto& key : iterator->second)
				m_occludedFrameByKey.erase(key);
			iterator = m_keysByHandle.erase(iterator);
			++stats.removedHandleCount;
		}

		stats.trackedHandleCountAfter = static_cast<uint64_t>(m_keysByHandle.size());
		return stats;
	}

	std::optional<uint64_t> SceneOcclusionHistory::FindOccludedFrame(const SceneOcclusionHistoryKey& key) const
	{
		const auto found = m_occludedFrameByKey.find(key);
		if (found == m_occludedFrameByKey.end())
			return std::nullopt;
		return found->second;
	}

	SceneOcclusionFallbackReason SceneOcclusionHistory::ClassifyMissingHistoryForHandle(
		const ScenePrimitiveHandle handle,
		const SceneOcclusionHistoryKey& key) const
	{
		const auto found = m_keysByHandle.find(handle);
		if (found == m_keysByHandle.end())
			return SceneOcclusionFallbackReason::MissingHistory;
		return ClassifyMissingHistory(key, found->second);
	}

	SceneOcclusionHistoryKey SceneOcclusionSystem::BuildHistoryKey(
		const SceneOcclusionFrameInput& frame,
		const SceneOcclusionPrimitiveInput& primitive)
	{
		SceneOcclusionHistoryKey key;
		key.handle = primitive.handle;
		key.viewKey = frame.viewKey;
		key.viewCompatibilityHash = frame.viewCompatibilityHash;
		key.projectionHash = frame.projectionHash;
		key.jitterHash = frame.jitterHash;
		key.depthFormatKey = frame.depthFormatKey;
		key.viewportWidth = frame.viewportWidth;
		key.viewportHeight = frame.viewportHeight;
		key.boundsGeneration = primitive.boundsGeneration;
		key.transformGeneration = primitive.transformGeneration;
		key.representationId = primitive.representationId;
		key.depthWriteEligibilityGeneration = primitive.depthWriteEligibilityGeneration;
		return key;
	}

	SceneOcclusionCapabilitySupport SceneOcclusionSystem::ResolveCapabilities(
		const NLS::Render::RHI::RHIDeviceCapabilities& capabilities,
		const SceneOcclusionCapabilityRequest& request)
	{
		if (auto support = CheckFeature(
				capabilities,
				RHIDeviceFeature::BackendReady,
				SceneOcclusionFallbackReason::BackendUnsupported,
				"RHI backend is not ready for occlusion");
			!support.backendSupported)
		{
			return support;
		}

		if (auto support = CheckFeature(
				capabilities,
				RHIDeviceFeature::Compute,
				SceneOcclusionFallbackReason::BackendUnsupported,
				"RHI backend does not expose compute for HZB occlusion");
			!support.backendSupported)
		{
			return support;
		}

		if (auto support = CheckFeature(
				capabilities,
				RHIDeviceFeature::ExplicitBarriers,
				SceneOcclusionFallbackReason::BackendUnsupported,
				"RHI backend does not expose explicit texture barriers for HZB occlusion");
			!support.backendSupported)
		{
			return support;
		}

		if (auto support = CheckFeature(
				capabilities,
				RHIDeviceFeature::HierarchicalZBuffer,
				SceneOcclusionFallbackReason::HZBUnsupported,
				"RHI backend does not expose hierarchical Z-buffer occlusion support");
			!support.backendSupported)
		{
			return support;
		}

		if (auto support = CheckFeature(
				capabilities,
				RHIDeviceFeature::ConservativeOcclusion,
				SceneOcclusionFallbackReason::OcclusionUnsupported,
				"RHI backend does not expose conservative occlusion support");
			!support.backendSupported)
		{
			return support;
		}

		if (request.requireAsyncReadback)
		{
			if (auto support = CheckFeature(
					capabilities,
					RHIDeviceFeature::AsyncReadback,
					SceneOcclusionFallbackReason::AsyncReadbackUnsupported,
					"RHI backend does not expose non-blocking occlusion readback support");
				!support.backendSupported)
			{
				return support;
			}
		}

		if (auto support = CheckTextureCapability(
				capabilities.GetTextureFormatCapability(request.opaqueDepthFormat),
				SceneOcclusionFallbackReason::OpaqueDepthTextureUnsupported,
				"Opaque depth texture format cannot be sampled by the HZB build pass",
				true,
				false);
			!support.backendSupported)
		{
			return support;
		}

		if (auto support = CheckTextureCapability(
				capabilities.GetTextureFormatCapability(request.hzbFormat),
				SceneOcclusionFallbackReason::HZBTextureUnsupported,
				"HZB texture format must support sampled and storage access",
				true,
				true);
			!support.backendSupported)
		{
			return support;
		}

		return Supported();
	}

	SceneOcclusionCapabilitySupport SceneOcclusionSystem::ResolveCapabilities(
		const NLS::Render::RHI::RHIDevice& device,
		const SceneOcclusionCapabilityRequest& request)
	{
		return ResolveCapabilities(device.GetCapabilities(), request);
	}

	SceneOcclusionResult SceneOcclusionSystem::Evaluate(
		const SceneOcclusionFrameInput& frame,
		const std::vector<SceneOcclusionPrimitiveInput>& primitives,
		const SceneOcclusionHistory& history)
	{
		SceneOcclusionResult result;
		result.primitiveResults.reserve(primitives.size());
		result.visiblePrimitiveHandles.reserve(primitives.size());

		for (const auto& primitive : primitives)
		{
			SceneOcclusionPrimitiveResult primitiveResult;
			primitiveResult.handle = primitive.handle;

			if (!frame.enabled)
			{
				primitiveResult.fallbackReason = SceneOcclusionFallbackReason::Disabled;
			}
			else if (!frame.backendSupported)
			{
				primitiveResult.fallbackReason = SceneOcclusionFallbackReason::BackendUnsupported;
			}
			else if (!frame.historyTextureValid)
			{
				primitiveResult.fallbackReason = SceneOcclusionFallbackReason::HistoryTextureInvalid;
			}
			else if (!primitive.depthWriteEligible)
			{
				primitiveResult.fallbackReason = SceneOcclusionFallbackReason::DepthWriteIneligible;
			}
			else
			{
				const auto key = BuildHistoryKey(frame, primitive);
				const auto occludedFrame = history.FindOccludedFrame(key);
				if (occludedFrame.has_value())
				{
					const auto historyAge = frame.frameSerial >= *occludedFrame
						? frame.frameSerial - *occludedFrame
						: (std::numeric_limits<uint64_t>::max)();
					if (historyAge <= frame.maxHistoryAge)
					{
						primitiveResult.culledByOcclusion = true;
						primitiveResult.fallbackReason = SceneOcclusionFallbackReason::None;
					}
					else
					{
						primitiveResult.fallbackReason = SceneOcclusionFallbackReason::HistoryTooOld;
					}
				}
				else
				{
					primitiveResult.fallbackReason =
						history.ClassifyMissingHistoryForHandle(primitive.handle, key);
				}
			}

			if (primitiveResult.culledByOcclusion)
				result.occludedPrimitiveHandles.push_back(primitive.handle);
			else
				result.visiblePrimitiveHandles.push_back(primitive.handle);
			result.primitiveResults.push_back(primitiveResult);
		}

		return result;
	}

	std::vector<SceneOcclusionObservation> SceneOcclusionSystem::BuildObservationsFromPrimitiveResultFlags(
		const SceneOcclusionFrameInput& frame,
		const std::vector<SceneOcclusionPrimitiveInput>& primitives,
		const std::vector<uint32_t>& primitiveResultFlags)
	{
		std::vector<SceneOcclusionObservation> observations;
		const auto count = (std::min)(primitives.size(), primitiveResultFlags.size());
		observations.reserve(count);

		for (size_t index = 0u; index < count; ++index)
		{
			SceneOcclusionObservation observation;
			observation.key = BuildHistoryKey(frame, primitives[index]);
			observation.frameSerial = frame.frameSerial;
			observation.occluded = primitiveResultFlags[index] != 0u;
			observations.push_back(observation);
		}

		return observations;
	}

	SceneOcclusionPrimitivePacketBuildResult SceneOcclusionSystem::BuildHZBPrimitivePackets(
		const SceneOcclusionPrimitivePacketBuildInput& input,
		const std::vector<SceneOcclusionPrimitivePacketSource>& sources)
	{
		SceneOcclusionPrimitivePacketBuildResult result;
		result.primitiveInputs.reserve(sources.size());
		result.primitivePackets.reserve(sources.size());

		for (const auto& source : sources)
		{
			auto packet = BuildPrimitivePacket(input, source);
			if (!packet.has_value())
			{
				++result.rejectedPrimitiveCount;
				continue;
			}

			result.primitiveInputs.push_back(source.primitive);
			result.primitivePackets.push_back(*packet);
		}

		return result;
	}

	SceneOcclusionPrimitivePacketSourceBuildResult SceneOcclusionSystem::BuildHZBPrimitivePacketSources(
		const ScenePrimitiveSnapshot& snapshot,
		const std::vector<ScenePrimitiveHandle>& visibleHandles)
	{
		struct HandleHash
		{
			size_t operator()(const ScenePrimitiveHandle& handle) const noexcept
			{
				size_t seed = 0u;
				HashHandle(seed, handle);
				return seed;
			}
		};

		std::unordered_map<ScenePrimitiveHandle, const ScenePrimitiveSnapshotRecord*, HandleHash> recordByHandle;
		recordByHandle.reserve(snapshot.primitiveRecords.size());
		for (const auto& record : snapshot.primitiveRecords)
			recordByHandle[record.handle] = &record;

		SceneOcclusionPrimitivePacketSourceBuildResult result;
		result.sources.reserve(visibleHandles.size());

		for (const auto handle : visibleHandles)
		{
			const auto found = recordByHandle.find(handle);
			if (found == recordByHandle.end() || found->second == nullptr)
			{
				++result.rejectedPrimitiveCount;
				continue;
			}

			const auto& record = *found->second;
			if (!record.occupied ||
				record.tombstoned ||
				!record.ownerAlive ||
				!record.ownerActive ||
				!record.hasMeshBinding ||
				!record.hasValidMaterial ||
				!record.depthWriteEligibleForOcclusion)
			{
				++result.rejectedPrimitiveCount;
				continue;
			}

			SceneOcclusionPrimitivePacketSource source;
			source.primitive.handle = record.handle;
			source.primitive.boundsGeneration = HashBoundsGeneration(record);
			source.primitive.transformGeneration = HashTransformGeneration(record);
			source.primitive.representationId = HashRepresentationId(record);
			source.primitive.depthWriteEligibilityGeneration = HashDepthWriteEligibilityGeneration(record);
			source.primitive.depthWriteEligible = true;
			source.modelBounds = record.modelBounds;
			source.worldMatrix = record.worldMatrix;
			result.sources.push_back(source);
		}

		return result;
	}

	std::vector<ScenePrimitiveHandle> SceneOcclusionSystem::BuildHZBObservationCandidateHandles(
		const std::vector<ScenePrimitiveHandle>& postOcclusionVisibleHandles,
		const std::vector<SceneOcclusionPrimitiveInput>& previousPrimitiveInputs,
		const uint64_t targetSceneId)
	{
		struct HandleHash
		{
			size_t operator()(const ScenePrimitiveHandle& handle) const noexcept
			{
				size_t seed = 0u;
				HashHandle(seed, handle);
				return seed;
			}
		};

		std::vector<ScenePrimitiveHandle> candidates;
		candidates.reserve(postOcclusionVisibleHandles.size() + previousPrimitiveInputs.size());
		std::unordered_set<ScenePrimitiveHandle, HandleHash> includedHandles;
		includedHandles.reserve(postOcclusionVisibleHandles.size() + previousPrimitiveInputs.size());

		const auto addUniqueHandle = [&candidates, &includedHandles](const ScenePrimitiveHandle handle)
		{
			if (includedHandles.insert(handle).second)
				candidates.push_back(handle);
		};

		for (const auto handle : postOcclusionVisibleHandles)
			addUniqueHandle(handle);
		for (const auto& input : previousPrimitiveInputs)
		{
			if (input.handle.sceneId != targetSceneId)
				continue;
			addUniqueHandle(input.handle);
		}

		return candidates;
	}

	SceneOcclusionObservationBatch SceneOcclusionSystem::CreatePendingObservationBatch(
		const SceneOcclusionFrameInput& frame,
		const std::vector<SceneOcclusionPrimitiveInput>& primitives)
	{
		SceneOcclusionObservationBatch batch;
		batch.frame = frame;
		batch.primitiveInputs = primitives;
		batch.ready = false;
		return batch;
	}

	SceneOcclusionObservationBatch SceneOcclusionSystem::CompleteObservationBatchWithPrimitiveResultFlags(
		const SceneOcclusionObservationBatch& pendingBatch,
		const std::vector<uint32_t>& primitiveResultFlags)
	{
		SceneOcclusionObservationBatch batch = pendingBatch;
		batch.observations = BuildObservationsFromPrimitiveResultFlags(
			batch.frame,
			batch.primitiveInputs,
			primitiveResultFlags);
		batch.ready = true;
		return batch;
	}

	SceneOcclusionObservationStats SceneOcclusionSystem::ApplyObservationResults(
		SceneOcclusionHistory& history,
		const std::vector<SceneOcclusionObservation>& observations)
	{
		SceneOcclusionObservationStats stats;
		stats.observedPrimitiveCount = static_cast<uint64_t>(observations.size());

		for (const auto& observation : observations)
		{
			if (observation.occluded)
			{
				history.RecordOccluded(observation.key, observation.frameSerial);
				++stats.occludedPrimitiveCount;
			}
			else
			{
				history.RecordVisible(observation.key);
				++stats.visiblePrimitiveCount;
			}
		}

		return stats;
	}

	SceneOcclusionObservationStats SceneOcclusionSystem::ApplyObservationResults(
		SceneOcclusionHistory& history,
		const SceneOcclusionFrameInput& frame,
		const std::vector<SceneOcclusionObservation>& observations)
	{
		SceneOcclusionObservationStats stats;
		stats.observedPrimitiveCount = static_cast<uint64_t>(observations.size());

		SceneOcclusionHistoryKey expectedView;
		expectedView.viewKey = frame.viewKey;
		expectedView.viewCompatibilityHash = frame.viewCompatibilityHash;
		expectedView.projectionHash = frame.projectionHash;
		expectedView.jitterHash = frame.jitterHash;
		expectedView.depthFormatKey = frame.depthFormatKey;
		expectedView.viewportWidth = frame.viewportWidth;
		expectedView.viewportHeight = frame.viewportHeight;

		for (const auto& observation : observations)
		{
			if (observation.frameSerial != frame.frameSerial)
			{
				++stats.discardedPrimitiveCount;
				++stats.staleFrameCount;
				continue;
			}

			if (!SameViewIdentity(observation.key, expectedView))
			{
				++stats.discardedPrimitiveCount;
				++stats.incompatibleViewCount;
				continue;
			}

			if (observation.occluded)
			{
				history.RecordOccluded(observation.key, observation.frameSerial);
				++stats.occludedPrimitiveCount;
			}
			else
			{
				history.RecordVisible(observation.key);
				++stats.visiblePrimitiveCount;
			}
		}

		return stats;
	}

	SceneOcclusionObservationStats SceneOcclusionSystem::ApplyReadyObservationBatch(
		SceneOcclusionHistory& history,
		const SceneOcclusionFrameInput& frame,
		const SceneOcclusionObservationBatch& batch)
	{
		if (!batch.ready)
		{
			SceneOcclusionObservationStats stats;
			stats.discardedPrimitiveCount = static_cast<uint64_t>(batch.primitiveInputs.size());
			return stats;
		}

		return ApplyObservationResults(history, frame, batch.observations);
	}
}
