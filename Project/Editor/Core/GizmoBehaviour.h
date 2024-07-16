#pragma once

#include <Math/Transform.h>
#include <Math/Vector2.h>

#include "GameObject.h"

namespace NLS::Editor::Core
{
	enum class EGizmoOperation
	{
		TRANSLATE,
		ROTATE,
		SCALE
	};

	/* Handle gizmo behaviours */
	class GizmoBehaviour
	{
	public:
		enum class EDirection
		{
			X,
			Y,
			Z
		};
		
		/**
		* Returns true if the snapping behaviour is enabled
		*/
		bool IsSnappedBehaviourEnabled() const;

		/**
		* Starts the gizmo picking behaviour for the given target in the given direction
		* @param p_actor
		* @param p_cameraPosition
		* @param p_operation
		* @param p_direction
		*/
		void StartPicking(Engine::GameObject& p_target, const Maths::Vector3& p_cameraPosition, EGizmoOperation p_operation, EDirection p_direction);

		/**
		* Stops the gizmo picking behaviour
		*/
		void StopPicking();

		/**
		* Handle the current behaviour
		* @param p_viewMatrix
		* @param p_projectionMatrix
		* @param p_viewSize
		*/
		void ApplyOperation(const Maths::Matrix4& p_viewMatrix, const Maths::Matrix4& p_projectionMatrix, const Maths::Vector2& p_viewSize);

		/**
		* Set the given mouse position as the current mouse position and update the previous mouse position
		* @param p_mousePosition
		*/
		void SetCurrentMouse(const Maths::Vector2& p_mousePosition);

		/**
		* Returns true if the gizmo is currently picked
		*/
		bool IsPicking() const;

		/**
		* Returns the current operation direction
		*/
		EDirection GetDirection() const;

	private:
		/**
		* Returns the global direction matching with the current m_direction
		*/
		Maths::Vector3 GetFakeDirection() const;

		/**
		* Returns the actual direction of the target matching with the current m_direction
		* @param p_relative (If true, the direction depends on hierarchy)
		*/
		Maths::Vector3 GetRealDirection(bool p_relative = false) const;

		/**
		* Returns the 3D vector of the arrow projected to the screen
		* @param p_viewMatrix
		* @param p_projectionMatrix
		* @param p_viewSize
		*/
		Maths::Vector2 GetScreenDirection(const Maths::Matrix4& p_viewMatrix, const Maths::Matrix4& p_projectionMatrix, const Maths::Vector2& p_viewSize) const;

		/**
		* Handle the translation behaviour
		* @param p_viewMatrix
		* @param p_projectionMatrix
		* @param p_viewSize
		*/
		void ApplyTranslation(const Maths::Matrix4& p_viewMatrix, const Maths::Matrix4& p_projectionMatrix, const Maths::Vector2& p_viewSize) const;

		/**
		* Handle the rotation behaviour
		* @param p_viewMatrix
		* @param p_projectionMatrix
		* @param p_viewSize
		*/
		void ApplyRotation(const Maths::Matrix4& p_viewMatrix, const Maths::Matrix4& p_projectionMatrix, const Maths::Vector2& p_viewSize) const;

		/**
		* Handle the scale behaviour
		* @param p_viewMatrix
		* @param p_projectionMatrix
		* @param p_viewSize
		*/
		void ApplyScale(const Maths::Matrix4& p_viewMatrix, const Maths::Matrix4& p_projectionMatrix, const Maths::Vector2& p_viewSize) const;

	private:
		bool m_firstMouse = true;
		float m_distanceToActor = 0.0f;
		Engine::GameObject* m_target = nullptr;
		EGizmoOperation m_currentOperation;
		EDirection m_direction;
		Maths::Transform m_originalTransform;
		Maths::Vector2 m_originMouse;
		Maths::Vector2 m_currentMouse;
		Maths::Vector2 m_screenDirection;
	};
}
