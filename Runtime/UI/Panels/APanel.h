#pragma once

#include <vector>
#include <unordered_map>

#include "UI/UIDef.h"
#include "UI/Internal/WidgetContainer.h"

namespace NLS::UI
{
	/**
	* A Panel is a component of a canvas. It is a sort of window in the UI
	*/
	class NLS_UI_API APanel : public IDrawable, public Internal::WidgetContainer
	{
	public:
		/**
		* Constructor
		*/
		APanel();

		/**
		* Draw the panel
		*/
		void Draw() override;

		/**
		* Returns the panel identifier
		*/
		const std::string& GetPanelID() const;

	protected:
		virtual void _Draw_Impl() = 0;

	public:
		bool enabled = true;

	protected:
		std::string m_panelID;

	private:
		static uint64_t __PANEL_ID_INCREMENT;
	};
}