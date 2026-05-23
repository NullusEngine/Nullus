#include "Panels/FrameInfo.h"

#include "Rendering/Core/CompositeRenderer.h"
#include "UI/Panels/APanel.h"
#include "Utils/String.h"

using namespace NLS;
using namespace NLS::UI;

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
	m_targetPanelDrawText(CreateWidget<Widgets::Text>("")),
	m_framesInFlightText(CreateWidget<Widgets::Text>("")),
	m_blockedFramesText(CreateWidget<Widgets::Text>("")),
	m_publishStateText(CreateWidget<Widgets::Text>("")),
	m_frameStageText(CreateWidget<Widgets::Text>("")),
	m_retirementStateText(CreateWidget<Widgets::Text>(""))
{
	m_polyCountText.lineBreak = false;
}

void Editor::Panels::FrameInfo::UpdateForRenderer(
	const std::string& viewName,
	const Render::Core::CompositeRenderer& renderer)
{
	using namespace Utils;

	m_viewNameText.content = "Target View: " + viewName;

	const auto& frameInfo = renderer.GetFrameInfo();

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
	SetTargetPanelDrawTime(nullptr);
	m_framesInFlightText.content = "Frames In Flight: " + String::ToString(frameInfo.inFlightFrameCount);
	m_blockedFramesText.content = "Blocked Frames: " + String::ToString(frameInfo.blockedFrameCount);
	const char* publishState = "Direct";
	switch (frameInfo.publishState)
	{
	case Render::Data::FramePublishState::Open:
		publishState = "Open";
		break;
	case Render::Data::FramePublishState::BackPressured:
		publishState = "BackPressured";
		break;
	case Render::Data::FramePublishState::Direct:
	default:
		break;
	}
	m_publishStateText.content = std::string("Publish State: ") + publishState;

	const char* frameStage = "Direct";
	switch (frameInfo.stageSummary)
	{
	case Render::Data::ThreadedFrameStageSummary::Logic:
		frameStage = "Logic";
		break;
	case Render::Data::ThreadedFrameStageSummary::RenderScene:
		frameStage = "RenderScene";
		break;
	case Render::Data::ThreadedFrameStageSummary::Rhi:
		frameStage = "RHI";
		break;
	case Render::Data::ThreadedFrameStageSummary::Retired:
		frameStage = "Retired";
		break;
	case Render::Data::ThreadedFrameStageSummary::Direct:
	default:
		break;
	}
	m_frameStageText.content = std::string("Frame Stage: ") + frameStage;

	const char* retirementState = "Direct";
	switch (frameInfo.retirementState)
	{
	case Render::Data::FrameRetirementState::Pending:
		retirementState = "Pending";
		break;
	case Render::Data::FrameRetirementState::Ready:
		retirementState = "Ready";
		break;
	case Render::Data::FrameRetirementState::Consumed:
		retirementState = "Consumed";
		break;
	case Render::Data::FrameRetirementState::Direct:
	default:
		break;
	}
	m_retirementStateText.content = std::string("Retirement State: ") + retirementState;
}

void Editor::Panels::FrameInfo::SetTargetPanelDrawTime(const UI::APanel* panel)
{
	using namespace Utils;
	const auto durationUs = panel != nullptr ? panel->GetLastDrawDurationUs() : 0u;
	m_targetPanelDrawText.content = "Target Panel Draw: " + String::ToString(durationUs) + " us";
}
