#pragma once

#include <cstdint>
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
		uint64_t GetLastDrawDurationUs() const;

	protected:
		virtual const std::string& GetProfilerName() const;
		const char* GetProfilerScopeName();

	protected:
		virtual void _Draw_Impl() = 0;

	public:
		bool enabled = true;

	protected:
		std::string m_panelID;
		std::string m_cachedProfilerName;
		std::string m_cachedProfilerScopeName;
		uint64_t m_lastDrawDurationUs = 0u;

	private:
		static uint64_t __PANEL_ID_INCREMENT;
	};
}
