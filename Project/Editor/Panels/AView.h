#pragma once


#include <UI/Panels/PanelWindow.h>
#include <UI/Widgets/Visual/Image.h>
#include <Rendering/Buffers/UniformBuffer.h>
#include <Rendering/Buffers/Framebuffer.h>
#include <Rendering/Entities/Camera.h>
#include <Rendering/Core/CompositeRenderer.h>
#include <Rendering/SceneRenderer.h>

namespace NLS::Editor::Panels
{
	/**
	* Base class for any view
	*/
	class AView : public UI::Panels::PanelWindow
	{
	public:
		/**
		* Constructor
		* @param p_title
		* @param p_opened
		* @param p_windowSettings
		*/
		AView(
			const std::string& p_title,
			bool p_opened,
			const UI::Settings::PanelWindowSettings& p_windowSettings
		);

		/**
		* Update the view
		* @param p_deltaTime
		*/
		virtual void Update(float p_deltaTime);

		/**
		* Custom implementation of the draw method
		*/
		void _Draw_Impl() override;

		/**
		* Prepare the renderer for rendering
		*/
		virtual void InitFrame();

		/**
		* Render the view
		*/
		void Render();

		/**
		* Draw the frame (m_renderer->Draw() if not overriden)
		* @note You don't need to begin/end frame inside of this method, as this is called after begin, and after end
		*/
		virtual void DrawFrame();

		/**
		* Returns the camera used by this view
		*/
		virtual NLS::Render::Entities::Camera* GetCamera() = 0;

		/**
		* Returns the scene used by this view
		*/
		virtual Engine::SceneSystem::Scene* GetScene() = 0;

		/**
		* Returns the size of the panel ignoring its titlebar height
		*/
		std::pair<uint16_t, uint16_t> GetSafeSize() const;

		/**
		* Returns the renderer used by this view
		*/
        const Engine::Rendering::SceneRenderer& GetRenderer() const;

	protected:
        virtual Engine::Rendering::SceneRenderer::SceneDescriptor CreateSceneDescriptor();

	protected:
		UI::Widgets::Visual::Image* m_image;

		Maths::Vector3 m_gridColor = Maths::Vector3 { 0.176f, 0.176f, 0.176f };

		Render::Buffers::Framebuffer m_fbo;
        std::unique_ptr<Engine::Rendering::SceneRenderer> m_renderer;
	};
}