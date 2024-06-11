
#include <Core/Utils/PathParser.h>

#include "GameObject.h"
#include "Components/MaterialRenderer.h"
#include "Components/MeshRenderer.h"
#include "Core/ServiceLocator.h"
using namespace NLS::Engine::Components;

NLS::Engine::Components::MaterialRenderer::MaterialRenderer()
{
	m_materials.fill(nullptr);
}

void MaterialRenderer::OnCreate()
{
    UpdateMaterialList();
}

void MaterialRenderer::FillWithMaterial(NLS::Rendering::Data::Material & p_material)
{
	for (uint8_t i = 0; i < m_materials.size(); ++i)
		m_materials[i] = &p_material;
}

void MaterialRenderer::SetMaterialAtIndex(uint8_t p_index,NLS::Rendering::Data::Material& p_material)
{
	m_materials[p_index] = &p_material;
}

NLS::Rendering::Data::Material* MaterialRenderer::GetMaterialAtIndex(uint8_t p_index)
{
	return m_materials.at(p_index);
}

void MaterialRenderer::RemoveMaterialAtIndex(uint8_t p_index)
{
	if (p_index < m_materials.size())
	{
		m_materials[p_index] = nullptr;;
	}
}

void MaterialRenderer::RemoveMaterialByInstance(NLS::Rendering::Data::Material& p_instance)
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

const Maths::Matrix4 & MaterialRenderer::GetUserMatrix() const
{
	return m_userMatrix;
}

const MaterialRenderer::MaterialList& MaterialRenderer::GetMaterials() const
{
	return m_materials;
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
