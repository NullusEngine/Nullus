#include <Time/Clock.h>

#include "Core/Application.h"
Game::Core::Application::Application() :
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
