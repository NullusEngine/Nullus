#include "Serialization/ProjectSettingsSerializedBackend.h"

#include <algorithm>
#include <functional>
#include <mutex>

#include <Reflection/Field.h>
#include <Serialize/NativeSerializedStream.h>

#include "Settings/EditorSettingObject.h"
#include "Settings/EditorSettingsRegistry.h"

namespace NLS::Editor::Serialization
{
namespace
{
using Engine::Serialize::NativeSerializedStreamCodec;
using Engine::Serialize::SerializedByteStream;
using Engine::Serialize::SerializedReferenceTable;

std::mutex g_settingsRegistryMutex;
Settings::EditorSettingsRegistry* g_settingsRegistry = nullptr;
uint64_t g_settingsRegistryRevision = 1u;

bool SameType(const meta::Type& left, const meta::Type& right)
{
    return left.IsValid() && right.IsValid() && (left == right || left.GetID() == right.GetID());
}

std::string RootFieldName(const std::string_view path)
{
    const auto dot = path.find('.');
    const auto array = path.find(".Array");
    const auto end = (std::min)(
        dot == std::string_view::npos ? path.size() : dot,
        array == std::string_view::npos ? path.size() : array);
    return std::string(path.substr(0u, end));
}

uint64_t HashCombine(const uint64_t seed, const uint64_t value)
{
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U));
}

uint64_t HashSettingValues(const Settings::EditorSettingObject& setting)
{
    auto instance = setting.makeVariant();
    if (!instance.IsValid())
        return 0u;

    uint64_t hash = static_cast<uint64_t>(std::hash<std::string> {}(setting.id));
    for (const auto& field : setting.type.GetFields())
    {
        if (!field.IsValid())
            continue;
        hash = HashCombine(hash, static_cast<uint64_t>(std::hash<std::string> {}(field.GetName())));
        const auto value = field.GetValue(instance);
        hash = HashCombine(hash, value.IsValid()
            ? static_cast<uint64_t>(std::hash<std::string> {}(value.ToString()))
            : 0u);
    }
    return hash;
}
} // namespace

std::shared_ptr<ProjectSettingsSerializedBackend> ProjectSettingsSerializedBackend::Create()
{
    return std::make_shared<ProjectSettingsSerializedBackend>();
}

bool ProjectSettingsSerializedBackend::Supports(const SerializedTargetSet& targets)
{
    if (targets.Empty())
        return false;

    const auto* first = ResolveSetting(targets.targets.front());
    if (first == nullptr)
        return false;

    return std::all_of(targets.targets.begin(), targets.targets.end(), [first](const SerializedTargetHandle& target)
    {
        const auto* setting = ResolveSetting(target);
        return setting != nullptr && SameType(setting->type, first->type);
    });
}

void ProjectSettingsSerializedBackend::BindRegistry(Settings::EditorSettingsRegistry& registry)
{
    std::lock_guard lock(g_settingsRegistryMutex);
    g_settingsRegistry = &registry;
    ++g_settingsRegistryRevision;
}

void ProjectSettingsSerializedBackend::UnbindRegistry(const Settings::EditorSettingsRegistry& registry)
{
    std::lock_guard lock(g_settingsRegistryMutex);
    if (g_settingsRegistry == &registry)
    {
        g_settingsRegistry = nullptr;
        ++g_settingsRegistryRevision;
    }
}

const Settings::EditorSettingObject* ProjectSettingsSerializedBackend::ResolveSetting(
    const SerializedTargetHandle& target)
{
    if (target.kind != Inspector::InspectorTargetKind::ProjectSettings || target.resourcePath.empty())
        return nullptr;

    std::lock_guard lock(g_settingsRegistryMutex);
    return g_settingsRegistry != nullptr ? g_settingsRegistry->Find(target.resourcePath) : nullptr;
}

uint64_t ProjectSettingsSerializedBackend::GetRegistryRevision()
{
    std::lock_guard lock(g_settingsRegistryMutex);
    return g_settingsRegistryRevision;
}

Engine::Serialize::TypeTreeResult ProjectSettingsSerializedBackend::BuildTypeTree(
    const SerializedTargetSet& targets,
    const SerializedInspectorMode mode) const
{
    if (!Supports(targets))
        return {nullptr, "Project Settings targets are unavailable or have incompatible schemas."};

    const auto* setting = ResolveSetting(targets.targets.front());
    auto tree = setting == nullptr ? nullptr : NativeSerializedStreamCodec::BuildTypeTree(
        setting->type, {"", mode != SerializedInspectorMode::Normal});
    if (tree != nullptr && tree->GetRoot() != 0u)
    {
        // The stream root is structural only. Keeping its path empty makes
        // public settings paths match the external editor's direct field form ("field"),
        // rather than exposing the codec's internal "root.field" prefix.
        tree->GetNodes()[tree->GetRoot() - 1u].serializedName.clear();
        tree->RebuildIndexes();
    }
    return tree == nullptr
        ? Engine::Serialize::TypeTreeResult {nullptr, "Failed to build the Project Settings TypeTree."}
        : Engine::Serialize::TypeTreeResult {std::move(tree), {}};
}

Engine::Serialize::StreamResult ProjectSettingsSerializedBackend::SerializeTarget(
    const SerializedTargetHandle& target,
    std::shared_ptr<const Engine::Serialize::SerializedTypeTree> schema,
    SerializedReferenceTable& references) const
{
    const auto* setting = ResolveSetting(target);
    if (setting == nullptr || schema == nullptr)
        return {nullptr, "Project Settings target is no longer valid."};

    const auto source = setting->makeVariant();
    if (!source.IsValid())
        return {nullptr, "Project Settings target instance is unavailable."};

    std::vector<uint8_t> bytes;
    std::string error;
    if (!NativeSerializedStreamCodec::EncodeVariant(source, *schema, references, bytes, &error))
        return {nullptr, error.empty() ? "Failed to serialize Project Settings target." : std::move(error)};

    auto stream = std::make_shared<SerializedByteStream>(std::move(schema));
    stream->SetBytes(std::move(bytes));
    stream->SetReferences(std::make_shared<SerializedReferenceTable>(references));
    return {std::move(stream), {}};
}

ValidationResult ProjectSettingsSerializedBackend::ValidateAndNormalize(
    const SerializedTargetHandle& target,
    const std::span<const SerializedPropertyPath> modifiedPaths,
    SerializedByteStream& stream) const
{
    const auto* setting = ResolveSetting(target);
    if (setting == nullptr || stream.GetSchema() == nullptr)
        return ValidationResult::Failure("Project Settings target is no longer valid.");

    for (const auto& path : modifiedPaths)
    {
        const auto field = setting->type.GetField(RootFieldName(path.ToString()));
        if (!field.IsValid() || field.IsReadOnly() || !NativeSerializedStreamCodec::ResolveCursor(
                std::make_shared<SerializedByteStream>(stream), path.ToString()).has_value())
        {
            return ValidationResult::Failure(
                "Project Settings property is no longer writable: " + path.ToString());
        }
    }
    return ValidationResult::Success();
}

ApplyResult ProjectSettingsSerializedBackend::DeserializeTarget(
    const SerializedTargetHandle& target,
    const SerializedByteStream& stream,
    const Undo::UndoExecutionContext&) const
{
    const auto* setting = ResolveSetting(target);
    const auto schema = stream.GetSchema();
    if (setting == nullptr || schema == nullptr)
        return ApplyResult::Failure("Project Settings target is no longer valid.");

    struct PendingField { std::string name; meta::Variant value; };
    std::vector<PendingField> pending;
    const auto references = stream.GetReferences() != nullptr
        ? *stream.GetReferences() : SerializedReferenceTable {};
    auto streamCopy = std::make_shared<SerializedByteStream>(stream);
    for (auto node = schema->GetFirstChild(schema->GetRoot()); node != nullptr;
         node = schema->GetNextSibling(node->id))
    {
        const auto field = setting->type.GetField(node->serializedName);
        if (!field.IsValid() || field.IsReadOnly())
            continue;

        const auto cursor = NativeSerializedStreamCodec::ResolveCursor(streamCopy, node->serializedName);
        const auto valueTree = NativeSerializedStreamCodec::BuildTypeTree(field.GetType(), {"value", false});
        if (!cursor.has_value() || valueTree == nullptr)
            return ApplyResult::Failure("Project Settings field is unavailable: " + node->serializedName);

        std::vector<uint8_t> bytes(cursor->GetByteSize());
        std::string error;
        if (!cursor->Read(bytes.data(), bytes.size()))
            return ApplyResult::Failure("Project Settings field is truncated: " + node->serializedName);
        const auto decoded = NativeSerializedStreamCodec::DecodeVariant(
            bytes, *valueTree, field.GetType(), references, &error, cursor->GetByteOffset());
        if (!decoded.has_value() || !decoded->IsValid())
            return ApplyResult::Failure(error.empty() ? "Failed to decode Project Settings field." : error);
        pending.push_back({node->serializedName, *decoded});
    }

    auto instance = setting->makeVariant();
    if (!instance.IsValid())
        return ApplyResult::Failure("Project Settings target instance is unavailable.");

    std::vector<PendingField> before;
    before.reserve(pending.size());
    for (const auto& value : pending)
    {
        const auto field = setting->type.GetField(value.name);
        before.push_back({value.name, field.GetValue(instance)});
        if (!field.SetValue(instance, value.value))
        {
            for (auto rollback = before.rbegin(); rollback != before.rend(); ++rollback)
                static_cast<void>(setting->type.GetField(rollback->name).SetValue(instance, rollback->value));
            return ApplyResult::Failure("Project Settings field rejected the decoded value: " + value.name);
        }
    }
    return ApplyResult::Success();
}

SerializedVersionStamp ProjectSettingsSerializedBackend::GetVersion(const SerializedTargetHandle& target) const
{
    const auto* setting = ResolveSetting(target);
    if (setting == nullptr)
        return {};

    const auto schema = HashCombine(
        GetRegistryRevision(),
        static_cast<uint64_t>(std::hash<std::string> {}(setting->type.GetName())));
    return {HashSettingValues(*setting), schema, schema};
}
} // namespace NLS::Editor::Serialization
