#include <Debug/Logger.h>

#include <Rendering/Features/FrameInfoRenderFeature.h>

#include "Panels/FrameInfo.h"
#include "Core/EditorActions.h"
#include "Utils/String.h"
using namespace NLS;
using namespace NLS::UI;

constexpr Render::Data::FrameInfo kEmptyFrameInfo{};

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

const Render::Data::FrameInfo& GetFrameInfoFromView(const Editor::Panels::AView& p_view)
{
	return p_view
		.GetRenderer()
		.GetFeature<Render::Features::FrameInfoRenderFeature>()
		.GetFrameInfo();
}

void Editor::Panels::FrameInfo::Update(AView* p_targetView)
{
	using namespace Utils;

	m_viewNameText.content = "Target View: " + (p_targetView ? p_targetView->name : "None");

	auto& frameInfo = p_targetView ? GetFrameInfoFromView(*p_targetView) : kEmptyFrameInfo;

	m_batchCountText.content = "Batches: " + String::ToString(frameInfo.batchCount);
    m_instanceCountText.content = "Instances: " + String::ToString(frameInfo.instanceCount);
    m_polyCountText.content = "Polygons: " + String::ToString(frameInfo.polyCount);
    m_vertexCountText.content = "Vertices: " + String::ToString(frameInfo.vertexCount);
}
