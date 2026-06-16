#include "Assets/EditorThumbnailPreviewRenderer.h"

#include "Assets/AssetThumbnailService.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/EditorAssetManifestJson.h"
#include "Assets/EditorAssetPath.h"
#include "Assets/ImportedPrefabRendererDependencyTemplates.h"
#include "Assets/NativeArtifactContainer.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Core/EditorActions.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Debug/Logger.h"
#include "Engine/Assets/PrefabAsset.h"
#include "Engine/Rendering/SceneRendererFactory.h"
#include "GameObject.h"
#include "Rendering/Buffers/Framebuffer.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Data/FrameDescriptor.h"
#include "Rendering/FrameGraph/ExternalResourceBridge.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Serialize/ObjectGraphInstantiator.h"
#include "SceneSystem/Scene.h"
#include "ServiceLocator.h"

#include <Json/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace NLS::Editor::Assets
{
namespace
{
constexpr float kUnityMeshPreviewFieldOfViewDegrees = 30.0f;
constexpr float kUnityMeshPreviewYawDegrees = -120.0f;
constexpr float kUnityMeshPreviewPitchDegrees = 20.0f;
constexpr float kUnityPrefabPreviewYawDegrees = -135.0f;
constexpr float kUnityPrefabPreviewPitchDegrees = -35.0f;
constexpr float kUnityMaterialPreviewDistance = 5.0f;
constexpr float kDegreesToRadians = 3.14159265358979323846f / 180.0f;
constexpr size_t kMaxGpuPreviewNativeArtifactFileBytes = 16u * 1024u * 1024u;
constexpr size_t kMaxGpuPreviewStructurePayloadBytes = 2u * 1024u * 1024u;
constexpr size_t kMaxGpuPreviewMeshVertices = 240000u;
constexpr size_t kMaxGpuPreviewMeshIndices = 720000u;
constexpr const char* kGpuPreviewMeshBudgetExceededDiagnostic = "thumbnail-model-preview-budget-exceeded";
constexpr const char* kGpuPreviewMaterialBudgetExceededDiagnostic = "thumbnail-material-preview-budget-exceeded";
constexpr const char* kGpuPreviewPrefabBudgetExceededDiagnostic = "thumbnail-prefab-preview-budget-exceeded";

std::string ToGenericPath(const std::filesystem::path& path)
{
    return path.lexically_normal().generic_string();
}

std::optional<NLS::Core::Assets::ArtifactManifest> LoadManifest(const AssetThumbnailRequest& request)
{
    const auto manifestPath =
        request.projectRoot /
        "Library" /
        "Artifacts" /
        request.assetId.ToString() /
        "manifest.json";

    std::ifstream input(manifestPath, std::ios::binary);
    if (!input)
        return std::nullopt;

    auto root = nlohmann::json::parse(input, nullptr, false);
    if (root.is_discarded())
        return std::nullopt;
    return ParseArtifactManifestJson(root, true);
}

std::optional<std::filesystem::path> ResolveArtifactPath(
    const AssetThumbnailRequest& request,
    const std::string& artifactPath)
{
    if (artifactPath.empty() || !request.assetId.IsValid())
        return std::nullopt;

    const auto rawPath = std::filesystem::path(artifactPath).lexically_normal();
    const auto artifactRoot = NLS::Core::Assets::NormalizeAssetPath(
        request.projectRoot / "Library" / "Artifacts" / request.assetId.ToString());
    if (artifactRoot.empty())
        return std::nullopt;

    auto resolveCandidate = [&artifactRoot](const std::filesystem::path& candidate)
        -> std::optional<std::filesystem::path>
    {
        const auto normalized = NLS::Core::Assets::NormalizeAssetPath(candidate);
        if (normalized.empty() ||
            !IsPhysicalRegularFileInsideEditorAssetRoot(normalized, artifactRoot))
        {
            return std::nullopt;
        }
        return normalized;
    };

    if (rawPath.is_absolute())
        return resolveCandidate(rawPath);

    if (auto resolved = resolveCandidate(request.projectRoot / rawPath);
        resolved.has_value())
    {
        return resolved;
    }
    return resolveCandidate(artifactRoot / rawPath);
}

std::optional<std::filesystem::path> ResolveSourceMaterialPath(
    const AssetThumbnailRequest& request,
    const std::string& artifactPath)
{
    if (artifactPath.empty())
        return std::nullopt;

    const auto rawPath = std::filesystem::path(artifactPath).lexically_normal();
    if (rawPath.extension() != ".mat")
        return std::nullopt;

    const auto assetsRoot = NLS::Core::Assets::NormalizeAssetPath(request.projectRoot / "Assets");
    if (assetsRoot.empty())
        return std::nullopt;

    const auto candidate = rawPath.is_absolute()
        ? rawPath
        : request.projectRoot / rawPath;
    const auto normalized = NLS::Core::Assets::NormalizeAssetPath(candidate);
    if (normalized.empty() ||
        !IsPhysicalRegularFileInsideEditorAssetRoot(normalized, assetsRoot))
    {
        return std::nullopt;
    }
    return normalized;
}

std::optional<std::filesystem::path> ResolveSourceMaterialPath(const AssetThumbnailRequest& request)
{
    if (auto resolved = ResolveSourceMaterialPath(request, request.artifactPath);
        resolved.has_value())
    {
        return resolved;
    }
    return ResolveSourceMaterialPath(request, request.sourceAssetPath);
}

std::vector<std::filesystem::path> ResolveMeshArtifactPaths(const AssetThumbnailRequest& request)
{
    std::vector<std::filesystem::path> paths;
    const auto manifest = LoadManifest(request);
    if (!manifest.has_value())
    {
        if (auto resolved = ResolveArtifactPath(request, request.artifactPath);
            resolved.has_value() && resolved->extension() == ".nmesh")
        {
            paths.push_back(*resolved);
        }
        return paths;
    }

    if (!request.subAssetKey.empty())
    {
        for (const auto& artifact : manifest->subAssets)
        {
            if (artifact.artifactType != NLS::Core::Assets::ArtifactType::Mesh ||
                artifact.subAssetKey != request.subAssetKey)
            {
                continue;
            }

            if (auto resolved = ResolveArtifactPath(request, artifact.artifactPath);
                resolved.has_value())
            {
                paths.push_back(*resolved);
            }
            return paths;
        }
    }

    for (const auto& artifact : manifest->subAssets)
    {
        if (artifact.artifactType != NLS::Core::Assets::ArtifactType::Mesh)
            continue;

        if (auto resolved = ResolveArtifactPath(request, artifact.artifactPath);
            resolved.has_value())
        {
            paths.push_back(*resolved);
        }
    }
    return paths;
}

std::vector<std::filesystem::path> ResolveMaterialArtifactPaths(const AssetThumbnailRequest& request)
{
    std::vector<std::filesystem::path> paths;
    const auto manifest = LoadManifest(request);
    if (!manifest.has_value())
    {
        if (auto resolved = ResolveArtifactPath(request, request.artifactPath);
            resolved.has_value() && resolved->extension() == ".nmat")
        {
            paths.push_back(*resolved);
        }
        else if (auto sourceMaterial = ResolveSourceMaterialPath(request);
            sourceMaterial.has_value())
        {
            paths.push_back(*sourceMaterial);
        }
        return paths;
    }

    if (!request.subAssetKey.empty())
    {
        for (const auto& artifact : manifest->subAssets)
        {
            if (artifact.artifactType != NLS::Core::Assets::ArtifactType::Material ||
                artifact.subAssetKey != request.subAssetKey)
            {
                continue;
            }

            if (auto resolved = ResolveArtifactPath(request, artifact.artifactPath);
                resolved.has_value())
            {
                paths.push_back(*resolved);
            }
            else if (auto sourceMaterial = ResolveSourceMaterialPath(request, artifact.artifactPath);
                sourceMaterial.has_value())
            {
                paths.push_back(*sourceMaterial);
            }
            return paths;
        }
    }

    for (const auto& artifact : manifest->subAssets)
    {
        if (artifact.artifactType != NLS::Core::Assets::ArtifactType::Material)
            continue;

        if (auto resolved = ResolveArtifactPath(request, artifact.artifactPath);
            resolved.has_value())
        {
            paths.push_back(*resolved);
        }
        else if (auto sourceMaterial = ResolveSourceMaterialPath(request, artifact.artifactPath);
            sourceMaterial.has_value())
        {
            paths.push_back(*sourceMaterial);
        }
    }
    if (paths.empty())
    {
        if (auto sourceMaterial = ResolveSourceMaterialPath(request);
            sourceMaterial.has_value())
        {
            paths.push_back(*sourceMaterial);
        }
    }
    return paths;
}

std::optional<std::filesystem::path> ResolvePrefabArtifactPath(const AssetThumbnailRequest& request)
{
    const auto manifest = LoadManifest(request);
    if (manifest.has_value())
    {
        if (!request.subAssetKey.empty())
        {
            for (const auto& artifact : manifest->subAssets)
            {
                if (artifact.artifactType == NLS::Core::Assets::ArtifactType::Prefab &&
                    artifact.subAssetKey == request.subAssetKey)
                {
                    return ResolveArtifactPath(request, artifact.artifactPath);
                }
            }
        }

        for (const auto& artifact : manifest->subAssets)
        {
            if (artifact.artifactType == NLS::Core::Assets::ArtifactType::Prefab)
                return ResolveArtifactPath(request, artifact.artifactPath);
        }
    }

    return ResolveArtifactPath(request, request.artifactPath);
}

uint64_t FileSizeOrMax(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return error ? std::numeric_limits<uint64_t>::max() : size;
}

bool NativeArtifactPayloadExceedsGpuPreviewBudget(
    const std::filesystem::path& path,
    const NLS::Core::Assets::ArtifactType type,
    const uint32_t schemaVersion)
{
    const auto prefix = NLS::Core::Assets::ReadNativeArtifactPayloadPrefixFromFile(
        path,
        type,
        schemaVersion,
        1u,
        kMaxGpuPreviewStructurePayloadBytes);
    if (prefix.has_value())
        return prefix->payloadSize > kMaxGpuPreviewStructurePayloadBytes;

    return FileSizeOrMax(path) > kMaxGpuPreviewStructurePayloadBytes;
}

bool MeshArtifactExceedsGpuPreviewBudget(const std::filesystem::path& path)
{
    if (FileSizeOrMax(path) > kMaxGpuPreviewNativeArtifactFileBytes)
        return true;

    const auto header = NLS::Render::Assets::ReadMeshArtifactHeaderPreview(
        path,
        kMaxGpuPreviewStructurePayloadBytes);
    return !header.has_value() ||
        header->vertexCount > kMaxGpuPreviewMeshVertices ||
        header->indexCount > kMaxGpuPreviewMeshIndices;
}

bool MaterialArtifactExceedsGpuPreviewBudget(const std::filesystem::path& path)
{
    return NativeArtifactPayloadExceedsGpuPreviewBudget(
        path,
        NLS::Core::Assets::ArtifactType::Material,
        1u);
}

bool PrefabArtifactExceedsGpuPreviewBudget(const std::filesystem::path& path)
{
    return NativeArtifactPayloadExceedsGpuPreviewBudget(
        path,
        NLS::Core::Assets::ArtifactType::Prefab,
        1u);
}

bool IsBuiltInPreviewResourcePath(const std::string& path)
{
    return !path.empty() && path.front() == ':';
}

std::optional<std::filesystem::path> ResolvePrefabPreviewMaterialBudgetPath(
    const AssetThumbnailRequest& request,
    const std::string& materialPath)
{
    if (materialPath.empty() || IsBuiltInPreviewResourcePath(materialPath))
        return std::nullopt;

    if (auto resolved = ResolveArtifactPath(request, materialPath);
        resolved.has_value())
    {
        return resolved;
    }
    return ResolveSourceMaterialPath(request, materialPath);
}

std::optional<std::filesystem::path> ResolvePrefabPreviewMeshBudgetPath(
    const AssetThumbnailRequest& request,
    const std::string& meshPath)
{
    if (meshPath.empty() || IsBuiltInPreviewResourcePath(meshPath))
        return std::nullopt;
    return ResolveArtifactPath(request, meshPath);
}

struct Bounds
{
    Maths::Vector3 min {
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()
    };
    Maths::Vector3 max {
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()
    };
    bool valid = false;
};

void IncludeBounds(Bounds& target, const Render::Geometry::Bounds& bounds)
{
    const auto halfSize = bounds.size * 0.5f;
    const auto min = bounds.center - halfSize;
    const auto max = bounds.center + halfSize;
    target.min.x = (std::min)(target.min.x, min.x);
    target.min.y = (std::min)(target.min.y, min.y);
    target.min.z = (std::min)(target.min.z, min.z);
    target.max.x = (std::max)(target.max.x, max.x);
    target.max.y = (std::max)(target.max.y, max.y);
    target.max.z = (std::max)(target.max.z, max.z);
    target.valid = true;
}

std::array<Maths::Vector3, 8> BoundsCorners(const Render::Geometry::Bounds& bounds)
{
    const auto halfSize = bounds.size * 0.5f;
    const auto min = bounds.center - halfSize;
    const auto max = bounds.center + halfSize;
    return {
        Maths::Vector3(min.x, min.y, min.z),
        Maths::Vector3(max.x, min.y, min.z),
        Maths::Vector3(min.x, max.y, min.z),
        Maths::Vector3(max.x, max.y, min.z),
        Maths::Vector3(min.x, min.y, max.z),
        Maths::Vector3(max.x, min.y, max.z),
        Maths::Vector3(min.x, max.y, max.z),
        Maths::Vector3(max.x, max.y, max.z)
    };
}

void IncludePoint(Bounds& target, const Maths::Vector3& point)
{
    target.min.x = (std::min)(target.min.x, point.x);
    target.min.y = (std::min)(target.min.y, point.y);
    target.min.z = (std::min)(target.min.z, point.z);
    target.max.x = (std::max)(target.max.x, point.x);
    target.max.y = (std::max)(target.max.y, point.y);
    target.max.z = (std::max)(target.max.z, point.z);
    target.valid = true;
}

void IncludeWorldBounds(Bounds& target, const Render::Geometry::Bounds& bounds, const Maths::Matrix4& worldMatrix)
{
    for (const auto& corner : BoundsCorners(bounds))
        IncludePoint(target, worldMatrix * Maths::Vector4(corner.x, corner.y, corner.z, 1.0f));
}

Maths::Vector3 PreviewDirection(const float yawDegrees, const float pitchDegrees)
{
    const auto yaw = yawDegrees * kDegreesToRadians;
    const auto pitch = pitchDegrees * kDegreesToRadians;
    const auto cy = std::cos(yaw);
    const auto sy = std::sin(yaw);
    const auto cp = std::cos(pitch);
    const auto sp = std::sin(pitch);
    return Maths::Vector3(
        sy * cp,
        sp,
        cy * cp).Normalised();
}

Maths::Vector3 PreviewDirection()
{
    return PreviewDirection(kUnityMeshPreviewYawDegrees, kUnityMeshPreviewPitchDegrees);
}
}

class EditorThumbnailPreviewRenderer::Impl
{
public:
    explicit Impl(NLS::Render::Context::Driver& driver)
        : m_driver(driver)
        , m_renderer(NLS::Engine::Rendering::CreateSceneRenderer(driver))
    {
        auto& cameraObject = m_scene.CreateEditorTransientGameObject("Thumbnail Preview Camera");
        m_camera = cameraObject.AddComponent<NLS::Engine::Components::CameraComponent>();
        m_camera->SetFov(kUnityMeshPreviewFieldOfViewDegrees);
        m_camera->SetClearColor({0.0f, 0.0f, 0.0f});
        m_camera->SetFrustumGeometryCulling(false);
        m_camera->SetFrustumLightCulling(false);

        auto& directional = m_scene.CreateEditorTransientGameObject("Thumbnail Preview Key Light");
        directional.GetTransform()->SetLocalRotation(
            Maths::Quaternion::LookAt({-0.35f, -1.0f, 0.25f}, Maths::Vector3::Up));
        auto* light = directional.AddComponent<NLS::Engine::Components::LightComponent>();
        light->SetLightType(NLS::Render::Settings::ELightType::DIRECTIONAL);
        light->SetIntensity(1.4f);

        auto& ambient = m_scene.CreateEditorTransientGameObject("Thumbnail Preview Ambient");
        auto* ambientLight = ambient.AddComponent<NLS::Engine::Components::LightComponent>();
        ambientLight->SetLightType(NLS::Render::Settings::ELightType::AMBIENT_SPHERE);
        ambientLight->SetRadius(10000.0f);
        ambientLight->SetIntensity(0.1f);

        m_framebuffer.SetOptimizedColorClearValue(0.0f, 0.0f, 0.0f, 0.0f);
    }

    bool Supports(const AssetThumbnailRequest& request) const
    {
        return request.kind == AssetThumbnailKind::MaterialSphere ||
            request.kind == AssetThumbnailKind::ModelPreview ||
            request.kind == AssetThumbnailKind::PrefabPreview;
    }

    EditorThumbnailPreviewResult Render(const AssetThumbnailRequest& request)
    {
        EditorThumbnailPreviewResult result;
        result.width = std::max(1u, request.requestedSize);
        result.height = result.width;
        if (!Supports(request))
        {
            result.diagnostic = "thumbnail-gpu-preview-kind-unsupported";
            return result;
        }
        if (m_renderer == nullptr || m_camera == nullptr || m_camera->GetCamera() == nullptr)
        {
            result.diagnostic = "thumbnail-gpu-preview-renderer-unavailable";
            return result;
        }
        if (!NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MeshManager>() ||
            !NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MaterialManager>())
        {
            result.diagnostic = "thumbnail-gpu-preview-resource-managers-unavailable";
            return result;
        }

        ClearPreviewObjects();

        if (request.kind == AssetThumbnailKind::MaterialSphere)
            return RenderMaterialSphere(request, result);

        if (request.kind == AssetThumbnailKind::PrefabPreview)
        {
            const auto prefabPath = ResolvePrefabArtifactPath(request);
            if (prefabPath.has_value() && PrefabArtifactExceedsGpuPreviewBudget(*prefabPath))
            {
                result.diagnostic = kGpuPreviewPrefabBudgetExceededDiagnostic;
                return result;
            }
            if (prefabPath.has_value() && RenderPrefabPreview(request, result))
                return result;
            ClearPreviewObjects();
        }

        const auto meshPaths = ResolveMeshArtifactPaths(request);
        if (meshPaths.empty())
        {
            result.diagnostic = "thumbnail-gpu-preview-mesh-artifact-missing";
            return result;
        }

        auto& meshManager = NLS_SERVICE(NLS::Core::ResourceManagement::MeshManager);
        std::vector<NLS::Render::Resources::Material*> materials;
        if (request.kind == AssetThumbnailKind::PrefabPreview)
        {
            auto& materialManager = NLS_SERVICE(NLS::Core::ResourceManagement::MaterialManager);
            for (const auto& materialPath : ResolveMaterialArtifactPaths(request))
            {
                if (MaterialArtifactExceedsGpuPreviewBudget(materialPath))
                {
                    result.diagnostic = kGpuPreviewMaterialBudgetExceededDiagnostic;
                    return result;
                }
                materials.push_back(materialManager.PrewarmArtifactWithDependencies(ToGenericPath(materialPath)));
            }
        }

        Bounds combinedBounds;
        for (const auto& meshPath : meshPaths)
        {
            if (MeshArtifactExceedsGpuPreviewBudget(meshPath))
            {
                result.diagnostic = kGpuPreviewMeshBudgetExceededDiagnostic;
                ClearPreviewObjects();
                return result;
            }
            auto* mesh = meshManager.PrewarmArtifact(ToGenericPath(meshPath));
            if (mesh == nullptr)
                continue;

            auto& object = m_scene.CreateEditorTransientGameObject("Thumbnail Preview Mesh");
            m_previewObjects.push_back(&object);
            auto* filter = object.AddComponent<NLS::Engine::Components::MeshFilter>();
            auto* renderer = object.AddComponent<NLS::Engine::Components::MeshRenderer>();
            filter->SetMesh(mesh);
            renderer->SetFrustumBehaviour(NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::DISABLED);
            if (materials.empty())
            {
                renderer->FillEmptySlotsWithMaterial(DefaultMaterial());
            }
            else
            {
                renderer->FillEmptySlotsWithMaterial(DefaultMaterial());
                for (size_t slot = 0u; slot < materials.size() && slot < NLS::Engine::Components::MeshRenderer::kMaxMaterialCount; ++slot)
                {
                    if (materials[slot] != nullptr)
                        renderer->SetMaterialAtIndex(static_cast<uint8_t>(slot), *materials[slot]);
                }
            }
            IncludeBounds(combinedBounds, mesh->GetBounds());
        }

        if (m_previewObjects.empty() || !combinedBounds.valid)
        {
            result.diagnostic = "thumbnail-gpu-preview-mesh-load-failed";
            ClearPreviewObjects();
            return result;
        }

        ConfigureCamera(combinedBounds, result.width, result.height);
        RenderCurrentPreviewScene(result);

        ClearPreviewObjects();
        return result;
    }

private:
    EditorThumbnailPreviewResult RenderMaterialSphere(
        const AssetThumbnailRequest& request,
        EditorThumbnailPreviewResult result)
    {
        const auto materialPaths = ResolveMaterialArtifactPaths(request);
        if (materialPaths.empty())
        {
            result.diagnostic = "thumbnail-gpu-preview-material-artifact-missing";
            return result;
        }
        if (MaterialArtifactExceedsGpuPreviewBudget(materialPaths.front()))
        {
            result.diagnostic = kGpuPreviewMaterialBudgetExceededDiagnostic;
            return result;
        }

        auto& materialManager = NLS_SERVICE(NLS::Core::ResourceManagement::MaterialManager);
        auto* material = materialManager.PrewarmArtifactWithDependencies(ToGenericPath(materialPaths.front()));
        if (material == nullptr)
        {
            result.diagnostic = "thumbnail-gpu-preview-material-load-failed";
            return result;
        }

        if (!EDITOR_CONTEXT(editorResources))
        {
            result.diagnostic = "thumbnail-gpu-preview-editor-resources-unavailable";
            return result;
        }

        auto* sphere = EDITOR_CONTEXT(editorResources)->GetMesh("Sphere");
        if (sphere == nullptr)
        {
            result.diagnostic = "thumbnail-gpu-preview-material-sphere-missing";
            return result;
        }

        auto& object = m_scene.CreateEditorTransientGameObject("Thumbnail Preview Material Sphere");
        m_previewObjects.push_back(&object);
        auto* filter = object.AddComponent<NLS::Engine::Components::MeshFilter>();
        auto* renderer = object.AddComponent<NLS::Engine::Components::MeshRenderer>();
        filter->SetMesh(sphere);
        renderer->SetFrustumBehaviour(NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::DISABLED);
        renderer->FillEmptySlotsWithMaterial(DefaultMaterial());
        renderer->SetMaterialAtIndex(0u, *material);

        ConfigureMaterialCamera(result.width, result.height);
        RenderCurrentPreviewScene(result);
        ClearPreviewObjects();
        return result;
    }

    bool RenderPrefabPreview(
        const AssetThumbnailRequest& request,
        EditorThumbnailPreviewResult& result)
    {
        if (request.assetId.IsValid() == false || request.subAssetKey.empty())
        {
            result.diagnostic = "thumbnail-gpu-preview-prefab-identity-missing";
            return false;
        }

        AssetDatabaseFacade database(MakeProjectEditorAssetRoots(request.projectRoot));
        auto prefab = database.LoadPrefabArtifactByAssetId(request.assetId, request.subAssetKey);
        if (!prefab.has_value())
        {
            result.diagnostic = "thumbnail-gpu-preview-prefab-load-failed";
            return false;
        }

        NLS::Engine::Serialize::LoadPolicy previewLoadPolicy;
        previewLoadPolicy.deferAssetReferenceResolution = true;
        previewLoadPolicy.suppressGameObjectCreatedEvents = true;
        auto instantiated = NLS::Engine::Assets::InstantiatePrefabArtifact(
            *prefab,
            m_scene,
            previewLoadPolicy);
        if (instantiated.diagnostics.HasErrors() || instantiated.root == nullptr)
        {
            result.diagnostic = "thumbnail-gpu-preview-prefab-instantiate-failed";
            return false;
        }

        instantiated.root->SetEditorTransient(true);
        m_previewObjects.push_back(instantiated.root);
        if (!ApplyPrefabPreviewResolvedMaterials(request, *prefab, instantiated, result))
        {
            ClearPreviewObjects();
            return false;
        }

        Bounds combinedBounds;
        if (!CollectRenderableBounds(request, *instantiated.root, combinedBounds, result))
        {
            ClearPreviewObjects();
            return false;
        }
        if (!combinedBounds.valid)
        {
            result.diagnostic = "thumbnail-gpu-preview-prefab-renderer-missing";
            ClearPreviewObjects();
            return false;
        }

        ConfigurePrefabCamera(combinedBounds, result.width, result.height);
        RenderCurrentPreviewScene(result);
        ClearPreviewObjects();
        return !result.rgbaPixels.empty();
    }

    bool ApplyPrefabPreviewResolvedMaterials(
        const AssetThumbnailRequest& request,
        const NLS::Engine::Assets::PrefabArtifact& prefab,
        const NLS::Engine::Assets::PrefabArtifactInstantiationResult& instantiated,
        EditorThumbnailPreviewResult& result)
    {
        if (prefab.resolvedAssets.empty() || instantiated.sourceToInstance.empty())
            return true;

        std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::GameObject*> instancesBySource;
        instancesBySource.reserve(instantiated.sourceByInstanceObject.size());
        for (const auto& [instance, source] : instantiated.sourceByInstanceObject)
        {
            if (instance != nullptr)
                instancesBySource.emplace(source, instance);
        }

        if (instancesBySource.empty())
            return true;

        auto& materialManager = NLS_SERVICE(NLS::Core::ResourceManagement::MaterialManager);
        const auto templates = BuildImportedPrefabRendererDependencyTemplates(prefab);
        for (const auto& item : templates)
        {
            if (item.materialPaths.empty())
                continue;

            const auto found = instancesBySource.find(item.sourceObject);
            if (found == instancesBySource.end() || found->second == nullptr)
                continue;

            auto* meshRenderer = found->second->GetComponent<NLS::Engine::Components::MeshRenderer>();
            if (meshRenderer == nullptr)
                continue;

            NLS::Array<std::string> materialPaths;
            materialPaths.reserve(item.materialPaths.size());
            std::vector<NLS::Render::Resources::Material*> resolvedMaterials;
            resolvedMaterials.reserve(item.materialPaths.size());
            for (const auto& materialPath : item.materialPaths)
            {
                auto genericPath = ToGenericPath(materialPath);
                materialPaths.push_back(genericPath);
                if (genericPath.empty())
                {
                    resolvedMaterials.push_back(nullptr);
                    continue;
                }

                if (auto resolvedBudgetPath = ResolvePrefabPreviewMaterialBudgetPath(request, genericPath);
                    resolvedBudgetPath.has_value() && MaterialArtifactExceedsGpuPreviewBudget(*resolvedBudgetPath))
                {
                    result.diagnostic = kGpuPreviewMaterialBudgetExceededDiagnostic;
                    return false;
                }

                resolvedMaterials.push_back(materialManager.PrewarmArtifactWithDependencies(genericPath));
            }
            meshRenderer->SetMaterialPathHints(materialPaths);
            for (size_t slot = 0u;
                slot < resolvedMaterials.size() &&
                slot < NLS::Engine::Components::MeshRenderer::kMaxMaterialCount;
                ++slot)
            {
                if (resolvedMaterials[slot] != nullptr)
                    meshRenderer->SetMaterialAtIndex(static_cast<uint8_t>(slot), *resolvedMaterials[slot]);
            }
        }
        return true;
    }

    void RenderCurrentPreviewScene(EditorThumbnailPreviewResult& result)
    {
        m_framebuffer.Resize(
            static_cast<uint16_t>(result.width),
            static_cast<uint16_t>(result.height),
            true);

        m_renderer->AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
            m_scene,
            std::nullopt,
            nullptr,
            {},
            false
        });

        NLS::Render::Data::FrameDescriptor frameDescriptor;
        frameDescriptor.renderWidth = static_cast<uint16_t>(result.width);
        frameDescriptor.renderHeight = static_cast<uint16_t>(result.height);
        frameDescriptor.camera = m_camera->GetCamera();
        NLS::Render::FrameGraph::SetExternalSceneOutputFramebuffer(frameDescriptor, &m_framebuffer);

        m_renderer->BeginFrame(frameDescriptor);
        m_renderer->DrawFrame();
        m_renderer->EndFrame();

        if (NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_driver))
            (void)NLS::Render::Context::DriverRendererAccess::TryDrainThreadedRendering(m_driver, false);

        auto readbackTexture = m_framebuffer.GetExplicitTextureHandle();
        if (readbackTexture == nullptr)
            readbackTexture = NLS::Render::Context::DriverRendererAccess::ResolveReadbackTexture(m_driver);
        if (readbackTexture == nullptr)
        {
            result.diagnostic = "thumbnail-gpu-preview-readback-texture-unavailable";
            return;
        }

        result.rgbaPixels.assign(static_cast<size_t>(result.width) * result.height * 4u, 0u);
        const auto readback = NLS::Render::Context::DriverRendererAccess::ReadPixelsChecked(
            m_driver,
            readbackTexture,
            0u,
            0u,
            result.width,
            result.height,
            NLS::Render::Settings::EPixelDataFormat::RGBA,
            NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
            result.rgbaPixels.data());
        if (!readback.Succeeded())
        {
            result.rgbaPixels.clear();
            result.diagnostic = "thumbnail-gpu-preview-readback-failed:" + readback.message;
        }
    }

    void ClearPreviewObjects()
    {
        for (auto* object : m_previewObjects)
        {
            if (object != nullptr)
                (void)m_scene.DestroyGameObject(*object);
        }
        m_previewObjects.clear();
        m_scene.CollectGarbages();
    }

    bool CollectRenderableBounds(
        const AssetThumbnailRequest& request,
        NLS::Engine::GameObject& object,
        Bounds& bounds,
        EditorThumbnailPreviewResult& result)
    {
        if (!object.IsActive())
            return true;

        auto* meshFilter = object.GetComponent<NLS::Engine::Components::MeshFilter>();
        auto* meshRenderer = object.GetComponent<NLS::Engine::Components::MeshRenderer>();
        if (meshFilter != nullptr && meshRenderer != nullptr)
        {
            auto* mesh = meshFilter->ResolveMesh();
            if (mesh == nullptr)
            {
                const auto meshPath = meshFilter->GetModelPath();
                if (!meshPath.empty() &&
                    NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MeshManager>())
                {
                    if (auto resolvedBudgetPath = ResolvePrefabPreviewMeshBudgetPath(request, meshPath);
                        resolvedBudgetPath.has_value() && MeshArtifactExceedsGpuPreviewBudget(*resolvedBudgetPath))
                    {
                        result.diagnostic = kGpuPreviewMeshBudgetExceededDiagnostic;
                        return false;
                    }

                    mesh = NLS_SERVICE(NLS::Core::ResourceManagement::MeshManager).PrewarmArtifact(meshPath);
                    meshFilter->SetResolvedMeshFromReference(mesh);
                }
            }

            meshRenderer->SetFrustumBehaviour(NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::DISABLED);
            (void)meshRenderer->ResolveMaterials();
            if (mesh != nullptr)
                IncludeWorldBounds(bounds, mesh->GetBounds(), object.GetTransform()->GetWorldMatrix());
        }

        for (auto* child : object.GetChildren())
        {
            if (child != nullptr && !CollectRenderableBounds(request, *child, bounds, result))
                return false;
        }
        return true;
    }

    NLS::Render::Resources::Material& DefaultMaterial()
    {
        if (!m_defaultMaterial.HasShader() &&
            NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::ShaderManager>())
        {
            m_defaultMaterial.SetShader(
                NLS_SERVICE(NLS::Core::ResourceManagement::ShaderManager).GetResource(":Shaders/Standard.hlsl"));
            m_defaultMaterial.Set("u_Albedo", Maths::Vector4(0.72f, 0.74f, 0.78f, 1.0f));
            m_defaultMaterial.Set("u_Metallic", 0.0f);
            m_defaultMaterial.Set("u_Roughness", 0.72f);
        }
        return m_defaultMaterial;
    }

    void ConfigureCamera(const Bounds& bounds, const uint32_t width, const uint32_t height)
    {
        const Maths::Vector3 center = (bounds.min + bounds.max) * 0.5f;
        const Maths::Vector3 extents = (bounds.max - bounds.min) * 0.5f;
        const auto halfSize = (std::max)(0.001f, extents.Length());
        const auto fovRadians = kUnityMeshPreviewFieldOfViewDegrees * kDegreesToRadians;
        const auto distance = (halfSize / std::tan(fovRadians * 0.5f)) * 1.6f;
        const auto direction = PreviewDirection();
        auto* owner = m_camera->gameobject();
        if (owner == nullptr)
            return;
        auto* transform = owner->GetTransform();
        transform->SetLocalPosition(center - direction * distance);
        transform->SetLocalRotation(Maths::Quaternion::LookAt(direction, Maths::Vector3::Up));
        m_camera->SetFov(kUnityMeshPreviewFieldOfViewDegrees);
        m_camera->SetNear((std::max)(0.001f, distance - halfSize * 3.0f));
        m_camera->SetFar(distance + halfSize * 4.0f);
        m_camera->GetCamera()->CacheMatrices(
            static_cast<uint16_t>(width),
            static_cast<uint16_t>(height));
    }

    void ConfigurePrefabCamera(const Bounds& bounds, const uint32_t width, const uint32_t height)
    {
        const Maths::Vector3 center = (bounds.min + bounds.max) * 0.5f;
        const auto direction = PreviewDirection(kUnityPrefabPreviewYawDegrees, kUnityPrefabPreviewPitchDegrees);
        const auto rotation = Maths::Quaternion::LookAt(direction, Maths::Vector3::Up);
        const Maths::Vector3 extents = (bounds.max - bounds.min) * 0.5f;
        const auto radius = (std::max)(0.001f, extents.Length());
        const auto fovRadians = kUnityMeshPreviewFieldOfViewDegrees * kDegreesToRadians;
        const auto aspect = height == 0u ? 1.0f : static_cast<float>(width) / static_cast<float>(height);
        const auto halfVerticalFov = fovRadians * 0.5f;
        const auto halfHorizontalFov = std::atan(std::tan(halfVerticalFov) * (std::max)(0.001f, aspect));
        const auto distanceVertical = radius / std::tan(halfVerticalFov);
        const auto distanceHorizontal = radius / std::tan(halfHorizontalFov);
        const auto distance = (std::max)(distanceVertical, distanceHorizontal) * 1.18f;
        auto* owner = m_camera->gameobject();
        if (owner == nullptr)
            return;
        auto* transform = owner->GetTransform();
        transform->SetLocalPosition(center - direction * distance);
        transform->SetLocalRotation(rotation);
        m_camera->SetFov(kUnityMeshPreviewFieldOfViewDegrees);
        m_camera->SetNear((std::max)(0.001f, distance - radius * 3.0f));
        m_camera->SetFar(distance + radius * 5.0f);
        m_camera->GetCamera()->CacheMatrices(
            static_cast<uint16_t>(width),
            static_cast<uint16_t>(height));
    }

    void ConfigureMaterialCamera(const uint32_t width, const uint32_t height)
    {
        auto* owner = m_camera->gameobject();
        if (owner == nullptr)
            return;

        auto* transform = owner->GetTransform();
        transform->SetLocalPosition({0.0f, 0.0f, -kUnityMaterialPreviewDistance});
        transform->SetLocalRotation(Maths::Quaternion::Identity);
        m_camera->SetFov(kUnityMeshPreviewFieldOfViewDegrees);
        m_camera->SetNear(0.1f);
        m_camera->SetFar(10.0f);
        m_camera->GetCamera()->CacheMatrices(
            static_cast<uint16_t>(width),
            static_cast<uint16_t>(height));
    }

    NLS::Render::Context::Driver& m_driver;
    std::unique_ptr<NLS::Engine::Rendering::BaseSceneRenderer> m_renderer;
    NLS::Engine::SceneSystem::Scene m_scene;
    NLS::Engine::Components::CameraComponent* m_camera = nullptr;
    NLS::Render::Buffers::Framebuffer m_framebuffer;
    NLS::Render::Resources::Material m_defaultMaterial;
    std::vector<NLS::Engine::GameObject*> m_previewObjects;
};

EditorThumbnailPreviewRenderer::EditorThumbnailPreviewRenderer(NLS::Render::Context::Driver& driver)
    : m_impl(std::make_unique<Impl>(driver))
{
}

EditorThumbnailPreviewRenderer::~EditorThumbnailPreviewRenderer() = default;

bool EditorThumbnailPreviewRenderer::Supports(const AssetThumbnailRequest& request) const
{
    return m_impl != nullptr && m_impl->Supports(request);
}

EditorThumbnailPreviewResult EditorThumbnailPreviewRenderer::Render(const AssetThumbnailRequest& request)
{
    return m_impl != nullptr ? m_impl->Render(request) : EditorThumbnailPreviewResult {};
}
}
