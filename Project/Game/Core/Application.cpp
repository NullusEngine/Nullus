#include <Time/Clock.h>

#include "Core/Application.h"
using namespace NLS;
Game::Core::Application::Application(
	const Render::Settings::RenderDocSettings& renderDocSettings,
	std::optional<Render::Settings::EGraphicsBackend> backendOverride,
	std::optional<std::string> projectPathOverride) :
	m_context(renderDocSettings, backendOverride, projectPathOverride),
	m_game(m_context)
{

}

Game::Core::Application::~Application()
{
}

void Game::Core::Application::Run()
{
	Time::Clock clock;

	while (IsRunning())
	{
		m_game.PreUpdate();
		m_game.Update(clock.GetDeltaTime());
		m_game.PostUpdate();

		clock.Update();
	}
}

bool Game::Core::Application::IsRunning() const
{
	return !m_context.window->ShouldClose();
}
