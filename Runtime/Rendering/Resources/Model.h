#pragma once

#include <vector>
#include <unordered_map>
#include <string>

#include "Rendering/Resources/Mesh.h"
#include "RenderDef.h"
namespace NLS::Render::Resources
{
	namespace Loaders { class ModelLoader; }

	/**
	* A model is a combinaison of meshes
	*/
	class NLS_RENDER_API Model
	{
		friend class Loaders::ModelLoader;

	public:
		/**
		* Returns the meshes
		*/
		const std::vector<Mesh*>& GetMeshes() const;

		/**
		* Returns the material names
		*/
		const std::vector<std::string>& GetMaterialNames() const;

		/**
		* Returns the bounding sphere of the model
		*/
        const Render::Geometry::BoundingSphere& GetBoundingSphere() const;

	private:
		Model(const std::string& p_path);
		~Model();

		void ComputeBoundingSphere();

	public:
		const std::string path;

	private:
		std::vector<Mesh*> m_meshes;
		std::vector<std::string> m_materialNames;

		Geometry::BoundingSphere m_boundingSphere;
	};
}