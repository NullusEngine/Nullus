#pragma once

#include <Windowing/Context/Device.h>
#include <Windowing/Inputs/InputManager.h>
#include <Windowing/Window.h>
namespace NLS
{
	namespace Editor::Core
	{
		/**
		* The Context handle the engine features setup
		*/
		class Context
		{
		public:
			/**
			* Constructor
			* @param p_projectPath
			* @param p_projectName
			*/
			Context(const std::string& p_projectPath, const std::string& p_projectName);

			/**
			* Destructor
			*/
			~Context();

			/**
			* Reset project settings ini file
			*/
			void ResetProjectSettings();

			/**
			* Verify that project settings are complete (No missing key).
			* Returns true if the integrity is verified
			*/
			bool IsProjectSettingsIntegrityVerified();

			/**
			* Apply project settings to the ini file
			*/
			void ApplyProjectSettings();

		public:
			const std::string projectPath;
			const std::string projectName;
			const std::string projectFilePath;
			const std::string engineAssetsPath;
			const std::string projectAssetsPath;
			const std::string projectScriptsPath;
			const std::string editorAssetsPath;

			std::unique_ptr<NLS::Context::Device> device;
			std::unique_ptr<NLS::Windowing::Window> window;
			std::unique_ptr<NLS::Windowing::Inputs::InputManager> inputManager;
			NLS::Windowing::Settings::WindowSettings windowSettings;
		};
	}
}

