#pragma once

#include <Rendering/Buffers/UniformBuffer.h>
#include <Rendering/Buffers/ShaderStorageBuffer.h>

#include <Windowing/Context/Device.h>
#include <Windowing/Inputs/InputManager.h>
#include <Windowing/Window.h>
#include <Rendering/Context/Driver.h>


#include <Core/ResourceManagement/MeshManager.h>
#include <Core/ResourceManagement/TextureManager.h>
#include <Core/ResourceManagement/ShaderManager.h>
#include <Core/ResourceManagement/MaterialManager.h>
#include <Core/ResourceManagement/ResourceLifetimeRegistry.h>
#include <Engine/Assets/RuntimeAssetDatabase.h>
#include <SceneSystem/SceneManager.h>
#include "Core/Filesystem/IniFile.h"
#include "Rendering/Settings/DriverSettings.h"
#include <optional>
#include <filesystem>
#include <Eventing/Event.h>
#include <Scripting/ScriptRuntime.h>
#include <Engine/Audio/AudioDevice.h>

namespace NLS::Game
{
	/**
	* The Context handle the engine features setup
	*/
	class Context
	{
        struct JobSystemLifetime
        {
            ~JobSystemLifetime();
            void Shutdown();
            bool ownsJobSystem = false;
        };
        // Keep the pool alive through all producers and unwind failed startup.
        JobSystemLifetime m_jobSystemLifetime;

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

			/**
			 * Starts the shared compute and background pools before Driver.
			 *
			 * The pool must outlive any background artifact work and must be
			 * drained before the rendering threads unregister, so it is started
			 * before Driver creates its TaskGraph. Before the RHI mode is known,
			 * the budget conservatively reserves a dedicated RHI lane. Uses
			 * TryInitializeJobSystem so an embedding host that already owns a
			 * pool keeps it.
			 */
			void StartJobSystemFromRenderingBudget();
			void ShutdownJobSystemIfOwned();

	public:
		const std::string engineAssetsPath;
		const std::string projectAssetsPath;
		const std::string projectScriptsPath;

		std::unique_ptr<NLS::Context::Device> device;
		std::unique_ptr<Windowing::Window> window;
		std::unique_ptr<Windowing::Inputs::InputManager> inputManager;
		std::unique_ptr<NLS::Render::Context::Driver> driver;

			// Render snapshots retain resource leases until their submitted work retires.
			NLS::Core::ResourceManagement::ResourceLifetimeRegistry resourceLifetimeRegistry;

			// Context-owned scripting state.  It is declared before sceneManager so
		// Scene/ScriptComponent destruction runs while the runtime is alive.
            std::shared_ptr<NLS::Engine::Audio::AudioDevice> audioDevice = NLS::Engine::Audio::AudioDevice::AcquireHost();
			NLS::Scripting::ScriptRuntime scriptRuntime;
		Engine::SceneSystem::SceneManager sceneManager;

		NLS::Core::ResourceManagement::MeshManager meshManager;
		NLS::Core::ResourceManagement::TextureManager textureManager;
		NLS::Core::ResourceManagement::ShaderManager shaderManager;
		NLS::Core::ResourceManagement::MaterialManager materialManager;
		std::optional<NLS::Engine::Assets::RuntimeAssetDatabase> runtimeAssetDatabase;
			std::filesystem::path runtimeContentRoot;
			bool packagedContent = false;

		NLS::Filesystem::IniFile projectSettings;

	private:
		std::optional<Render::Settings::RenderDocSettings> m_renderDocOverride;
		std::optional<Render::Settings::EGraphicsBackend> m_backendOverride;
		std::optional<std::string> m_projectPathOverride;
		NLS::ListenerID m_sceneLoadListener = NLS::InvalidListenerID;
		NLS::ListenerID m_sceneComponentAddedListener = NLS::InvalidListenerID;
	};
}
