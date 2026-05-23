#include "Panels/FrameInfo.h"
using namespace NLS;

void Editor::Panels::FrameInfo::Update(AView* p_targetView)
{
	if (p_targetView == nullptr)
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
