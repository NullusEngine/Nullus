#pragma once

#include "UI/Plugins/IPlugin.h"
#include "UI/Internal/WidgetContainer.h"
#include "UI/Widgets/Menu/MenuList.h"
#include "UI/Widgets/Menu/MenuItem.h"

namespace NLS::UI
{
	/**
	* A simple plugin that will show a contextual menu on right click
	* You can add widgets to a contextual menu
	*/
	class NLS_UI_API ContextualMenu : public IPlugin, public Internal::WidgetContainer
	{
	public:
		/**
		* Execute the plugin
		*/
		void Execute();

		/**
		* Force close the contextual menu
		*/
		void Close();
	};
}
