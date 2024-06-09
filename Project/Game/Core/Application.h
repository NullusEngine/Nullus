#pragma once

#include "Core/Context.h"
#include "Core/Game.h"

namespace Game::Core
{
	/**
	* Entry point of Game
	*/
	class Application
	{
	public:
		/**
		* Constructor
		*/
		Application();

		/**
		* Destructor
		*/
		~Application();

		/**
		* Run the app
		*/
		void Run();

		/**
		* Returns true if the app is running
		*/
		bool IsRunning() const;

	private:
		Context m_context;
		Game m_game;
	};
}