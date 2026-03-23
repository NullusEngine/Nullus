#include "Rendering/Resources/Loaders/ModelLoader.h"

namespace NLS::Render::Resources::Loaders
{
Parsers::AssimpParser ModelLoader::__ASSIMP;

Model* ModelLoader::Create(const std::string& p_filepath, Parsers::EModelParserFlags p_parserFlags)
{
    Model* result = new Model(p_filepath);

    if (__ASSIMP.LoadModel(p_filepath, result->m_meshes, result->m_materialNames, p_parserFlags))
    {
        result->ComputeBoundingSphere();
        return result;
    }

    delete result;

    return nullptr;
}

Model* ModelLoader::Create(const std::vector<Mesh*>& meshes)
{
    Model* result = new Model("");
    result->m_meshes = meshes;
    return result;
}

void ModelLoader::Reload(Model& p_model, const std::string& p_filePath, Parsers::EModelParserFlags p_parserFlags)
{
    Model* newModel = Create(p_filePath, p_parserFlags);

    if (newModel)
    {
        p_model.m_meshes = newModel->m_meshes;
        p_model.m_materialNames = newModel->m_materialNames;
        p_model.m_boundingSphere = newModel->m_boundingSphere;
        newModel->m_meshes.clear();
        delete newModel;
    }
}

bool ModelLoader::Destroy(Model*& p_modelInstance)
{
    if (p_modelInstance)
    {
        delete p_modelInstance;
        p_modelInstance = nullptr;

        return true;
    }

    return false;
}
}
