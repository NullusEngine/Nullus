#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "EngineDef.h"
#include "Rendering/Data/FrameInfo.h"
#include "Rendering/LargeSceneSettings.h"
#include "Rendering/RenderScene.h"

namespace NLS::Engine::Rendering
{
	struct NLS_ENGINE_API ScenePrimitiveHandleStableHash
	{
		[[nodiscard]] size_t operator()(const ScenePrimitiveHandle& handle) const noexcept;
	};

	struct NLS_ENGINE_API RepresentationResidencySnapshot
	{
		std::vector<ScenePrimitiveHandle> readyPrimitiveResources;
		std::vector<ScenePrimitiveHandle> readyHLODProxyResources;
		std::vector<ScenePrimitiveHandle> fallbackPrimitiveResources;
		std::vector<ScenePrimitiveHandle> notResidentResources;
		std::unordered_set<ScenePrimitiveHandle, ScenePrimitiveHandleStableHash> readyPrimitiveResourceSet;
		std::unordered_set<ScenePrimitiveHandle, ScenePrimitiveHandleStableHash> readyHLODProxyResourceSet;
		std::unordered_set<ScenePrimitiveHandle, ScenePrimitiveHandleStableHash> fallbackPrimitiveResourceSet;
		std::unordered_set<ScenePrimitiveHandle, ScenePrimitiveHandleStableHash> notResidentResourceSet;

		void MarkReady(ScenePrimitiveHandle handle);
		void MarkHLODProxyReady(ScenePrimitiveHandle handle);
		void MarkFallback(ScenePrimitiveHandle handle);
		void MarkNotResident(ScenePrimitiveHandle handle);

		[[nodiscard]] bool IsReady(ScenePrimitiveHandle handle) const;
		[[nodiscard]] bool IsHLODProxyReady(ScenePrimitiveHandle handle) const;
		[[nodiscard]] bool HasFallback(ScenePrimitiveHandle handle) const;
		[[nodiscard]] bool IsNotResident(ScenePrimitiveHandle handle) const;
	};

	enum class StreamingDependencySource : uint8_t
	{
		Visibility,
		LOD,
		HLOD,
		EditorSelection,
		Import,
		Validation
	};

	enum class StreamingResourceType : uint8_t
	{
		Mesh,
		Material,
		Texture,
		HLODProxy,
		SceneCell,
		Placeholder
	};

	enum class ResidencyTicketState : uint8_t
	{
		Requested,
		LoadingCpu,
		PendingGpuUpload,
		Resident,
		VisibleResident,
		CancelPending,
		EvictPending,
		Evicted
	};

	struct NLS_ENGINE_API StreamingResourceDependency
	{
		uint64_t dependencyId = 0u;
		StreamingDependencySource source = StreamingDependencySource::Visibility;
		StreamingResourceType resourceType = StreamingResourceType::Mesh;
		std::string artifactId;
		uint64_t cpuBytes = 0u;
		uint64_t gpuBytes = 0u;
		uint64_t ioBytes = 0u;
		uint64_t gpuUploadBytes = 0u;
		uint64_t cpuCommitUs = 0u;
		uint32_t priority = 0u;
		bool requiredForVisibleRepresentation = false;
		std::vector<uint64_t> childDependencyIds;
		std::optional<uint64_t> fallbackDependencyId;

		[[nodiscard]] bool IsValid() const { return dependencyId != 0u; }
	};

	struct NLS_ENGINE_API ResidencyTicket
	{
		uint64_t ticketId = 0u;
		uint64_t dependencyId = 0u;
		uint32_t priority = 0u;
		ResidencyTicketState state = ResidencyTicketState::Requested;
		uint64_t requestFrame = 0u;
		uint64_t lastInterestFrame = 0u;
		uint32_t pinCount = 0u;
		std::string cancelReason;
		uint32_t coalescedRequestCount = 0u;
	};

	struct NLS_ENGINE_API StreamingResidencyPlanInput
	{
		uint64_t frameSerial = 0u;
		std::vector<ScenePrimitiveHandle> visiblePrimitiveHandles;
		std::vector<ScenePrimitiveHandle> representationStreamingInterest;
	};

	struct NLS_ENGINE_API StreamingResidencyFramePins
	{
		std::vector<uint64_t> pinnedDependencyIds;
	};

	struct NLS_ENGINE_API StreamingResidencyPlan
	{
		struct PrimitiveDependencyBinding
		{
			ScenePrimitiveHandle primitive;
			uint64_t dependencyId = 0u;
		};

		std::vector<StreamingResourceDependency> dependencyClosure;
		std::vector<ResidencyTicket> tickets;
		std::vector<PrimitiveDependencyBinding> primitiveDependencyBindings;
		NLS::Render::Data::LargeSceneTelemetry telemetry;

		[[nodiscard]] const ResidencyTicket* FindTicket(uint64_t dependencyId) const;
	};

	struct NLS_ENGINE_API StreamingCommitResult
	{
		std::vector<ResidencyTicket> tickets;
		NLS::Render::Data::LargeSceneTelemetry telemetry;
		bool cpuBudgetExhausted = false;
		bool ioBudgetExhausted = false;
		bool gpuUploadBudgetExhausted = false;
		bool cpuMemoryBudgetExhausted = false;
		bool gpuMemoryBudgetExhausted = false;

		[[nodiscard]] const ResidencyTicket* FindTicket(uint64_t dependencyId) const;
	};

	class NLS_ENGINE_API SceneStreamingResidency
	{
	public:
		void RegisterDependency(const StreamingResourceDependency& dependency);
		void RegisterPrimitiveDependency(ScenePrimitiveHandle primitive, uint64_t dependencyId);

		[[nodiscard]] StreamingResidencyPlan Plan(
			const StreamingResidencyPlanInput& input,
			const LargeSceneSettings& settings);
		[[nodiscard]] StreamingCommitResult Commit(
			const StreamingResidencyPlan& plan,
			const LargeSceneSettings& settings,
			const StreamingResidencyFramePins& framePins);

	private:
		struct PrimitiveDependencyBinding
		{
			ScenePrimitiveHandle primitive;
			uint64_t dependencyId = 0u;

			[[nodiscard]] bool operator==(const PrimitiveDependencyBinding& other) const noexcept
			{
				return primitive == other.primitive && dependencyId == other.dependencyId;
			}
		};

		struct PrimitiveDependencyBindingHash
		{
			[[nodiscard]] size_t operator()(const PrimitiveDependencyBinding& binding) const noexcept
			{
				size_t hash = ScenePrimitiveHandleStableHash{}(binding.primitive);
				hash ^= static_cast<size_t>(binding.dependencyId) + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
				return hash;
			}
		};

		[[nodiscard]] const StreamingResourceDependency* FindDependency(uint64_t dependencyId) const;
		void AppendDependencyClosure(
			uint64_t dependencyId,
			StreamingResidencyPlan& plan,
			std::unordered_set<uint64_t>& visitedDependencyIds) const;
		ResidencyTicket& GetOrCreateTicket(
			uint64_t dependencyId,
			uint32_t basePriority,
			uint64_t frameSerial,
			uint32_t coalescedRequestCount);
		[[nodiscard]] uint32_t ComputeAgedPriority(const ResidencyTicket& ticket, uint32_t basePriority, uint64_t frameSerial) const;
		[[nodiscard]] bool IsDependencyPinned(uint64_t dependencyId, const StreamingResidencyFramePins& framePins) const;
		void SetTicketState(
			ResidencyTicket& ticket,
			const StreamingResourceDependency& dependency,
			ResidencyTicketState state);
		void AppendResultTicket(
			StreamingCommitResult& result,
			std::unordered_set<uint64_t>& resultDependencyIds,
			const ResidencyTicket& ticket) const;
		void ApplyMemoryBudgetEviction(
			const StreamingResidencyPlan& plan,
			const LargeSceneSettings& settings,
			const StreamingResidencyFramePins& framePins,
			std::unordered_set<uint64_t>& resultDependencyIds,
			StreamingCommitResult& result);

		std::unordered_map<uint64_t, StreamingResourceDependency> dependencies;
		std::unordered_set<PrimitiveDependencyBinding, PrimitiveDependencyBindingHash> primitiveDependencyBindings;
		std::unordered_map<ScenePrimitiveHandle, std::vector<uint64_t>, ScenePrimitiveHandleStableHash> dependencyIdsByPrimitive;
		std::unordered_map<uint64_t, ResidencyTicket> tickets;
		std::unordered_set<uint64_t> residentDependencyIds;
		uint64_t residentCpuBytes = 0u;
		uint64_t residentGpuBytes = 0u;
		uint64_t requestedCpuBytes = 0u;
		uint64_t requestedGpuBytes = 0u;
		uint64_t nextTicketId = 1u;
	};
}
