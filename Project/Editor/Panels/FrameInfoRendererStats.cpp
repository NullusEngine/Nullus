#include "Panels/FrameInfo.h"

#include "Rendering/Core/CompositeRenderer.h"
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
	m_vertexCountText(CreateWidget<Widgets::Text>(""))
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
}
