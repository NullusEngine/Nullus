#pragma once

#include "Context.h"
#include "Editor.h"
#include <memory>
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
			* @param p_renderDocOverride optional one-shot RenderDoc override from command line
			*/
			Application(const std::string& p_projectPath, const std::string& p_projectName,
			            std::optional<Render::Settings::EGraphicsBackend> p_backendOverride = std::nullopt,
			            std::optional<Render::Settings::RenderDocSettings> p_renderDocOverride = std::nullopt,
			            std::optional<Render::Settings::EngineDiagnosticsSettings> p_diagnosticsOverride = std::nullopt);

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
		std::unique_ptr<Editor> m_editor;
	};
}

}
