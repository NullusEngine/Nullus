#pragma once

#include <Rendering/Buffers/UniformBuffer.h>
#include <Rendering/Buffers/ShaderStorageBuffer.h>

#include <Windowing/Context/Device.h>
#include <Windowing/Inputs/InputManager.h>
#include <Windowing/Window.h>


#include <Core/ResourceManagement/ModelManager.h>
#include <Core/ResourceManagement/TextureManager.h>
#include <Core/ResourceManagement/ShaderManager.h>
#include <Core/ResourceManagement/MaterialManager.h>
#include <SceneSystem/SceneManager.h>
#include "Core/Filesystem/IniFile.h"

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
		*/
		Context();

		/**
		* Destructor
		*/
		~Context();

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
		
		NLS::Filesystem::IniFile projectSettings;
	};
}