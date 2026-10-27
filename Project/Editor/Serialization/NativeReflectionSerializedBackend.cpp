#include "Serialization/NativeReflectionSerializedBackend.h"

#include <algorithm>
#include <functional>

#include <Object/Object.h>
#include <Reflection/Argument.h>
#include <Serialize/NativeSerializedStream.h>
#include <Serialize/ObjectGraphPPtr.h>

namespace NLS::Editor::Serialization
{
namespace
{
using Engine::Serialize::NativeSerializedStreamCodec;
using Engine::Serialize::SerializedByteStream;
using Engine::Serialize::SerializedReferenceTable;

bool SameType(const meta::Type& left, const meta::Type& right)
{
    return left.IsValid() && right.IsValid() && (left == right || left.GetID() == right.GetID());
}

std::string RootFieldName(const Engine::Serialize::SerializedTypeTree& schema, std::string_view path)
{
    const auto rootPath = schema.GetPath(schema.GetRoot());
    if (!rootPath.empty() && path.size() > rootPath.size() && path.starts_with(rootPath) && path[rootPath.size()] == '.')
        path.remove_prefix(rootPath.size() + 1);
    const auto dot = path.find('.');
    const auto array = path.find(".Array");
    const auto end = (std::min)(
        dot == std::string_view::npos ? path.size() : dot,
        array == std::string_view::npos ? path.size() : array);
    return std::string(path.substr(0u, end));
}
} // namespace

std::shared_ptr<NativeReflectionSerializedBackend> NativeReflectionSerializedBackend::Create()
{
    return std::make_shared<NativeReflectionSerializedBackend>();
}

Object* NativeReflectionSerializedBackend::Resolve(const SerializedTargetHandle& target)
{
    if (!target.IsObjectTarget())
        return nullptr;
    if (target.objectIdentifier.IsValid())
    {
        Engine::Serialize::InstanceID instanceId = Engine::Serialize::InstanceID_None;
        if (!Engine::Serialize::PersistentManager::Instance().TryGetInstanceID(
                target.objectIdentifier, instanceId))
            return nullptr;
        return Object::IDToPointer(instanceId);
    }
    return target.instanceId == Engine::Serialize::InstanceID_None ? nullptr : Object::IDToPointer(target.instanceId);
}

Engine::Serialize::TypeTreeResult NativeReflectionSerializedBackend::BuildTypeTree(
    const SerializedTargetSet& targets,
    const SerializedInspectorMode mode) const
{
    if (targets.Empty())
        return {nullptr, "SerializedObject requires at least one reflected target."};
    auto* first = Resolve(targets.targets.front());
    if (first == nullptr)
        return {nullptr, "Serialized reflected target is no longer valid."};
    const auto type = first->GetType();
    for (const auto& target : targets.targets)
    {
        const auto* object = Resolve(target);
        if (object == nullptr || !SameType(object->GetType(), type))
            return {nullptr, "Multi-object SerializedObject requires compatible reflected target types."};
    }
    auto tree = NativeSerializedStreamCodec::BuildTypeTree(
        type, {"", mode != SerializedInspectorMode::Normal});
    if (tree == nullptr)
        return {nullptr, "Failed to build the reflected SerializedObject TypeTree."};
    return {std::move(tree), {}};
}

Engine::Serialize::StreamResult NativeReflectionSerializedBackend::SerializeTarget(
    const SerializedTargetHandle& target,
    std::shared_ptr<const Engine::Serialize::SerializedTypeTree> schema,
    SerializedReferenceTable& references) const
{
    auto* object = Resolve(target);
    if (object == nullptr || schema == nullptr)
        return {nullptr, "Serialized reflected target is no longer valid."};
    meta::Variant source(object, meta::variant_policy::WrapObject{});
    std::vector<uint8_t> bytes;
    std::string error;
    if (!NativeSerializedStreamCodec::EncodeVariant(source, *schema, references, bytes, &error))
        return {nullptr, error.empty() ? "Failed to encode reflected target." : std::move(error)};
    auto stream = std::make_shared<SerializedByteStream>(std::move(schema));
    stream->SetBytes(std::move(bytes));
    stream->SetReferences(std::make_shared<SerializedReferenceTable>(references));
    return {std::move(stream), {}};
}

ValidationResult NativeReflectionSerializedBackend::ValidateAndNormalize(
    const SerializedTargetHandle& target,
    const std::span<const SerializedPropertyPath> modifiedPaths,
    SerializedByteStream& stream) const
{
    const auto* object = Resolve(target);
    if (object == nullptr || stream.GetSchema() == nullptr)
        return ValidationResult::Failure("Serialized reflected target is no longer valid.");
    for (const auto& path : modifiedPaths)
    {
        const auto field = object->GetType().GetField(RootFieldName(*stream.GetSchema(), path.ToString()));
        if (!field.IsValid() || field.IsReadOnly() || !NativeSerializedStreamCodec::ResolveCursor(std::make_shared<SerializedByteStream>(stream), path.ToString()).has_value())
        {
            return ValidationResult::Failure(
                "Serialized reflected property is no longer writable: " + path.ToString());
        }
    }
    return ValidationResult::Success();
}

ApplyResult NativeReflectionSerializedBackend::DeserializeTarget(
    const SerializedTargetHandle& target,
    const SerializedByteStream& stream,
    const Undo::UndoExecutionContext&) const
{
    auto* object = Resolve(target);
    const auto schema = stream.GetSchema();
    if (object == nullptr || schema == nullptr)
        return ApplyResult::Failure("Serialized reflected target is no longer valid.");

    struct PendingField
    {
        std::string name;
        meta::Variant value;
    };
    std::vector<PendingField> pending;
    const auto references = stream.GetReferences() != nullptr ? *stream.GetReferences() : SerializedReferenceTable{};
    auto streamCopy = std::make_shared<SerializedByteStream>(stream);
    for (auto node = schema->GetFirstChild(schema->GetRoot()); node != nullptr;
         node = schema->GetNextSibling(node->id))
    {
        const auto field = object->GetType().GetField(node->serializedName);
        if (!field.IsValid() || field.IsReadOnly())
            continue;
        const auto cursor = NativeSerializedStreamCodec::ResolveCursor(streamCopy, schema->GetPath(node->id));
        const auto valueTree = NativeSerializedStreamCodec::BuildTypeTree(field.GetType(), {"value", false});
        if (!cursor.has_value() || valueTree == nullptr)
            return ApplyResult::Failure("Serialized reflected field is unavailable: " + node->serializedName);
        std::vector<uint8_t> bytes(cursor->GetByteSize());
        std::string error;
        if (!cursor->Read(bytes.data(), bytes.size()))
            return ApplyResult::Failure("Serialized reflected field is truncated: " + node->serializedName);
        const auto decoded = NativeSerializedStreamCodec::DecodeVariant(
            bytes, *valueTree, field.GetType(), references, &error, cursor->GetByteOffset());
        if (!decoded.has_value() || !decoded->IsValid())
            return ApplyResult::Failure(error.empty() ? "Failed to decode reflected field." : error);
        pending.push_back({node->serializedName, *decoded});
    }

    meta::Variant instance(object, meta::variant_policy::WrapObject{});
    std::vector<PendingField> before;
    before.reserve(pending.size());
    for (const auto& value : pending)
    {
        const auto field = object->GetType().GetField(value.name);
        before.push_back({value.name, field.GetValue(instance)});
        if (!field.SetValue(instance, value.value))
        {
            for (auto rollback = before.rbegin(); rollback != before.rend(); ++rollback)
                static_cast<void>(object->GetType().GetField(rollback->name).SetValue(instance, rollback->value));
            return ApplyResult::Failure("Serialized reflected field rejected the decoded value: " + value.name);
        }
    }
    return ApplyResult::Success();
}

SerializedVersionStamp NativeReflectionSerializedBackend::GetVersion(
    const SerializedTargetHandle& target) const
{
    const auto* object = Resolve(target);
    if (object == nullptr)
        return {};
    const auto typeHash = static_cast<uint64_t>(
        std::hash<std::string>{}(object->GetType().GetName()));
    const auto instance = static_cast<uint64_t>(object->GetInstanceID());
    return {instance ^ (typeHash << 1u), typeHash, typeHash};
}
} // namespace NLS::Editor::Serialization
