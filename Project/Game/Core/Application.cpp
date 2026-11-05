#include <Time/Clock.h>

#include "Core/Application.h"
#include "Core/Modules/ModuleServices.h"
using namespace NLS;
Game::Core::Application::Application(
	std::optional<Render::Settings::RenderDocSettings> renderDocOverride,
	std::optional<Render::Settings::EGraphicsBackend> backendOverride,
	std::optional<std::string> projectPathOverride,
	std::optional<Launch::MaterialValidationLaunchSettings> materialValidation,
    Scripting::RuntimePlaySession::ECSSetup registerECS) :
        m_context(renderDocOverride, backendOverride, projectPathOverride)
{
    NLS::Core::Modules::AdvanceHostPhase(NLS::Core::Modules::LoadingPhase::PostEngine);
    m_game = std::make_unique<Game>(m_context, std::move(materialValidation), std::move(registerECS));
}

Game::Core::Application::~Application()
{
    m_game.reset();
    NLS::Core::Modules::CloseHostModules();
    // Context drains accepted jobs before it unregisters rendering lanes.
}

void Game::Core::Application::Run()
{
	NLS_LOG_INFO("Application::Run: starting game loop");
	Time::Clock clock;

	while (IsRunning())
	{
		NLS_LOG_INFO("Application::Run: frame start");
            m_game->PreUpdate();
            m_game->Update(clock.GetDeltaTime());
            m_game->PostUpdate();

		clock.Update();
	}
	NLS_LOG_INFO("Application::Run: game loop ended");
}

bool Game::Core::Application::IsRunning() const
{
	return !m_context.window->ShouldClose();
}
