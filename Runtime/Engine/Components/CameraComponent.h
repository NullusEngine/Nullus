
#pragma once

#include "Components/Component.h"
#include "EngineDef.h"
#include "Engine/LayerMask.h"
#include "Math/Vector3.h"
#include "Reflection/Macros.h"
#include "Rendering/Settings/EProjectionMode.h"
#include "Components/CameraComponent.generated.h"

namespace NLS::Render::Entities
{
	class Camera;
}

namespace NLS::Engine::Components
{
	/**
	* Represents a camera entity. Its position will determine its view matrix
	*/
	CLASS(NLS_ENGINE_API CameraComponent, Scriptable, ComponentMenu("Rendering/Camera"), DisallowMultipleComponent) : public Component
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
        FUNCTION(Scriptable)
        void SetFov(float p_value);

        /**
        * Sets the size of the camera to the given value
        * @param p_value
        */
        FUNCTION(Scriptable)
        void SetSize(float p_value);

		/**
		* Sets the near of the camera to the given value
		* @param p_value
		*/
        FUNCTION(Scriptable)
        void SetNear(float p_value);

		/**
		* Sets the far of the camera to the given value
		* @param p_value
		*/
        FUNCTION(Scriptable)
        void SetFar(float p_value);

		/**
		* Sets the clear color of the camera to the given value
		* @param p_value
		*/
        FUNCTION(Scriptable)
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
        FUNCTION(Scriptable)
        void SetProjectionMode(NLS::Render::Settings::EProjectionMode p_projectionMode);

		/**
		* Defines which object layers this camera should render.
		* @param p_visibleLayers
		*/
		 FUNCTION()
		 void SetVisibleLayers(NLS::Engine::LayerMask p_visibleLayers);

		/**
		* Returns the fov of the camera
		*/
        FUNCTION(Scriptable)
        float GetFov() const;

        /**
        * Returns the size of the camera
        */
        FUNCTION(Scriptable)
        float GetSize() const;

		/**
		* Returns the near of the camera
		*/
        FUNCTION(Scriptable)
        float GetNear() const;

		/**
		* Returns the far of the camera
		*/
        FUNCTION(Scriptable)
        float GetFar() const;

		/**
		* Returns the clear color of the camera
		*/
        FUNCTION(Scriptable)
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
        FUNCTION(Scriptable)
        NLS::Render::Settings::EProjectionMode GetProjectionMode() const;

		/**
		* Returns which object layers this camera renders.
		*/
		 FUNCTION()
		 NLS::Engine::LayerMask GetVisibleLayers() const;

		/**
		* Returns the Rendering camera instance attached to this component
		*/
        FUNCTION()
		Render::Entities::Camera* GetCamera();


		private:
			PROPERTY(DisplayName("Field Of View"), Getter(GetFov), Setter(SetFov))
			float fov = 45.0f;
			PROPERTY(DisplayName("Orthographic Size"), Getter(GetSize), Setter(SetSize))
			float size = 5.0f;
			// near/far are legacy preprocessor macros on Windows, so the backing
			// members keep a distinct name and map the historical keys.
			PROPERTY(DisplayName("Near Clip"), FormerlySerializedAs("near"), Getter(GetNear), Setter(SetNear))
			float nearClip = 0.1f;
			PROPERTY(DisplayName("Far Clip"), FormerlySerializedAs("far"), Getter(GetFar), Setter(SetFar))
			float farClip = 100.0f;
			PROPERTY(DisplayName("Clear Color"), Getter(GetClearColor), Setter(SetClearColor))
			Maths::Vector3 clearColor = {0.1921569f, 0.3019608f, 0.4745098f};
			PROPERTY(DisplayName("Frustum Geometry Culling"), Getter(HasFrustumGeometryCulling), Setter(SetFrustumGeometryCulling))
			bool frustumGeometryCulling = false;
			PROPERTY(DisplayName("Frustum Light Culling"), Getter(HasFrustumLightCulling), Setter(SetFrustumLightCulling))
			bool frustumLightCulling = false;
			PROPERTY(DisplayName("Projection Mode"), Getter(GetProjectionMode), Setter(SetProjectionMode))
			NLS::Render::Settings::EProjectionMode projectionMode = NLS::Render::Settings::EProjectionMode::PERSPECTIVE;
			PROPERTY(DisplayName("Visible Layers"), Getter(GetVisibleLayers), Setter(SetVisibleLayers))
			NLS::Engine::LayerMask visibleLayers = NLS::Engine::LayerMask{0xFFFFFFFFu};
			Render::Entities::Camera* m_camera = nullptr;
	};
}
