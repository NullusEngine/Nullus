#pragma once

#include "Core/Context.h"
#include "Core/Game.h"
#include "Rendering/Settings/DriverSettings.h"

#include <optional>
#include <string>

namespace NLS::Game::Core
{
	/**
	* Entry point of Game
	*/
	class Application
	{
	public:
		/**
		* Constructor
		* @param renderDocSettings RenderDoc settings from command line
		*/
		Application(
			const Render::Settings::RenderDocSettings& renderDocSettings = {},
			std::optional<Render::Settings::EGraphicsBackend> backendOverride = std::nullopt,
			std::optional<std::string> projectPathOverride = std::nullopt);

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
