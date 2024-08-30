#pragma once

#include "UI/UIDef.h"

namespace NLS::UI
{
	/**
	* Interface to any plugin of UI.
	* A plugin is basically a behaviour that you can plug to a widget
	*/
	class NLS_UI_API IPlugin
	{
	public:
		/**
		* Execute the plugin behaviour
		*/
		virtual void Execute() = 0;

		/* Feel free to store any data you want here */
		void* userData = nullptr;
	};
}