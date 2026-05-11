#include <Time/Clock.h>

#include "Core/Application.h"
using namespace NLS;
Game::Core::Application::Application(
	std::optional<Render::Settings::RenderDocSettings> renderDocOverride,
	std::optional<Render::Settings::EGraphicsBackend> backendOverride,
	std::optional<std::string> projectPathOverride) :
	m_context(renderDocOverride, backendOverride, projectPathOverride),
	m_game(m_context)
{

}

Game::Core::Application::~Application()
{
	m_context.ShutdownThreadedRendering();
}

void Game::Core::Application::Run()
{
	NLS_LOG_INFO("Application::Run: starting game loop");
	Time::Clock clock;

	while (IsRunning())
	{
		NLS_LOG_INFO("Application::Run: frame start");
		m_game.PreUpdate();
		m_game.Update(clock.GetDeltaTime());
		m_game.PostUpdate();

		clock.Update();
	}
	NLS_LOG_INFO("Application::Run: game loop ended");
}

bool Game::Core::Application::IsRunning() const
{
	return !m_context.window->ShouldClose();
}
