#pragma once

#include "Context.h"
#include "Editor.h"
#include <string>

namespace NLS
{
	namespace Editor::Core
	{
		/**
		* Entry point of Editor
		*/
		class Application
		{
		public:
			/**
			* Constructor
			* @param p_projectPath
			* @param p_projectName
			*/
			Application(const std::string& p_projectPath, const std::string& p_projectName);

			/**
			* Destructor
			*/
			~Application();

			/**
			* Run the app
			*/
		void Run();

		void TickFrame(float p_deltaTime, bool p_pollEvents);

			/**
			* Returns true if the app is running
			*/
		bool IsRunning() const;

	private:
		bool m_isTicking = false;
		Context m_context;
		Editor m_editor;
	};
}

}
