#pragma once

#include "UI/UIDef.h"

namespace NLS::UI
{
	/**
	* Data structure to send to the panel window constructor to define its settings
	*/
	struct NLS_UI_API PanelWindowSettings
	{
		bool closable					= false;
		bool resizable					= true;
		bool movable					= true;
		bool dockable					= false;
		bool scrollable					= true;
		bool hideBackground				= false;
		bool forceHorizontalScrollbar	= false;
		bool forceVerticalScrollbar		= false;
		bool allowHorizontalScrollbar	= false;
		bool bringToFrontOnFocus		= true;
		bool collapsable				= false;
		bool allowInputs				= true;
		bool titleBar					= true;
		bool autoSize					= false;
	};
}