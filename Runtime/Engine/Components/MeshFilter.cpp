#include "Components/MeshFilter.h"

#include "Components/MeshRenderer.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ServiceLocator.h"
#include "Debug/Logger.h"
#include "GameObject.h"
#include "PrimitiveFactory.h"
#include "Serialize/ObjectReferenceResolver.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace NLS::Engine::Components
{
namespace
{
NLS::Engine::Serialize::ObjectIdentifier MakeMeshPathObjectIdentifier(const std::string& path)
{
    if (path.empty())
        return {};

    const auto guid = NLS::Guid::NewDeterministic("NLS.MeshReference:" + path);
    return NLS::Engine::Serialize::ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(guid),
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(guid, path),
        path);
}

bool MeshArtifactPathExists(const std::string& path)
{
    if (path.empty())
        return false;

    auto extension = std::filesystem::path(path).extension().string();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    if (extension != ".nmesh")
        return false;

    std::error_code error;
    const auto resolvedPath = Core::ResourceManagement::MeshManager::ResolveResourcePath(path);
    return !resolvedPath.empty() && std::filesystem::is_regular_file(resolvedPath, error);
}

Render::Resources::Mesh* TryLoadCanonicalPrimitiveMesh(
    Core::ResourceManagement::MeshManager& meshManager,
    const std::string& path)
{
    const auto primitiveType = NLS::Engine::TryGetPrimitiveTypeFromMeshResourcePath(path);
    if (!primitiveType.has_value() || path != NLS::Engine::GetPrimitiveMeshResourcePath(*primitiveType))
        return nullptr;

    return meshManager.GetResource(path, true);
}
}

MeshFilter::MeshFilter() = default;

MeshFilter::MeshFilter(const MeshFilter& other)
    : mesh(other.mesh)
    , m_meshPath(other.m_meshPath)
{
    m_owner = nullptr;
}

MeshFilter& MeshFilter::operator=(const MeshFilter& other)
{
    if (this == &other)
        return *this;

    m_enabled = other.m_enabled;
    m_transientMesh.reset();
    mesh = other.mesh;
    m_meshPath = other.m_meshPath;
    m_failedMeshPath.clear();
    NotifyMeshChanged();
    return *this;
}

MeshFilter::~MeshFilter() = default;

void MeshFilter::NotifyMeshChanged()
{
    if (m_owner)
    {
        if (auto meshRenderer = m_owner->GetComponent<MeshRenderer>())
            meshRenderer->UpdateMaterialList();
    }
}

void MeshFilter::SetMesh(Render::Resources::Mesh* p_mesh)
{
    m_transientMesh.reset();
    mesh = p_mesh;
    m_meshPath.clear();
    m_failedMeshPath.clear();
    NotifyMeshChanged();
}

void MeshFilter::SetMeshPath(const std::string& p_path)
{
    m_transientMesh.reset();
    mesh = {};
    m_meshPath = p_path;
    m_failedMeshPath.clear();

    if (p_path.empty())
    {
        NotifyMeshChanged();
        return;
    }

    if (!Core::ServiceLocator::Contains<Core::ResourceManagement::MeshManager>())
    {
        NotifyMeshChanged();
        return;
    }

    auto& meshManager = NLS_SERVICE(Core::ResourceManagement::MeshManager);
    auto* resolvedMesh = meshManager.GetResource(p_path, false);

    if (resolvedMesh == nullptr)
    {
        NotifyMeshChanged();
        return;
    }

    NLS::Engine::Serialize::ObjectIdentifier existingMeshIdentifier;
    const bool meshAlreadyHasPersistentIdentity =
        NLS::Engine::Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(
            resolvedMesh->GetInstanceID(),
            existingMeshIdentifier);
    auto identifier = MakeMeshPathObjectIdentifier(p_path);
    if (!meshAlreadyHasPersistentIdentity && identifier.IsValid())
    {
        const auto instanceID =
            NLS::Engine::Serialize::PersistentManager::Instance().BindObjectIdentifier(*resolvedMesh, identifier);
        if (instanceID != NLS::Engine::Serialize::InstanceID_None)
            mesh.SetInstanceID(instanceID);
    }

    SetResolvedMeshFromReference(resolvedMesh);
    m_meshPath = p_path;
}

void MeshFilter::SetResolvedMeshFromReference(Render::Resources::Mesh* p_mesh)
{
    if (p_mesh)
    {
        NLS::Engine::Serialize::ObjectIdentifier identifier;
        if (NLS::Engine::Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(
                mesh.GetInstanceID(),
                identifier) &&
            !NLS::Engine::Serialize::BindResolvedObjectReference(*p_mesh, mesh))
        {
            return;
        }

        m_transientMesh.reset();
        if (mesh.Get() != p_mesh)
            mesh = p_mesh;
        if (NLS::Engine::Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(
                mesh.GetInstanceID(),
                identifier))
        {
            m_meshPath = identifier.filePath;
        }
        m_failedMeshPath.clear();
    }
    else
    {
        m_transientMesh.reset();
        NLS::Engine::Serialize::ObjectIdentifier identifier;
        if (NLS::Engine::Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(
                mesh.GetInstanceID(),
                identifier))
        {
            m_meshPath = identifier.filePath;
        }
        m_failedMeshPath.clear();
    }
    NotifyMeshChanged();
}

void MeshFilter::SetResolvedTransientMeshFromReference(std::shared_ptr<Render::Resources::Mesh> p_mesh)
{
    auto* rawMesh = p_mesh.get();
    if (rawMesh)
    {
        NLS::Engine::Serialize::ObjectIdentifier identifier;
        if (NLS::Engine::Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(
                mesh.GetInstanceID(),
                identifier) &&
            !NLS::Engine::Serialize::BindResolvedObjectReference(*rawMesh, mesh))
        {
            return;
        }

        m_transientMesh = std::move(p_mesh);
        if (mesh.Get() != rawMesh)
            mesh = rawMesh;
        if (NLS::Engine::Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(
                mesh.GetInstanceID(),
                identifier))
        {
            m_meshPath = identifier.filePath;
        }
        m_failedMeshPath.clear();
    }
    else
    {
        m_transientMesh.reset();
        mesh = {};
        m_meshPath.clear();
        m_failedMeshPath.clear();
    }
    NotifyMeshChanged();
}

Render::Resources::Mesh* MeshFilter::ResolveMesh()
{
    if (auto* directMesh = mesh.Get())
        return directMesh;

    NLS::Engine::Serialize::ObjectIdentifier identifier;
    if (NLS::Engine::Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(
            mesh.GetInstanceID(),
            identifier) &&
        identifier.IsAsset())
    {
        const auto path = NLS::Engine::Serialize::ResolveAssetReferencePath(identifier);
        if (path.empty())
            return nullptr;

        if (!Core::ServiceLocator::Contains<Core::ResourceManagement::MeshManager>())
            return nullptr;

        auto& meshManager = NLS_SERVICE(Core::ResourceManagement::MeshManager);
        auto* resolvedMesh = meshManager.GetResource(path, false);
        if (!resolvedMesh)
        {
            const auto primitiveType = NLS::Engine::TryGetPrimitiveTypeFromMeshResourcePath(path);
            if (primitiveType.has_value() && path == NLS::Engine::GetPrimitiveMeshResourcePath(*primitiveType))
                resolvedMesh = meshManager.GetResource(path, true);
        }
        if (!resolvedMesh)
        {
            if (m_failedMeshPath != path && !MeshArtifactPathExists(path))
                NLS_LOG_WARNING("Failed to resolve mesh filter mesh path during reflection load: " + path);
            m_failedMeshPath = path;
            return nullptr;
        }

        if (!NLS::Engine::Serialize::BindResolvedObjectReference(*resolvedMesh, mesh))
            return nullptr;

        m_meshPath = path;
        m_failedMeshPath.clear();
        NotifyMeshChanged();
        return resolvedMesh;
    }

    if (m_meshPath.empty())
        return nullptr;

    if (!Core::ServiceLocator::Contains<Core::ResourceManagement::MeshManager>())
        return nullptr;

    auto& meshManager = NLS_SERVICE(Core::ResourceManagement::MeshManager);
    auto* resolvedMesh = meshManager.GetResource(m_meshPath, false);
    if (!resolvedMesh)
        resolvedMesh = TryLoadCanonicalPrimitiveMesh(meshManager, m_meshPath);
    if (!resolvedMesh)
        return nullptr;

    NLS::Engine::Serialize::ObjectIdentifier existingMeshIdentifier;
    const bool meshAlreadyHasPersistentIdentity =
        NLS::Engine::Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(
            resolvedMesh->GetInstanceID(),
            existingMeshIdentifier);
    auto meshPathIdentifier = MakeMeshPathObjectIdentifier(m_meshPath);
    if (!meshAlreadyHasPersistentIdentity && meshPathIdentifier.IsValid())
    {
        const auto meshPathInstanceID =
            NLS::Engine::Serialize::PersistentManager::Instance().BindObjectIdentifier(*resolvedMesh, meshPathIdentifier);
        if (meshPathInstanceID != NLS::Engine::Serialize::InstanceID_None)
            mesh.SetInstanceID(meshPathInstanceID);
    }

    SetResolvedMeshFromReference(resolvedMesh);
    return resolvedMesh;
}

std::string MeshFilter::GetModelPath() const
{
    return m_meshPath;
}

NLS::Engine::Serialize::PPtr<MeshFilter::Mesh> MeshFilter::GetMeshReference() const
{
    return mesh;
}

void MeshFilter::SetMeshReference(NLS::Engine::Serialize::PPtr<Mesh> p_reference)
{
    m_transientMesh.reset();
    mesh = p_reference;
    NLS::Engine::Serialize::ObjectIdentifier identifier;
    if (NLS::Engine::Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(mesh.GetInstanceID(), identifier))
        SetModelPathHint(identifier.guid.IsValid() ? identifier.filePath : std::string {});
    else
        SetModelPathHint({});
}

void MeshFilter::SetMeshObjectIdentifier(const NLS::Engine::Serialize::ObjectIdentifier& p_identifier)
{
    m_transientMesh.reset();
    mesh = NLS::Engine::Serialize::PPtr<Mesh>(
        NLS::Engine::Serialize::PersistentManager::Instance().ObjectIdentifierToInstanceID(p_identifier));
    SetModelPathHint(p_identifier.guid.IsValid() ? p_identifier.filePath : std::string {});
}

void MeshFilter::SetModelPath(const std::string& p_path)
{
    SetMeshPath(p_path);
}

void MeshFilter::SetModelPathHint(const std::string& p_path)
{
    m_transientMesh.reset();
    m_meshPath = p_path;
    m_failedMeshPath.clear();
    NotifyMeshChanged();
}
}
