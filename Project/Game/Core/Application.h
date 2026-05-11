#pragma once

#include "Core/Context.h"
#include "Core/Game.h"
#include "Rendering/Settings/DriverSettings.h"
#include <cstdint>
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
		* @param renderDocOverride optional one-shot RenderDoc override from command line
		* @param backendOverride optional backend override from command line
		* @param projectPathOverride optional project path override from command line
		*/
		Application(
			std::optional<Render::Settings::RenderDocSettings> renderDocOverride = std::nullopt,
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
