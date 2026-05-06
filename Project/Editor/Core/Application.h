#pragma once

#include "Context.h"
#include "Editor.h"
#include <string>
#include <optional>
#include "Math/Vector2.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "Rendering/Settings/DriverSettings.h"

namespace NLS
{
	namespace Editor::Core
	{
		/**
		* Entry point of Editor
		*/
		class Application
		{
		public:
			/**
			* Constructor
			* @param p_projectPath
			* @param p_projectName
			* @param p_backendOverride optional backend override from command line
			* @param p_renderDocSettings RenderDoc settings from command line
			* @param p_enableThreadedRendering enable threaded rendering from command line
			* @param p_diagnosticsSettings Engine diagnostics settings from command line
			*/
			Application(const std::string& p_projectPath, const std::string& p_projectName,
			            std::optional<Render::Settings::EGraphicsBackend> p_backendOverride = std::nullopt,
			            const Render::Settings::RenderDocSettings& p_renderDocSettings = {},
			            bool p_enableThreadedRendering = false,
			            bool p_enableRhiDebugValidation = true,
			            const Render::Settings::EngineDiagnosticsSettings& p_diagnosticsSettings = {});

			/**
			* Destructor
			*/
			~Application();

			/**
			* Run the app
			*/
		void Run();

		void TickFrame(float p_deltaTime, bool p_pollEvents);
        void TickResizeFrame();
		void QueueResizeTick();
		void FlushDeferredResizeTick();

			/**
			* Returns true if the app is running
			*/
		bool IsRunning() const;

	private:
        void RunEditorFrame(float deltaTime);
        void SyncPlatformSwapchainToFramebufferSize();
        bool m_isPollingEvents = false;
        bool m_isResizeTicking = false;
		bool m_isTicking = false;
		bool m_pendingResizeTick = false;
        bool m_hasLastNativeResizeTickSize = false;
        Maths::Vector2 m_lastNativeResizeTickSize;
		Context m_context;
		Editor m_editor;
	};
}

}
