#include "Rendering/SceneStreamingResidency.h"

#include <limits>

namespace NLS::Engine::Rendering
{
namespace
{
	void AddUnique(
		std::vector<ScenePrimitiveHandle>& handles,
		std::unordered_set<ScenePrimitiveHandle, ScenePrimitiveHandleStableHash>& handleSet,
		const ScenePrimitiveHandle handle)
	{
		if (!handle.IsValid())
			return;
		if (handleSet.insert(handle).second)
			handles.push_back(handle);
	}

	void RemoveValue(
		std::vector<ScenePrimitiveHandle>& handles,
		std::unordered_set<ScenePrimitiveHandle, ScenePrimitiveHandleStableHash>& handleSet,
		const ScenePrimitiveHandle handle)
	{
		if (!handle.IsValid() || handleSet.erase(handle) == 0u)
			return;

		handles.erase(
			std::remove(handles.begin(), handles.end(), handle),
			handles.end());
	}

	template <typename T>
	bool ContainsValue(const std::vector<T>& values, const T& value)
	{
		return std::find(values.begin(), values.end(), value) != values.end();
	}

	void AccumulateRequestedBytes(
		const StreamingResourceDependency& dependency,
		NLS::Render::Data::LargeSceneTelemetry& telemetry)
	{
		telemetry.requestedCpuBytes += dependency.cpuBytes;
		telemetry.requestedGpuBytes += dependency.gpuBytes;
	}

	void AccumulateResidentBytes(
		const StreamingResourceDependency& dependency,
		NLS::Render::Data::LargeSceneTelemetry& telemetry)
	{
		telemetry.residentCpuBytes += dependency.cpuBytes;
		telemetry.residentGpuBytes += dependency.gpuBytes;
	}

	bool IsResidentState(const ResidencyTicketState state)
	{
		return state == ResidencyTicketState::Resident ||
			state == ResidencyTicketState::VisibleResident;
	}

	void SaturatingSubtract(uint64_t& value, const uint64_t amount)
	{
		value = value > amount ? value - amount : 0u;
	}
}

size_t ScenePrimitiveHandleStableHash::operator()(
	const ScenePrimitiveHandle& handle) const noexcept
{
	auto hash = static_cast<size_t>(handle.sceneId);
	hash ^= static_cast<size_t>(handle.index) + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
	hash ^= static_cast<size_t>(handle.generation) + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
	return hash;
}

void RepresentationResidencySnapshot::MarkReady(const ScenePrimitiveHandle handle)
{
	RemoveValue(notResidentResources, notResidentResourceSet, handle);
	RemoveValue(fallbackPrimitiveResources, fallbackPrimitiveResourceSet, handle);
	RemoveValue(readyHLODProxyResources, readyHLODProxyResourceSet, handle);
	AddUnique(readyPrimitiveResources, readyPrimitiveResourceSet, handle);
}

void RepresentationResidencySnapshot::MarkHLODProxyReady(const ScenePrimitiveHandle handle)
{
	RemoveValue(notResidentResources, notResidentResourceSet, handle);
	RemoveValue(fallbackPrimitiveResources, fallbackPrimitiveResourceSet, handle);
	AddUnique(readyHLODProxyResources, readyHLODProxyResourceSet, handle);
	AddUnique(readyPrimitiveResources, readyPrimitiveResourceSet, handle);
}

void RepresentationResidencySnapshot::MarkFallback(const ScenePrimitiveHandle handle)
{
	RemoveValue(notResidentResources, notResidentResourceSet, handle);
	RemoveValue(readyPrimitiveResources, readyPrimitiveResourceSet, handle);
	RemoveValue(readyHLODProxyResources, readyHLODProxyResourceSet, handle);
	AddUnique(fallbackPrimitiveResources, fallbackPrimitiveResourceSet, handle);
}

void RepresentationResidencySnapshot::MarkNotResident(const ScenePrimitiveHandle handle)
{
	RemoveValue(readyPrimitiveResources, readyPrimitiveResourceSet, handle);
	RemoveValue(readyHLODProxyResources, readyHLODProxyResourceSet, handle);
	RemoveValue(fallbackPrimitiveResources, fallbackPrimitiveResourceSet, handle);
	AddUnique(notResidentResources, notResidentResourceSet, handle);
}

bool RepresentationResidencySnapshot::IsReady(const ScenePrimitiveHandle handle) const
{
	return readyPrimitiveResourceSet.find(handle) != readyPrimitiveResourceSet.end();
}

bool RepresentationResidencySnapshot::IsHLODProxyReady(const ScenePrimitiveHandle handle) const
{
	return readyHLODProxyResourceSet.find(handle) != readyHLODProxyResourceSet.end();
}

bool RepresentationResidencySnapshot::HasFallback(const ScenePrimitiveHandle handle) const
{
	return fallbackPrimitiveResourceSet.find(handle) != fallbackPrimitiveResourceSet.end();
}

bool RepresentationResidencySnapshot::IsNotResident(const ScenePrimitiveHandle handle) const
{
	return notResidentResourceSet.find(handle) != notResidentResourceSet.end();
}

const ResidencyTicket* StreamingResidencyPlan::FindTicket(const uint64_t dependencyId) const
{
	const auto ticketIt = std::find_if(
		tickets.begin(),
		tickets.end(),
		[dependencyId](const ResidencyTicket& ticket)
		{
			return ticket.dependencyId == dependencyId;
		});
	return ticketIt == tickets.end() ? nullptr : &*ticketIt;
}

const ResidencyTicket* StreamingCommitResult::FindTicket(const uint64_t dependencyId) const
{
	const auto ticketIt = std::find_if(
		tickets.begin(),
		tickets.end(),
		[dependencyId](const ResidencyTicket& ticket)
		{
			return ticket.dependencyId == dependencyId;
		});
	return ticketIt == tickets.end() ? nullptr : &*ticketIt;
}

void SceneStreamingResidency::RegisterDependency(const StreamingResourceDependency& dependency)
{
	if (!dependency.IsValid())
		return;

	const auto existingDependencyIt = dependencies.find(dependency.dependencyId);
	const auto ticketIt = tickets.find(dependency.dependencyId);
	if (existingDependencyIt != dependencies.end() && ticketIt != tickets.end())
	{
		const auto& existingDependency = existingDependencyIt->second;
		if (IsResidentState(ticketIt->second.state))
		{
			SaturatingSubtract(residentCpuBytes, existingDependency.cpuBytes);
			SaturatingSubtract(residentGpuBytes, existingDependency.gpuBytes);
			residentCpuBytes += dependency.cpuBytes;
			residentGpuBytes += dependency.gpuBytes;
		}
		else
		{
			SaturatingSubtract(requestedCpuBytes, existingDependency.cpuBytes);
			SaturatingSubtract(requestedGpuBytes, existingDependency.gpuBytes);
			requestedCpuBytes += dependency.cpuBytes;
			requestedGpuBytes += dependency.gpuBytes;
		}
	}
	dependencies[dependency.dependencyId] = dependency;
}

void SceneStreamingResidency::RegisterPrimitiveDependency(
	const ScenePrimitiveHandle primitive,
	const uint64_t dependencyId)
{
	if (!primitive.IsValid() || dependencyId == 0u)
		return;

	if (primitiveDependencyBindings.insert({ primitive, dependencyId }).second)
	{
		dependencyIdsByPrimitive[primitive].push_back(dependencyId);
	}
}

StreamingResidencyPlan SceneStreamingResidency::Plan(
	const StreamingResidencyPlanInput& input,
	const LargeSceneSettings& settings)
{
	(void)settings;

	StreamingResidencyPlan plan;
	std::unordered_map<uint64_t, uint32_t> requestCounts;
	std::unordered_set<uint64_t> visitedDependencyIds;
	std::unordered_set<PrimitiveDependencyBinding, PrimitiveDependencyBindingHash> plannedPrimitiveDependencyBindings;

	auto recordPrimitiveInterest = [&](const ScenePrimitiveHandle primitive)
	{
		if (!primitive.IsValid())
			return;
		const auto dependenciesIt = dependencyIdsByPrimitive.find(primitive);
		if (dependenciesIt == dependencyIdsByPrimitive.end())
			return;

		for (const auto dependencyId : dependenciesIt->second)
		{
			++requestCounts[dependencyId];
			if (plannedPrimitiveDependencyBindings.insert({ primitive, dependencyId }).second)
				plan.primitiveDependencyBindings.push_back({ primitive, dependencyId });
			AppendDependencyClosure(dependencyId, plan, visitedDependencyIds);
		}
	};

	for (const auto primitive : input.visiblePrimitiveHandles)
		recordPrimitiveInterest(primitive);
	for (const auto primitive : input.representationStreamingInterest)
		recordPrimitiveInterest(primitive);

	for (const auto& dependency : plan.dependencyClosure)
	{
		const auto requestCountIt = requestCounts.find(dependency.dependencyId);
		const uint32_t requestCount =
			requestCountIt == requestCounts.end() ? 1u : std::max(1u, requestCountIt->second);
		auto& ticket = GetOrCreateTicket(
			dependency.dependencyId,
			dependency.priority,
			input.frameSerial,
			requestCount);
		ticket.priority = ComputeAgedPriority(ticket, dependency.priority, input.frameSerial);
		plan.tickets.push_back(ticket);
		AccumulateRequestedBytes(dependency, plan.telemetry);
	}

	plan.telemetry.streamingDependencyCount = static_cast<uint64_t>(plan.dependencyClosure.size());
	plan.telemetry.streamingRequestCount = static_cast<uint64_t>(plan.tickets.size());
	plan.telemetry.residencyTicketCount = tickets.size();
	return plan;
}

StreamingCommitResult SceneStreamingResidency::Commit(
	const StreamingResidencyPlan& plan,
	const LargeSceneSettings& settings,
	const StreamingResidencyFramePins& framePins)
{
	StreamingCommitResult result;
	std::unordered_set<uint64_t> resultDependencyIds;
	uint64_t cpuUsedUs = 0u;
	uint64_t ioUsedBytes = 0u;
	uint64_t gpuUploadUsedBytes = 0u;

	for (const auto& dependency : plan.dependencyClosure)
	{
		auto ticketIt = tickets.find(dependency.dependencyId);
		if (ticketIt == tickets.end())
			continue;

		auto& ticket = ticketIt->second;
		ticket.pinCount = IsDependencyPinned(dependency.dependencyId, framePins) ? 1u : 0u;

		switch (ticket.state)
		{
		case ResidencyTicketState::Requested:
			if (cpuUsedUs + dependency.cpuCommitUs <= settings.streamingCpuBudgetUs &&
				ioUsedBytes + dependency.ioBytes <= settings.streamingIoBudgetBytes)
			{
				cpuUsedUs += dependency.cpuCommitUs;
				ioUsedBytes += dependency.ioBytes;
				SetTicketState(ticket, dependency, ResidencyTicketState::LoadingCpu);
			}
			else
			{
				result.cpuBudgetExhausted =
					cpuUsedUs + dependency.cpuCommitUs > settings.streamingCpuBudgetUs;
				result.ioBudgetExhausted =
					ioUsedBytes + dependency.ioBytes > settings.streamingIoBudgetBytes;
			}
			break;
		case ResidencyTicketState::LoadingCpu:
			SetTicketState(ticket, dependency, ResidencyTicketState::PendingGpuUpload);
			break;
		case ResidencyTicketState::PendingGpuUpload:
			if (gpuUploadUsedBytes + dependency.gpuUploadBytes <= settings.streamingGpuUploadBudgetBytes)
			{
				gpuUploadUsedBytes += dependency.gpuUploadBytes;
				SetTicketState(
					ticket,
					dependency,
					dependency.requiredForVisibleRepresentation
						? ResidencyTicketState::VisibleResident
						: ResidencyTicketState::Resident);
				++result.telemetry.streamingCommitCount;
			}
			else
			{
				result.gpuUploadBudgetExhausted = true;
			}
			break;
		case ResidencyTicketState::Resident:
			if (dependency.requiredForVisibleRepresentation)
				SetTicketState(ticket, dependency, ResidencyTicketState::VisibleResident);
			break;
		case ResidencyTicketState::EvictPending:
			if (!IsDependencyPinned(dependency.dependencyId, framePins))
				SetTicketState(ticket, dependency, ResidencyTicketState::Evicted);
			break;
		default:
			break;
		}

		AppendResultTicket(result, resultDependencyIds, ticket);
	}

	ApplyMemoryBudgetEviction(plan, settings, framePins, resultDependencyIds, result);
	std::sort(
		result.tickets.begin(),
		result.tickets.end(),
		[](const ResidencyTicket& left, const ResidencyTicket& right)
		{
			return left.dependencyId < right.dependencyId;
		});
	result.telemetry.residencyTicketCount = tickets.size();
	result.telemetry.residentCpuBytes = residentCpuBytes;
	result.telemetry.residentGpuBytes = residentGpuBytes;
	result.telemetry.requestedCpuBytes = requestedCpuBytes;
	result.telemetry.requestedGpuBytes = requestedGpuBytes;
	return result;
}

const StreamingResourceDependency* SceneStreamingResidency::FindDependency(
	const uint64_t dependencyId) const
{
	const auto dependencyIt = dependencies.find(dependencyId);
	return dependencyIt == dependencies.end() ? nullptr : &dependencyIt->second;
}

void SceneStreamingResidency::AppendDependencyClosure(
	const uint64_t dependencyId,
	StreamingResidencyPlan& plan,
	std::unordered_set<uint64_t>& visitedDependencyIds) const
{
	if (dependencyId == 0u || !visitedDependencyIds.insert(dependencyId).second)
		return;

	const auto* dependency = FindDependency(dependencyId);
	if (dependency == nullptr)
		return;

	plan.dependencyClosure.push_back(*dependency);
	for (const auto childDependencyId : dependency->childDependencyIds)
		AppendDependencyClosure(childDependencyId, plan, visitedDependencyIds);
	if (dependency->fallbackDependencyId.has_value())
		AppendDependencyClosure(*dependency->fallbackDependencyId, plan, visitedDependencyIds);
}

ResidencyTicket& SceneStreamingResidency::GetOrCreateTicket(
	const uint64_t dependencyId,
	const uint32_t basePriority,
	const uint64_t frameSerial,
	const uint32_t coalescedRequestCount)
{
	auto [ticketIt, inserted] = tickets.emplace(dependencyId, ResidencyTicket{});
	auto& ticket = ticketIt->second;
	if (inserted)
	{
		ticket.ticketId = nextTicketId++;
		ticket.dependencyId = dependencyId;
		ticket.priority = basePriority;
		ticket.state = ResidencyTicketState::Requested;
		ticket.requestFrame = frameSerial;
		if (const auto* dependency = FindDependency(dependencyId); dependency != nullptr)
		{
			requestedCpuBytes += dependency->cpuBytes;
			requestedGpuBytes += dependency->gpuBytes;
		}
	}
	ticket.lastInterestFrame = frameSerial;
	ticket.coalescedRequestCount = coalescedRequestCount;
	return ticket;
}

uint32_t SceneStreamingResidency::ComputeAgedPriority(
	const ResidencyTicket& ticket,
	const uint32_t basePriority,
	const uint64_t frameSerial) const
{
	const auto age = frameSerial > ticket.requestFrame ? frameSerial - ticket.requestFrame : 0u;
	const auto clampedAge = std::min<uint64_t>(age, std::numeric_limits<uint32_t>::max());
	return basePriority + static_cast<uint32_t>(clampedAge);
}

bool SceneStreamingResidency::IsDependencyPinned(
	const uint64_t dependencyId,
	const StreamingResidencyFramePins& framePins) const
{
	return ContainsValue(framePins.pinnedDependencyIds, dependencyId);
}

void SceneStreamingResidency::SetTicketState(
	ResidencyTicket& ticket,
	const StreamingResourceDependency& dependency,
	const ResidencyTicketState state)
{
	if (ticket.state == state)
		return;

	const auto wasResident = IsResidentState(ticket.state);
	const auto willBeResident = IsResidentState(state);
	if (wasResident != willBeResident)
	{
		if (wasResident)
		{
			SaturatingSubtract(residentCpuBytes, dependency.cpuBytes);
			SaturatingSubtract(residentGpuBytes, dependency.gpuBytes);
			requestedCpuBytes += dependency.cpuBytes;
			requestedGpuBytes += dependency.gpuBytes;
			residentDependencyIds.erase(dependency.dependencyId);
		}
		else
		{
			SaturatingSubtract(requestedCpuBytes, dependency.cpuBytes);
			SaturatingSubtract(requestedGpuBytes, dependency.gpuBytes);
			residentCpuBytes += dependency.cpuBytes;
			residentGpuBytes += dependency.gpuBytes;
			residentDependencyIds.insert(dependency.dependencyId);
		}
	}

	ticket.state = state;
}

void SceneStreamingResidency::AppendResultTicket(
	StreamingCommitResult& result,
	std::unordered_set<uint64_t>& resultDependencyIds,
	const ResidencyTicket& ticket) const
{
	if (resultDependencyIds.insert(ticket.dependencyId).second)
	{
		result.tickets.push_back(ticket);
		return;
	}

	auto ticketIt = std::find_if(
		result.tickets.begin(),
		result.tickets.end(),
		[dependencyId = ticket.dependencyId](const ResidencyTicket& existing)
		{
			return existing.dependencyId == dependencyId;
		});
	if (ticketIt != result.tickets.end())
		*ticketIt = ticket;
}

void SceneStreamingResidency::ApplyMemoryBudgetEviction(
	const StreamingResidencyPlan& plan,
	const LargeSceneSettings& settings,
	const StreamingResidencyFramePins& framePins,
	std::unordered_set<uint64_t>& resultDependencyIds,
	StreamingCommitResult& result)
{
	if (residentCpuBytes <= settings.streamingCpuMemoryBudgetBytes &&
		residentGpuBytes <= settings.streamingGpuMemoryBudgetBytes)
	{
		result.cpuMemoryBudgetExhausted = false;
		result.gpuMemoryBudgetExhausted = false;
		return;
	}

	std::unordered_set<uint64_t> currentInterest;
	for (const auto& dependency : plan.dependencyClosure)
		currentInterest.insert(dependency.dependencyId);

	const auto residentIds = std::vector<uint64_t>(
		residentDependencyIds.begin(),
		residentDependencyIds.end());
	for (const auto dependencyId : residentIds)
	{
		auto ticketIt = tickets.find(dependencyId);
		if (ticketIt == tickets.end() ||
			ticketIt->second.state != ResidencyTicketState::VisibleResident ||
			currentInterest.find(dependencyId) != currentInterest.end())
		{
			continue;
		}

		const auto* dependency = FindDependency(dependencyId);
		if (dependency == nullptr)
			continue;
		SetTicketState(ticketIt->second, *dependency, ResidencyTicketState::Resident);
	}

	std::vector<const StreamingResourceDependency*> evictionCandidates;
	evictionCandidates.reserve(residentDependencyIds.size());
	for (const auto dependencyId : residentDependencyIds)
	{
		if (IsDependencyPinned(dependencyId, framePins))
			continue;
		const auto* dependency = FindDependency(dependencyId);
		if (dependency == nullptr || !dependency->fallbackDependencyId.has_value())
			continue;
		const auto fallbackTicketIt = tickets.find(*dependency->fallbackDependencyId);
		if (fallbackTicketIt == tickets.end() || !IsResidentState(fallbackTicketIt->second.state))
			continue;
		evictionCandidates.push_back(dependency);
	}

	std::sort(
		evictionCandidates.begin(),
		evictionCandidates.end(),
		[this](const StreamingResourceDependency* left, const StreamingResourceDependency* right)
		{
			const auto leftTicket = tickets.find(left->dependencyId);
			const auto rightTicket = tickets.find(right->dependencyId);
			const auto leftInterest = leftTicket == tickets.end() ? 0u : leftTicket->second.lastInterestFrame;
			const auto rightInterest = rightTicket == tickets.end() ? 0u : rightTicket->second.lastInterestFrame;
			if (leftInterest != rightInterest)
				return leftInterest < rightInterest;
			if (left->priority != right->priority)
				return left->priority < right->priority;
			return left->dependencyId < right->dependencyId;
		});

	for (const auto* candidate : evictionCandidates)
	{
		if (residentCpuBytes <= settings.streamingCpuMemoryBudgetBytes &&
			residentGpuBytes <= settings.streamingGpuMemoryBudgetBytes)
		{
			break;
		}
		if (candidate == nullptr)
			continue;
		const auto& dependency = *candidate;
		auto ticketIt = tickets.find(dependency.dependencyId);
		if (ticketIt == tickets.end())
			continue;
		if (!IsResidentState(ticketIt->second.state))
			continue;

		SetTicketState(ticketIt->second, dependency, ResidencyTicketState::Evicted);
		AppendResultTicket(result, resultDependencyIds, ticketIt->second);
		if (dependency.fallbackDependencyId.has_value())
		{
			const auto fallbackTicketIt = tickets.find(*dependency.fallbackDependencyId);
			if (fallbackTicketIt != tickets.end())
				AppendResultTicket(result, resultDependencyIds, fallbackTicketIt->second);
		}
		++result.telemetry.streamingEvictCount;
	}

	result.cpuMemoryBudgetExhausted = residentCpuBytes > settings.streamingCpuMemoryBudgetBytes;
	result.gpuMemoryBudgetExhausted = residentGpuBytes > settings.streamingGpuMemoryBudgetBytes;
}
}
