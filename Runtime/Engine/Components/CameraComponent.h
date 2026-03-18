
#pragma once

#include "Rendering/Entities/Camera.h"
#include "Components/Component.h"
#include "EngineDef.h"
#include "Reflection/Macros.h"
#include "Components/CameraComponent.generated.h"
namespace NLS::Engine::Components
{
	/**
	* Represents a camera entity. Its position will determine its view matrix
	*/
	CLASS() class NLS_ENGINE_API CameraComponent : public Component
	{
    public:
		GENERATED_BODY()
		/**
		* Constructor
		*/
		CameraComponent();

		/**
		* Destructor
		*/
		~CameraComponent() = default;

		void OnCreate() override;
		/**
		* Sets the fov of the camera to the given value
		* @param p_value
		*/
        FUNCTION()
		void SetFov(float p_value);

        /**
        * Sets the size of the camera to the given value
        * @param p_value
        */
        FUNCTION()
        void SetSize(float p_value);

		/**
		* Sets the near of the camera to the given value
		* @param p_value
		*/
        FUNCTION()
		void SetNear(float p_value);

		/**
		* Sets the far of the camera to the given value
		* @param p_value
		*/
        FUNCTION()
		void SetFar(float p_value);

		/**
		* Sets the clear color of the camera to the given value
		* @param p_value
		*/
        FUNCTION()
		void SetClearColor(const Maths::Vector3& p_clearColor);

		/**
		* Defines if the camera should apply frustum geometry culling in rendering
		* @param p_enable
		*/
        FUNCTION()
		void SetFrustumGeometryCulling(bool p_enable);

		/**
		* Defines if the camera should apply frustum light culling in rendering
		* @param p_enable
		*/
        FUNCTION()
		void SetFrustumLightCulling(bool p_enable);

        /**
        * Defines the projection mode the camera should adopt
        * @param p_projectionMode
        */
        FUNCTION()
        void SetProjectionMode(NLS::Render::Settings::EProjectionMode p_projectionMode);

		/**
		* Returns the fov of the camera
		*/
        FUNCTION()
		float GetFov() const;

        /**
        * Returns the size of the camera
        */
        FUNCTION()
        float GetSize() const;

		/**
		* Returns the near of the camera
		*/
        FUNCTION()
		float GetNear() const;

		/**
		* Returns the far of the camera
		*/
        FUNCTION()
		float GetFar() const;

		/**
		* Returns the clear color of the camera
		*/
        FUNCTION()
		const Maths::Vector3& GetClearColor() const;

		/**
		* Returns true if the frustum geometry culling is enabled
		*/
        FUNCTION()
		bool HasFrustumGeometryCulling() const;

		/**
		* Returns true if the frustum light culling is enabled
		*/
        FUNCTION()
		bool HasFrustumLightCulling() const;

        /**
        * Returns the current projection mode
        */
        FUNCTION()
        NLS::Render::Settings::EProjectionMode GetProjectionMode() const;

		/**
		* Returns the Rendering camera instance attached to this component
		*/
        FUNCTION()
		Render::Entities::Camera* GetCamera();


	private:
		Render::Entities::Camera* m_camera;
	};
}
