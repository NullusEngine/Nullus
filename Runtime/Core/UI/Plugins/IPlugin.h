#pragma once


namespace NLS::UI::Plugins
{
	/**
	* Interface to any plugin of UI.
	* A plugin is basically a behaviour that you can plug to a widget
	*/
	class IPlugin
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