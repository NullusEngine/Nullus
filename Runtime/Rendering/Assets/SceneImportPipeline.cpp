#include "Rendering/Assets/SceneImportPipeline.h"

#include "Assets/AssetMeta.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Resources/Parsers/EModelParserFlags.h"
#include "Rendering/Resources/Parsers/IModelParser.h"

#include <Json/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

namespace
{
using ImportedSceneJson = nlohmann::json;

constexpr uint32_t kGlbMagic = 0x46546C67u;
constexpr uint32_t kGlbVersion = 2u;
constexpr uint32_t kGlbJsonChunkType = 0x4E4F534Au;
constexpr uint32_t kGlbBinChunkType = 0x004E4942u;

std::string ToLower(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

bool IsEmbeddedUri(const std::string& uri)
{
    return uri.rfind("data:", 0u) == 0u;
}

std::string IndexedKey(const char* prefix, const size_t index)
{
    return std::string(prefix) + "/" + std::to_string(index);
}

uint32_t ReadLittleEndianU32(const std::vector<uint8_t>& bytes, const size_t offset)
{
    if (offset + 4u > bytes.size())
        return 0u;

    return static_cast<uint32_t>(bytes[offset]) |
        (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
        (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
        (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
}

std::string StableSceneKey(const NLS::Render::Assets::ImportedScene& scene)
{
    if (!scene.sceneKey.empty())
        return scene.sceneKey;
    if (scene.sourceAssetId.IsValid())
        return scene.sourceAssetId.ToString();
    return "scene";
}

const ImportedSceneJson* FindArray(const ImportedSceneJson& object, const char* key)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->is_array())
        return nullptr;
    return &*found;
}

const ImportedSceneJson* FindObject(const ImportedSceneJson& object, const char* key)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->is_object())
        return nullptr;
    return &*found;
}

uint32_t ToUint(const size_t value)
{
    return static_cast<uint32_t>(value);
}

std::string ImageKeyFromTextureIndex(const ImportedSceneJson& root, const int textureIndex)
{
    if (textureIndex < 0)
        return {};

    const auto* textures = FindArray(root, "textures");
    if (!textures || static_cast<size_t>(textureIndex) >= textures->size())
        return {};

    const auto& texture = (*textures)[textureIndex];
    if (!texture.is_object())
        return {};

    const auto imageIndex = texture.value("source", -1);
    if (imageIndex < 0)
        return {};

    return IndexedKey("image", static_cast<size_t>(imageIndex));
}

std::string MaterialTextureKey(
    const ImportedSceneJson& root,
    const ImportedSceneJson& material,
    const char* propertyName)
{
    const auto* textureInfo = FindObject(material, propertyName);
    if (!textureInfo)
        return {};
    return ImageKeyFromTextureIndex(root, textureInfo->value("index", -1));
}

std::vector<double> ReadNumberArray(
    const ImportedSceneJson& object,
    const char* key,
    std::vector<double> fallback)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->is_array())
        return fallback;

    std::vector<double> values;
    values.reserve(found->size());
    for (const auto& value : *found)
    {
        if (value.is_number())
            values.push_back(value.get<double>());
    }
    return values.empty() ? std::move(fallback) : values;
}

NLS::Render::Assets::ImportedSceneTextureSampler ReadTextureSampler(
    const ImportedSceneJson& root,
    const ImportedSceneJson& material,
    const char* propertyName)
{
    NLS::Render::Assets::ImportedSceneTextureSampler sampler;
    const auto* textureInfo = FindObject(material, propertyName);
    if (!textureInfo)
        return sampler;

    const auto textureIndex = textureInfo->value("index", -1);
    if (textureIndex < 0)
        return sampler;

    const auto* textures = FindArray(root, "textures");
    if (!textures || static_cast<size_t>(textureIndex) >= textures->size())
        return sampler;

    const auto& texture = (*textures)[textureIndex];
    if (!texture.is_object())
        return sampler;

    const auto samplerIndex = texture.value("sampler", -1);
    if (samplerIndex < 0)
        return sampler;

    const auto* samplers = FindArray(root, "samplers");
    if (!samplers || static_cast<size_t>(samplerIndex) >= samplers->size())
        return sampler;

    const auto& source = (*samplers)[samplerIndex];
    if (!source.is_object())
        return sampler;

    sampler.wrapS = source.value("wrapS", sampler.wrapS);
    sampler.wrapT = source.value("wrapT", sampler.wrapT);
    sampler.minFilter = source.value("minFilter", sampler.minFilter);
    sampler.magFilter = source.value("magFilter", sampler.magFilter);
    return sampler;
}

double ReadTextureInfoScalar(
    const ImportedSceneJson& material,
    const char* propertyName,
    const char* scalarName,
    const double fallback)
{
    const auto* textureInfo = FindObject(material, propertyName);
    if (!textureInfo)
        return fallback;
    return textureInfo->value(scalarName, fallback);
}

std::string BuildSubAssetKey(
    const NLS::Render::Assets::ImportedSceneSubAssetType type,
    const std::string& sourceKey)
{
    return std::string(NLS::Render::Assets::ToSubAssetPrefix(type)) + ":" + sourceKey;
}

void AddSubAssets(
    std::vector<NLS::Render::Assets::GeneratedSceneSubAsset>& outRecords,
    const NLS::Render::Assets::ImportedSceneSubAssetType type,
    const std::vector<NLS::Render::Assets::ImportedSceneNamedRecord>& sourceRecords)
{
    for (const auto& sourceRecord : sourceRecords)
    {
        const auto sourceKey = sourceRecord.sourceKey.empty() ? sourceRecord.name : sourceRecord.sourceKey;
        if (sourceKey.empty())
            continue;

        outRecords.push_back({
            type,
            BuildSubAssetKey(type, sourceKey),
            sourceKey,
            sourceRecord.name
        });
    }
}

void AddSubAsset(
    std::vector<NLS::Render::Assets::GeneratedSceneSubAsset>& outRecords,
    const NLS::Render::Assets::ImportedSceneSubAssetType type,
    const std::string& sourceKey,
    std::string name)
{
    if (sourceKey.empty())
        return;

    outRecords.push_back({
        type,
        BuildSubAssetKey(type, sourceKey),
        sourceKey,
        std::move(name)
    });
}

void AddMeshSubAssets(
    std::vector<NLS::Render::Assets::GeneratedSceneSubAsset>& outRecords,
    const std::vector<NLS::Render::Assets::ImportedSceneNamedRecord>& meshes)
{
    for (const auto& mesh : meshes)
    {
        const auto sourceKey = mesh.sourceKey.empty() ? mesh.name : mesh.sourceKey;
        if (sourceKey.empty())
            continue;

        if (mesh.primitives.size() <= 1u)
        {
            AddSubAsset(outRecords, NLS::Render::Assets::ImportedSceneSubAssetType::Mesh, sourceKey, mesh.name);
            continue;
        }

        for (size_t index = 0u; index < mesh.primitives.size(); ++index)
        {
            AddSubAsset(
                outRecords,
                NLS::Render::Assets::ImportedSceneSubAssetType::Mesh,
                NLS::Render::Assets::BuildPrimitiveMeshSourceKey(sourceKey, index),
                mesh.name + " Primitive " + std::to_string(index));
        }
    }
}

void AddParserLimitDiagnostic(
    NLS::Render::Assets::ImportedScene& scene,
    const NLS::Render::Assets::SceneModelSourceFormat sourceFormat,
    const bool hasDetailedParserData)
{
    if (sourceFormat == NLS::Render::Assets::SceneModelSourceFormat::Obj)
    {
        scene.diagnostics.push_back({
            "obj-no-skeleton-animation-support",
            "OBJ does not define native skeleton, skin, animation, or morph data."
        });
        return;
    }

    if (sourceFormat == NLS::Render::Assets::SceneModelSourceFormat::Fbx && !hasDetailedParserData)
    {
        scene.diagnostics.push_back({
            "fbx-parser-limited-scene-data",
            "The selected FBX parser path currently exposes mesh and material data only."
        });
    }
}

NLS::Render::Assets::ImportedScene BuildFallbackParserScene(
    const std::vector<NLS::Render::Resources::Parsers::ParsedMeshData>& meshes,
    const std::vector<std::string>& materialNames,
    const NLS::Render::Assets::SceneModelSourceFormat sourceFormat,
    const NLS::Core::Assets::AssetId sourceAssetId,
    std::string sceneKey)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = sourceAssetId;
    scene.sceneKey = std::move(sceneKey);

    for (size_t index = 0u; index < meshes.size(); ++index)
    {
        NLS::Render::Assets::ImportedSceneNamedRecord record;
        if (!meshes[index].sourceKey.empty())
        {
            record.sourceKey = meshes[index].sourceKey;
        }
        else
        {
            const auto sourceMeshIndex = meshes[index].sourceMeshIndex == std::numeric_limits<uint32_t>::max()
                ? index
                : static_cast<size_t>(meshes[index].sourceMeshIndex);
            record.sourceKey = IndexedKey("parser/mesh", sourceMeshIndex);
        }
        record.name = "Mesh " + std::to_string(index);
        record.primitiveCount = 1u;
        NLS::Render::Assets::ImportedScenePrimitive primitive;
        const auto materialIndex = meshes[index].materialIndex;
        if (materialIndex < materialNames.size())
            primitive.materialKey = IndexedKey("parser/material", materialIndex);
        record.primitives.push_back(std::move(primitive));
        const auto meshName = record.name;
        const auto meshKey = record.sourceKey;
        scene.meshes.push_back(std::move(record));

        scene.nodes.push_back({
            IndexedKey("parser/node", index),
            meshName,
            {},
            meshKey,
            {}
        });
    }

    for (size_t index = 0u; index < materialNames.size(); ++index)
    {
        NLS::Render::Assets::ImportedSceneNamedRecord record;
        record.sourceKey = IndexedKey("parser/material", index);
        record.name = materialNames[index];
        scene.materials.push_back(std::move(record));
    }

    AddParserLimitDiagnostic(scene, sourceFormat, false);
    return scene;
}

void ImportGltfBufferViews(const ImportedSceneJson& root, NLS::Render::Assets::ImportedScene& scene)
{
    const auto* bufferViews = FindArray(root, "bufferViews");
    if (!bufferViews)
        return;

    for (size_t index = 0u; index < bufferViews->size(); ++index)
    {
        const auto& bufferView = (*bufferViews)[index];
        if (!bufferView.is_object())
            continue;

        NLS::Render::Assets::ImportedSceneBufferView record;
        record.sourceKey = IndexedKey("bufferView", index);
        record.bufferKey = IndexedKey("buffer", static_cast<size_t>(bufferView.value("buffer", 0)));
        record.byteOffset = bufferView.value("byteOffset", 0u);
        record.byteLength = bufferView.value("byteLength", 0u);
        record.byteStride = bufferView.value("byteStride", 0u);
        record.target = bufferView.value("target", 0u);
        scene.bufferViews.push_back(std::move(record));
    }
}

void ImportGltfAccessors(const ImportedSceneJson& root, NLS::Render::Assets::ImportedScene& scene)
{
    const auto* accessors = FindArray(root, "accessors");
    if (!accessors)
        return;

    for (size_t index = 0u; index < accessors->size(); ++index)
    {
        const auto& accessor = (*accessors)[index];
        if (!accessor.is_object())
            continue;

        NLS::Render::Assets::ImportedSceneAccessor record;
        record.sourceKey = IndexedKey("accessor", index);
        const auto bufferViewIndex = accessor.value("bufferView", -1);
        if (bufferViewIndex >= 0)
            record.bufferViewKey = IndexedKey("bufferView", static_cast<size_t>(bufferViewIndex));
        record.byteOffset = accessor.value("byteOffset", 0u);
        record.componentType = accessor.value("componentType", 0u);
        record.count = accessor.value("count", 0u);
        record.type = accessor.value("type", std::string {});
        scene.accessors.push_back(std::move(record));
    }
}

void PopulateGltfSceneFromJson(
    const ImportedSceneJson& root,
    NLS::Render::Assets::ImportedScene& scene,
    const uint64_t embeddedBufferLength)
{
    if (const auto* buffers = FindArray(root, "buffers"))
    {
        for (size_t index = 0u; index < buffers->size(); ++index)
        {
            const auto& buffer = (*buffers)[index];
            if (!buffer.is_object())
                continue;

            NLS::Render::Assets::ImportedSceneBuffer record;
            record.sourceKey = IndexedKey("buffer", index);
            record.uri = buffer.value("uri", std::string {});
            record.byteLength = buffer.value("byteLength", 0u);
            record.embedded = IsEmbeddedUri(record.uri) || (index == 0u && embeddedBufferLength > 0u);
            record.embeddedByteLength = record.embedded && index == 0u ? embeddedBufferLength : 0u;
            scene.buffers.push_back(std::move(record));
        }
    }

    ImportGltfBufferViews(root, scene);
    ImportGltfAccessors(root, scene);

    if (const auto* images = FindArray(root, "images"))
    {
        for (size_t index = 0u; index < images->size(); ++index)
        {
            const auto& image = (*images)[index];
            if (!image.is_object())
                continue;

            NLS::Render::Assets::ImportedSceneNamedRecord record;
            record.sourceKey = IndexedKey("image", index);
            record.name = image.value("name", record.sourceKey);
            record.uri = image.value("uri", std::string {});
            record.mimeType = image.value("mimeType", std::string {});
            const auto bufferViewIndex = image.value("bufferView", -1);
            if (bufferViewIndex >= 0)
                record.bufferViewKey = IndexedKey("bufferView", static_cast<size_t>(bufferViewIndex));
            record.embedded = bufferViewIndex >= 0 || IsEmbeddedUri(record.uri);
            scene.textures.push_back(std::move(record));
        }
    }

    if (const auto* materials = FindArray(root, "materials"))
    {
        for (size_t index = 0u; index < materials->size(); ++index)
        {
            const auto& material = (*materials)[index];
            if (!material.is_object())
                continue;

            NLS::Render::Assets::ImportedSceneNamedRecord record;
            record.sourceKey = IndexedKey("material", index);
            record.name = material.value("name", record.sourceKey);
            record.doubleSided = material.value("doubleSided", false);
            record.alphaMode = material.value("alphaMode", std::string("OPAQUE"));
            record.alphaCutoff = material.value("alphaCutoff", 0.5);
            record.emissiveFactor = ReadNumberArray(material, "emissiveFactor", {0.0, 0.0, 0.0});
            record.occlusionTextureKey = MaterialTextureKey(root, material, "occlusionTexture");
            record.emissiveTextureKey = MaterialTextureKey(root, material, "emissiveTexture");
            record.normalScale = ReadTextureInfoScalar(material, "normalTexture", "scale", 1.0);
            record.occlusionStrength = ReadTextureInfoScalar(material, "occlusionTexture", "strength", 1.0);
            record.sampler = ReadTextureSampler(root, material, "normalTexture");

            if (const auto* pbr = FindObject(material, "pbrMetallicRoughness"))
            {
                record.pbrWorkflow = "metallic-roughness";
                record.baseColorTextureKey = MaterialTextureKey(root, *pbr, "baseColorTexture");
                record.metallicRoughnessTextureKey = MaterialTextureKey(root, *pbr, "metallicRoughnessTexture");
                record.baseColorFactor = ReadNumberArray(*pbr, "baseColorFactor", {1.0, 1.0, 1.0, 1.0});
                record.metallicFactor = pbr->value("metallicFactor", 1.0);
                record.roughnessFactor = pbr->value("roughnessFactor", 1.0);
                record.sampler = ReadTextureSampler(root, *pbr, "baseColorTexture");
            }
            record.normalTextureKey = MaterialTextureKey(root, material, "normalTexture");
            scene.materials.push_back(std::move(record));
        }
    }

    if (const auto* meshes = FindArray(root, "meshes"))
    {
        for (size_t index = 0u; index < meshes->size(); ++index)
        {
            const auto& mesh = (*meshes)[index];
            if (!mesh.is_object())
                continue;

            NLS::Render::Assets::ImportedSceneNamedRecord record;
            record.sourceKey = IndexedKey("mesh", index);
            record.name = mesh.value("name", record.sourceKey);
            if (const auto* primitives = FindArray(mesh, "primitives"))
            {
                record.primitiveCount = ToUint(primitives->size());
                for (const auto& primitive : *primitives)
                {
                    if (!primitive.is_object())
                        continue;

                    NLS::Render::Assets::ImportedScenePrimitive primitiveRecord;
                    const auto indexAccessor = primitive.value("indices", -1);
                    if (indexAccessor >= 0)
                        primitiveRecord.indexAccessorKey = IndexedKey("accessor", static_cast<size_t>(indexAccessor));
                    const auto materialIndex = primitive.value("material", -1);
                    if (materialIndex >= 0)
                        primitiveRecord.materialKey = IndexedKey("material", static_cast<size_t>(materialIndex));

                    if (const auto* attributes = FindObject(primitive, "attributes"))
                    {
                        for (const auto& attribute : attributes->items())
                        {
                            record.attributes.push_back(attribute.key());
                            if (attribute.value().is_number_integer())
                            {
                                primitiveRecord.vertexStreams.push_back({
                                    attribute.key(),
                                    IndexedKey("accessor", attribute.value().get<size_t>())
                                });
                            }
                        }
                    }

                    if (const auto* targets = FindArray(primitive, "targets"))
                        record.morphTargetCount += ToUint(targets->size());

                    record.primitives.push_back(std::move(primitiveRecord));
                }
            }
            scene.meshes.push_back(std::move(record));
        }
    }

    if (const auto* nodes = FindArray(root, "nodes"))
    {
        for (size_t index = 0u; index < nodes->size(); ++index)
        {
            const auto& node = (*nodes)[index];
            if (!node.is_object())
                continue;

            NLS::Render::Assets::ImportedSceneNode record;
            record.sourceKey = IndexedKey("node", index);
            record.name = node.value("name", record.sourceKey);
            const auto meshIndex = node.value("mesh", -1);
            if (meshIndex >= 0)
                record.meshKey = IndexedKey("mesh", static_cast<size_t>(meshIndex));
            const auto skinIndex = node.value("skin", -1);
            if (skinIndex >= 0)
                record.skinKey = IndexedKey("skin", static_cast<size_t>(skinIndex));
            record.translation = ReadNumberArray(node, "translation", {});
            record.rotation = ReadNumberArray(node, "rotation", {});
            record.scale = ReadNumberArray(node, "scale", {});
            scene.nodes.push_back(std::move(record));
        }

        for (size_t parentIndex = 0u; parentIndex < nodes->size(); ++parentIndex)
        {
            const auto& node = (*nodes)[parentIndex];
            if (!node.is_object())
                continue;

            const auto* children = FindArray(node, "children");
            if (!children)
                continue;

            for (const auto& child : *children)
            {
                if (!child.is_number_integer())
                    continue;

                const auto childIndex = child.get<size_t>();
                if (childIndex >= scene.nodes.size())
                    continue;

                scene.nodes[childIndex].parentKey = IndexedKey("node", parentIndex);
            }
        }
    }

    if (const auto* skins = FindArray(root, "skins"))
    {
        for (size_t index = 0u; index < skins->size(); ++index)
        {
            const auto& skin = (*skins)[index];
            if (!skin.is_object())
                continue;

            NLS::Render::Assets::ImportedSceneNamedRecord record;
            record.sourceKey = IndexedKey("skin", index);
            record.name = skin.value("name", record.sourceKey);
            const auto skeleton = skin.value("skeleton", -1);
            if (skeleton >= 0)
                record.skeletonKey = IndexedKey("node", static_cast<size_t>(skeleton));
            if (const auto* joints = FindArray(skin, "joints"))
            {
                for (const auto& joint : *joints)
                {
                    if (joint.is_number_integer())
                        record.joints.push_back(IndexedKey("node", joint.get<size_t>()));
                }
            }
            scene.skins.push_back(std::move(record));
            scene.skeletons.push_back({record.skeletonKey.empty() ? record.sourceKey : record.skeletonKey, record.name});
        }
    }

    if (const auto* animations = FindArray(root, "animations"))
    {
        for (size_t index = 0u; index < animations->size(); ++index)
        {
            const auto& animation = (*animations)[index];
            if (!animation.is_object())
                continue;

            NLS::Render::Assets::ImportedSceneNamedRecord record;
            record.sourceKey = IndexedKey("animation", index);
            record.name = animation.value("name", record.sourceKey);
            if (const auto* channels = FindArray(animation, "channels"))
            {
                for (const auto& channel : *channels)
                {
                    if (!channel.is_object())
                        continue;
                    const auto* target = FindObject(channel, "target");
                    if (!target)
                        continue;
                    const auto nodeIndex = target->value("node", -1);
                    const auto path = target->value("path", std::string {});
                    if (nodeIndex >= 0 && !path.empty())
                        record.targets.push_back(IndexedKey("node", static_cast<size_t>(nodeIndex)) + ":" + path);
                }
            }
            scene.animations.push_back(std::move(record));
        }
    }

    for (const auto& mesh : scene.meshes)
    {
        for (uint32_t index = 0u; index < mesh.morphTargetCount; ++index)
        {
            scene.morphTargets.push_back({
                mesh.sourceKey + "/morph/" + std::to_string(index),
                mesh.name + " Morph " + std::to_string(index),
                {},
                {},
                mesh.sourceKey
            });
        }
    }
}

void ApplySceneImportSettingsCore(NLS::Render::Assets::ImportedScene& scene)
{
    const auto& settings = scene.importSettings;
    if (!settings.importMaterials)
    {
        scene.materials.clear();
        for (auto& mesh : scene.meshes)
        {
            for (auto& primitive : mesh.primitives)
                primitive.materialKey.clear();
        }
    }

    if (!settings.importSkeleton)
    {
        scene.skeletons.clear();
        scene.skins.clear();
        for (auto& node : scene.nodes)
            node.skinKey.clear();
    }

    if (!settings.importAnimations)
        scene.animations.clear();

    if (!settings.importMorphTargets)
    {
        scene.morphTargets.clear();
        for (auto& mesh : scene.meshes)
            mesh.morphTargetCount = 0u;
    }

    for (auto& mesh : scene.meshes)
    {
        auto keepAttribute = [&settings](const std::string& attribute)
        {
            if (!settings.importNormals && attribute == "NORMAL")
                return false;
            if (!settings.importTangents && attribute == "TANGENT")
                return false;
            if (!settings.importUvs && attribute.rfind("TEXCOORD", 0u) == 0u)
                return false;
            return true;
        };

        mesh.attributes.erase(
            std::remove_if(
                mesh.attributes.begin(),
                mesh.attributes.end(),
                [&keepAttribute](const std::string& attribute)
                {
                    return !keepAttribute(attribute);
                }),
            mesh.attributes.end());

        for (auto& primitive : mesh.primitives)
        {
            primitive.vertexStreams.erase(
                std::remove_if(
                    primitive.vertexStreams.begin(),
                    primitive.vertexStreams.end(),
                    [&keepAttribute](const NLS::Render::Assets::ImportedSceneVertexStream& stream)
                    {
                        return !keepAttribute(stream.semantic);
                    }),
                primitive.vertexStreams.end());
        }
    }
}

void ApplyGltfSceneImportSettings(
    const ImportedSceneJson& root,
    NLS::Render::Assets::ImportedScene& scene)
{
    ApplySceneImportSettingsCore(scene);

    const auto& settings = scene.importSettings;
    if (settings.importCameras && FindArray(root, "cameras"))
    {
        scene.diagnostics.push_back({
            "model-import-cameras-unsupported",
            "Model importer requested cameras, but Nullus has no model camera conversion path yet."
        });
    }

    if (settings.importLights && FindArray(root, "lights"))
    {
        scene.diagnostics.push_back({
            "model-import-lights-unsupported",
            "Model importer requested lights, but Nullus has no model light conversion path yet."
        });
    }
}
}

namespace NLS::Render::Assets
{
SceneImporterRegistry SceneImporterRegistry::CreateDefault()
{
    SceneImporterRegistry registry;
    const auto modelImporterVersion =
        NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
    registry.Register({"gltf-scene", modelImporterVersion, {".gltf", ".glb"}});
    registry.Register({"fbx-scene", modelImporterVersion, {".fbx"}});
    registry.Register({"obj-scene", modelImporterVersion, {".obj"}});
    return registry;
}

void SceneImporterRegistry::Register(SceneImporterDescriptor descriptor)
{
    for (auto& extension : descriptor.extensions)
        extension = ToLower(extension);
    m_importers.push_back(std::move(descriptor));
}

void SceneImporterRegistry::RegisterScripted(ScriptedSceneImporterDescriptor descriptor)
{
    SceneImporterDescriptor publicDescriptor;
    publicDescriptor.importerId = descriptor.importerId;
    publicDescriptor.importerVersion = descriptor.importerVersion;
    publicDescriptor.extensions = descriptor.extensions;
    Register(std::move(publicDescriptor));

    for (auto& extension : descriptor.extensions)
        extension = ToLower(extension);
    m_scriptedImporters.push_back(std::move(descriptor));
}

const SceneImporterDescriptor* SceneImporterRegistry::FindImporterForPath(const std::filesystem::path& sourcePath) const
{
    const auto extension = ToLower(sourcePath.extension().string());
    if (extension.empty())
        return nullptr;

    for (const auto& importer : m_importers)
    {
        if (std::find(importer.extensions.begin(), importer.extensions.end(), extension) != importer.extensions.end())
            return &importer;
    }
    return nullptr;
}

std::optional<ImportedScene> SceneImporterRegistry::ImportScripted(
    const ScriptedSceneImportRequest& request) const
{
    const auto extension = ToLower(request.sourcePath.extension().string());
    for (const auto& importer : m_scriptedImporters)
    {
        if (std::find(importer.extensions.begin(), importer.extensions.end(), extension) != importer.extensions.end())
        {
            if (!importer.import)
                return std::nullopt;
            return importer.import(request);
        }
    }
    return std::nullopt;
}

const std::vector<SceneImporterDescriptor>& SceneImporterRegistry::GetImporters() const
{
    return m_importers;
}

const char* ToSubAssetPrefix(const ImportedSceneSubAssetType type)
{
    switch (type)
    {
    case ImportedSceneSubAssetType::Mesh: return "mesh";
    case ImportedSceneSubAssetType::Material: return "material";
    case ImportedSceneSubAssetType::Texture: return "texture";
    case ImportedSceneSubAssetType::Skeleton: return "skeleton";
    case ImportedSceneSubAssetType::Skin: return "skin";
    case ImportedSceneSubAssetType::AnimationClip: return "animation";
    case ImportedSceneSubAssetType::MorphTarget: return "morph-target";
    case ImportedSceneSubAssetType::Model: return "model";
    case ImportedSceneSubAssetType::Prefab: return "prefab";
    default: return "unknown";
    }
}

std::string BuildPrimitiveMeshSourceKey(const std::string& meshKey, const size_t primitiveIndex)
{
    return meshKey + "/primitive/" + std::to_string(primitiveIndex);
}

std::optional<std::pair<std::string, size_t>> ParsePrimitiveMeshSourceKey(const std::string& meshKey)
{
    constexpr std::string_view marker = "/primitive/";
    const auto markerOffset = meshKey.rfind(marker);
    if (markerOffset == std::string::npos)
        return std::nullopt;

    const auto parentKey = meshKey.substr(0u, markerOffset);
    const auto indexText = meshKey.substr(markerOffset + marker.size());
    if (parentKey.empty() || indexText.empty())
        return std::nullopt;

    size_t primitiveIndex = 0u;
    for (const char character : indexText)
    {
        if (character < '0' || character > '9')
            return std::nullopt;
        const auto digit = static_cast<size_t>(character - '0');
        if (primitiveIndex > (std::numeric_limits<size_t>::max() - digit) / 10u)
            return std::nullopt;
        primitiveIndex = primitiveIndex * 10u + digit;
    }

    return std::make_pair(parentKey, primitiveIndex);
}

std::vector<GeneratedSceneSubAsset> GenerateSceneSubAssets(const ImportedScene& scene)
{
    size_t meshSubAssetCount = 0u;
    for (const auto& mesh : scene.meshes)
        meshSubAssetCount += (std::max)(size_t {1u}, mesh.primitives.size());

    std::vector<GeneratedSceneSubAsset> records;
    records.reserve(
        meshSubAssetCount +
        scene.materials.size() +
        scene.textures.size() +
        scene.skeletons.size() +
        scene.skins.size() +
        scene.animations.size() +
        scene.morphTargets.size() +
        1u);

    AddSubAssets(records, ImportedSceneSubAssetType::AnimationClip, scene.animations);
    AddSubAssets(records, ImportedSceneSubAssetType::Material, scene.materials);
    AddMeshSubAssets(records, scene.meshes);
    AddSubAssets(records, ImportedSceneSubAssetType::MorphTarget, scene.morphTargets);
    AddSubAssets(records, ImportedSceneSubAssetType::Skeleton, scene.skeletons);
    AddSubAssets(records, ImportedSceneSubAssetType::Skin, scene.skins);
    AddSubAssets(records, ImportedSceneSubAssetType::Texture, scene.textures);

    const auto sceneKey = StableSceneKey(scene);
    records.push_back({
        ImportedSceneSubAssetType::Prefab,
        std::string(ToSubAssetPrefix(ImportedSceneSubAssetType::Prefab)) + ":" + sceneKey,
        sceneKey,
        sceneKey
    });

    std::sort(
        records.begin(),
        records.end(),
        [](const GeneratedSceneSubAsset& lhs, const GeneratedSceneSubAsset& rhs)
        {
            return lhs.key < rhs.key;
        });

    return records;
}

ImportedScene ImportGltfSceneJson(
    const std::string& jsonText,
    NLS::Core::Assets::AssetId sourceAssetId,
    std::string sceneKey)
{
    return ImportGltfSceneJson(
        jsonText,
        sourceAssetId,
        std::move(sceneKey),
        SceneImportSettings {});
}

ImportedScene ImportGltfSceneJson(
    const std::string& jsonText,
    NLS::Core::Assets::AssetId sourceAssetId,
    std::string sceneKey,
    const SceneImportSettings& settings)
{
    ImportedScene scene;
    scene.sourceAssetId = sourceAssetId;
    scene.sceneKey = std::move(sceneKey);
    scene.importSettings = settings;

    const auto root = ImportedSceneJson::parse(jsonText, nullptr, false);
    if (root.is_discarded() || !root.is_object())
    {
        scene.diagnostics.push_back({"gltf-invalid-json", "glTF JSON could not be parsed."});
        return scene;
    }

    PopulateGltfSceneFromJson(root, scene, 0u);
    ApplyGltfSceneImportSettings(root, scene);

    return scene;
}

ImportedScene ImportGltfSceneBytes(
    const std::vector<uint8_t>& sourceBytes,
    NLS::Core::Assets::AssetId sourceAssetId,
    std::string sceneKey)
{
    return ImportGltfSceneBytes(
        sourceBytes,
        sourceAssetId,
        std::move(sceneKey),
        SceneImportSettings {});
}

ImportedScene ImportGltfSceneBytes(
    const std::vector<uint8_t>& sourceBytes,
    NLS::Core::Assets::AssetId sourceAssetId,
    std::string sceneKey,
    const SceneImportSettings& settings)
{
    ImportedScene scene;
    scene.sourceAssetId = sourceAssetId;
    scene.sceneKey = std::move(sceneKey);
    scene.importSettings = settings;

    if (sourceBytes.size() < 20u ||
        ReadLittleEndianU32(sourceBytes, 0u) != kGlbMagic ||
        ReadLittleEndianU32(sourceBytes, 4u) != kGlbVersion)
    {
        scene.diagnostics.push_back({"glb-invalid-header", "GLB header is missing or not version 2."});
        return scene;
    }

    const auto declaredLength = ReadLittleEndianU32(sourceBytes, 8u);
    if (declaredLength > sourceBytes.size())
    {
        scene.diagnostics.push_back({"glb-length-out-of-range", "GLB declared length exceeds source byte size."});
        return scene;
    }

    std::string jsonText;
    uint64_t binaryChunkLength = 0u;
    size_t offset = 12u;
    while (offset + 8u <= declaredLength)
    {
        const auto chunkLength = ReadLittleEndianU32(sourceBytes, offset);
        const auto chunkType = ReadLittleEndianU32(sourceBytes, offset + 4u);
        offset += 8u;
        if (offset + chunkLength > declaredLength)
        {
            scene.diagnostics.push_back({"glb-chunk-out-of-range", "GLB chunk length exceeds declared source length."});
            return scene;
        }

        if (chunkType == kGlbJsonChunkType)
        {
            jsonText.assign(
                reinterpret_cast<const char*>(sourceBytes.data() + offset),
                reinterpret_cast<const char*>(sourceBytes.data() + offset + chunkLength));
        }
        else if (chunkType == kGlbBinChunkType)
        {
            binaryChunkLength = chunkLength;
        }

        offset += chunkLength;
    }

    const auto root = ImportedSceneJson::parse(jsonText, nullptr, false);
    if (root.is_discarded() || !root.is_object())
    {
        scene.diagnostics.push_back({"glb-invalid-json", "GLB JSON chunk could not be parsed."});
        return scene;
    }

    PopulateGltfSceneFromJson(root, scene, binaryChunkLength);
    ApplyGltfSceneImportSettings(root, scene);
    return scene;
}

void ApplySceneImportSettings(ImportedScene& scene)
{
    ApplySceneImportSettingsCore(scene);
}

ImportedScene ImportParserModelScene(
    Resources::Parsers::IModelParser& parser,
    const std::filesystem::path& sourcePath,
    const SceneModelSourceFormat sourceFormat,
    NLS::Core::Assets::AssetId sourceAssetId,
    std::string sceneKey)
{
    ImportedScene scene;
    scene.sourceAssetId = sourceAssetId;
    scene.sceneKey = std::move(sceneKey);

    std::vector<Resources::Mesh*> rawMeshes;
    std::vector<std::string> materialNames;
    const auto loaded = parser.LoadModel(
        sourcePath.string(),
        rawMeshes,
        materialNames,
        Resources::Parsers::EModelParserFlags::TRIANGULATE);

    std::vector<std::unique_ptr<Resources::Mesh>> ownedMeshes;
    ownedMeshes.reserve(rawMeshes.size());
    for (auto* mesh : rawMeshes)
        ownedMeshes.emplace_back(mesh);

    if (!loaded)
    {
        scene.diagnostics.push_back({"parser-load-failed", "Model parser failed to load the source file."});
        return scene;
    }

    bool hasDetailedParserData = false;
    if (auto* detailedParser = dynamic_cast<IImportedSceneParserDataProvider*>(&parser))
    {
        ImportedScene detailedScene;
        detailedScene.sourceAssetId = scene.sourceAssetId;
        detailedScene.sceneKey = scene.sceneKey;
        hasDetailedParserData = detailedParser->PopulateImportedSceneData(sourcePath, sourceFormat, detailedScene);
        if (hasDetailedParserData)
            scene = std::move(detailedScene);
        else
        {
            scene.diagnostics.push_back({
                "parser-detailed-scene-data-failed",
                "Parser detailed scene data was incomplete; falling back to mesh/material adapter output."
            });
        }
    }

    if (hasDetailedParserData)
    {
        AddParserLimitDiagnostic(scene, sourceFormat, true);
        return scene;
    }

    std::vector<Resources::Parsers::ParsedMeshData> parsedMeshes;
    parsedMeshes.reserve(rawMeshes.size());
    for (const auto* mesh : rawMeshes)
    {
        Resources::Parsers::ParsedMeshData parsedMesh;
        if (mesh)
            parsedMesh.materialIndex = mesh->GetMaterialIndex();
        parsedMeshes.push_back(std::move(parsedMesh));
    }

    auto fallbackScene = BuildFallbackParserScene(
        parsedMeshes,
        materialNames,
        sourceFormat,
        scene.sourceAssetId,
        std::move(scene.sceneKey));
    fallbackScene.diagnostics.insert(
        fallbackScene.diagnostics.begin(),
        std::make_move_iterator(scene.diagnostics.begin()),
        std::make_move_iterator(scene.diagnostics.end()));
    return fallbackScene;
}

ImportedScene ImportParsedModelScene(
    const std::vector<Resources::Parsers::ParsedMeshData>& meshes,
    const std::vector<std::string>& materialNames,
    const std::filesystem::path&,
    const SceneModelSourceFormat sourceFormat,
    NLS::Core::Assets::AssetId sourceAssetId,
    std::string sceneKey)
{
    return BuildFallbackParserScene(
        meshes,
        materialNames,
        sourceFormat,
        sourceAssetId,
        std::move(sceneKey));
}
}
