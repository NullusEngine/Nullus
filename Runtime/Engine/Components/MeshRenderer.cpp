#include "Components/MeshRenderer.h"

#include <algorithm>
#include <filesystem>
#include <limits>

#include "Components/MeshFilter.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ServiceLocator.h"
#include "GameObject.h"
#include "Rendering/Resources/Mesh.h"
#include "Serialize/ObjectReferenceResolver.h"

namespace NLS::Engine::Components
{
using RenderMaterial = Render::Resources::Material;

namespace
{
    size_t GetMaterialSlotCountFromIndex(const uint32_t materialIndex, const size_t maxSlotCount)
    {
        if (materialIndex == std::numeric_limits<uint32_t>::max())
            return 1u;

        const auto materialCount = static_cast<size_t>(materialIndex) + 1u;
        return (std::min)(materialCount, maxSlotCount);
    }

    std::string NormalizeMaterialArtifactPath(const std::string& path)
    {
        if (path.empty())
            return {};

        auto resolved = Core::ResourceManagement::MaterialManager::ResolveResourcePath(path);
        std::replace(resolved.begin(), resolved.end(), '\\', '/');
        return std::filesystem::path(resolved).lexically_normal().generic_string();
    }

    bool MaterialArtifactPathMatches(const std::string& lhs, const std::string& rhs)
    {
        if (lhs == rhs)
            return true;
        if (lhs.empty() || rhs.empty())
            return false;
        return NormalizeMaterialArtifactPath(lhs) == NormalizeMaterialArtifactPath(rhs);
    }

    RenderMaterial* FindCachedMaterialByEquivalentPath(
        Core::ResourceManagement::MaterialManager& materialManager,
        const std::string& path)
    {
        if (auto* material = materialManager.GetResource(path, false))
            return material;

        const auto target = NormalizeMaterialArtifactPath(path);
        if (target.empty())
            return nullptr;

        for (const auto& [resourcePath, material] : materialManager.GetResources())
        {
            if (material == nullptr)
                continue;

            if (NormalizeMaterialArtifactPath(resourcePath) == target ||
                NormalizeMaterialArtifactPath(material->path) == target)
            {
                return material;
            }
        }
        return nullptr;
    }
}

MeshRenderer::MeshRenderer()
{
    m_materials.fill(nullptr);
    m_materialPaths.fill({});
    m_failedMaterialPaths.fill({});
}

MeshRenderer::MeshRenderer(const MeshRenderer& other)
    : materials(other.materials)
    , m_materialPaths(other.m_materialPaths)
    , m_materialNames(other.m_materialNames)
    , m_userMatrix(other.m_userMatrix)
    , m_customBoundingSphere(other.m_customBoundingSphere)
    , m_frustumBehaviour(other.m_frustumBehaviour)
    , m_transientRenderingSuppressed(false)
{
    m_materials.fill(nullptr);
    m_failedMaterialPaths.fill({});
    m_owner = nullptr;
}

MeshRenderer& MeshRenderer::operator=(const MeshRenderer& other)
{
    if (this == &other)
        return *this;

    m_enabled = other.m_enabled;
    materials = other.materials;
    m_materials.fill(nullptr);
    m_materialPaths = other.m_materialPaths;
    m_failedMaterialPaths.fill({});
    m_materialNames = other.m_materialNames;
    m_userMatrix = other.m_userMatrix;
    m_customBoundingSphere = other.m_customBoundingSphere;
    m_frustumBehaviour = other.m_frustumBehaviour;
    m_transientRenderingSuppressed = false;
    return *this;
}

MeshRenderer::~MeshRenderer() = default;

void MeshRenderer::OnCreate()
{
    UpdateMaterialList();
}

void MeshRenderer::SetFrustumBehaviour(EFrustumBehaviour p_boundingMode)
{
    if (m_frustumBehaviour == p_boundingMode)
        return;
    m_frustumBehaviour = p_boundingMode;
    MarkRenderStateChanged();
}

MeshRenderer::EFrustumBehaviour MeshRenderer::GetFrustumBehaviour() const
{
    return m_frustumBehaviour;
}

const Render::Geometry::BoundingSphere& MeshRenderer::GetCustomBoundingSphere() const
{
    return m_customBoundingSphere;
}

void MeshRenderer::SetCustomBoundingSphere(const Render::Geometry::BoundingSphere& p_boundingSphere)
{
    m_customBoundingSphere = p_boundingSphere;
    MarkRenderStateChanged();
}

void MeshRenderer::FillWithMaterial(RenderMaterial& p_material)
{
    RemoveAllMaterials();
    const auto materialCount = GetExpectedMaterialSlotCount();
    for (size_t i = 0; i < materialCount && i < m_materials.size(); ++i)
    {
        m_materials[i] = &p_material;
        if (i < materials.size())
            materials[i] = {};
        m_materialPaths[i] = p_material.path;
        m_failedMaterialPaths[i].clear();
    }
    MarkRenderStateChanged();
}

void MeshRenderer::SetMaterialAtIndex(uint8_t p_index, RenderMaterial& p_material)
{
    if (p_index >= m_materials.size())
        return;

    m_materials[p_index] = &p_material;
    if (p_index < materials.size())
        materials[p_index] = {};
    m_materialPaths[p_index] = p_material.path;
    m_failedMaterialPaths[p_index].clear();
    MarkRenderStateChanged();
}

void MeshRenderer::SetResolvedMaterialFromReference(uint8_t p_index, RenderMaterial& p_material)
{
    if (p_index >= m_materials.size())
        return;

    if (p_index < materials.size())
    {
        NLS::Engine::Serialize::ObjectIdentifier identifier;
        if (NLS::Engine::Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(
                materials[p_index].GetInstanceID(),
                identifier) &&
            !NLS::Engine::Serialize::BindResolvedObjectReference(p_material, materials[p_index]))
        {
            const auto referencePath = NLS::Engine::Serialize::ResolveAssetReferencePath(identifier);
            if (MaterialArtifactPathMatches(p_material.path, referencePath))
            {
                m_materials[p_index] = &p_material;
                m_materialPaths[p_index] = referencePath.empty() ? p_material.path : referencePath;
                m_failedMaterialPaths[p_index].clear();
            }
            return;
        }
    }
    m_materials[p_index] = &p_material;
    m_materialPaths[p_index] = p_material.path;
    m_failedMaterialPaths[p_index].clear();
    MarkRenderStateChanged();
}

RenderMaterial* MeshRenderer::GetMaterialAtIndex(uint8_t p_index)
{
    return p_index < m_materials.size() ? m_materials[p_index] : nullptr;
}

void MeshRenderer::RemoveMaterialAtIndex(uint8_t p_index)
{
    if (p_index < m_materials.size())
    {
        m_materials[p_index] = nullptr;
        if (p_index < materials.size())
            materials[p_index] = {};
        m_materialPaths[p_index].clear();
        m_failedMaterialPaths[p_index].clear();
        MarkRenderStateChanged();
    }
}

void MeshRenderer::RemoveMaterialByInstance(RenderMaterial& p_instance)
{
    for (uint8_t i = 0; i < m_materials.size(); ++i)
    {
        if (m_materials[i] == &p_instance)
        {
            m_materials[i] = nullptr;
            if (i < materials.size())
                materials[i] = {};
            m_materialPaths[i].clear();
            m_failedMaterialPaths[i].clear();
            MarkRenderStateChanged();
        }
    }
}

void MeshRenderer::RemoveAllMaterials()
{
    for (uint8_t i = 0; i < m_materials.size(); ++i)
    {
        m_materials[i] = nullptr;
        if (i < materials.size())
            materials[i] = {};
        m_materialPaths[i].clear();
        m_failedMaterialPaths[i].clear();
    }
    materials.clear();
    MarkRenderStateChanged();
}

const Maths::Matrix4& MeshRenderer::GetUserMatrix() const
{
    return m_userMatrix;
}

const MeshRenderer::MaterialList& MeshRenderer::GetMaterials() const
{
    return m_materials;
}

uint64_t MeshRenderer::GetRenderRevision() const
{
    return m_renderRevision;
}

MeshRenderer::Material* MeshRenderer::ResolveMaterialSlot(const size_t p_index)
{
    if (p_index >= m_materialPaths.size())
        return nullptr;

    NLS::Engine::Serialize::ObjectIdentifier identifier;
    const auto path = p_index < materials.size() &&
                      NLS::Engine::Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(materials[p_index].GetInstanceID(), identifier) &&
                      identifier.IsAsset()
        ? NLS::Engine::Serialize::ResolveAssetReferencePath(identifier)
        : m_materialPaths[p_index];

    if (path.empty())
        return m_materials[p_index];

    if (m_materials[p_index] != nullptr && MaterialArtifactPathMatches(m_materials[p_index]->path, path))
        return m_materials[p_index];

    if (!Core::ServiceLocator::Contains<Core::ResourceManagement::MaterialManager>())
        return m_materials[p_index];

    auto& materialManager = NLS_SERVICE(Core::ResourceManagement::MaterialManager);
    auto* material = FindCachedMaterialByEquivalentPath(materialManager, path);

    if (material)
    {
        if (p_index < materials.size() &&
            NLS::Engine::Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(materials[p_index].GetInstanceID(), identifier) &&
            !NLS::Engine::Serialize::BindResolvedObjectReference(*material, materials[p_index]))
        {
            return m_materials[p_index];
        }
        m_materials[p_index] = material;
        m_materialPaths[p_index] = material->path;
        m_failedMaterialPaths[p_index].clear();
        MarkRenderStateChanged();
        return material;
    }

    m_failedMaterialPaths[p_index] = path;
    return nullptr;
}

MeshRenderer::Material* MeshRenderer::ResolveMaterialAtIndex(const uint8_t p_index)
{
    return ResolveMaterialSlot(p_index);
}

const MeshRenderer::MaterialList& MeshRenderer::ResolveMaterials()
{
    for (size_t index = 0; index < m_materialPaths.size(); ++index)
        ResolveMaterialSlot(index);

    return m_materials;
}

NLS::Array<std::string> MeshRenderer::GetMaterialPaths() const
{
    NLS::Array<std::string> result;
    size_t lastUsedIndex = 0;
    bool hasMaterial = false;
    for (size_t index = 0; index < m_materials.size(); ++index)
    {
        if (!m_materialPaths[index].empty() || (m_materials[index] && !m_materials[index]->path.empty()))
        {
            lastUsedIndex = index;
            hasMaterial = true;
        }
    }

    if (!hasMaterial)
        return result;

    for (size_t index = 0; index <= lastUsedIndex; ++index)
    {
        if (!m_materialPaths[index].empty())
            result.push_back(m_materialPaths[index]);
        else if (m_materials[index] && !m_materials[index]->path.empty())
            result.push_back(m_materials[index]->path);
        else
            result.push_back({});
    }

    return result;
}

NLS::Array<NLS::Engine::Serialize::PPtr<MeshRenderer::Material>> MeshRenderer::GetMaterialReferences() const
{
    return materials;
}

void MeshRenderer::SetMaterialPaths(const NLS::Array<std::string>& p_paths)
{
    RemoveAllMaterials();
    for (size_t index = 0; index < p_paths.size() && index < kMaxMaterialCount; ++index)
    {
        if (materials.size() <= index)
            materials.resize(index + 1);
        materials[index] = {};
        m_materialPaths[index] = p_paths[index];
        m_failedMaterialPaths[index].clear();
        MarkRenderStateChanged();
    }

    if (!Core::ServiceLocator::Contains<Core::ResourceManagement::MaterialManager>())
        return;

    for (size_t index = 0; index < p_paths.size() && index < kMaxMaterialCount; ++index)
    {
        if (p_paths[index].empty())
            continue;

        if (auto* material = NLS_SERVICE(Core::ResourceManagement::MaterialManager)[p_paths[index]])
            SetMaterialAtIndex(static_cast<uint8_t>(index), *material);
    }
}

void MeshRenderer::SetMaterialPathHints(const NLS::Array<std::string>& p_paths)
{
    for (size_t index = 0; index < kMaxMaterialCount; ++index)
    {
        const std::string path = index < p_paths.size() ? p_paths[index] : std::string {};
        if (m_materials[index] != nullptr && !MaterialArtifactPathMatches(m_materials[index]->path, path))
        {
            m_materials[index] = nullptr;
            MarkRenderStateChanged();
        }
        m_materialPaths[index] = path;
        m_failedMaterialPaths[index].clear();
        MarkRenderStateChanged();
    }
}

void MeshRenderer::SetTransientRenderingSuppressed(const bool suppressed)
{
    if (m_transientRenderingSuppressed == suppressed)
        return;
    m_transientRenderingSuppressed = suppressed;
    MarkRenderStateChanged();
}

bool MeshRenderer::IsTransientRenderingSuppressed() const
{
    return m_transientRenderingSuppressed;
}

void MeshRenderer::SetMaterialReferences(const NLS::Array<NLS::Engine::Serialize::PPtr<Material>>& p_references)
{
    RemoveAllMaterials();
    materials = p_references;
    for (size_t index = 0; index < p_references.size() && index < kMaxMaterialCount; ++index)
    {
        NLS::Engine::Serialize::ObjectIdentifier identifier;
        if (NLS::Engine::Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(p_references[index].GetInstanceID(), identifier))
            m_materialPaths[index] = identifier.guid.IsValid() ? identifier.filePath : std::string {};
        else
            m_materialPaths[index].clear();
        m_failedMaterialPaths[index].clear();
        MarkRenderStateChanged();
    }
}

void MeshRenderer::SetMaterialObjectIdentifiers(const NLS::Array<NLS::Engine::Serialize::ObjectIdentifier>& p_identifiers)
{
    NLS::Array<NLS::Engine::Serialize::PPtr<Material>> references;
    references.reserve(p_identifiers.size());
    for (const auto& identifier : p_identifiers)
    {
        references.emplace_back(NLS::Engine::Serialize::PersistentManager::Instance().ObjectIdentifierToInstanceID(identifier));
    }
    SetMaterialReferences(references);
}

void MeshRenderer::FillEmptySlotsWithMaterial(RenderMaterial& p_material)
{
    const auto materialCount = GetExpectedMaterialSlotCount();
    for (size_t i = 0; i < materialCount && i < m_materials.size(); ++i)
    {
        if (m_materials[i] == nullptr)
        {
            m_materials[i] = &p_material;
            MarkRenderStateChanged();
        }
    }
}

NLS::Array<float> MeshRenderer::GetUserMatrixValues() const
{
    NLS::Array<float> result;
    result.reserve(16);
    for (float value : m_userMatrix.data)
        result.push_back(value);
    return result;
}

void MeshRenderer::SetUserMatrixValues(const NLS::Array<float>& p_values)
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

void MeshRenderer::UpdateMaterialList()
{
    if (auto meshFilter = gameobject() ? gameobject()->GetComponent<MeshFilter>() : nullptr;
        meshFilter)
    {
        auto* mesh = meshFilter->ResolveMesh();
        if (mesh == nullptr)
            return;

        const auto materialSlotCount = GetMaterialSlotCountFromIndex(mesh->GetMaterialIndex(), m_materialNames.size());
        for (size_t i = 0; i < materialSlotCount; ++i)
            m_materialNames[i] = "Material " + std::to_string(i);
        for (size_t i = materialSlotCount; i < m_materialNames.size(); ++i)
            m_materialNames[i] = "";
    }
}

size_t MeshRenderer::GetExpectedMaterialSlotCount()
{
    if (auto meshFilter = gameobject() ? gameobject()->GetComponent<MeshFilter>() : nullptr;
        meshFilter)
    {
        if (auto* mesh = meshFilter->ResolveMesh())
        {
            return GetMaterialSlotCountFromIndex(mesh->GetMaterialIndex(), m_materialNames.size());
        }
    }

    size_t materialCount = 0;
    for (const auto& materialName : m_materialNames)
    {
        if (!materialName.empty())
            ++materialCount;
    }

    return materialCount > 0 ? materialCount : 1;
}

void MeshRenderer::SetUserMatrixElement(uint32_t p_row, uint32_t p_column, float p_value)
{
    if (p_row < 4 && p_column < 4)
    {
        if (m_userMatrix.data[4 * p_row + p_column] == p_value)
            return;
        m_userMatrix.data[4 * p_row + p_column] = p_value;
        MarkRenderStateChanged();
    }
}

float MeshRenderer::GetUserMatrixElement(uint32_t p_row, uint32_t p_column) const
{
    if (p_row < 4 && p_column < 4)
        return m_userMatrix.data[4 * p_row + p_column];
    else
        return 0.0f;
}

void MeshRenderer::MarkRenderStateChanged()
{
    ++m_renderRevision;
}
}
