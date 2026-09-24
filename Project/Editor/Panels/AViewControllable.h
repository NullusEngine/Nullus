#pragma once

#include "Panels/AView.h"
#include "Core/CameraController.h"

namespace NLS::Editor::Panels
{
	class AViewControllable : public Editor::Panels::AView
	{
	public:
		/**
		* Constructor
		* @param p_title
		* @param p_opened
		* @param p_windowSettings
		*/
		AViewControllable(
			const std::string& p_title,
			bool p_opened,
			const UI::PanelWindowSettings& p_windowSettings
		);

		/**
		* Update the controllable view (Handle inputs)
		* @param p_deltaTime
		*/
		virtual void Update(float p_deltaTime) override;

		/**
		* Prepare the renderer for rendering
		*/
		virtual void InitFrame() override;

		/**
		* Reset the camera transform to its initial value
		*/
		virtual void ResetCameraTransform();

		/**
		* Returns the camera controller of the controllable view
		*/
		Editor::Core::CameraController& GetCameraController();

		/**
		* Returns the camera used by the camera controller
		*/
		virtual NLS::Render::Entities::Camera* GetCamera();

		/**
		* Reset the grid color to its initial value
		*/
		void ResetGridColor();

		/**
		* Set the camera clear color
		*/
		void ResetClearColor();

	protected:
		/**
		* Applies the reflected editor view settings that belong to this panel:
		* camera speed, grid color and background color. Implemented by each
		* view so the Scene View and Asset View keep independent settings.
		*/
		virtual void ApplyViewSettings() = 0;

        Maths::Transform camTrans;
		Maths::Vector3 m_gridColor;
		NLS::Render::Entities::Camera m_camera;
		Editor::Core::CameraController m_cameraController;
	};
}