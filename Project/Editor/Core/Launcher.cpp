#include "Launcher.h"
#define _CRT_SECURE_NO_WARNINGS

#include <filesystem>
#include <fstream>

#include <UI/Widgets/Texts/Text.h>
#include <UI/Widgets/Visual/Separator.h>
#include <UI/Widgets/Layout/Columns.h>
#include <UI/Widgets/Layout/Spacing.h>
#include <UI/Widgets/Layout/Group.h>
#include <UI/Widgets/Buttons/Button.h>
#include <UI/Widgets/InputFields/InputText.h>

#include <Utils/PathParser.h>

#include <Windowing/Dialogs/SaveFileDialog.h>
#include <Windowing/Dialogs/OpenFileDialog.h>
#include <Windowing/Dialogs/MessageBox.h>

#define PROJECTS_FILE std::filesystem::canonical(std::filesystem::path("../Settings")).string() + "/projects.ini"
namespace NLS
{
	class LauncherPanel : public UI::Panels::PanelWindow
	{
	public:
		LauncherPanel(bool& p_readyToGo, std::string& p_path, std::string& p_projectName) :
			PanelWindow("Nullus - Launcher", true),
			m_readyToGo(p_readyToGo),
			m_path(p_path),
			m_projectName(p_projectName)
		{
			resizable = false;
			movable = false;
			titleBar = false;

			std::filesystem::create_directories(std::filesystem::path("../Settings"));

			SetSize({ 1000, 580 });
			SetPosition({ 0.f, 0.f });

			auto& openProjectButton = CreateWidget<UI::Widgets::Buttons::Button>("Open Project");
			auto& newProjectButton = CreateWidget<UI::Widgets::Buttons::Button>("New Project");

			openProjectButton.idleBackgroundColor = { 0.7f, 0.5f, 0.f };
			newProjectButton.idleBackgroundColor = { 0.f, 0.5f, 0.0f };

			openProjectButton.ClickedEvent += [this]
				{
					Dialogs::OpenFileDialog dialog("Open project");
					dialog.AddFileType("Overload Project", "*.nullus");
					dialog.Show();

					std::string projectPath = dialog.GetSelectedFilePath();
					std::string rootFolderPath = std::filesystem::path(projectPath).parent_path().string();

					if (dialog.HasSucceeded())
					{
						RegisterProject(rootFolderPath);
						OpenProject(rootFolderPath);
					}
				};

			newProjectButton.ClickedEvent += [this]
				{
					Dialogs::SaveFileDialog dialog("New project location");
					dialog.DefineExtension("Nullus Project", "..");
					dialog.Show();
					if (dialog.HasSucceeded())
					{
						std::string result = dialog.GetSelectedFilePath();
						std::string path = std::string(result.data(), result.data() + result.size() - std::string("..").size());
						CreateProject(path);
						RegisterProject(path);
					}
				};

			openProjectButton.lineBreak = false;
			newProjectButton.lineBreak = false;

			for (uint8_t i = 0; i < 4; ++i)
				CreateWidget<UI::Widgets::Layout::Spacing>();

			CreateWidget<UI::Widgets::Visual::Separator>();

			for (uint8_t i = 0; i < 4; ++i)
				CreateWidget<UI::Widgets::Layout::Spacing>();

			columns = &CreateWidget<UI::Widgets::Layout::Columns>(2);

			columns->widths = { 750, 500 };

			std::string line;
			std::ifstream myfile(PROJECTS_FILE);
			if (myfile.is_open())
			{
				while (getline(myfile, line))
				{
					if (std::filesystem::exists(line)) // TODO: Delete line from the file
					{
						auto& text = columns->CreateWidget<UI::Widgets::Texts::Text>(line);
						auto& actions = columns->CreateWidget<UI::Widgets::Layout::Group>();
						auto& openButton = actions.CreateWidget<UI::Widgets::Buttons::Button>("Open");
						auto& deleteButton = actions.CreateWidget<UI::Widgets::Buttons::Button>("Delete");

						openButton.idleBackgroundColor = { 0.7f, 0.5f, 0.f };
						deleteButton.idleBackgroundColor = { 0.5f, 0.f, 0.f };

						openButton.ClickedEvent += [this, line]
							{
								OpenProject(line);
							};

						std::string toErase = line;
						deleteButton.ClickedEvent += [this, &text, &actions, toErase]
							{
								text.Destroy();
								actions.Destroy();

								std::string line;
								std::ifstream fin(PROJECTS_FILE);
								std::ofstream temp("temp");

								while (getline(fin, line))
									if (line != toErase)
										temp << line << std::endl;

								temp.close();
								fin.close();

								std::filesystem::remove(PROJECTS_FILE);
								std::filesystem::rename("temp", PROJECTS_FILE);
							};

						openButton.lineBreak = false;
						deleteButton.lineBreak;
					}
				}
				myfile.close();
			}
		}

		void CreateProject(const std::string& p_path)
		{
			if (!std::filesystem::exists(p_path))
			{
				std::filesystem::create_directory(p_path);
				std::filesystem::create_directory(p_path + "/Assets");
				std::filesystem::create_directory(p_path + "/Script");
				std::ofstream projectFile(p_path + '/' + Utils::PathParser::GetElementName(p_path) + ".nullus");
			}
		}

		void RegisterProject(const std::string& p_path)
		{
			bool pathAlreadyRegistered = false;

			{
				std::string line;
				std::ifstream myfile(PROJECTS_FILE);
				if (myfile.is_open())
				{
					while (getline(myfile, line))
					{
						if (line == p_path)
						{
							pathAlreadyRegistered = true;
							break;
						}
					}
					myfile.close();
				}
			}

			if (!pathAlreadyRegistered)
			{
				std::ofstream projectsFile(PROJECTS_FILE, std::ios::app);
				projectsFile << p_path << std::endl;
				if (std::filesystem::exists(p_path)) // TODO: Delete line from the file
				{
					auto& text = columns->CreateWidget<UI::Widgets::Texts::Text>(p_path);
					auto& actions = columns->CreateWidget<UI::Widgets::Layout::Group>();
					auto& openButton = actions.CreateWidget<UI::Widgets::Buttons::Button>("Open");
					auto& deleteButton = actions.CreateWidget<UI::Widgets::Buttons::Button>("Delete");

					openButton.idleBackgroundColor = { 0.7f, 0.5f, 0.f };
					deleteButton.idleBackgroundColor = { 0.5f, 0.f, 0.f };

					openButton.ClickedEvent += [this, p_path]
						{
							OpenProject(p_path);
						};

					std::string toErase = p_path;
					deleteButton.ClickedEvent += [this, &text, &actions, toErase]
						{
							text.Destroy();
							actions.Destroy();

							std::string line;
							std::ifstream fin(PROJECTS_FILE);
							std::ofstream temp("temp");

							while (getline(fin, line))
								if (line != toErase)
									temp << line << std::endl;

							temp.close();
							fin.close();

							std::filesystem::remove(PROJECTS_FILE);
							std::filesystem::rename("temp", PROJECTS_FILE);
						};

					openButton.lineBreak = false;
					deleteButton.lineBreak;
				}
			}
		}

		void OpenProject(const std::string& p_path)
		{
			m_readyToGo = std::filesystem::exists(p_path);
			if (!m_readyToGo)
			{
				using namespace Dialogs;
				MessageBox errorMessage("Project not found", "The selected project does not exists", MessageBox::EMessageType::ERROR, MessageBox::EButtonLayout::OK);
			}
			else
			{
				m_path = p_path;
				m_projectName = Utils::PathParser::GetElementName(m_path);
				Close();
			}
		}

		void Draw() override
		{
			UI::Panels::PanelWindow::Draw();
		}

	private:
		bool& m_readyToGo;
		std::string& m_path;
		std::string& m_projectName;
		UI::Widgets::Layout::Columns* columns = nullptr;
	};


	Launcher::Launcher()
	{
		SetupContext();
		m_mainPanel = std::make_unique<LauncherPanel>(m_readyToGo, m_projectPath, m_projectName);

		m_uiManager->SetCanvas(m_canvas);
		m_canvas.AddPanel(*m_mainPanel);
	}
	std::tuple<bool, std::string, std::string> Launcher::Run()
	{
		while (!m_window->ShouldClose())
		{
			m_device->PollEvents();
			m_uiManager->Render();
			m_window->SwapBuffers();

			if (!m_mainPanel->IsOpened())
				m_window->SetShouldClose(true);
		}

		return { m_readyToGo, m_projectPath, m_projectName };
	}
	void Launcher::SetupContext()
	{
		/* Settings */
		Windowing::Settings::DeviceSettings deviceSettings;
		Windowing::Settings::WindowSettings windowSettings;
		windowSettings.title = "Nullus - Launcher";
		windowSettings.width = 1000;
		windowSettings.height = 580;
		windowSettings.maximized = false;
		windowSettings.resizable = false;
		windowSettings.decorated = true;

		/* Window creation */
		m_device = std::make_unique<Context::Device>(deviceSettings);
		m_window = std::make_unique<Windowing::Window>(*m_device, windowSettings);
		m_window->MakeCurrentContext();

		auto monSize = m_device->GetMonitorSize();
		auto winSize = m_window->GetSize();
		m_window->SetPosition(monSize.x / 2 - winSize.x / 2, monSize.y / 2 - winSize.y / 2);

		/* Graphics context creation */
		m_driver = std::make_unique<Rendering::Context::Driver>(Rendering::Settings::DriverSettings{ false });

		m_uiManager = std::make_unique<UI::UIManager>(m_window->GetGlfwWindow(), UI::Styling::EStyle::ALTERNATIVE_DARK);
		m_uiManager->LoadFont("Ruda_Big", "../Assets/Fonts/Ruda-Bold.ttf", 18);
		m_uiManager->UseFont("Ruda_Big");
		m_uiManager->EnableEditorLayoutSave(false);
		m_uiManager->EnableDocking(false);
	}
	void Launcher::RegisterProject(const std::string& p_path)
	{
		static_cast<LauncherPanel*>(m_mainPanel.get())->RegisterProject(p_path);
	}
}
