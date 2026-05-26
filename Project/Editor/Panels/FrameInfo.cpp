#include "Panels/FrameInfo.h"
using namespace NLS;

void Editor::Panels::FrameInfo::SetTargetView(AView* p_targetView)
{
	m_targetView = p_targetView;
}

Editor::Panels::AView* Editor::Panels::FrameInfo::GetTargetView() const
{
	return m_targetView;
}

void Editor::Panels::FrameInfo::OnBeforeDrawWidgets()
{
	RefreshForView(m_targetView);
}

void Editor::Panels::FrameInfo::RefreshForView(AView* p_targetView)
{
	if (p_targetView == nullptr || !p_targetView->IsOpened())
	{
		m_viewNameText.content = "Target View: None";
		m_batchCountText.content = "Batches: 0";
		m_instanceCountText.content = "Instances: 0";
		m_polyCountText.content = "Polygons: 0";
		m_vertexCountText.content = "Vertices: 0";
		m_parseSceneText.content = "ParseScene Calls: 0";
		m_drawableCountText.content = "Drawables O/T/S: 0/0/0";
		m_gBufferMaterialSyncText.content = "GBuffer Material Syncs: 0";
		m_bindingSetCreationText.content = "Binding Sets Created: 0";
		m_snapshotBufferCreationText.content = "Snapshot Buffers Created: 0";
		SetTargetPanelDrawTime(nullptr);
		m_framesInFlightText.content = "Frames In Flight: 0";
		m_blockedFramesText.content = "Blocked Frames: 0";
		m_publishStateText.content = "Publish State: Direct";
		m_frameStageText.content = "Frame Stage: Direct";
		m_retirementStateText.content = "Retirement State: Direct";
		return;
	}

	UpdateForRenderer(p_targetView->name, p_targetView->GetRenderer());
	SetTargetPanelDrawTime(p_targetView);
}
