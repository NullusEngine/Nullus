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
	static const Render::Data::FrameInfo kEmptyFrameInfo;
	if (p_targetView == nullptr || !p_targetView->IsOpened())
	{
		UpdateForFrameInfo("None", kEmptyFrameInfo);
		return;
	}

	const auto& frameInfo = p_targetView->GetLastRenderedFrameInfoSnapshot();
	UpdateForFrameInfo(
		p_targetView->name,
		frameInfo.has_value() ? frameInfo.value() : kEmptyFrameInfo);
}
