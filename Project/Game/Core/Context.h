#pragma once

#include <Rendering/Buffers/UniformBuffer.h>
#include <Rendering/Buffers/ShaderStorageBuffer.h>

#include <Windowing/Context/Device.h>
#include <Windowing/Inputs/InputManager.h>
#include <Windowing/Window.h>
#include <Rendering/Context/Driver.h>


#include <Core/ResourceManagement/ModelManager.h>
#include <Core/ResourceManagement/TextureManager.h>
#include <Core/ResourceManagement/ShaderManager.h>
#include <Core/ResourceManagement/MaterialManager.h>
#include <SceneSystem/SceneManager.h>
#include "Core/Filesystem/IniFile.h"
#include "Rendering/Settings/DriverSettings.h"
#include <optional>

namespace NLS::Game
{
	/**
	* The Context handle the engine features setup
	*/
	class Context
	{
	public:
		/**
		* Constructor
		* @param renderDocOverride optional one-shot RenderDoc override from command line
		* @param backendOverride optional backend override from command line
		* @param projectPathOverride optional project path override from command line
		*/
		Context(
			std::optional<Render::Settings::RenderDocSettings> renderDocOverride = std::nullopt,
			std::optional<Render::Settings::EGraphicsBackend> backendOverride = std::nullopt,
			std::optional<std::string> projectPathOverride = std::nullopt);

		/**
		* Destructor
		*/
		~Context();

		void ShutdownThreadedRendering();

	public:
		const std::string engineAssetsPath;
		const std::string projectAssetsPath;
		const std::string projectScriptsPath;

		std::unique_ptr<NLS::Context::Device> device;
		std::unique_ptr<Windowing::Window> window;
		std::unique_ptr<Windowing::Inputs::InputManager> inputManager;
		std::unique_ptr<NLS::Render::Context::Driver> driver;

		Engine::SceneSystem::SceneManager sceneManager;

		NLS::Core::ResourceManagement::ModelManager modelManager;
		NLS::Core::ResourceManagement::TextureManager textureManager;
		NLS::Core::ResourceManagement::ShaderManager shaderManager;
		NLS::Core::ResourceManagement::MaterialManager materialManager;

		NLS::Filesystem::IniFile projectSettings;

	private:
		std::optional<Render::Settings::RenderDocSettings> m_renderDocOverride;
		std::optional<Render::Settings::EGraphicsBackend> m_backendOverride;
		std::optional<std::string> m_projectPathOverride;
	};
}
