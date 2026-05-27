#include "Panels/FrameInfo.h"

#include "Utils/String.h"

using namespace NLS;
using namespace NLS::UI;

namespace
{
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
}

Editor::Panels::FrameInfo::FrameInfo
(
	const std::string& p_title,
	bool p_opened,
	const UI::PanelWindowSettings& p_windowSettings
) : PanelWindow(p_title, p_opened, p_windowSettings),

	m_viewNameText(CreateWidget<Widgets::Text>()),

	m_separator(CreateWidget<Widgets::Separator>()),

	m_batchCountText(CreateWidget<Widgets::Text>("")),
	m_instanceCountText(CreateWidget<Widgets::Text>("")),
	m_polyCountText(CreateWidget<Widgets::Text>("")),
	m_vertexCountText(CreateWidget<Widgets::Text>("")),
	m_parseSceneText(CreateWidget<Widgets::Text>("")),
	m_drawableCountText(CreateWidget<Widgets::Text>("")),
	m_gBufferMaterialSyncText(CreateWidget<Widgets::Text>("")),
	m_bindingSetCreationText(CreateWidget<Widgets::Text>("")),
	m_snapshotBufferCreationText(CreateWidget<Widgets::Text>("")),
	m_framesInFlightText(CreateWidget<Widgets::Text>("")),
	m_blockedFramesText(CreateWidget<Widgets::Text>("")),
	m_publishStateText(CreateWidget<Widgets::Text>("")),
	m_frameStageText(CreateWidget<Widgets::Text>("")),
	m_retirementStateText(CreateWidget<Widgets::Text>(""))
{
	m_polyCountText.lineBreak = false;
}

void Editor::Panels::FrameInfo::UpdateForFrameInfo(
	const std::string& viewName,
	const Render::Data::FrameInfo& frameInfo)
{
	using namespace Utils;

	m_viewNameText.content = "Target View: " + viewName;

	m_batchCountText.content = "Batches: " + String::ToString(frameInfo.batchCount);
	m_instanceCountText.content = "Instances: " + String::ToString(frameInfo.instanceCount);
	m_polyCountText.content = "Polygons: " + String::ToString(frameInfo.polyCount);
	m_vertexCountText.content = "Vertices: " + String::ToString(frameInfo.vertexCount);
	m_parseSceneText.content = "ParseScene Calls: " + String::ToString(frameInfo.parseSceneCallCount);
	m_drawableCountText.content =
		"Drawables O/T/S: " +
		String::ToString(frameInfo.parsedOpaqueDrawableCount) + "/" +
		String::ToString(frameInfo.parsedTransparentDrawableCount) + "/" +
		String::ToString(frameInfo.parsedSkyboxDrawableCount);
	m_gBufferMaterialSyncText.content = "GBuffer Material Syncs: " + String::ToString(frameInfo.gBufferMaterialSyncCount);
	m_bindingSetCreationText.content = "Binding Sets Created: " + String::ToString(frameInfo.renderBindingSetCreationCount);
	m_snapshotBufferCreationText.content = "Snapshot Buffers Created: " + String::ToString(frameInfo.renderSnapshotBufferCreationCount);
	m_framesInFlightText.content = "Frames In Flight: " + String::ToString(frameInfo.inFlightFrameCount);
	m_blockedFramesText.content = "Blocked Frames: " + String::ToString(frameInfo.blockedFrameCount);
	m_publishStateText.content =
		std::string("Publish State: ") + ToFramePublishStateText(frameInfo.publishState);
	m_frameStageText.content =
		std::string("Frame Stage: ") + ToThreadedFrameStageSummaryText(frameInfo.stageSummary);
	m_retirementStateText.content =
		std::string("Retirement State: ") + ToFrameRetirementStateText(frameInfo.retirementState);
}
