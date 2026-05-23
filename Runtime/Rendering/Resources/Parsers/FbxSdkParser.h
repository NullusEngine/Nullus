#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "RenderDef.h"
#include "Rendering/Assets/SceneImportPipeline.h"
#include "Rendering/Resources/Parsers/IModelParser.h"

namespace NLS::Render::Resources::Parsers
{
class NLS_RENDER_API FbxSdkParser :
	public IModelParser,
	public NLS::Render::Assets::IImportedSceneParserDataProvider
{
public:
	bool LoadModel(
		const std::string& p_fileName,
		std::vector<Mesh*>& p_meshes,
		std::vector<std::string>& p_materials,
		EModelParserFlags p_parserFlags) override;

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
	NLS::Render::Assets::ImportedScene m_lastImportedScene;
	bool m_hasImportedSceneData = false;
};
}
