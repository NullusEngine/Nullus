#pragma once

#include <UI/Panels/PanelWindow.h>
#include <UI/Widgets/Texts/TextColored.h>
#include <UI/Widgets/Selection/CheckBox.h>
#include <UI/Widgets/Visual/Separator.h>

#include <Panels/AView.h>

#include <Rendering/Data/FrameInfo.h>

namespace NLS::Editor::Panels
{
	class FrameInfo : public UI::PanelWindow
	{
	public:
		/**
		* Constructor
		* @param p_title
		* @param p_opened
		* @param p_windowSettings
		*/
		FrameInfo(
			const std::string& p_title,
			bool p_opened,
			const UI::PanelWindowSettings& p_windowSettings
		);

		/**
		* Update frame info information
		* @param p_targetView
		*/
		void RefreshForView(AView* p_targetView);
		void UpdateForFrameInfo(const std::string& viewName, const Render::Data::FrameInfo& frameInfo);
		void SetTargetView(AView* p_targetView);
		AView* GetTargetView() const;

	protected:
		void OnBeforeDrawWidgets() override;

	private:
		UI::Widgets::Text& m_viewNameText;
		UI::Widgets::Separator& m_separator;
		UI::Widgets::Text& m_batchCountText;
		UI::Widgets::Text& m_instanceCountText;
		UI::Widgets::Text& m_polyCountText;
		UI::Widgets::Text& m_vertexCountText;
		UI::Widgets::Text& m_parseSceneText;
		UI::Widgets::Text& m_drawableCountText;
		UI::Widgets::Text& m_drawCallOptimizationText;
		UI::Widgets::Text& m_parallelWorkText;
		UI::Widgets::Text& m_largeScenePrimitiveText;
		UI::Widgets::Text& m_largeSceneVisibilityText;
		UI::Widgets::Text& m_largeSceneCullReasonText;
		UI::Widgets::Text& m_largeSceneSyncText;
		UI::Widgets::Text& m_largeSceneFinalizationText;
		UI::Widgets::Text& m_largeSceneResidencyText;
		UI::Widgets::Text& m_largeSceneStreamingText;
		UI::Widgets::Text& m_largeSceneHZBHistoryPruneText;
		UI::Widgets::Text& m_largeSceneTimingText;
		UI::Widgets::Text& m_gBufferMaterialSyncText;
		UI::Widgets::Text& m_gBufferMaterialResolveText;
		UI::Widgets::Text& m_preparedDrawStaticBaseCacheText;
		UI::Widgets::Text& m_bindingSetCreationText;
		UI::Widgets::Text& m_snapshotBufferCreationText;
		UI::Widgets::Text& m_framesInFlightText;
		UI::Widgets::Text& m_blockedFramesText;
		UI::Widgets::Text& m_publishStateText;
		UI::Widgets::Text& m_frameStageText;
		UI::Widgets::Text& m_retirementStateText;
		UI::Widgets::Text& m_rhiSafetyText;
		AView* m_targetView = nullptr;
	};
}
