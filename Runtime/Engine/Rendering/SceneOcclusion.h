#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "EngineDef.h"
#include "Rendering/Data/SceneOcclusionPacketLayout.h"
#include "Rendering/Geometry/Bounds.h"
#include "Rendering/RenderScene.h"
#include "Rendering/RHI/RHITypes.h"

namespace NLS::Render::RHI
{
	class RHIDevice;
}

namespace NLS::Engine::Rendering
{
	enum class SceneOcclusionFallbackReason : uint8_t
	{
		None,
		Disabled,
		BackendUnsupported,
		HZBUnsupported,
		OcclusionUnsupported,
		AsyncReadbackUnsupported,
		OpaqueDepthTextureUnsupported,
		HZBTextureUnsupported,
		HistoryTextureInvalid,
		MissingHistory,
		HistoryTooOld,
		ViewChanged,
		PrimitiveChanged,
		RepresentationChanged,
		DepthWriteIneligible
	};

	struct SceneOcclusionFrameInput
	{
		bool enabled = false;
		bool backendSupported = false;
		bool historyTextureValid = false;
		uint64_t frameSerial = 0u;
		uint32_t maxHistoryAge = 2u;
		uint64_t viewKey = 0u;
		uint64_t viewCompatibilityHash = 0u;
		uint64_t projectionHash = 0u;
		uint64_t jitterHash = 0u;
		uint64_t depthFormatKey = 0u;
		uint32_t viewportWidth = 0u;
		uint32_t viewportHeight = 0u;
	};

	struct SceneOcclusionPrimitiveInput
	{
		ScenePrimitiveHandle handle;
		uint64_t boundsGeneration = 0u;
		uint64_t transformGeneration = 0u;
		uint64_t representationId = 0u;
		uint64_t depthWriteEligibilityGeneration = 0u;
		bool depthWriteEligible = false;
	};

	struct SceneOcclusionPrimitivePacketBuildInput
	{
		Maths::Matrix4 viewProjection = Maths::Matrix4::Identity;
		uint32_t viewportWidth = 0u;
		uint32_t viewportHeight = 0u;
	};

	struct SceneOcclusionPrimitivePacketSource
	{
		SceneOcclusionPrimitiveInput primitive;
		NLS::Render::Geometry::Bounds modelBounds;
		Maths::Matrix4 worldMatrix = Maths::Matrix4::Identity;
	};

	struct SceneOcclusionPrimitivePacket
	{
		float screenMinX = 0.0f;
		float screenMinY = 0.0f;
		float screenMaxX = 0.0f;
		float screenMaxY = 0.0f;
		float nearestDepth = 1.0f;
		uint32_t flags = 0u;
	};

	static_assert(
		sizeof(SceneOcclusionPrimitivePacket) == NLS::Render::Data::kSceneOcclusionPrimitivePacketStride,
		"SceneOcclusionPrimitivePacket layout must match HZBOcclusion.hlsl OcclusionPrimitiveInput.");

	struct SceneOcclusionPrimitivePacketBuildResult
	{
		std::vector<SceneOcclusionPrimitiveInput> primitiveInputs;
		std::vector<SceneOcclusionPrimitivePacket> primitivePackets;
		uint64_t rejectedPrimitiveCount = 0u;
	};

	struct SceneOcclusionPrimitivePacketSourceBuildResult
	{
		std::vector<SceneOcclusionPrimitivePacketSource> sources;
		uint64_t rejectedPrimitiveCount = 0u;
	};

	struct SceneOcclusionHistoryKey
	{
		ScenePrimitiveHandle handle;
		uint64_t viewKey = 0u;
		uint64_t viewCompatibilityHash = 0u;
		uint64_t projectionHash = 0u;
		uint64_t jitterHash = 0u;
		uint64_t depthFormatKey = 0u;
		uint32_t viewportWidth = 0u;
		uint32_t viewportHeight = 0u;
		uint64_t boundsGeneration = 0u;
		uint64_t transformGeneration = 0u;
		uint64_t representationId = 0u;
		uint64_t depthWriteEligibilityGeneration = 0u;

		[[nodiscard]] bool operator==(const SceneOcclusionHistoryKey& other) const = default;
	};

	struct SceneOcclusionPrimitiveResult
	{
		ScenePrimitiveHandle handle;
		bool culledByOcclusion = false;
		SceneOcclusionFallbackReason fallbackReason = SceneOcclusionFallbackReason::MissingHistory;
	};

	struct SceneOcclusionResult
	{
		std::vector<SceneOcclusionPrimitiveResult> primitiveResults;
		std::vector<ScenePrimitiveHandle> occludedPrimitiveHandles;
		std::vector<ScenePrimitiveHandle> visiblePrimitiveHandles;
		bool usedSynchronousReadback = false;
		bool waitedForGpuFence = false;
		bool blockedOnReadbackMap = false;
		bool requestedCurrentFrameReadback = false;
	};

	struct SceneOcclusionObservation
	{
		SceneOcclusionHistoryKey key;
		uint64_t frameSerial = 0u;
		bool occluded = false;
	};

	struct SceneOcclusionObservationBatch
	{
		SceneOcclusionFrameInput frame;
		std::vector<SceneOcclusionPrimitiveInput> primitiveInputs;
		std::vector<SceneOcclusionObservation> observations;
		bool ready = false;
	};

	struct SceneOcclusionObservationStats
	{
		uint64_t observedPrimitiveCount = 0u;
		uint64_t visiblePrimitiveCount = 0u;
		uint64_t occludedPrimitiveCount = 0u;
		uint64_t discardedPrimitiveCount = 0u;
		uint64_t staleFrameCount = 0u;
		uint64_t incompatibleViewCount = 0u;
		bool usedSynchronousReadback = false;
		bool waitedForGpuFence = false;
		bool blockedOnReadbackMap = false;
		bool requestedCurrentFrameReadback = false;
	};

	struct SceneOcclusionHistoryPruneStats
	{
		uint64_t trackedHandleCountBefore = 0u;
		uint64_t trackedHandleCountAfter = 0u;
		uint64_t touchedHandleCount = 0u;
		uint64_t removedHandleCount = 0u;
		uint64_t removedKeyCount = 0u;
	};

	struct SceneOcclusionCapabilityRequest
	{
		NLS::Render::RHI::TextureFormat opaqueDepthFormat = NLS::Render::RHI::TextureFormat::Depth32F;
		NLS::Render::RHI::TextureFormat hzbFormat = NLS::Render::RHI::TextureFormat::R32F;
		bool requireAsyncReadback = true;
	};

	struct SceneOcclusionCapabilitySupport
	{
		bool backendSupported = false;
		SceneOcclusionFallbackReason fallbackReason = SceneOcclusionFallbackReason::BackendUnsupported;
		std::string diagnosticReason;
	};

	class SceneOcclusionHistory;

	struct SceneOcclusionState
	{
		SceneOcclusionFrameInput frameInput;
		const SceneOcclusionHistory* history = nullptr;
		const std::vector<SceneOcclusionPrimitiveInput>* primitiveInputs = nullptr;
	};

	class NLS_ENGINE_API SceneOcclusionHistory
	{
	public:
		SceneOcclusionHistory();
		~SceneOcclusionHistory();

		void Clear();
		void RecordOccluded(const SceneOcclusionHistoryKey& key, uint64_t frameSerial);
		void RecordVisible(const SceneOcclusionHistoryKey& key);
		SceneOcclusionHistoryPruneStats PruneHandles(std::span<const ScenePrimitiveHandle> removedHandles);
		// Lifecycle fallback only: ordinary frame removal must use PruneHandles() to avoid hidden live-scene scans.
		SceneOcclusionHistoryPruneStats PruneByLiveHandleSweepForLifecycleFallback(
			std::span<const ScenePrimitiveHandle> liveHandles);
		[[nodiscard]] std::optional<uint64_t> FindOccludedFrame(const SceneOcclusionHistoryKey& key) const;
		[[nodiscard]] SceneOcclusionFallbackReason ClassifyMissingHistoryForHandle(
			ScenePrimitiveHandle handle,
			const SceneOcclusionHistoryKey& key) const;

	private:
		struct ScenePrimitiveHandleHash
		{
			[[nodiscard]] size_t operator()(const ScenePrimitiveHandle& handle) const noexcept;
		};

		struct HistoryKeyHash
		{
			[[nodiscard]] size_t operator()(const SceneOcclusionHistoryKey& key) const noexcept;
		};

		std::unordered_map<SceneOcclusionHistoryKey, uint64_t, HistoryKeyHash> m_occludedFrameByKey;
		std::unordered_map<ScenePrimitiveHandle, std::vector<SceneOcclusionHistoryKey>, ScenePrimitiveHandleHash> m_keysByHandle;
	};

	class NLS_ENGINE_API SceneOcclusionSystem
	{
	public:
		[[nodiscard]] static SceneOcclusionHistoryKey BuildHistoryKey(
			const SceneOcclusionFrameInput& frame,
			const SceneOcclusionPrimitiveInput& primitive);
		[[nodiscard]] static SceneOcclusionCapabilitySupport ResolveCapabilities(
			const NLS::Render::RHI::RHIDeviceCapabilities& capabilities,
			const SceneOcclusionCapabilityRequest& request);
		[[nodiscard]] static SceneOcclusionCapabilitySupport ResolveCapabilities(
			const NLS::Render::RHI::RHIDevice& device,
			const SceneOcclusionCapabilityRequest& request);
		[[nodiscard]] static SceneOcclusionResult Evaluate(
			const SceneOcclusionFrameInput& frame,
			const std::vector<SceneOcclusionPrimitiveInput>& primitives,
			const SceneOcclusionHistory& history);
		[[nodiscard]] static std::vector<SceneOcclusionObservation> BuildObservationsFromPrimitiveResultFlags(
			const SceneOcclusionFrameInput& frame,
			const std::vector<SceneOcclusionPrimitiveInput>& primitives,
			const std::vector<uint32_t>& primitiveResultFlags);
		[[nodiscard]] static SceneOcclusionPrimitivePacketBuildResult BuildHZBPrimitivePackets(
			const SceneOcclusionPrimitivePacketBuildInput& input,
			const std::vector<SceneOcclusionPrimitivePacketSource>& sources);
		[[nodiscard]] static SceneOcclusionPrimitivePacketSourceBuildResult BuildHZBPrimitivePacketSources(
			const ScenePrimitiveSnapshot& snapshot,
			const std::vector<ScenePrimitiveHandle>& visibleHandles);
		[[nodiscard]] static std::vector<ScenePrimitiveHandle> BuildHZBObservationCandidateHandles(
			const std::vector<ScenePrimitiveHandle>& postOcclusionVisibleHandles,
			const std::vector<SceneOcclusionPrimitiveInput>& previousPrimitiveInputs,
			uint64_t targetSceneId);
		[[nodiscard]] static SceneOcclusionObservationBatch CreatePendingObservationBatch(
			const SceneOcclusionFrameInput& frame,
			const std::vector<SceneOcclusionPrimitiveInput>& primitives);
		[[nodiscard]] static SceneOcclusionObservationBatch CompleteObservationBatchWithPrimitiveResultFlags(
			const SceneOcclusionObservationBatch& pendingBatch,
			const std::vector<uint32_t>& primitiveResultFlags);
		[[nodiscard]] static SceneOcclusionObservationStats ApplyObservationResults(
			SceneOcclusionHistory& history,
			const std::vector<SceneOcclusionObservation>& observations);
		[[nodiscard]] static SceneOcclusionObservationStats ApplyObservationResults(
			SceneOcclusionHistory& history,
			const SceneOcclusionFrameInput& frame,
			const std::vector<SceneOcclusionObservation>& observations);
		[[nodiscard]] static SceneOcclusionObservationStats ApplyReadyObservationBatch(
			SceneOcclusionHistory& history,
			const SceneOcclusionFrameInput& frame,
			const SceneOcclusionObservationBatch& batch);
	};
}
