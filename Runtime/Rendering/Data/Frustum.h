#pragma once

#include <array>

#include <Math/Matrix4.h>
#include <Math/Transform.h>

#include "Rendering/Geometry/BoundingSphere.h"
#include "Rendering/Resources/Model.h"
using namespace NLS;
namespace NLS::Rendering::Data
{
	/**
	* Mathematic representation of a 3D frustum
	*/
	class Frustum
	{
	public:
		/**
		* Update frustum values
		* @param p_viewProjection
		*/ 
		void CalculateFrustum(const Maths::Matrix4& p_viewProjection);

		/**
		* Returns true if the given point is in frustum
		* @param p_x
		* @param p_y
		* @param p_z
		*/
		bool PointInFrustum(float p_x, float p_y, float p_z) const;

		/**
		* Returns true if the given sphere is in frustum
		* @param p_x
		* @param p_y
		* @param p_z
		* @param p_radius
		*/
		bool SphereInFrustum(float p_x, float p_y, float p_z, float p_radius) const;

		/**
		* Returns true if the given cube is in frustum
		* @param p_x
		* @param p_y
		* @param p_z
		* @param p_size
		*/
		bool CubeInFrustum(float p_x, float p_y, float p_z, float p_size) const;

		/**
		* Returns true if the given bouding sphere is in frustum
		* @param p_boundingSphere
		* @param p_transform
		*/
		bool BoundingSphereInFrustum(const Rendering::Geometry::BoundingSphere& p_boundingSphere, const Maths::Transform& p_transform) const;

		/**
		* Returns true if the 
		* @param p_mesh
		* @param p_transform
		*/
		bool IsMeshInFrustum(const NLS::Rendering::Resources::Mesh& p_mesh, const Maths::Transform& p_transform) const;

		/**
		* Returns the list of meshes from a model that should be rendered
		* @param p_model
		* @param p_modelBoundingSphere
		* @param p_modelTransform
		* @param p_frustum
		* @param p_cullingOptions
		*/
		std::vector<NLS::Rendering::Resources::Mesh*> GetMeshesInFrustum(
			const NLS::Rendering::Resources::Model& p_model,
			const Rendering::Geometry::BoundingSphere& p_modelBoundingSphere,
			const Maths::Transform& p_modelTransform,
			NLS::Rendering::Settings::ECullingOptions p_cullingOptions
		) const;

		/**
		* Returns the near plane
		*/
		std::array<float, 4> GetNearPlane() const;

		/**
		* Returns the far plane
		*/
		std::array<float, 4> GetFarPlane() const;

	private:
		float m_frustum[6][4];
	};
}