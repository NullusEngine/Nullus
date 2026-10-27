#include "Serialization/MaterialSerializedBackend.h"

#include <any>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Math/Vector2.h>
#include <Math/Vector3.h>
#include <Math/Vector4.h>
#include <Object/Object.h>
#include <Rendering/Resources/Material.h>
#include <Rendering/Resources/Shader.h>
#include <ServiceLocator.h>

#include "Core/Context.h"
#include "Materials/MaterialEditingService.h"

namespace NLS::Editor::Serialization
{
namespace
{
using Engine::Serialize::BinaryBufferWriter;
using Engine::Serialize::NativeSerializedStreamCodec;
using Engine::Serialize::SerializedByteStream;
using Engine::Serialize::SerializedNodeFlags;
using Engine::Serialize::SerializedNodeKind;
using Engine::Serialize::SerializedNumericKind;
using Engine::Serialize::SerializedReferenceEntry;
using Engine::Serialize::SerializedReferenceTable;
using Engine::Serialize::SerializedTypeTree;
using Engine::Serialize::SerializedTypeTreeNode;
using Render::Resources::Material;
using Render::Resources::Shader;

#if defined(NLS_ENABLE_TEST_HOOKS)
MaterialSerializedBackend::TargetResolverForTesting& GetTargetResolverForTesting()
{
    static MaterialSerializedBackend::TargetResolverForTesting resolver;
    return resolver;
}
#endif

std::string NormalizeAssetRelativePath(std::string path)
{
    const auto separator = path.find('#');
    if (separator != std::string::npos)
        path.erase(separator);
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());
    constexpr std::string_view prefix = "Assets/";
    if (path.rfind(prefix, 0u) == 0u)
        path.erase(0u, prefix.size());
    return path;
}

Material* ResolveMaterial(const SerializedTargetHandle& target)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    if (auto& resolver = GetTargetResolverForTesting(); resolver)
        return resolver(target);
#endif
    if (target.kind != Inspector::InspectorTargetKind::Material ||
        !NLS::Core::ServiceLocator::Contains<Core::Context>())
    {
        return nullptr;
    }
    auto& context = NLS::Core::ServiceLocator::Get<Core::Context>();
    if (NLS::Core::ServiceLocator::Contains<Materials::MaterialEditingService>())
    {
        std::string error;
        const auto session=NLS::Core::ServiceLocator::Get<Materials::MaterialEditingService>().OpenTarget(
            target.assetId,target.resourcePath,error);
        if (session) return session->draft.get();
    }
    return context.materialManager[NormalizeAssetRelativePath(target.resourcePath)];
}

uint64_t HashText(const std::string_view text)
{
    constexpr uint64_t offset = 1469598103934665603ULL;
    constexpr uint64_t prime = 1099511628211ULL;
    uint64_t hash = offset;
    for (const auto character : text)
    {
        hash ^= static_cast<uint8_t>(character);
        hash *= prime;
    }
    return hash;
}

uint64_t StableFieldId(const std::string_view path)
{
    const auto hash = HashText(path);
    return hash == 0u ? 1u : hash;
}

enum class UniformKind : uint8_t
{
    Boolean,
    Integer,
    Float,
    Vector2,
    Vector3,
    Vector4,
    ReadOnlyPath
};

UniformKind GetUniformKind(const std::any& value)
{
    if (value.type() == typeid(bool)) return UniformKind::Boolean;
    if (value.type() == typeid(int)) return UniformKind::Integer;
    if (value.type() == typeid(float)) return UniformKind::Float;
    if (value.type() == typeid(Maths::Vector2)) return UniformKind::Vector2;
    if (value.type() == typeid(Maths::Vector3)) return UniformKind::Vector3;
    if (value.type() == typeid(Maths::Vector4)) return UniformKind::Vector4;
    return UniformKind::ReadOnlyPath;
}

meta::Type TypeForUniform(const UniformKind kind)
{
    switch (kind)
    {
    case UniformKind::Boolean: return NLS_TYPEOF(bool);
    case UniformKind::Integer: return NLS_TYPEOF(int);
    case UniformKind::Float: return NLS_TYPEOF(float);
    case UniformKind::Vector2: return NLS_TYPEOF(Maths::Vector2);
    case UniformKind::Vector3: return NLS_TYPEOF(Maths::Vector3);
    case UniformKind::Vector4: return NLS_TYPEOF(Maths::Vector4);
    case UniformKind::ReadOnlyPath: return NLS_TYPEOF(std::string);
    }
    return {};
}

void CopyValueLayout(SerializedTypeTreeNode& destination, const meta::Type& type)
{
    const auto sourceTree = NativeSerializedStreamCodec::BuildTypeTree(type, {"value", false});
    const auto* source = sourceTree != nullptr ? sourceTree->FindNode(sourceTree->GetRoot()) : nullptr;
    if (source == nullptr)
        return;
    destination.typeName = source->typeName;
    destination.typeId = source->typeId;
    destination.kind = source->kind;
    destination.numericKind = source->numericKind;
    destination.fixedByteSize = source->fixedByteSize;
    destination.flags = source->flags;
}

Engine::Serialize::SerializedNodeId AppendNode(
    SerializedTypeTree& tree,
    const Engine::Serialize::SerializedNodeId parent,
    Engine::Serialize::SerializedNodeId& previous,
    SerializedTypeTreeNode node)
{
    node.parent = parent;
    const auto id = tree.AddNode(std::move(node));
    if (previous == 0u)
        tree.GetNodes()[parent - 1u].firstChild = id;
    else
        tree.GetNodes()[previous - 1u].nextSibling = id;
    previous = id;
    return id;
}

Engine::Serialize::SerializedNodeId AppendValueNode(
    SerializedTypeTree& tree,
    const Engine::Serialize::SerializedNodeId parent,
    Engine::Serialize::SerializedNodeId& previous,
    const std::string_view name,
    const meta::Type& type,
    const bool readOnly = false)
{
    SerializedTypeTreeNode node;
    node.serializedName = name;
    node.fieldId = StableFieldId(tree.GetPath(parent) + "." + std::string(name));
    CopyValueLayout(node, type);
    if (readOnly)
        node.flags = node.flags | SerializedNodeFlags::ReadOnly;
    return AppendNode(tree, parent, previous, std::move(node));
}

std::shared_ptr<SerializedTypeTree> BuildMaterialTypeTree(const Material& material)
{
    auto tree = std::make_shared<SerializedTypeTree>();
    SerializedTypeTreeNode root;
    root.serializedName = "Material";
    root.typeName = "Material";
    root.kind = SerializedNodeKind::Generic;
    root.fixedByteSize = -1;
    const auto rootId = tree->AddNode(std::move(root));

    Engine::Serialize::SerializedNodeId previous = 0u;
    AppendValueNode(*tree, rootId, previous, "blendable", NLS_TYPEOF(bool));
    AppendValueNode(*tree, rootId, previous, "backfaceCulling", NLS_TYPEOF(bool));
    AppendValueNode(*tree, rootId, previous, "frontfaceCulling", NLS_TYPEOF(bool));
    AppendValueNode(*tree, rootId, previous, "depthTest", NLS_TYPEOF(bool));
    AppendValueNode(*tree, rootId, previous, "depthWriting", NLS_TYPEOF(bool));
    AppendValueNode(*tree, rootId, previous, "colorWriting", NLS_TYPEOF(bool));
    AppendValueNode(*tree, rootId, previous, "gpuInstances", NLS_TYPEOF(int));
    AppendValueNode(*tree, rootId, previous, "shader", NLS_TYPEOF(Shader*));

    SerializedTypeTreeNode uniforms;
    uniforms.serializedName = "Uniforms";
    uniforms.fieldId = StableFieldId("Material.Uniforms");
    uniforms.typeName = "MaterialUniforms";
    uniforms.kind = SerializedNodeKind::Generic;
    uniforms.fixedByteSize = -1;
    Engine::Serialize::SerializedNodeId uniformsPrevious = 0u;
    const auto uniformsId = AppendNode(*tree, rootId, previous, std::move(uniforms));
    for (const auto& [name, value] : material.GetUniformsData())
        AppendValueNode(*tree, uniformsId, uniformsPrevious, name, TypeForUniform(GetUniformKind(value)),
            GetUniformKind(value) == UniformKind::ReadOnlyPath);

    tree->RebuildIndexes();
    return tree;
}

uint64_t MaterialSchemaSignature(const Material& material)
{
    const auto tree = BuildMaterialTypeTree(material);
    return tree != nullptr ? tree->GetSchemaHash() : 0u;
}

bool AppendMaterialLeaf(
    const Material& material,
    const SerializedTypeTree& tree,
    const SerializedTypeTreeNode& node,
    SerializedReferenceTable& references,
    BinaryBufferWriter& writer,
    std::string& error)
{
    const auto path = tree.GetPath(node.id);
    if (path == "Material.blendable") { writer.WriteBool(material.IsBlendable()); return true; }
    if (path == "Material.backfaceCulling") { writer.WriteBool(material.HasBackfaceCulling()); return true; }
    if (path == "Material.frontfaceCulling") { writer.WriteBool(material.HasFrontfaceCulling()); return true; }
    if (path == "Material.depthTest") { writer.WriteBool(material.HasDepthTest()); return true; }
    if (path == "Material.depthWriting") { writer.WriteBool(material.HasDepthWriting()); return true; }
    if (path == "Material.colorWriting") { writer.WriteBool(material.HasColorWriting()); return true; }
    if (path == "Material.gpuInstances") { writer.WriteI32(material.GetGPUInstances()); return true; }
    if (path == "Material.shader")
    {
        SerializedReferenceEntry entry;
        entry.expectedTypeName = NLS_TYPEOF(Shader*).GetName();
        const auto* shader = material.GetShader();
        entry.isNull = shader == nullptr;
        if (shader != nullptr)
        {
            entry.resourcePath = shader->path;
            if (!Engine::Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(
                    shader->GetInstanceID(), entry.identifier))
            {
                entry.identifier = Engine::Serialize::PersistentManager::Instance()
                    .GetOrCreateLocalObjectIdentifier(const_cast<Shader&>(*shader));
            }
        }
        writer.WriteU32(references.Intern(std::move(entry)));
        return true;
    }
    constexpr std::string_view uniformPrefix = "Material.Uniforms.";
    if (path.rfind(uniformPrefix, 0u) != 0u)
    {
        error = "Material TypeTree contains an unknown property '" + path + "'.";
        return false;
    }
    const auto name = path.substr(uniformPrefix.size());
    const auto found = material.GetUniformsData().find(name);
    if (found == material.GetUniformsData().end())
    {
        error = "Material uniform changed while the stream was being encoded: " + name;
        return false;
    }
    switch (GetUniformKind(found->second))
    {
    case UniformKind::Boolean: writer.WriteBool(std::any_cast<bool>(found->second)); return true;
    case UniformKind::Integer: writer.WriteI32(std::any_cast<int>(found->second)); return true;
    case UniformKind::Float: writer.WriteFloat(std::any_cast<float>(found->second)); return true;
    case UniformKind::Vector2:
    {
        const auto value = std::any_cast<Maths::Vector2>(found->second);
        writer.WriteFloat(value.x); writer.WriteFloat(value.y); return true;
    }
    case UniformKind::Vector3:
    {
        const auto value = std::any_cast<Maths::Vector3>(found->second);
        writer.WriteFloat(value.x); writer.WriteFloat(value.y); writer.WriteFloat(value.z); return true;
    }
    case UniformKind::Vector4:
    {
        const auto value = std::any_cast<Maths::Vector4>(found->second);
        writer.WriteFloat(value.x); writer.WriteFloat(value.y); writer.WriteFloat(value.z); writer.WriteFloat(value.w); return true;
    }
    case UniformKind::ReadOnlyPath:
        writer.WriteAlignedString(material.GetTextureResourcePath(name));
        return true;
    }
    error = "Material uniform has an unsupported stream type: " + name;
    return false;
}

bool AppendMaterialNode(
    const Material& material,
    const SerializedTypeTree& tree,
    const SerializedTypeTreeNode& node,
    SerializedReferenceTable& references,
    BinaryBufferWriter& writer,
    std::string& error)
{
    if (node.firstChild == 0u)
        return AppendMaterialLeaf(material, tree, node, references, writer, error);
    for (auto child = tree.GetFirstChild(node.id); child != nullptr; child = tree.GetNextSibling(child->id))
    {
        if (!AppendMaterialNode(material, tree, *child, references, writer, error))
            return false;
    }
    return true;
}

std::optional<meta::Variant> DecodeMaterialLeaf(
    const SerializedByteStream& stream,
    const SerializedTypeTreeNode& node,
    const std::string_view path,
    std::string& error)
{
    const auto copy = std::make_shared<SerializedByteStream>(stream);
    const auto cursor = NativeSerializedStreamCodec::ResolveCursor(copy, path);
    if (!cursor.has_value())
    {
        error = "Material stream is truncated at '" + std::string(path) + "'.";
        return std::nullopt;
    }
    const meta::Type type(node.typeId);
    if (!type.IsValid())
    {
        error = "Material stream property has no native type: '" + std::string(path) + "'.";
        return std::nullopt;
    }
    const auto leafTree = NativeSerializedStreamCodec::BuildTypeTree(type, {"value", false});
    if (leafTree == nullptr)
    {
        error = "Material stream property has no native codec: '" + std::string(path) + "'.";
        return std::nullopt;
    }
    std::vector<uint8_t> bytes(cursor->GetByteSize());
    if (!cursor->Read(bytes.data(), bytes.size()))
    {
        error = "Material stream property cannot be read: '" + std::string(path) + "'.";
        return std::nullopt;
    }
    const auto references = stream.GetReferences() != nullptr
        ? *stream.GetReferences() : SerializedReferenceTable {};
    return NativeSerializedStreamCodec::DecodeVariant(
        bytes, *leafTree, type, references, &error, cursor->GetByteOffset());
}

bool DecodeMaterialValues(
    const SerializedByteStream& stream,
    std::unordered_map<std::string, meta::Variant>& values,
    std::string& error)
{
    const auto schema = stream.GetSchema();
    const auto* root = schema != nullptr ? schema->FindNode(schema->GetRoot()) : nullptr;
    if (root == nullptr || root->serializedName != "Material")
    {
        error = "Material stream has an invalid TypeTree root.";
        return false;
    }
    const auto visit = [&](const auto& self, const SerializedTypeTreeNode& node) -> bool
    {
        if (node.firstChild == 0u)
        {
            const auto path = schema->GetPath(node.id);
            auto value = DecodeMaterialLeaf(stream, node, path, error);
            if (!value.has_value())
                return false;
            values.emplace(path, std::move(*value));
            return true;
        }
        for (auto child = schema->GetFirstChild(node.id); child != nullptr; child = schema->GetNextSibling(child->id))
        {
            if (!self(self, *child))
                return false;
        }
        return true;
    };
    return visit(visit, *root);
}

bool ApplyDecodedMaterialValues(
    Material& material,
    const std::unordered_map<std::string, meta::Variant>& values,
    std::string& error)
{
    const auto get = [&values, &error](const std::string_view path) -> const meta::Variant*
    {
        const auto found = values.find(std::string(path));
        if (found == values.end() || !found->second.IsValid())
        {
            error = "Material stream is missing property '" + std::string(path) + "'.";
            return nullptr;
        }
        return &found->second;
    };
    const auto* blendable = get("Material.blendable");
    const auto* backface = get("Material.backfaceCulling");
    const auto* frontface = get("Material.frontfaceCulling");
    const auto* depthTest = get("Material.depthTest");
    const auto* depthWriting = get("Material.depthWriting");
    const auto* colorWriting = get("Material.colorWriting");
    const auto* instances = get("Material.gpuInstances");
    const auto* shader = get("Material.shader");
    if (blendable == nullptr || backface == nullptr || frontface == nullptr || depthTest == nullptr ||
        depthWriting == nullptr || colorWriting == nullptr || instances == nullptr || shader == nullptr)
    {
        return false;
    }
    // Object references are represented by meta::ObjectWrapper, whose public
    // Variant type is the wrapped object type (Shader), not Shader*.  The
    // pointer is recovered below through Variant::GetValue<Shader*>.
    if (shader->GetType() != NLS_TYPEOF(Shader))
    {
        error = "Material shader reference has an incompatible type: " +
            (shader->IsValid() && shader->GetType().IsValid()
                ? shader->GetType().GetName() : std::string {"<invalid>"});
        return false;
    }

    struct UniformChange final { std::string name; UniformKind kind; meta::Variant value; };
    std::vector<UniformChange> uniformChanges;
    for (const auto& [name, source] : material.GetUniformsData())
    {
        const auto kind = GetUniformKind(source);
        if (kind == UniformKind::ReadOnlyPath)
            continue;
        const auto* value = get("Material.Uniforms." + name);
        if (value == nullptr || value->GetType() != TypeForUniform(kind))
        {
            error = "Material uniform has an incompatible stream value: " + name;
            return false;
        }
        uniformChanges.push_back({name, kind, *value});
    }

    auto* shaderValue = shader->GetValue<Shader*>();
    if (material.GetShader() != shaderValue)
    {
        material.SetShader(shaderValue);
        material.SetShaderReferencePath(shaderValue != nullptr ? shaderValue->path : std::string {});
    }
    material.SetBlendable(blendable->GetValue<bool>());
    material.SetBackfaceCulling(backface->GetValue<bool>());
    material.SetFrontfaceCulling(frontface->GetValue<bool>());
    material.SetDepthTest(depthTest->GetValue<bool>());
    material.SetDepthWriting(depthWriting->GetValue<bool>());
    material.SetColorWriting(colorWriting->GetValue<bool>());
    material.SetGPUInstances(instances->GetValue<int>());
    for (const auto& change : uniformChanges)
    {
        // An unloaded material can still expose persistent raw values. Preserve
        // those values through SerializedObject edits instead of dropping them
        // merely because a shader is not currently attached.
        if (material.GetShader() == nullptr)
        {
            switch (change.kind)
            {
            case UniformKind::Boolean: material.SetRawParameter(change.name, change.value.GetValue<bool>()); break;
            case UniformKind::Integer: material.SetRawParameter(change.name, change.value.GetValue<int>()); break;
            case UniformKind::Float: material.SetRawParameter(change.name, change.value.GetValue<float>()); break;
            case UniformKind::Vector2: material.SetRawParameter(change.name, change.value.GetValue<Maths::Vector2>()); break;
            case UniformKind::Vector3: material.SetRawParameter(change.name, change.value.GetValue<Maths::Vector3>()); break;
            case UniformKind::Vector4: material.SetRawParameter(change.name, change.value.GetValue<Maths::Vector4>()); break;
            case UniformKind::ReadOnlyPath: break;
            }
            continue;
        }
        switch (change.kind)
        {
        case UniformKind::Boolean: material.Set<bool>(change.name, change.value.GetValue<bool>()); break;
        case UniformKind::Integer: material.Set<int>(change.name, change.value.GetValue<int>()); break;
        case UniformKind::Float: material.Set<float>(change.name, change.value.GetValue<float>()); break;
        case UniformKind::Vector2: material.Set<Maths::Vector2>(change.name, change.value.GetValue<Maths::Vector2>()); break;
        case UniformKind::Vector3: material.Set<Maths::Vector3>(change.name, change.value.GetValue<Maths::Vector3>()); break;
        case UniformKind::Vector4: material.Set<Maths::Vector4>(change.name, change.value.GetValue<Maths::Vector4>()); break;
        case UniformKind::ReadOnlyPath: break;
        }
    }
    material.MarkParametersDirty();
    return true;
}
} // namespace

std::shared_ptr<MaterialSerializedBackend> MaterialSerializedBackend::Create()
{
    return std::make_shared<MaterialSerializedBackend>();
}

#if defined(NLS_ENABLE_TEST_HOOKS)
void MaterialSerializedBackend::SetTargetResolverForTesting(TargetResolverForTesting resolver)
{
    GetTargetResolverForTesting() = std::move(resolver);
}
#endif

bool MaterialSerializedBackend::Supports(const SerializedTargetSet& targets)
{
    if (targets.Empty())
        return false;
    const auto* first = ResolveMaterial(targets.targets.front());
    if (first == nullptr)
        return false;
    const auto schema = MaterialSchemaSignature(*first);
    return std::all_of(targets.targets.begin(), targets.targets.end(), [schema](const SerializedTargetHandle& target)
    {
        const auto* material = ResolveMaterial(target);
        return material != nullptr && MaterialSchemaSignature(*material) == schema;
    });
}

Engine::Serialize::TypeTreeResult MaterialSerializedBackend::BuildTypeTree(
    const SerializedTargetSet& targets,
    const SerializedInspectorMode) const
{
    if (!Supports(targets))
        return {nullptr, "Materials are unavailable or have incompatible parameter schemas."};
    return {BuildMaterialTypeTree(*ResolveMaterial(targets.targets.front())), {}};
}

Engine::Serialize::StreamResult MaterialSerializedBackend::SerializeTarget(
    const SerializedTargetHandle& target,
    std::shared_ptr<const SerializedTypeTree> schema,
    SerializedReferenceTable& references) const
{
    const auto* material = ResolveMaterial(target);
    if (material == nullptr || schema == nullptr)
        return {nullptr, "Material target is no longer available."};
    const auto* root = schema->FindNode(schema->GetRoot());
    if (root == nullptr || root->serializedName != "Material")
        return {nullptr, "Material TypeTree is invalid."};
    BinaryBufferWriter writer;
    std::string error;
    if (!AppendMaterialNode(*material, *schema, *root, references, writer, error))
        return {nullptr, error.empty() ? "Material stream could not be encoded." : std::move(error)};
    auto stream = std::make_shared<SerializedByteStream>(std::move(schema));
    stream->SetBytes(std::move(writer.bytes));
    stream->SetReferences(std::make_shared<SerializedReferenceTable>(references));
    return {std::move(stream), {}};
}

ValidationResult MaterialSerializedBackend::ValidateAndNormalize(
    const SerializedTargetHandle& target,
    const std::span<const SerializedPropertyPath> modifiedPaths,
    SerializedByteStream& stream) const
{
    if (ResolveMaterial(target) == nullptr)
        return ValidationResult::Failure("Material target is no longer available.");
    const auto schema = stream.GetSchema();
    for (const auto& path : modifiedPaths)
    {
        const auto* node = schema != nullptr ? schema->FindPath(path.ToString()) : nullptr;
        if (node == nullptr || Engine::Serialize::HasSerializedNodeFlag(node->flags, SerializedNodeFlags::ReadOnly))
            return ValidationResult::Failure("Material property is no longer writable: " + path.ToString());
    }
    std::unordered_map<std::string, meta::Variant> values;
    std::string error;
    return DecodeMaterialValues(stream, values, error)
        ? ValidationResult::Success()
        : ValidationResult::Failure(error.empty() ? "Material stream is invalid." : std::move(error));
}

ApplyResult MaterialSerializedBackend::DeserializeTarget(
    const SerializedTargetHandle& target,
    const SerializedByteStream& stream,
    const Undo::UndoExecutionContext&) const
{
    auto* material = ResolveMaterial(target);
    if (material == nullptr)
        return ApplyResult::Failure("Material target is no longer available.");
    std::unordered_map<std::string, meta::Variant> values;
    std::string error;
    auto before=material->CreateDynamicCopy();
    if (!DecodeMaterialValues(stream, values, error) || !ApplyDecodedMaterialValues(*material, values, error))
        return ApplyResult::Failure(error.empty() ? "Material stream could not be applied." : std::move(error));
    if (NLS::Core::ServiceLocator::Contains<Materials::MaterialEditingService>())
    {
        auto& service=NLS::Core::ServiceLocator::Get<Materials::MaterialEditingService>();
        if (const auto session=service.FindDraft(material);session && !service.CaptureInspectorChanges(*session,*before))
        {material->CopyRuntimeFrom(*before);return ApplyResult::Failure(session->error);}
    }
    return ApplyResult::Success();
}

SerializedVersionStamp MaterialSerializedBackend::GetVersion(const SerializedTargetHandle& target) const
{
    const auto* material = ResolveMaterial(target);
    if (material == nullptr)
        return {};
    const auto schema = MaterialSchemaSignature(*material);
    const auto revision = material->GetParameterRevision() ^ (material->GetRenderStateRevision() << 1u) ^
        (material->GetBindingRevision() << 2u);
    return {revision, schema, schema};
}
} // namespace NLS::Editor::Serialization
