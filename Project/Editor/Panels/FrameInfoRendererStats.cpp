#include "Panels/FrameInfo.h"

#include "Rendering/SceneVisibilityPipeline.h"
#include "UI/Widgets/AWidget.h"
#include "Utils/String.h"

#include "ImGui/imgui.h"

#include <iomanip>
#include <sstream>
#include <utility>

using namespace NLS;
using namespace NLS::UI;

namespace
{
	using FrameInfoTableRow = Editor::Panels::FrameInfoTableRow;

	std::string FormatUInt(const uint64_t value)
	{
		return Utils::String::ToString(value);
	}

	std::string FormatMs(const uint64_t nanoseconds)
	{
		std::ostringstream stream;
		stream << std::fixed << std::setprecision(3) << (static_cast<double>(nanoseconds) / 1000000.0);
		return stream.str();
	}

	std::string FormatPercent(const uint64_t numerator, const uint64_t denominator)
	{
		if (denominator == 0u)
			return "0.0%";

		std::ostringstream stream;
		stream << std::fixed << std::setprecision(1)
			<< ((static_cast<double>(numerator) * 100.0) / static_cast<double>(denominator))
			<< "%";
		return stream.str();
	}

	void AddRow(
		std::vector<FrameInfoTableRow>& rows,
		std::string section,
		std::string metric,
		std::string value,
		std::string note = "")
	{
		rows.push_back({
			std::move(section),
			std::move(metric),
			std::move(value),
			std::move(note)
		});
	}

	void AddMetricRow(
		std::vector<FrameInfoTableRow>& rows,
		std::string section,
		std::string metric,
		const uint64_t value,
		std::string note = "")
	{
		AddRow(
			rows,
			std::move(section),
			std::move(metric),
			FormatUInt(value),
			std::move(note));
	}

	void AddTimeRow(
		std::vector<FrameInfoTableRow>& rows,
		std::string section,
		std::string metric,
		const uint64_t nanoseconds,
		std::string note = "")
	{
		AddRow(
			rows,
			std::move(section),
			std::move(metric),
			FormatMs(nanoseconds) + " ms",
			std::move(note));
	}

	uint64_t LargeSceneCullReasonCount(
		const std::array<uint64_t, Render::Data::kLargeSceneCullReasonCount>& counts,
		const Engine::Rendering::CullReason reason)
	{
		const auto denseReason = static_cast<size_t>(reason);
		return denseReason < counts.size() ? counts[denseReason] : 0u;
	}

	uint64_t LargeSceneHLODCullCount(
		const std::array<uint64_t, Render::Data::kLargeSceneCullReasonCount>& counts)
	{
		return LargeSceneCullReasonCount(counts, Engine::Rendering::CullReason::HLODChildSuppressed) +
			LargeSceneCullReasonCount(counts, Engine::Rendering::CullReason::HLODProxyInactive);
	}

	uint64_t LargeSceneOtherCullCount(
		const std::array<uint64_t, Render::Data::kLargeSceneCullReasonCount>& counts)
	{
		uint64_t total = 0u;
		for (size_t reasonIndex = 0u; reasonIndex < counts.size(); ++reasonIndex)
			total += counts[reasonIndex];

		const auto knownSummaryTotal =
			LargeSceneCullReasonCount(counts, Engine::Rendering::CullReason::Visible) +
			LargeSceneCullReasonCount(counts, Engine::Rendering::CullReason::FrustumCulled) +
			LargeSceneCullReasonCount(counts, Engine::Rendering::CullReason::Occluded) +
			LargeSceneCullReasonCount(counts, Engine::Rendering::CullReason::LODInactive) +
			LargeSceneHLODCullCount(counts);
		return total > knownSummaryTotal ? total - knownSummaryTotal : 0u;
	}

	bool HasLegacyGeometryData(const Render::Data::FrameInfo& frameInfo)
	{
		return frameInfo.batchCount != 0u ||
			frameInfo.instanceCount != 0u ||
			frameInfo.polyCount != 0u ||
			frameInfo.vertexCount != 0u;
	}

	bool HasLargeSceneTelemetry(const Render::Data::FrameInfo& frameInfo)
	{
		return frameInfo.largeScene.registeredPrimitiveCount != 0u ||
			frameInfo.largeScene.visiblePrimitiveCount != 0u ||
			frameInfo.largeScene.visibilityTestedPrimitiveCount != 0u ||
			frameInfo.largeScene.rawVisibleDrawCount != 0u ||
			frameInfo.largeScene.submittedDrawCount != 0u ||
			frameInfo.largeScene.occlusionTestCount != 0u ||
			frameInfo.largeScene.streamingRequestCount != 0u ||
			frameInfo.largeScene.residentCpuBytes != 0u ||
			frameInfo.largeScene.residentGpuBytes != 0u ||
			frameInfo.largeScene.syncTimeNs != 0u ||
			frameInfo.largeScene.parallelVisibilityTimeNs != 0u ||
			frameInfo.largeScene.serialVisibilityTimeNs != 0u ||
			frameInfo.largeScene.queueFinalizationTimeNs != 0u ||
			frameInfo.largeScene.hzbBuildTimeNs != 0u ||
			frameInfo.largeScene.hzbHistoryPruneTimeNs != 0u ||
			frameInfo.largeScene.streamingCommitTimeNs != 0u ||
			frameInfo.largeScene.finalizationTouchedCommandCount != 0u;
	}

	uint64_t PreferredSubmittedDrawCount(const Render::Data::FrameInfo& frameInfo)
	{
		return frameInfo.largeScene.submittedDrawCount != 0u
			? frameInfo.largeScene.submittedDrawCount
			: frameInfo.submittedSceneDrawCount;
	}

	uint64_t PreferredRawDrawCount(const Render::Data::FrameInfo& frameInfo)
	{
		return frameInfo.largeScene.rawVisibleDrawCount != 0u
			? frameInfo.largeScene.rawVisibleDrawCount
			: frameInfo.rawVisibleObjectCount;
	}

	uint64_t PreferredDynamicGroupCount(const Render::Data::FrameInfo& frameInfo)
	{
		return frameInfo.largeScene.dynamicInstanceGroupCount != 0u
			? frameInfo.largeScene.dynamicInstanceGroupCount
			: frameInfo.dynamicInstanceGroupCount;
	}

	std::string BuildSafetyValue(const Render::Data::FrameInfo& frameInfo)
	{
		if (frameInfo.deviceLostDetected)
			return "Device Lost";

		return "Device OK";
	}

	std::string BuildSafetyNote(const Render::Data::FrameInfo& frameInfo)
	{
		if (frameInfo.deviceLostDetected && !frameInfo.deviceLostReason.empty())
			return frameInfo.deviceLostReason;

		if (frameInfo.unsafeGpuWorkQuarantined)
		{
			if (!frameInfo.unsafeGpuWorkQuarantineReason.empty())
				return frameInfo.unsafeGpuWorkQuarantineReason;

			return "Unsafe GPU work quarantined";
		}

		return "No GPU safety quarantine";
	}

	std::string BuildBottleneckValue(
		const Render::Data::FrameInfo& frameInfo,
		const bool hasLargeSceneTelemetry)
	{
		const auto submittedDrawCount = PreferredSubmittedDrawCount(frameInfo);
		const auto rawDrawCount = PreferredRawDrawCount(frameInfo);
		if (submittedDrawCount == 0u && rawDrawCount == 0u)
			return "Idle";

		if (hasLargeSceneTelemetry)
			return "Visibility and Draw Submission";

		return "Draw Submission";
	}

	std::string BuildBottleneckNote(const Render::Data::FrameInfo& frameInfo)
	{
		return "Submitted " +
			FormatUInt(PreferredSubmittedDrawCount(frameInfo)) +
			" of " +
			FormatUInt(PreferredRawDrawCount(frameInfo)) +
			" raw draws";
	}

	std::string BuildOcclusionVerdictValue(const Render::Data::FrameInfo& frameInfo)
	{
		const auto tests = frameInfo.largeScene.occlusionTestCount;
		if (tests == 0u)
			return "No Data";

		if (frameInfo.largeScene.occlusionCulledCount == 0u)
			return "Active, No Benefit";

		const auto percentage =
			(static_cast<double>(frameInfo.largeScene.occlusionCulledCount) * 100.0) /
			static_cast<double>(tests);
		return percentage >= 10.0 ? "Active, Useful" : "Active, Low Benefit";
	}

	std::string BuildOcclusionVerdictNote(const Render::Data::FrameInfo& frameInfo)
	{
		const auto tests = frameInfo.largeScene.occlusionTestCount;
		if (tests == 0u)
			return "Large-scene occlusion telemetry unavailable";

		return FormatUInt(frameInfo.largeScene.occlusionCulledCount) +
			" of " +
			FormatUInt(tests) +
			" tests culled (" +
			FormatPercent(frameInfo.largeScene.occlusionCulledCount, tests) +
			")";
	}

	const char* ToFramePublishStateText(const Render::Data::FramePublishState publishState)
	{
		switch (publishState)
		{
		case Render::Data::FramePublishState::Direct:
			return "Direct";
		case Render::Data::FramePublishState::Open:
			return "Open";
		case Render::Data::FramePublishState::BackPressured:
			return "BackPressured";
		default:
			return "Unknown";
		}
	}

	const char* ToThreadedFrameStageSummaryText(const Render::Data::ThreadedFrameStageSummary stageSummary)
	{
		switch (stageSummary)
		{
		case Render::Data::ThreadedFrameStageSummary::Direct:
			return "Direct";
		case Render::Data::ThreadedFrameStageSummary::Logic:
			return "Logic";
		case Render::Data::ThreadedFrameStageSummary::RenderScene:
			return "RenderScene";
		case Render::Data::ThreadedFrameStageSummary::Rhi:
			return "RHI";
		case Render::Data::ThreadedFrameStageSummary::Retired:
			return "Retired";
		default:
			return "Unknown";
		}
	}

	const char* ToFrameRetirementStateText(const Render::Data::FrameRetirementState retirementState)
	{
		switch (retirementState)
		{
		case Render::Data::FrameRetirementState::Direct:
			return "Direct";
		case Render::Data::FrameRetirementState::Pending:
			return "Pending";
		case Render::Data::FrameRetirementState::Ready:
			return "Ready";
		case Render::Data::FrameRetirementState::Consumed:
			return "Consumed";
		default:
			return "Unknown";
		}
	}

	std::string BuildFrameStateValue(const Render::Data::FrameInfo& frameInfo)
	{
		return std::string(ToThreadedFrameStageSummaryText(frameInfo.stageSummary)) +
			" -> " +
			ToFramePublishStateText(frameInfo.publishState) +
			" -> " +
			ToFrameRetirementStateText(frameInfo.retirementState);
	}

	std::string BuildFrameStateNote(const Render::Data::FrameInfo& frameInfo)
	{
		return "InFlight " +
			FormatUInt(frameInfo.inFlightFrameCount) +
			", Blocked " +
			FormatUInt(frameInfo.blockedFrameCount);
	}

	std::string BuildSubmittedDrawNote(const Render::Data::FrameInfo& frameInfo)
	{
		return "Raw " +
			FormatUInt(PreferredRawDrawCount(frameInfo)) +
			", Groups " +
			FormatUInt(PreferredDynamicGroupCount(frameInfo)) +
			", Largest " +
			FormatUInt(frameInfo.largestInstanceGroupSize) +
			", Dropped " +
			FormatUInt(frameInfo.objectDataOverflowDroppedObjectCount);
	}

	std::string BuildStreamingRequestNote(const Render::Data::FrameInfo& frameInfo)
	{
		return "Dependencies " +
			FormatUInt(frameInfo.largeScene.streamingDependencyCount) +
			", Tickets " +
			FormatUInt(frameInfo.largeScene.residencyTicketCount);
	}
}

namespace NLS::Editor::Panels
{
	class FrameInfoTableWidget final : public UI::Widgets::AWidget
	{
	public:
		void SetRows(std::vector<FrameInfoTableRow> rows)
		{
			m_rows = std::move(rows);
		}

		const std::vector<FrameInfoTableRow>& GetRows() const
		{
			return m_rows;
		}

	private:
		void _Draw_Impl() override
		{
			if (m_rows.empty())
				return;

			constexpr ImGuiTableFlags tableFlags =
				ImGuiTableFlags_BordersInnerV |
				ImGuiTableFlags_BordersOuter |
				ImGuiTableFlags_RowBg |
				ImGuiTableFlags_Resizable |
				ImGuiTableFlags_SizingStretchProp;

			if (!ImGui::BeginTable("FrameInfoTable", 4, tableFlags))
				return;

			ImGui::TableSetupColumn("Section", ImGuiTableColumnFlags_WidthFixed, 110.0f);
			ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthFixed, 150.0f);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthStretch, 1.4f);
			ImGui::TableHeadersRow();

			std::string previousSection;
			for (const auto& row : m_rows)
			{
				ImGui::TableNextRow();

				ImGui::TableSetColumnIndex(0);
				if (row.section != previousSection)
				{
					ImGui::TextColored(ImVec4(0.58f, 0.77f, 1.0f, 1.0f), "%s", row.section.c_str());
					previousSection = row.section;
				}

				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%s", row.metric.c_str());

				ImGui::TableSetColumnIndex(2);
				ImGui::Text("%s", row.value.c_str());

				ImGui::TableSetColumnIndex(3);
				ImGui::TextWrapped("%s", row.note.c_str());
			}

			ImGui::EndTable();
		}

		std::vector<FrameInfoTableRow> m_rows;
	};
}

Editor::Panels::FrameInfo::FrameInfo
(
	const std::string& p_title,
	bool p_opened,
	const UI::PanelWindowSettings& p_windowSettings
) : PanelWindow(p_title, p_opened, p_windowSettings),
	m_table(CreateWidget<FrameInfoTableWidget>())
{
}

const std::vector<Editor::Panels::FrameInfoTableRow>& Editor::Panels::FrameInfo::GetDebugRowsForTesting() const
{
	return m_table.GetRows();
}

void Editor::Panels::FrameInfo::UpdateForFrameInfo(
	const std::string& viewName,
	const Render::Data::FrameInfo& frameInfo)
{
	const bool hasLargeSceneTelemetry = HasLargeSceneTelemetry(frameInfo);
	std::vector<FrameInfoTableRow> rows;
	rows.reserve(hasLargeSceneTelemetry ? 42u : 20u);

	AddRow(rows, "Status", "Target View", viewName);
	AddRow(rows, "Status", "Frame State", BuildFrameStateValue(frameInfo), BuildFrameStateNote(frameInfo));
	AddRow(rows, "Status", "Safety", BuildSafetyValue(frameInfo), BuildSafetyNote(frameInfo));
	AddRow(rows, "Verdict", "Bottleneck", BuildBottleneckValue(frameInfo, hasLargeSceneTelemetry), BuildBottleneckNote(frameInfo));
	AddRow(rows, "Verdict", "Occlusion", BuildOcclusionVerdictValue(frameInfo), BuildOcclusionVerdictNote(frameInfo));
	AddMetricRow(rows, "Render Load", "Submitted Draws", PreferredSubmittedDrawCount(frameInfo), BuildSubmittedDrawNote(frameInfo));
	AddMetricRow(rows, "Render Load", "Raw Visible Draws", PreferredRawDrawCount(frameInfo));
	AddMetricRow(rows, "Render Load", "Instance Groups", PreferredDynamicGroupCount(frameInfo));
	AddMetricRow(rows, "Render Load", "Largest Instance Group", frameInfo.largestInstanceGroupSize);
	AddMetricRow(rows, "Render Load", "Dropped Objects", frameInfo.objectDataOverflowDroppedObjectCount);

	if (hasLargeSceneTelemetry)
	{
		const auto& counts = frameInfo.largeScene.culledByReason;

		AddMetricRow(rows, "Large Scene", "Registered Primitives", frameInfo.largeScene.registeredPrimitiveCount);
		AddMetricRow(rows, "Large Scene", "Visible Primitives", frameInfo.largeScene.visiblePrimitiveCount);
		AddMetricRow(rows, "Large Scene", "Visible Meshes", frameInfo.largeScene.visibleMeshCount);
		AddMetricRow(rows, "Large Scene", "Finalized Commands", frameInfo.largeScene.finalizationTouchedCommandCount);
		AddMetricRow(rows, "Culling", "Tested Primitives", frameInfo.largeScene.visibilityTestedPrimitiveCount);
		AddMetricRow(rows, "Culling", "Visible", LargeSceneCullReasonCount(counts, Engine::Rendering::CullReason::Visible));
		AddMetricRow(rows, "Culling", "Frustum Culled", LargeSceneCullReasonCount(counts, Engine::Rendering::CullReason::FrustumCulled));
		AddMetricRow(rows, "Culling", "Occluded", LargeSceneCullReasonCount(counts, Engine::Rendering::CullReason::Occluded));
		AddMetricRow(rows, "Culling", "LOD Inactive", LargeSceneCullReasonCount(counts, Engine::Rendering::CullReason::LODInactive));
		AddMetricRow(rows, "Culling", "HLOD Suppressed", LargeSceneHLODCullCount(counts));
		AddMetricRow(rows, "Culling", "Other Reasons", LargeSceneOtherCullCount(counts));
		AddMetricRow(rows, "Culling", "Spatial Candidates", frameInfo.largeScene.spatialCandidateCount);
		AddMetricRow(rows, "Culling", "Full Scan Candidates", frameInfo.largeScene.fullScanCandidateCount);
		AddMetricRow(rows, "Culling", "Records Touched", frameInfo.largeScene.primitiveRecordsTouched);
		AddRow(
			rows,
			"Occlusion",
			"Efficiency",
			FormatPercent(frameInfo.largeScene.occlusionCulledCount, frameInfo.largeScene.occlusionTestCount),
			FormatUInt(frameInfo.largeScene.occlusionCulledCount) +
				" culled from " +
				FormatUInt(frameInfo.largeScene.occlusionTestCount) +
				" tests");
		AddMetricRow(rows, "Occlusion", "Tests", frameInfo.largeScene.occlusionTestCount);
		AddMetricRow(rows, "Occlusion", "Culled", frameInfo.largeScene.occlusionCulledCount);
		AddTimeRow(
			rows,
			"Occlusion",
			"HZB Build",
			frameInfo.largeScene.hzbBuildTimeNs,
			"History pruned " +
				FormatUInt(frameInfo.largeScene.hzbHistoryPruneRemovedHandleCount) +
				" handles");
		AddMetricRow(rows, "Streaming", "Requests", frameInfo.largeScene.streamingRequestCount, BuildStreamingRequestNote(frameInfo));
		AddMetricRow(rows, "Streaming", "Commits", frameInfo.largeScene.streamingCommitCount);
		AddMetricRow(rows, "Streaming", "Evicts", frameInfo.largeScene.streamingEvictCount);
		AddMetricRow(rows, "Streaming", "CPU Resident Bytes", frameInfo.largeScene.residentCpuBytes);
		AddMetricRow(rows, "Streaming", "CPU Requested Bytes", frameInfo.largeScene.requestedCpuBytes);
		AddMetricRow(rows, "Streaming", "GPU Resident Bytes", frameInfo.largeScene.residentGpuBytes);
		AddMetricRow(rows, "Streaming", "GPU Requested Bytes", frameInfo.largeScene.requestedGpuBytes);
		AddTimeRow(rows, "Timing", "Sync", frameInfo.largeScene.syncTimeNs);
		AddTimeRow(rows, "Timing", "Visibility", frameInfo.largeScene.serialVisibilityTimeNs + frameInfo.largeScene.parallelVisibilityTimeNs);
		AddTimeRow(rows, "Timing", "Finalize", frameInfo.largeScene.queueFinalizationTimeNs);
		AddTimeRow(rows, "Timing", "Streaming Commit", frameInfo.largeScene.streamingCommitTimeNs);
	}

	AddMetricRow(rows, "Inputs", "Opaque Drawables", frameInfo.parsedOpaqueDrawableCount);
	AddMetricRow(rows, "Inputs", "Transparent Drawables", frameInfo.parsedTransparentDrawableCount);
	AddMetricRow(rows, "Inputs", "Skybox Drawables", frameInfo.parsedSkyboxDrawableCount);

	if (HasLegacyGeometryData(frameInfo))
	{
		AddMetricRow(rows, "Debug", "Batches", frameInfo.batchCount);
		AddMetricRow(rows, "Debug", "Instances", frameInfo.instanceCount);
		AddMetricRow(rows, "Debug", "Polygons", frameInfo.polyCount);
		AddMetricRow(rows, "Debug", "Vertices", frameInfo.vertexCount);
	}

	AddMetricRow(rows, "Debug", "Command Rebuilds", frameInfo.cachedCommandRebuildCount);
	AddMetricRow(rows, "Debug", "Binding Sets", frameInfo.renderBindingSetCreationCount);
	AddMetricRow(rows, "Debug", "Snapshot Buffers", frameInfo.renderSnapshotBufferCreationCount);
	AddMetricRow(rows, "Debug", "ParseScene Calls", frameInfo.parseSceneCallCount);
	AddMetricRow(rows, "Debug", "Parallel Work Units", frameInfo.parallelCommandWorkUnitCount);
	AddMetricRow(
		rows,
		"Debug",
		"Parallel Workers",
		frameInfo.parallelRecordingWorkerCount,
		frameInfo.parallelFallbackReason.empty() ? "No fallback" : frameInfo.parallelFallbackReason);
	AddMetricRow(rows, "Debug", "GBuffer Resolve Hits", frameInfo.gBufferMaterialResolveHitCount);
	AddMetricRow(rows, "Debug", "GBuffer Resolve Misses", frameInfo.gBufferMaterialResolveMissCount);
	AddMetricRow(rows, "Debug", "Prepared Cache Hits", frameInfo.preparedRecordedDrawStaticBaseCacheHitCount, "Static base");
	AddMetricRow(rows, "Debug", "Prepared Cache Misses", frameInfo.preparedRecordedDrawStaticBaseCacheMissCount, "Static base");
	AddRow(rows, "Debug", "GBuffer Syncs", FormatUInt(frameInfo.gBufferMaterialSyncCount));

	if (hasLargeSceneTelemetry)
	{
		AddMetricRow(rows, "Debug", "Static Primitives", frameInfo.largeScene.staticPrimitiveCount);
		AddMetricRow(rows, "Debug", "Dynamic Primitives", frameInfo.largeScene.dynamicPrimitiveCount);
		AddMetricRow(rows, "Debug", "Unclassified Primitives", frameInfo.largeScene.unclassifiedPrimitiveCount);
		AddMetricRow(rows, "Debug", "Allocated Primitive Slots", frameInfo.largeScene.allocatedPrimitiveSlotCount);
		AddMetricRow(rows, "Debug", "Tombstoned Primitive Slots", frameInfo.largeScene.tombstonedPrimitiveSlotCount);
	}

	m_table.SetRows(std::move(rows));
}
