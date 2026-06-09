#pragma once

#include <string>
#include <utility>
#include <vector>

#include <UI/Panels/PanelWindow.h>

#include <Panels/AView.h>

#include <Rendering/Data/FrameInfo.h>

namespace NLS::Editor::Panels
{
	struct FrameInfoTableRow
	{
		std::string section;
		std::string metric;
		std::string value;
		std::string note;
	};

	struct FrameInfoViewSnapshot
	{
		std::string viewName;
		Render::Data::FrameInfo frameInfo;
	};

	class FrameInfoTableWidget;

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
		void RefreshForViews(const std::vector<AView*>& targetViews);
		void UpdateForFrameInfo(const std::string& viewName, const Render::Data::FrameInfo& frameInfo);
		void UpdateForFrameInfoViews(const std::vector<FrameInfoViewSnapshot>& viewSnapshots);
		void SetCandidateViews(std::vector<AView*> candidateViews);
		void SetTargetView(AView* p_targetView);
		AView* GetTargetView() const;
		const std::vector<FrameInfoTableRow>& GetDebugRowsForTesting() const;

	protected:
		void OnBeforeDrawWidgets() override;

	private:
		FrameInfoTableWidget& m_table;
		AView* m_targetView = nullptr;
		std::vector<AView*> m_candidateViews;
	};
}
