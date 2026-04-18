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
		return;
	}

	UpdateForRenderer(p_targetView->name, p_targetView->GetRenderer());
}
