#pragma once

#include <SceneSystem/SceneManager.h>

#include "Panels/AView.h"
#include "Core/GameViewInputRouter.h"
#include <Engine/UI/UIRuntimeService.h>

namespace NLS::Editor::Panels
{
	class GameView : public Editor::Panels::AView
	{
	public:
        using AView::Render;
        void BeginPlayPreview();
        void EndPlayPreview();
        NLS::Scripting::GameInputFrame CaptureGameInput();
        bool OwnsGameInput() const { return m_input.IsFocused(); }
        void ReleaseGameInput();
        const Core::GameViewPresentation& GetPresentation() const { return m_presentation; }
		/**
		* Constructor
		* @param p_title
		* @param p_opened
		* @param p_windowSettings
		*/
		GameView(
			const std::string& p_title,
			bool p_opened,
			const UI::PanelWindowSettings& p_windowSettings
		);

		/**
		* Returns the main camera used by the attached scene
		*/
		virtual Render::Entities::Camera* GetCamera();
        void EnsureRenderer() override;

		/**
		* Returns the scene used by this view
		*/
		virtual Engine::SceneSystem::Scene* GetScene();

	protected:
        void _Draw_Impl() override;
        void OnBeforeDrawWidgets() override;
        void OnAfterDrawWidgets() override;
        std::pair<uint16_t, uint16_t> ResolveRenderSize(Maths::Vector2 available) const override;
        void DrawControls();
        void DrawGizmos();
        void Render(uint16_t width,uint16_t height) override;
        void ConfigureFrameDescriptor(Render::Data::FrameDescriptor& descriptor) override;
		Engine::Rendering::BaseSceneRenderer::SceneDescriptor CreateSceneDescriptor() override;
		Engine::SceneSystem::SceneManager& m_sceneManager;
        bool m_wasOpenedBeforePlay = true;
        bool m_canvasStarted = false;
        bool m_maximized = false;
        Core::GameViewPresentation m_presentation;
        Core::GameViewInputRouter m_input;
        Maths::Vector2 m_pan;
        bool m_renderingOutput=false, m_backgroundPass=false, m_lastCameraPass=false;
        bool m_includeSkybox=true, m_outputNeedsClear=true;
        Render::Entities::Camera m_outputCamera;
        Maths::Vector4 m_outputViewport;
        std::vector<std::unique_ptr<Engine::Rendering::BaseSceneRenderer>> m_cameraRenderers;
        Engine::UI::UIRuntimeService m_previewUI;
        std::shared_ptr<const Render::UI::UiDrawDataSnapshot> m_previewSnapshot;
        std::optional<Render::Data::FrameInfo> m_outputStats;
        double m_lastRenderMilliseconds=0;
        uint64_t m_outputSession=0,m_outputFrame=0;
        uint64_t m_outputSceneEpoch=0;
        uint32_t m_outputDisplay=0,m_outputWidth=0,m_outputHeight=0;
        uint32_t m_renderOutputWidth=0,m_renderOutputHeight=0;
	};
}
