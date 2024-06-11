#include "Windowing/Window.h"
#include "Core/Application.h"
#include "Core/Utils/PathParser.h"
#include <filesystem>
int main(int argc, char** argv)
{
    std::filesystem::current_path(NLS::Utils::PathParser::GetContainingFolder(argv[0]));
	Game::Core::Application app;
	app.Run();
	return EXIT_SUCCESS;

}