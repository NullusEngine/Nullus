
#pragma once

#include <Math/Vector3.h>
#include <Math/Matrix4.h>
#include <Math/Quaternion.h>

#include "Rendering/Data/Frustum.h"
#include "Rendering/Settings/EProjectionMode.h"
#include "Rendering/Entities/Entity.h"

namespace NLS::Rendering::Entities
{
	/**
	* Represents a camera. Wraps projection and view calculation based on applied rotation and the given positions
	*/
	class Camera : public Entity
	{
	public:
		Camera();
		/**
		* Constructor
		* @param p_transform
		*/
		Camera(Maths::Transform p_transform);

		/**
		* Cache the projection, view and frustum matrices
		* @param p_windowWidth
		* @param p_windowHeight
		*/
		void CacheMatrices(uint16_t p_windowWidth, uint16_t p_windowHeight);

		/**
		* Calculate and cache the result projection matrix
		* @param p_windowWidth
		* @param p_windowHeight
		*/
		void CacheProjectionMatrix(uint16_t p_windowWidth, uint16_t p_windowHeight);

		/**
		* Calculate and cache the result view matrix
		*/
		void CacheViewMatrix();

		/**
		* Calculate and cache the result frustum.
		* This method should be called after projection and view matrices are cached.
		* @param p_view
		* @param p_projection
		*/
		void CacheFrustum(const Maths::Matrix4& p_view, const Maths::Matrix4& p_projection);

		/**
		* Returns the camera position
		*/
		const Maths::Vector3& GetPosition() const;

		/**
		* Returns the camera rotation
		*/
		const Maths::Quaternion& GetRotation() const;

		/**
		* Returns the fov of the camera
		*/
		float GetFov() const;

		/**
		* Returns the size of the camera
		*/
		float GetSize() const;

		/**
		* Returns the near of the camera
		*/
		float GetNear() const;

		/**
		* Returns the far of the camera
		*/
		float GetFar() const;

		/**
		* Returns the clear color of the camera
		*/
		const Maths::Vector3& GetClearColor() const;

		/**
		* Returns true if the camera is setup to clear the color buffer
		*/
		bool GetClearColorBuffer() const;

		/**
		* Returns true if the camera is setup to clear the depth buffer
		*/
		bool GetClearDepthBuffer() const;

		/**
		* Returns true if the camera is setup to clear the stencil buffer 
		*/
		bool GetClearStencilBuffer() const;

		/**
		* Returns the cached projection matrix
		*/
		const Maths::Matrix4& GetProjectionMatrix() const;

		/**
		* Returns the cached view matrix
		*/
		const Maths::Matrix4& GetViewMatrix() const;

		/**
		* Returns the cached frustum
		*/
		const NLS::Rendering::Data::Frustum& GetFrustum() const;

		/**
		* Returns the frustum of the camera used for lights culling
		*/
		const NLS::Rendering::Data::Frustum* GetGeometryFrustum() const;

		/**
		* Returns the frustum of the camera used for lights culling
		*/
		const NLS::Rendering::Data::Frustum* GetLightFrustum() const;

		/**
		* Returns true if the frustum culling for geometry is enabled
		*/
		bool HasFrustumGeometryCulling() const;

		/**
		* Returns true if the frustum culling for lights is enabled
		*/
		bool HasFrustumLightCulling() const;

		/**
		* Returns the current projection mode
		*/
		NLS::Rendering::Settings::EProjectionMode GetProjectionMode() const;

		/**
		* Set the camera position
		* @param p_position
		*/
		void SetPosition(const Maths::Vector3& p_position);

		/**
		* Set the camera rotation
		* @param p_rotation
		*/
		void SetRotation(const Maths::Quaternion& p_rotation);

		/**
		* Sets the fov of the camera to the given value
		* @param p_value
		*/
		void SetFov(float p_value);

		/**
		* Sets the size of the camera to the given value
		* @param p_value
		*/
		void SetSize(float p_value);

		/**
		* Sets the near of the camera to the given value
		* @param p_value
		*/
		void SetNear(float p_value);

		/**
		* Sets the far of the camera to the given value
		* @param p_value
		*/
		void SetFar(float p_value);

		/**
		* Sets the clear color of the camera to the given value
		* @param p_value
		*/
		void SetClearColor(const Maths::Vector3& p_clearColor);

		/**
		* Sets if the camera should request the color buffer to be cleared
		* @param p_value
		*/
		void SetClearColorBuffer(bool p_value);

		/**
		* Sets if the camera should request the depth buffer to be cleared
		* @param p_value
		*/
		void SetClearDepthBuffer(bool p_value);

		/**
		* Sets if the camera should request the stencil buffer to be cleared
		* @param p_value
		*/
		void SetClearStencilBuffer(bool p_value);

		/**
		* Defines if the camera should apply frustum culling to geometry while rendering
		* @param p_enable
		*/
		void SetFrustumGeometryCulling(bool p_enable);

		/**
		* Defines if the camera should apply frustum culling to lights while rendering
		* @param p_enable
		*/
		void SetFrustumLightCulling(bool p_enable);

		/**
		* Defines the projection mode the camera should adopt
		* @param p_projectionMode
		*/
		void SetProjectionMode(NLS::Rendering::Settings::EProjectionMode p_projectionMode);

	private:
		Maths::Matrix4 CalculateProjectionMatrix(uint16_t p_windowWidth, uint16_t p_windowHeight) const;
		Maths::Matrix4 CalculateViewMatrix() const;

	private:
		NLS::Rendering::Data::Frustum m_frustum;
		Maths::Matrix4 m_viewMatrix;
		Maths::Matrix4 m_projectionMatrix;
		NLS::Rendering::Settings::EProjectionMode m_projectionMode;

		float m_fov;
		float m_size;
		float m_near;
		float m_far;

		bool m_frustumLightCulling;
		bool m_frustumGeometryCulling;

		Maths::Vector3 m_clearColor;

		// Buffer clearing falgs
		bool m_clearColorBuffer;
		bool m_clearDepthBuffer;
		bool m_clearStencilBuffer;
	};
}