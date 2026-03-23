
#include <Utils/PathParser.h>

#include "GameObject.h"
#include "Components/MaterialRenderer.h"
#include "Components/MeshRenderer.h"
#include "Core/ServiceLocator.h"
#include "Core/ResourceManagement/MaterialManager.h"

namespace NLS::Engine::Components
{
using RenderMaterial = Render::Resources::Material;

MaterialRenderer::MaterialRenderer()
{
    m_materials.fill(nullptr);
}

void MaterialRenderer::OnCreate()
{
    UpdateMaterialList();
}

void MaterialRenderer::FillWithMaterial(RenderMaterial& p_material)
{
    for (uint8_t i = 0; i < m_materials.size(); ++i)
        m_materials[i] = &p_material;
}

void MaterialRenderer::SetMaterialAtIndex(uint8_t p_index, RenderMaterial& p_material)
{
    m_materials[p_index] = &p_material;
}

RenderMaterial* MaterialRenderer::GetMaterialAtIndex(uint8_t p_index)
{
    return m_materials.at(p_index);
}

void MaterialRenderer::RemoveMaterialAtIndex(uint8_t p_index)
{
    if (p_index < m_materials.size())
    {
        m_materials[p_index] = nullptr;
    }
}

void MaterialRenderer::RemoveMaterialByInstance(RenderMaterial& p_instance)
{
    for (uint8_t i = 0; i < m_materials.size(); ++i)
        if (m_materials[i] == &p_instance)
            m_materials[i] = nullptr;
}

void MaterialRenderer::RemoveAllMaterials()
{
    for (uint8_t i = 0; i < m_materials.size(); ++i)
        m_materials[i] = nullptr;
}

const Maths::Matrix4& MaterialRenderer::GetUserMatrix() const
{
    return m_userMatrix;
}

const MaterialRenderer::MaterialList& MaterialRenderer::GetMaterials() const
{
    return m_materials;
}

Array<std::string> MaterialRenderer::GetMaterialPaths() const
{
    Array<std::string> result;
    size_t lastUsedIndex = 0;
    bool hasMaterial = false;
    for (size_t index = 0; index < m_materials.size(); ++index)
    {
        if (m_materials[index] && !m_materials[index]->path.empty())
        {
            lastUsedIndex = index;
            hasMaterial = true;
        }
    }

    if (!hasMaterial)
        return result;

    for (size_t index = 0; index <= lastUsedIndex; ++index)
    {
        if (m_materials[index] && !m_materials[index]->path.empty())
            result.push_back(m_materials[index]->path);
        else
            result.push_back({});
    }

    return result;
}

void MaterialRenderer::SetMaterialPaths(const Array<std::string>& p_paths)
{
    RemoveAllMaterials();
    for (size_t index = 0; index < p_paths.size() && index < kMaxMaterialCount; ++index)
    {
        if (p_paths[index].empty())
            continue;

        if (auto* material = NLS_SERVICE(Core::ResourceManagement::MaterialManager)[p_paths[index]])
            SetMaterialAtIndex(static_cast<uint8_t>(index), *material);
    }
}

Array<float> MaterialRenderer::GetUserMatrixValues() const
{
    Array<float> result;
    result.reserve(16);
    for (float value : m_userMatrix.data)
        result.push_back(value);
    return result;
}

void MaterialRenderer::SetUserMatrixValues(const Array<float>& p_values)
{
    for (uint32_t row = 0; row < 4; ++row)
    {
        for (uint32_t column = 0; column < 4; ++column)
        {
            const size_t index = static_cast<size_t>(row) * 4 + column;
            if (index < p_values.size())
                SetUserMatrixElement(row, column, p_values[index]);
        }
    }
}


void MaterialRenderer::UpdateMaterialList()
{
    if (auto modelRenderer = gameobject()->GetComponent<MeshRenderer>(); modelRenderer && modelRenderer->GetModel())
    {
        uint8_t materialIndex = 0;

        for (const std::string& materialName : modelRenderer->GetModel()->GetMaterialNames())
        {
            m_materialNames[materialIndex++] = materialName;
        }

        for (uint8_t i = materialIndex; i < kMaxMaterialCount; ++i)
            m_materialNames[i] = "";
    }
}

void MaterialRenderer::SetUserMatrixElement(uint32_t p_row, uint32_t p_column, float p_value)
{
    if (p_row < 4 && p_column < 4)
        m_userMatrix.data[4 * p_row + p_column] = p_value;
}

float MaterialRenderer::GetUserMatrixElement(uint32_t p_row, uint32_t p_column) const
{
    if (p_row < 4 && p_column < 4)
        return m_userMatrix.data[4 * p_row + p_column];
    else
        return 0.0f;
}
}
