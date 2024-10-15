#pragma once

#include "Math/Quaternion.h"
#include "Math/Matrix4.h"
#include "Math/Matrix3.h"
#include "Math/Vector3.h"
#include "MathDef.h"
#include <functional>
namespace NLS
{
	namespace Maths
	{
		/**
		* The TransformNotifier is a simple notification system used by transform to send notifications to his children
		*/
		class TransformNotifier
		{
		public:
			/**
			* Notifications that the transform can send
			*/
			enum class ENotification
			{
				TRANSFORM_CHANGED,
				TRANSFORM_DESTROYED
			};

			/**
			* The NotificationHandler is a function callback that takes a ENotification in parameters
			*/
			using NotificationHandler = std::function<void(ENotification)>;

			/**
			* The ID of a NotificationHandler
			* This value is needed to remove a NotificationHandler from a Notifier
			*/
			using NotificationHandlerID = uint64_t;

			/**
			* Add a NotificationHandler to the Notifier. The NotificationHandler will receive
			* every notifications sent by this Notifier.
			* This method also return a NotificationHandlerID needed to remove the handler later
			* @param p_notificationHandler
			*/
			NotificationHandlerID AddNotificationHandler(NotificationHandler p_notificationHandler);

			/**
			* Notify children (NotificationHandler) by sending the given notification
			* @param p_notification
			*/
			void NotifyChildren(ENotification p_notification);

			/**
			* Remove a NotificationHandler to the Notifier using the NotificationHandlerID generated
			* by the "AddNotificationHandler" method.
			* This method returns true on success (NotificationHandler removed)
			* @param p_notificationHandlerID
			*/
			bool RemoveNotificationHandler(const NotificationHandlerID& p_notificationHandlerID);

		private:
			std::unordered_map<NotificationHandlerID, NotificationHandler> m_notificationHandlers;
			NotificationHandlerID m_availableHandlerID = 0;
		};

		/**
		* Mathematic representation of a 3D transformation with float precision
		*/
        class NLS_MATH_API Transform
		{
        public:
            static void Bind();
		public:
			/**
			* Create a transform without setting a parent
			* @param p_localPosition
			* @param p_localRotation
			* @param p_localScale
			*/
			Transform(Vector3 p_localPosition = Vector3(0.0f, 0.0f, 0.0f), Quaternion p_localRotation = Quaternion::Identity, Vector3 p_localScale = Vector3(1.0f, 1.0f, 1.0f));

			/**
			* Destructor of the transform.
			* Will notify children on destruction
			*/
			~Transform();

			/**
			* Simple callback that will treat parent notifications
			* @param p_notification
			*/
			void NotificationHandler(TransformNotifier::ENotification p_notification);

			/**
			* Defines a parent to the transform
			* @param p_parent
			*/
			void SetParent(Transform& p_parent);

			/**
			* Set the parent to nullptr and recalculate world matrix
			* Returns true on success
			*/
			bool RemoveParent();

			/**
			* Check if the transform has a parent
			*/
			bool HasParent() const;

			/**
			* Initialize transform with raw data from world info
			* @param p_position
			* @param p_rotation
			* @param p_scale
			*/
			void GenerateMatricesWorld(Vector3 p_position, Quaternion p_rotation, Vector3 p_scale);

			/**
			* Initialize transform with raw data from local info
			* @param p_position
			* @param p_rotation
			* @param p_scale
			*/
			void GenerateMatricesLocal(Vector3 p_position, Quaternion p_rotation, Vector3 p_scale);

			/**
			* Re-update world matrix to use parent transformations
			*/
			void UpdateWorldMatrix();

			/**
			* Re-update local matrix to use parent transformations
			*/
			void UpdateLocalMatrix();

			/**
			* Set the position of the transform in the local space
			* @param p_newPosition
			*/
			void SetLocalPosition(Vector3 p_newPosition);

			/**
			* Set the rotation of the transform in the local space
			* @param p_newRotation
			*/
			void SetLocalRotation(Quaternion p_newRotation);

			/**
			* Set the scale of the transform in the local space
			* @param p_newScale
			*/
			void SetLocalScale(Vector3 p_newScale);

			/**
			* Set the position of the transform in world space
			* @param p_newPosition
			*/
			void SetWorldPosition(Vector3 p_newPosition);

			/**
			* Set the rotation of the transform in world space
			* @param p_newRotation
			*/
			void SetWorldRotation(Quaternion p_newRotation);

			/**
			* Set the scale of the transform in world space
			* @param p_newScale
			*/
			void SetWorldScale(Vector3 p_newScale);

			/**
			* Translate in the local space
			* @param p_translation
			*/
			void TranslateLocal(const Vector3& p_translation);

			/**
			* Rotate in the local space
			* @param p_rotation
			*/
			void RotateLocal(const Quaternion& p_rotation);

			/**
			* Scale in the local space
			* @param p_scale
			*/
			void ScaleLocal(const Vector3& p_scale);

			/**
			* Return the position in local space
			*/
			const Vector3& GetLocalPosition() const;

			/**
			* Return the rotation in local space
			*/
			const Quaternion& GetLocalRotation() const;

			/**
			* Return the scale in local space
			*/
			const Vector3& GetLocalScale() const;

			/**
			* Return the position in world space
			*/
			const Vector3& GetWorldPosition() const;

			/**
			* Return the rotation in world space
			*/
			const Quaternion& GetWorldRotation() const;

			/**
			* Return the scale in world space
			*/
			const Vector3& GetWorldScale() const;

			/**
			* Return the local matrix
			*/
			const Matrix4& GetLocalMatrix() const;

			/**
			* Return the world matrix
			*/
			const Matrix4& GetWorldMatrix() const;

			/**
			* Return the transform world forward
			*/
			Vector3 GetWorldForward() const;

			/**
			* Return the transform world up
			*/
			Vector3 GetWorldUp() const;

			/**
			* Return the transform world right
			*/
			Vector3 GetWorldRight() const;

			/**
			* Return the transform local forward
			*/
			Vector3 GetLocalForward() const;

			/**
			* Return the transform local up
			*/
			Vector3 GetLocalUp() const;

			/**
			* Return the transform local right
			*/
			Vector3 GetLocalRight() const;

		public:
			TransformNotifier Notifier;
			TransformNotifier::NotificationHandlerID m_notificationHandlerID;

		private:
			void PreDecomposeWorldMatrix();
			void PreDecomposeLocalMatrix();

			/* Pre-decomposed data to prevent multiple decomposition */
			Vector3 m_localPosition;
			Quaternion m_localRotation;
			Vector3 m_localScale;
			Vector3 m_worldPosition;
			Quaternion m_worldRotation;
			Vector3 m_worldScale;

			Matrix4 m_localMatrix;
			Matrix4 m_worldMatrix;

			Transform* m_parent;
		};
	}
}
