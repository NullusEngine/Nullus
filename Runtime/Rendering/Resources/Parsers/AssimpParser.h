#include <vector>


#include "RenderDef.h"
#include "Rendering/Assets/SceneImportPipeline.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Resources/Parsers/IModelParser.h"

#pragma once

namespace NLS::Render::Resources::Parsers
{
	/**
	* A simple class to load assimp model data (Vertices only)
	*/
	class NLS_RENDER_API AssimpParser :
		public IModelParser,
		public NLS::Render::Assets::IImportedSceneParserDataProvider
	{
	public:
		/**
		* Simply load meshes from a file using assimp
		* Return true on success
		* @param p_filename
		* @param p_meshes
		* @param p_parserFlags
		*/
		bool LoadModel
		(
			const std::string& p_fileName,
			std::vector<Mesh*>& p_meshes,
			std::vector<std::string>& p_materials,
			EModelParserFlags p_parserFlags
		) override;

		bool LoadModelData(
			const std::string& p_fileName,
			std::vector<ParsedMeshData>& p_meshes,
			std::vector<std::string>& p_materials,
			EModelParserFlags p_parserFlags,
			std::vector<std::string>* p_externalDependencies = nullptr,
			bool p_bakeNodeTransforms = false);

		bool PopulateImportedSceneData(
			const std::filesystem::path& p_sourcePath,
			NLS::Render::Assets::SceneModelSourceFormat p_sourceFormat,
			NLS::Render::Assets::ImportedScene& p_scene) override;

	private:
		void ProcessMaterials(
			const struct aiScene* p_scene,
			NLS::Render::Assets::SceneModelSourceFormat p_sourceFormat,
			std::vector<std::string>& p_materials,
			std::vector<std::string>* p_externalDependencies = nullptr);
		void ProcessNode(void* p_transform, struct aiNode* p_node, const struct aiScene* p_scene, std::vector<ParsedMeshData>& p_meshes);
		void ProcessSourceMeshes(const struct aiScene* p_scene, std::vector<ParsedMeshData>& p_meshes);
		void ProcessMesh(void* p_transform, struct aiMesh* p_mesh, const struct aiScene* p_scene, std::vector<Geometry::Vertex>& p_outVertices, std::vector<uint32_t>& p_outIndices);
		void BuildImportedSceneData(
			const struct aiScene* p_scene,
			NLS::Render::Assets::SceneModelSourceFormat p_sourceFormat,
			NLS::Render::Assets::ImportedScene& p_outScene);

		NLS::Render::Assets::ImportedScene m_lastImportedScene;
		bool m_hasImportedSceneData = false;
	};
}
