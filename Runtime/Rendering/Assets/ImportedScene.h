#pragma once

#include "Assets/ArtifactManifest.h"
#include "RenderDef.h"
#include <Animation/ImportSettings.h>

#include <cstdint>
#include <string>
#include <vector>

namespace NLS::Render::Assets
{
enum class ImportedSceneSubAssetType
{
    Mesh,
    Material,
    Texture,
    Skeleton,
    Skin,
    AnimationClip,
    MorphTarget,
    Model,
    Prefab,
    SkeletalMesh
};

struct ImportedSceneVertexStream
{
    std::string semantic;
    std::string accessorKey;
};

struct ImportedScenePrimitive
{
    std::vector<ImportedSceneVertexStream> vertexStreams;
    std::string indexAccessorKey;
    std::string materialKey;
};

struct ImportedSceneMaterialChannel
{
    std::string name;
    std::string textureKey;
    std::vector<double> values;
    bool hasScalar = false;
    double scalar = 0.0;
};

struct ImportedSceneTextureSampler
{
    int wrapS = 10497;
    int wrapT = 10497;
    int minFilter = 9729;
    int magFilter = 9729;
};

struct ImportedSceneNamedRecord
{
    std::string sourceKey;
    std::string name;
    std::string uri;
    std::string mimeType;
    std::string meshKey;
    std::string skinKey;
    std::string skeletonKey;
    std::string pbrWorkflow;
    std::string baseColorTextureKey;
    std::string metallicRoughnessTextureKey;
    std::string normalTextureKey;
    std::string occlusionTextureKey;
    std::string emissiveTextureKey;
    std::string bufferViewKey;
    // Embedded image payloads keep their encoded bytes (PNG/JPEG) when the
    // importer can provide them. Raw texel data is normalized to RGBA8.
    std::vector<uint8_t> embeddedBytes;
    std::string embeddedFormat;
    uint32_t embeddedWidth = 0u;
    uint32_t embeddedHeight = 0u;
    bool embeddedCompressed = false;
    uint32_t primitiveCount = 0u;
    uint32_t morphTargetCount = 0u;
    bool embedded = false;
    bool doubleSided = false;
    std::string alphaMode = "OPAQUE";
    double alphaCutoff = 0.5;
    double metallicFactor = 1.0;
    double roughnessFactor = 1.0;
    double normalScale = 1.0;
    double occlusionStrength = 1.0;
    std::vector<double> baseColorFactor;
    std::vector<double> emissiveFactor;
    ImportedSceneTextureSampler sampler;
    std::vector<std::string> attributes;
    std::vector<std::string> joints;
    std::vector<std::string> targets;
    std::vector<ImportedScenePrimitive> primitives;
    std::vector<ImportedSceneMaterialChannel> materialChannels;
};

struct ImportedSceneNode
{
    std::string sourceKey;
    std::string name;
    std::string parentKey;
    std::string meshKey;
    std::string skinKey;
    std::vector<double> translation;
    std::vector<double> rotation;
    std::vector<double> scale;
};

struct ImportedSceneBuffer
{
    std::string sourceKey;
    std::string uri;
    uint64_t byteLength = 0u;
    uint64_t embeddedByteLength = 0u;
    bool embedded = false;
};

struct ImportedSceneBufferView
{
    std::string sourceKey;
    std::string bufferKey;
    uint64_t byteOffset = 0u;
    uint64_t byteLength = 0u;
    uint64_t byteStride = 0u;
    uint32_t target = 0u;
};

struct ImportedSceneAccessor
{
    std::string sourceKey;
    std::string bufferViewKey;
    uint64_t byteOffset = 0u;
    uint32_t componentType = 0u;
    uint32_t count = 0u;
    std::string type;
};

struct ImportedSceneDiagnostic
{
    std::string code;
    std::string message;
};

struct SceneImportSettings
{
    double globalScale = 1.0;
    std::string axisConversion;
    std::string unitConversion;
    std::string hierarchyPolicy = "preserve";
    bool importNormals = true;
    bool importTangents = true;
    bool importUvs = true;
    bool importMaterials = true;
    bool importSkeleton = true;
    bool importAnimations = true;
    bool importMorphTargets = true;
    bool importCameras = false;
    bool importLights = false;
    bool ignoreFbxTexturedNeutralDiffuseTint = true;
    std::string lodGroup = "None";
    bool importMeshLODs = false;
    uint32_t minLOD = 0u;
    bool autoComputeLODScreenSize = true;
    Animation::FbxReaderSelection fbxReaderSelection = Animation::FbxReaderSelection::AutodeskWithAssimpFallback;
    double animationSampleRate = 0;
    std::vector<std::string> animationTakes;
    std::string animationTargetSkeleton;
};

struct ImportedScene
{
    NLS::Core::Assets::AssetId sourceAssetId;
    std::string sceneKey;
    SceneImportSettings importSettings;
    std::vector<ImportedSceneDiagnostic> diagnostics;
    std::vector<NLS::Core::Assets::AssetDependencyRecord> dependencies;
    std::vector<ImportedSceneBuffer> buffers;
    std::vector<ImportedSceneBufferView> bufferViews;
    std::vector<ImportedSceneAccessor> accessors;
    std::vector<ImportedSceneNode> nodes;
    std::vector<ImportedSceneNamedRecord> meshes;
    std::vector<ImportedSceneNamedRecord> materials;
    std::vector<ImportedSceneNamedRecord> textures;
    std::vector<ImportedSceneNamedRecord> skeletons;
    std::vector<ImportedSceneNamedRecord> skins;
    std::vector<ImportedSceneNamedRecord> animations;
    std::vector<ImportedSceneNamedRecord> morphTargets;
    std::shared_ptr<Animation::Model> animationData;
};

struct GeneratedSceneSubAsset
{
    ImportedSceneSubAssetType type = ImportedSceneSubAssetType::Model;
    std::string key;
    std::string sourceKey;
    std::string displayName;
};
}
