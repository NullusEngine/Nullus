#include "Serialization/SerializedObject.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <iterator>
#include <unordered_set>

#include <Core/ServiceLocator.h>
#include <Reflection/Argument.h>
#include <Reflection/ArrayWrapper.h>
#include <Reflection/Enum.h>

#include "Undo/EditorUndoMutationContext.h"

namespace NLS::Editor::Serialization
{
namespace
{
using Inspector::InspectorTargetHandle;
using Inspector::PropertyPath;
using Inspector::PropertyValueState;

struct ArrayElementAddress
{
    PropertyPath parent;
    size_t index = 0u;
};

std::optional<ArrayElementAddress> ParseArrayElementPath(const std::string& path)
{
    constexpr std::string_view marker = ".Array.data[";
    const auto markerPosition = path.rfind(marker);
    size_t prefixLength = marker.size();
    size_t indexPosition = markerPosition;
    if (markerPosition == std::string::npos)
    {
        const auto open = path.rfind('[');
        if (open == std::string::npos)
            return std::nullopt;
        indexPosition = open;
        prefixLength = 1u;
    }
    if (indexPosition == std::string::npos || path.empty() || path.back() != ']')
        return std::nullopt;
    const auto digitsBegin = indexPosition + prefixLength;
    if (digitsBegin >= path.size() - 1u)
        return std::nullopt;
    const auto digits = std::string_view(path).substr(digitsBegin, path.size() - digitsBegin - 1u);
    size_t index = 0u;
    for (const auto character : digits)
    {
        if (character < '0' || character > '9')
            return std::nullopt;
        index = index * 10u + static_cast<size_t>(character - '0');
    }
    return ArrayElementAddress {PropertyPath(path.substr(0u, indexPosition)), index};
}

const SerializedSchema& EmptySchema()
{
    static const SerializedSchema value;
    return value;
}

const SerializedTargetSet& EmptyTargets()
{
    static const SerializedTargetSet value;
    return value;
}

const meta::Type& EmptyType()
{
    static const meta::Type value;
    return value;
}

const Inspector::EditorPropertyMetadata& EmptyMetadata()
{
    static const Inspector::EditorPropertyMetadata value;
    return value;
}

bool IsDescendant(const PropertyPath& path, const PropertyPath& parent)
{
    if (parent.Empty() || path == parent)
        return false;
    const auto& value = path.ToString();
    const auto& prefix = parent.ToString();
    return value.size() > prefix.size() &&
        value.compare(0u, prefix.size(), prefix) == 0 &&
        (value[prefix.size()] == '.' || value[prefix.size()] == '[');
}


bool EquivalentVariant(const meta::Variant& left, const meta::Variant& right)
{
    if (!left.IsValid() || !right.IsValid())
        return !left.IsValid() && !right.IsValid();
    const auto leftSnapshot = SerializedValue::Capture(left);
    const auto rightSnapshot = SerializedValue::Capture(right);
    return leftSnapshot.has_value() && rightSnapshot.has_value() &&
        leftSnapshot->Equivalent(*rightSnapshot);
}

bool IsCompatibleType(const meta::Type& expected, const meta::Type& actual)
{
    if (!expected.IsValid() || !actual.IsValid())
        return false;
    if (actual == expected || actual.GetName() == expected.GetName())
        return true;
    if (expected.IsArray() && actual.IsArray())
        return IsCompatibleType(expected.GetArrayType(), actual.GetArrayType());
    if (expected.IsFloatingPoint())
        return actual.IsFloatingPoint();
    return expected.IsPrimitive() && actual.IsPrimitive() &&
        !expected.IsFloatingPoint() && !actual.IsFloatingPoint() &&
        expected != NLS_TYPEOF(bool) && actual != NLS_TYPEOF(bool);
}

std::string MakeTargetKey(const InspectorTargetHandle& target)
{
    if (target.objectIdentifier.IsValid())
    {
        if (target.objectIdentifier.IsLocalObject())
            return "object:" + std::to_string(target.objectIdentifier.localIdentifierInFile);
        return "asset-object:" + target.objectIdentifier.guid.ToString() + ":" +
            std::to_string(target.objectIdentifier.localIdentifierInFile);
    }
    if (!target.assetId.empty())
        return "asset:" + target.assetId;
    if (!target.resourcePath.empty())
        return "path:" + target.resourcePath;
    return {};
}

Undo::UndoScopeId MakeScope(const InspectorTargetHandle& target)
{
    const auto kind = target.kind == Inspector::InspectorTargetKind::ProjectSettings
        ? Undo::UndoScopeKind::ProjectSettings
        : (target.kind == Inspector::InspectorTargetKind::AssetImporter ||
           target.kind == Inspector::InspectorTargetKind::Material)
            ? Undo::UndoScopeKind::Asset
            : Undo::UndoScopeKind::Scene;
    return {kind, MakeTargetKey(target), 0u};
}

InspectorTargetHandle MakeDurableTarget(InspectorTargetHandle target)
{
    target.instanceId = InstanceID_None;
    return target;
}

} // namespace

std::string EditorExposedReferenceResolver::MakeKey(
    const InspectorTargetHandle& context,
    const std::string_view exposedName)
{
    std::string stableContext;
    if (context.objectIdentifier.IsValid())
    {
        stableContext = context.objectIdentifier.IsLocalObject()
            ? "object:" + std::to_string(context.objectIdentifier.localIdentifierInFile)
            : "asset-object:" + context.objectIdentifier.guid.ToString() + ":" +
                std::to_string(context.objectIdentifier.localIdentifierInFile);
    }
    else if (!context.assetId.empty())
        stableContext = "asset:" + context.assetId;
    else if (!context.resourcePath.empty())
        stableContext = "path:" + context.resourcePath;
    else
        return {};
    return stableContext + "|" + std::string(exposedName);
}

void EditorExposedReferenceResolver::Bind(
    const InspectorTargetHandle& context,
    std::string exposedName,
    InspectorTargetHandle target)
{
    const auto key = MakeKey(context, exposedName);
    if (key.empty() || exposedName.empty() || !target.IsValid())
        return;
    // Resolver entries outlive a frame. InstanceID is only a transient lookup
    // cache and must never become the retained identity for an exposed target.
    target.instanceId = InstanceID_None;
    std::scoped_lock lock(m_mutex);
    m_entries.insert_or_assign(key, std::move(target));
}

void EditorExposedReferenceResolver::Unbind(
    const InspectorTargetHandle& context,
    const std::string_view exposedName)
{
    const auto key = MakeKey(context, exposedName);
    if (key.empty())
        return;
    std::scoped_lock lock(m_mutex);
    m_entries.erase(key);
}

void EditorExposedReferenceResolver::ClearContext(const InspectorTargetHandle& context)
{
    const auto prefix = MakeKey(context, {});
    if (prefix.empty())
        return;
    std::scoped_lock lock(m_mutex);
    for (auto iterator = m_entries.begin(); iterator != m_entries.end();)
    {
        if (iterator->first.rfind(prefix, 0u) == 0u)
            iterator = m_entries.erase(iterator);
        else
            ++iterator;
    }
}

std::optional<InspectorTargetHandle> EditorExposedReferenceResolver::Resolve(
    const InspectorTargetHandle& context,
    const std::string_view exposedName) const
{
    const auto key = MakeKey(context, exposedName);
    if (key.empty())
        return std::nullopt;
    std::scoped_lock lock(m_mutex);
    const auto found = m_entries.find(key);
    return found == m_entries.end() ? std::nullopt : std::optional<InspectorTargetHandle>(found->second);
}

bool EditorExposedReferenceResolver::Set(
    const InspectorTargetHandle& context,
    const std::string_view exposedName,
    std::optional<InspectorTargetHandle> target) const
{
    const auto key = MakeKey(context, exposedName);
    if (key.empty())
        return false;
    std::scoped_lock lock(m_mutex);
    if (!target.has_value())
    {
        m_entries.erase(key);
        return true;
    }
    if (!target->IsValid())
        return false;
    // A resolver is a durable editor table. Retain no transient InstanceID
    // cache across frames or object reloads.
    target->instanceId = InstanceID_None;
    m_entries.insert_or_assign(key, std::move(*target));
    return true;
}

EditorExposedReferenceResolver& GetEditorExposedReferenceResolver()
{
    static EditorExposedReferenceResolver resolver;
    return resolver;
}

struct SerializedObject::State
{
    SerializedTargetSet targets;
    std::optional<SerializedTargetHandle> context;
    std::shared_ptr<ISerializedTargetBackend> backend;
    std::shared_ptr<const IExposedReferenceResolver> exposedReferenceResolver;
    SerializedSchema schema;
    std::shared_ptr<Engine::Serialize::SerializedByteStream> stagedStream;
    std::vector<std::shared_ptr<Engine::Serialize::SerializedByteStream>> targetStreams;
    SerializedVersionStamp version;
    uint64_t generation = 0u;
    uint64_t localStructureGeneration = 0u;
    SerializedInspectorMode inspectorMode = SerializedInspectorMode::Normal;
    size_t maxArraySizeForMultiEditing = 64u;
    bool valid = false;
    bool modified = false;
    std::unordered_set<std::string> modifiedPaths;
    std::unordered_map<std::string, bool> expanded;
    // Structural array edits keep the array property itself usable while the
    // element handles that existed before the edit become stale. Full schema
    // rebuilds still invalidate every property through schemaGeneration.
    std::unordered_set<std::string> stalePropertyPaths;
    std::string lastError;
};

namespace
{


using Engine::Serialize::NativeSerializedStreamCodec;
using Engine::Serialize::SerializedByteStream;
using Engine::Serialize::SerializedNodeId;
using Engine::Serialize::SerializedNodeFlags;
using Engine::Serialize::SerializedTypeTree;
using Engine::Serialize::SerializedTypeTreeNode;
using Engine::Serialize::SerializedNodeKind;

SerializedVersionStamp ComputeVersion(SerializedObject::State& state)
{
    SerializedVersionStamp result;
    if (state.backend != nullptr)
    {
        uint64_t combined = 1469598103934665603ULL;
        bool first = true;
        for (const auto& target : state.targets.targets)
        {
            const auto targetVersion = state.backend->GetVersion(target);
            if (first)
            {
                result = targetVersion;
                first = false;
            }
            combined ^= targetVersion.dataGeneration +
                0x9e3779b97f4a7c15ULL + (combined << 6u) + (combined >> 2u);
        }
        if (!first)
            result.dataGeneration = combined;
    }
    result.schemaGeneration = state.generation;
    result.structureGeneration = std::max(result.structureGeneration, state.localStructureGeneration);
    return result;
}


bool EquivalentStreams(
    const std::shared_ptr<NLS::Engine::Serialize::SerializedByteStream>& left,
    const std::shared_ptr<NLS::Engine::Serialize::SerializedByteStream>& right)
{
    if (left == nullptr || right == nullptr)
        return left == right;
    const auto leftSchema = left->GetSchema();
    const auto rightSchema = right->GetSchema();
    const auto leftReferences = left->GetReferences();
    const auto rightReferences = right->GetReferences();
    return leftSchema != nullptr && rightSchema != nullptr &&
        leftSchema->GetSchemaHash() == rightSchema->GetSchemaHash() &&
        left->GetBytes() == right->GetBytes() &&
        ((leftReferences == nullptr && rightReferences == nullptr) ||
            (leftReferences != nullptr && rightReferences != nullptr &&
                leftReferences->GetEntries() == rightReferences->GetEntries()));
}

std::shared_ptr<SerializedByteStream> CloneStream(
    const std::shared_ptr<SerializedByteStream>& source)
{
    if (source == nullptr)
        return {};
    auto result = std::make_shared<SerializedByteStream>(*source);
    if (const auto& references = source->GetReferences(); references != nullptr)
    {
        auto copiedReferences = std::make_shared<Engine::Serialize::SerializedReferenceTable>();
        copiedReferences->ReplaceEntries(references->CopyEntries());
        result->SetReferences(std::move(copiedReferences));
    }
    return result;
}

std::vector<SerializedPropertyPath> GetMinimalModifiedPaths(const SerializedObject::State& state)
{
    std::vector<SerializedPropertyPath> paths;
    paths.reserve(state.modifiedPaths.size());
    for (const auto& path : state.modifiedPaths)
        paths.emplace_back(path);

    std::sort(paths.begin(), paths.end(), [](const auto& left, const auto& right)
    {
        const auto leftText = left.ToString();
        const auto rightText = right.ToString();
        if (IsDescendant(PropertyPath(rightText), PropertyPath(leftText)))
            return true;
        if (IsDescendant(PropertyPath(leftText), PropertyPath(rightText)))
            return false;
        return leftText < rightText;
    });
    paths.erase(std::unique(paths.begin(), paths.end(), [](const auto& left, const auto& right)
    {
        return left.ToString() == right.ToString();
    }), paths.end());
    paths.erase(std::remove_if(paths.begin(), paths.end(), [&paths](const auto& path)
    {
        return std::any_of(paths.begin(), paths.end(), [&path](const auto& candidate)
        {
            return candidate.ToString() != path.ToString() &&
                IsDescendant(path, candidate);
        });
    }), paths.end());
    return paths;
}

bool CopyStreamPath(
    const std::shared_ptr<SerializedByteStream>& source,
    const std::shared_ptr<SerializedByteStream>& destination,
    const SerializedPropertyPath& path,
    std::string& error)
{
    if (source == nullptr || destination == nullptr ||
        source->GetSchema() == nullptr || destination->GetSchema() == nullptr ||
        source->GetSchema()->GetSchemaHash() != destination->GetSchema()->GetSchemaHash())
    {
        error = "Serialized streams have incompatible schemas.";
        return false;
    }
    const auto sourceCursor = NativeSerializedStreamCodec::ResolveCursor(source, path.ToString());
    const auto destinationCursor = NativeSerializedStreamCodec::ResolveCursor(destination, path.ToString());
    if (!sourceCursor.has_value() || !destinationCursor.has_value())
    {
        error = "Modified serialized property is no longer available: " + path.ToString();
        return false;
    }
    const auto slice = sourceCursor->Capture(source->GetSchema()->GetTypeSignature());
    if (!slice.has_value() || !slice->IsValid())
    {
        error = "Failed to capture modified serialized property: " + path.ToString();
        return false;
    }
    if (destination->GetReferences() == nullptr)
        destination->SetReferences(std::make_shared<Engine::Serialize::SerializedReferenceTable>());
    auto bytes = slice->bytes;
    auto& destinationReferences = *destination->GetReferences();
    for (const auto& relocation : slice->referenceRelocations)
    {
        if (relocation.referenceSlot == 0u || relocation.referenceSlot > slice->references.size() ||
            relocation.byteOffset > bytes.size() || sizeof(uint32_t) > bytes.size() - relocation.byteOffset)
        {
            error = "Modified serialized property contains an invalid object reference.";
            return false;
        }
        const auto replacementSlot = destinationReferences.Intern(
            slice->references[relocation.referenceSlot - 1u]);
        std::memcpy(bytes.data() + relocation.byteOffset, &replacementSlot, sizeof(replacementSlot));
    }
    if (!destination->Replace(destinationCursor->GetByteOffset(), destinationCursor->GetByteSize(), bytes))
    {
        error = "Failed to write modified serialized property: " + path.ToString();
        return false;
    }
    return true;
}

bool AppendDefaultStreamValue(
    const Engine::Serialize::SerializedTypeTree& tree,
    const Engine::Serialize::SerializedTypeTreeNode& node,
    std::vector<uint8_t>& output)
{
    const auto start = output.size();
    if (Engine::Serialize::HasSerializedNodeFlag(node.flags, SerializedNodeFlags::Array))
    {
        const uint32_t count = 0u;
        const auto* bytes = reinterpret_cast<const uint8_t*>(&count);
        output.insert(output.end(), bytes, bytes + sizeof(count));
    }
    else if (node.kind == SerializedNodeKind::String || node.fixedByteSize < 0 &&
        tree.GetFirstChild(node.id) == nullptr)
    {
        const uint32_t length = 0u;
        const auto* bytes = reinterpret_cast<const uint8_t*>(&length);
        output.insert(output.end(), bytes, bytes + sizeof(length));
    }
    else if (node.fixedByteSize >= 0)
    {
        output.resize(output.size() + static_cast<size_t>(node.fixedByteSize), 0u);
    }
    else
    {
        for (auto* child = tree.GetFirstChild(node.id); child != nullptr;
             child = tree.GetNextSibling(child->id))
        {
            if (!AppendDefaultStreamValue(tree, *child, output))
                return false;
        }
    }
    if (Engine::Serialize::HasSerializedNodeFlag(node.flags, SerializedNodeFlags::Align4After))
    {
        const auto length = output.size() - start;
        const auto alignedLength = (length + 3u) & ~size_t {3u};
        output.resize(start + alignedLength, 0u);
    }
    return true;
}

bool MutateStreamArray(
    SerializedObject::State& state,
    const SerializedPropertyPath& path,
    const bool invalidateElements,
    const std::function<bool(
        std::vector<std::vector<uint8_t>>&,
        const SerializedTypeTree&,
        const SerializedTypeTreeNode&)>& operation)
{
    if (state.stagedStream == nullptr || state.stagedStream->GetSchema() == nullptr)
        return false;
    const auto& tree = *state.stagedStream->GetSchema();
    const auto arrayCursor = NativeSerializedStreamCodec::ResolveCursor(state.stagedStream, path.ToString());
    if (!arrayCursor.has_value())
        return false;
    const auto* array = tree.FindNode(arrayCursor->GetNodeId());
    const auto* element = array != nullptr ? tree.GetFirstChild(array->id) : nullptr;
    if (array == nullptr || element == nullptr ||
        !Engine::Serialize::HasSerializedNodeFlag(array->flags, SerializedNodeFlags::Array) ||
        Engine::Serialize::HasSerializedNodeFlag(array->flags, SerializedNodeFlags::FixedBuffer))
    {
        return false;
    }

    uint32_t count = 0u;
    if (!arrayCursor->Read(&count, sizeof(count)))
        return false;
    std::vector<std::vector<uint8_t>> elements;
    elements.reserve(count);
    for (uint32_t index = 0u; index < count; ++index)
    {
        const auto elementPath = path.ToString() + ".Array.data[" + std::to_string(index) + "]";
        const auto cursor = NativeSerializedStreamCodec::ResolveCursor(state.stagedStream, elementPath);
        if (!cursor.has_value())
            return false;
        std::vector<uint8_t> bytes(cursor->GetByteSize());
        if (!cursor->Read(bytes.data(), bytes.size()))
            return false;
        elements.push_back(std::move(bytes));
    }
    const auto originalCount = elements.size();
    if (!operation(elements, tree, *element) || elements.size() > std::numeric_limits<uint32_t>::max())
        return false;

    std::vector<uint8_t> replacement;
    replacement.reserve(arrayCursor->GetByteSize());
    const auto replacementCount = static_cast<uint32_t>(elements.size());
    const auto* countBytes = reinterpret_cast<const uint8_t*>(&replacementCount);
    replacement.insert(replacement.end(), countBytes, countBytes + sizeof(replacementCount));
    for (const auto& bytes : elements)
        replacement.insert(replacement.end(), bytes.begin(), bytes.end());
    if (Engine::Serialize::HasSerializedNodeFlag(array->flags, SerializedNodeFlags::Align4After))
        replacement.resize((replacement.size() + 3u) & ~size_t {3u}, 0u);
    if (!state.stagedStream->Replace(
            arrayCursor->GetByteOffset(), arrayCursor->GetByteSize(), replacement))
    {
        return false;
    }
    if (invalidateElements || elements.size() != originalCount)
    {
        state.stagedStream->MarkDataChanged(false, true);
        for (size_t index = 0u; index < originalCount; ++index)
            state.stalePropertyPaths.insert(path.ToString() + ".Array.data[" + std::to_string(index) + "]");
        state.localStructureGeneration = std::max(
            state.localStructureGeneration,
            state.version.structureGeneration) + 1u;
        // Properties carry the structure generation observed at creation. Keep
        // the owner stamp current while a staged array mutation is pending so
        // pre-existing Array.data[n] cursors are rejected immediately, rather
        // than only after the next Update()/Apply cycle.
        state.version = ComputeVersion(state);
    }
    state.modifiedPaths.insert(path.ToString());
    state.modified = true;
    return true;
}

SerializedStreamChangeSet BuildStreamChangeSet(SerializedObject::State& state)
{
    SerializedStreamChangeSet result;
    result.modifiedPaths = GetMinimalModifiedPaths(state);

    for (size_t index = 0u; index < state.targets.targets.size(); ++index)
    {
        const auto before = index < state.targetStreams.size()
            ? state.targetStreams[index]
            : std::shared_ptr<NLS::Engine::Serialize::SerializedByteStream> {};
        auto after = CloneStream(before);
        if (after != nullptr)
        {
            for (const auto& path : result.modifiedPaths)
            {
                std::string error;
                if (!CopyStreamPath(state.stagedStream, after, path, error))
                {
                    state.lastError = error.empty()
                        ? "Failed to copy the staged serialized property into every target."
                        : std::move(error);
                    return {};
                }
            }
        }
        if (EquivalentStreams(before, after))
            continue;

        SerializedTargetStreamChange change;
        change.target = MakeDurableTarget(state.targets.targets[index]);
        change.before = CloneStream(before);
        change.after = after;
        result.targets.push_back(std::move(change));
    }
    return result;
}

class SerializedStreamChangeCommand final : public Undo::IEditorUndoCommand
{
public:
    SerializedStreamChangeCommand(
        Undo::UndoCommandInfo info,
        std::shared_ptr<ISerializedTargetBackend> backend,
        SerializedStreamChangeSet changes)
        : m_info(std::move(info))
        , m_backend(std::move(backend))
        , m_changes(std::move(changes))
    {
        m_info.estimatedBytes = std::max(m_info.estimatedBytes, m_changes.EstimatedBytes());
    }

    Undo::UndoCommandInfo GetInfo() const override
    {
        return m_info;
    }

    Undo::UndoValidationResult Validate(
        const Undo::UndoDirection direction,
        const Undo::UndoExecutionContext& context) const override
    {
        if (!context.IsOperationCurrent())
            return Undo::UndoValidationResult::Stale("Serialized operation is no longer current.");
        if (m_backend == nullptr)
            return Undo::UndoValidationResult::Failed("Serialized backend is unavailable.");
        for (const auto& change : m_changes.targets)
        {
            if (!change.target.IsValid())
                return Undo::UndoValidationResult::Stale("A serialized target is no longer valid.");
            if (change.target.IsObjectTarget() && !change.target.objectIdentifier.IsValid())
                return Undo::UndoValidationResult::Stale("Serialized object target has no persistent identity.");
            const auto& source = direction == Undo::UndoDirection::Redo
                ? change.after
                : change.before;
            if (source == nullptr)
                return Undo::UndoValidationResult::Failed("Serialized stream snapshot is unavailable.");
            auto candidate = CloneStream(source);
            const auto validation = m_backend->ValidateAndNormalize(
                change.target,
                m_changes.modifiedPaths,
                *candidate);
            if (!validation.valid)
                return Undo::UndoValidationResult::Failed(validation.error);
        }
        return Undo::UndoValidationResult::Ready();
    }

    Undo::UndoStartResult Apply(
        const Undo::UndoDirection direction,
        const Undo::UndoExecutionContext& context,
        Undo::UndoCompletion) override
    {
        if (m_backend == nullptr)
            return Undo::UndoStartResult::Failed("Serialized backend is unavailable.");
        std::vector<const SerializedTargetStreamChange*> applied;
        const auto mutation = Undo::ScopedEditorUndoMutationContext(context);
        for (const auto& change : m_changes.targets)
        {
            const auto& source = direction == Undo::UndoDirection::Redo
                ? change.after
                : change.before;
            if (source == nullptr)
                return Undo::UndoStartResult::Failed("Serialized stream snapshot is unavailable.");
            auto candidate = CloneStream(source);
            const auto validation = m_backend->ValidateAndNormalize(
                change.target,
                m_changes.modifiedPaths,
                *candidate);
            if (!validation.valid)
                return Undo::UndoStartResult::Failed(validation.error);
            const auto result = m_backend->DeserializeTarget(change.target, *candidate, context);
            if (result.succeeded)
            {
                applied.push_back(&change);
                continue;
            }
            for (auto rollback = applied.rbegin(); rollback != applied.rend(); ++rollback)
            {
                const auto& rollbackSource = direction == Undo::UndoDirection::Redo
                    ? (*rollback)->before
                    : (*rollback)->after;
                if (rollbackSource != nullptr)
                    (void)m_backend->DeserializeTarget(
                        (*rollback)->target,
                        *rollbackSource,
                        context);
            }
            return Undo::UndoStartResult::Failed(result.error);
        }
        return Undo::UndoStartResult::Completed();
    }

    bool TryMergeFrom(const Undo::IEditorUndoCommand& newer) override
    {
        const auto* other = dynamic_cast<const SerializedStreamChangeCommand*>(&newer);
        if (other == nullptr || m_info.mergeKey != other->m_info.mergeKey ||
            m_info.scopes != other->m_info.scopes)
        {
            return false;
        }
        m_changes = other->m_changes;
        m_info.estimatedBytes = m_changes.EstimatedBytes();
        return true;
    }

private:
    Undo::UndoCommandInfo m_info;
    std::shared_ptr<ISerializedTargetBackend> m_backend;
    SerializedStreamChangeSet m_changes;
};

Undo::UndoCommandInfo BuildCommandInfo(
    const SerializedObject::State& state,
    const SerializedStreamChangeSet& changes)
{
    Undo::UndoCommandInfo info;
    info.label = changes.modifiedPaths.size() == 1u
        ? "Edit " + changes.modifiedPaths.front().ToString()
        : "Edit Serialized Properties";
    info.sourceModule = "SerializedObject";
    info.allowMerge = false;
    if (!changes.modifiedPaths.empty())
        info.mergeKey = changes.modifiedPaths.front().ToString();

    for (const auto& change : changes.targets)
    {
        const auto target = MakeDurableTarget(change.target);
        auto scope = MakeScope(target);
        if (scope.IsValid())
            info.scopes.push_back(scope);
        info.affected.push_back(MakeTargetKey(target));
    }
    info.estimatedBytes = changes.EstimatedBytes();
    return info;
}

bool ApplyDirect(
    const std::shared_ptr<ISerializedTargetBackend>& backend,
    const SerializedStreamChangeSet& changes,
    const bool withoutUndo,
    std::string& error)
{
    if (backend == nullptr)
    {
        error = "Serialized backend is unavailable.";
        return false;
    }
    std::vector<const SerializedTargetStreamChange*> applied;
    for (const auto& change : changes.targets)
    {
        if (change.after == nullptr)
        {
            error = "Serialized stream snapshot is unavailable.";
            return false;
        }
        const auto scope = MakeScope(change.target);
        Undo::ScopedEditorUndoTransientMutationContext context(scope);
        const Undo::UndoExecutionContext executionContext {
            0u,
            Undo::UndoDirection::Redo,
            0u,
            scope.IsValid() ? std::vector<Undo::UndoScopeId> {scope} : std::vector<Undo::UndoScopeId> {},
            {}};
        auto candidate = CloneStream(change.after);
        const auto validation = backend->ValidateAndNormalize(
            change.target,
            changes.modifiedPaths,
            *candidate);
        if (!validation.valid)
        {
            error = validation.error;
            return false;
        }
        const auto result = backend->DeserializeTarget(change.target, *candidate, executionContext);
        if (result.succeeded)
        {
            applied.push_back(&change);
            continue;
        }
        error = result.error;
        for (auto rollback = applied.rbegin(); rollback != applied.rend(); ++rollback)
        {
            if ((*rollback)->before != nullptr)
            {
                Undo::ScopedEditorUndoTransientMutationContext rollbackContext(MakeScope((*rollback)->target));
                (void)backend->DeserializeTarget((*rollback)->target, *(*rollback)->before, executionContext);
            }
        }
        return false;
    }
    static_cast<void>(withoutUndo);
    return true;
}


} // namespace

bool SerializedValue::Equivalent(const SerializedValue& other) const
{
    if (valid != other.valid || typeName != other.typeName || typeSignature != other.typeSignature)
        return false;
    if (!valid)
        return true;
    return bytes == other.bytes && byteReferences == other.byteReferences;
}

std::optional<SerializedValue> SerializedValue::Capture(const meta::Variant& value)
{
    SerializedValue result;
    if (!value.IsValid() || !value.GetType().IsValid())
        return std::nullopt;

    const auto schema = NLS::Engine::Serialize::NativeSerializedStreamCodec::BuildTypeTree(value.GetType());
    if (schema == nullptr)
        return std::nullopt;

    NLS::Engine::Serialize::SerializedReferenceTable references;
    std::string streamError;
    if (!NLS::Engine::Serialize::NativeSerializedStreamCodec::EncodeVariant(
            value,
            *schema,
            references,
            result.bytes,
            &streamError))
    {
        return std::nullopt;
    }

    result.typeName = value.GetType().GetName();
    result.typeSignature = schema->GetSchemaHash();
    result.byteReferences = references.CopyEntries();
    result.references.reserve(result.byteReferences.size());
    for (const auto& reference : result.byteReferences)
    {
        result.references.push_back({
            {},
            reference.identifier,
            reference.resourcePath,
            reference.expectedTypeName,
            reference.isNull});
    }
    result.valid = true;
    return result;
}

std::optional<meta::Variant> SerializedValue::Resolve(const meta::Type& type) const
{
    if (!valid)
        return std::nullopt;
    if (!type.IsValid())
        return std::nullopt;
    if (type.GetName() != typeName)
        return std::nullopt;
    const auto schema = NLS::Engine::Serialize::NativeSerializedStreamCodec::BuildTypeTree(type);
    if (schema == nullptr || (typeSignature != 0u && schema->GetSchemaHash() != typeSignature))
        return std::nullopt;
    NLS::Engine::Serialize::SerializedReferenceTable referenceTable;
    referenceTable.ReplaceEntries(byteReferences);
    std::string streamError;
    return NLS::Engine::Serialize::NativeSerializedStreamCodec::DecodeVariant(
        bytes,
        *schema,
        type,
        referenceTable,
        &streamError);
}

size_t SerializedStreamChangeSet::EstimatedBytes() const
{
    size_t total = sizeof(*this);
    for (const auto& path : modifiedPaths)
        total += path.ToString().size();
    const auto estimateStream = [&total](
        const std::shared_ptr<NLS::Engine::Serialize::SerializedByteStream>& stream)
    {
        if (stream == nullptr)
            return;
        total += stream->GetBytes().size();
        if (stream->GetSchema() != nullptr)
            total += sizeof(uint64_t);
        if (stream->GetReferences() != nullptr)
        {
            for (const auto& reference : stream->GetReferences()->GetEntries())
                total += sizeof(reference) + reference.resourcePath.size() +
                    reference.expectedTypeName.size() + reference.identifier.filePath.size();
        }
    };
    for (const auto& change : targets)
    {
        total += sizeof(change.target);
        estimateStream(change.before);
        estimateStream(change.after);
    }
    return total;
}


SerializedObject::SerializedObject(
    SerializedTargetSet targets,
    std::shared_ptr<ISerializedTargetBackend> backend,
    const SerializedInspectorMode mode,
    std::optional<SerializedTargetHandle> context,
    std::shared_ptr<const IExposedReferenceResolver> exposedReferenceResolver)
    : m_state(std::make_shared<State>())
{
    Reset(std::move(targets), std::move(backend), mode, std::move(context), std::move(exposedReferenceResolver));
}

SerializedObject::~SerializedObject() = default;

void SerializedObject::Reset(
    SerializedTargetSet targets,
    std::shared_ptr<ISerializedTargetBackend> backend,
    const SerializedInspectorMode mode,
    std::optional<SerializedTargetHandle> context,
    std::shared_ptr<const IExposedReferenceResolver> exposedReferenceResolver)
{
    if (!m_state)
        m_state = std::make_shared<State>();
    *m_state = State {};
    m_state->targets = std::move(targets);
    m_state->targets.Normalize();
    m_state->backend = std::move(backend);
    m_state->context = std::move(context);
    m_state->exposedReferenceResolver = std::move(exposedReferenceResolver);
    m_state->inspectorMode = mode;
    m_state->valid = !m_state->targets.Empty() && m_state->backend != nullptr;
    m_state->generation = 1u;
    if (!m_state->valid)
        return;
    const auto treeResult = m_state->backend->BuildTypeTree(m_state->targets, mode);
    if (!treeResult.Succeeded())
    {
        m_state->valid = false;
        m_state->lastError = treeResult.error;
        return;
    }
    auto references = std::make_shared<Engine::Serialize::SerializedReferenceTable>();
    for (const auto& target : m_state->targets.targets)
    {
        const auto streamResult = m_state->backend->SerializeTarget(target, treeResult.tree, *references);
        if (!streamResult.Succeeded())
        {
            m_state->valid = false;
            m_state->lastError = streamResult.error;
            m_state->targetStreams.clear();
            m_state->stagedStream.reset();
            break;
        }
        m_state->targetStreams.push_back(streamResult.stream);
    }
    if (m_state->valid && !m_state->targetStreams.empty())
        m_state->stagedStream = CloneStream(m_state->targetStreams.front());
    m_state->version = ComputeVersion(*m_state);
}


void SerializedObject::Update()
{
    if (!m_state || !m_state->valid)
        return;
    if (m_state->backend != nullptr)
    {
        const auto previousSchemaHash = m_state->stagedStream != nullptr &&
                m_state->stagedStream->GetSchema() != nullptr
            ? m_state->stagedStream->GetSchema()->GetSchemaHash()
            : 0u;
        const auto typeTree = m_state->backend->BuildTypeTree(m_state->targets, m_state->inspectorMode);
        if (!typeTree.Succeeded())
        {
            m_state->valid = false;
            m_state->lastError = typeTree.error;
            return;
        }
        auto references = std::make_shared<Engine::Serialize::SerializedReferenceTable>();
        std::vector<std::shared_ptr<Engine::Serialize::SerializedByteStream>> streams;
        streams.reserve(m_state->targets.targets.size());
        for (const auto& target : m_state->targets.targets)
        {
            const auto stream = m_state->backend->SerializeTarget(target, typeTree.tree, *references);
            if (!stream.Succeeded())
            {
                m_state->lastError = stream.error;
                return;
            }
            streams.push_back(stream.stream);
        }
        if (previousSchemaHash != 0u && previousSchemaHash != typeTree.tree->GetSchemaHash())
            ++m_state->generation;
        m_state->targetStreams = std::move(streams);
        m_state->stagedStream = m_state->targetStreams.empty() ? nullptr
            : CloneStream(m_state->targetStreams.front());
        m_state->modifiedPaths.clear();
        m_state->modified = false;
        m_state->version = ComputeVersion(*m_state);
        return;
    }
}

bool SerializedObject::UpdateIfRequiredOrScript()
{
    if (!m_state || !m_state->valid || m_state->backend == nullptr)
        return false;
    const auto current = ComputeVersion(*m_state);
    if (current == m_state->version)
        return false;
    Update();
    return true;
}

bool SerializedObject::UpdateIfDirtyOrScript()
{
    return UpdateIfRequiredOrScript();
}

bool SerializedObject::IsValid() const
{
    return m_state != nullptr && m_state->valid &&
        std::all_of(m_state->targets.targets.begin(), m_state->targets.targets.end(),
            [this](const auto& target)
            {
                return target.IsValid();
            });
}

uint64_t SerializedObject::GetGeneration() const
{
    return m_state ? m_state->generation : 0u;
}

SerializedVersionStamp SerializedObject::GetVersion() const
{
    return m_state ? m_state->version : SerializedVersionStamp {};
}

const NLS::Engine::Serialize::SerializedByteStream* SerializedObject::GetPrimaryByteStream() const
{
    return m_state != nullptr && m_state->stagedStream != nullptr
        ? m_state->stagedStream.get()
        : nullptr;
}

std::vector<SerializedTargetHandle> SerializedObject::GetTargetHandles() const
{
    return m_state ? m_state->targets.targets : std::vector<SerializedTargetHandle> {};
}

const SerializedTargetSet& SerializedObject::GetTargetSet() const
{
    return m_state ? m_state->targets : EmptyTargets();
}

const SerializedTargetHandle* SerializedObject::GetTargetObject() const
{
    return m_state != nullptr && !m_state->targets.targets.empty()
        ? &m_state->targets.targets.front()
        : nullptr;
}

const SerializedTargetHandle* SerializedObject::GetContext() const
{
    return m_state != nullptr && m_state->context.has_value() &&
            m_state->context->IsValid()
        ? &*m_state->context
        : nullptr;
}

void SerializedObject::SetContext(std::optional<SerializedTargetHandle> context)
{
    if (m_state != nullptr)
        m_state->context = std::move(context);
}

std::optional<SerializedTargetHandle> SerializedObject::ResolveExposedReference(
    const SerializedProperty& property) const
{
    if (m_state == nullptr || !property ||
        property.GetPropertyType() != SerializedPropertyType::ExposedReference)
    {
        return std::nullopt;
    }

    const auto value = property.GetExposedReferenceValue();
    if (m_state->context.has_value() && m_state->context->IsValid() && !value.exposedName.empty())
    {
        const auto resolver = m_state->exposedReferenceResolver != nullptr
            ? m_state->exposedReferenceResolver.get()
            : static_cast<const IExposedReferenceResolver*>(&GetEditorExposedReferenceResolver());
        if (auto resolved = resolver->Resolve(*m_state->context, value.exposedName);
            resolved.has_value() && resolved->IsValid())
        {
            return resolved;
        }
    }

    // external editor resolves through the context table first and falls back to the
    // serialized default. Do not retain the pointer beyond this call.
    if (auto* fallback = value.defaultValue.Get(); fallback != nullptr)
    {
        return Inspector::InspectorTargetHandle::FromObject(
            *fallback,
            Inspector::InspectorTargetKind::GameObject);
    }
    return std::nullopt;
}

bool SerializedObject::SetResolvedExposedReference(
    const SerializedProperty& property,
    std::optional<SerializedTargetHandle> target)
{
    if (m_state == nullptr || !m_state->valid || !property ||
        property.GetPropertyType() != SerializedPropertyType::ExposedReference)
    {
        return false;
    }

    auto value = property.GetExposedReferenceValue();
    if (m_state->context.has_value() && m_state->context->IsValid() && !value.exposedName.empty())
    {
        const auto resolver = m_state->exposedReferenceResolver != nullptr
            ? m_state->exposedReferenceResolver.get()
            : static_cast<const IExposedReferenceResolver*>(&GetEditorExposedReferenceResolver());
        return resolver->Set(*m_state->context, value.exposedName, std::move(target));
    }

    // Without a context table external editor writes the ExposedReference default. The
    // native carrier is GameObject PPtr, so a non-null target must be resolvable
    // to the same kind before staging its stable instance identity.
    InstanceID instanceId = InstanceID_None;
    if (target.has_value())
    {
        if (!target->IsValid() || target->kind != Inspector::InspectorTargetKind::GameObject)
            return false;
        instanceId = target->instanceId;
        if (instanceId == InstanceID_None && target->objectIdentifier.IsValid())
        {
            (void)Engine::Serialize::PersistentManager::Instance().TryGetInstanceID(
                target->objectIdentifier,
                instanceId);
        }
        if (instanceId == InstanceID_None)
            return false;
    }
    value.defaultValue.SetInstanceID(instanceId);
    return property.SetExposedReferenceValue(value);
}

SerializedProperty SerializedObject::FindProperty(const std::string_view path) const
{
    if (!m_state || !m_state->valid || m_state->stagedStream == nullptr || path.empty())
        return {};
    // Canonical byte-stream backends resolve every external editor-format path directly
    // against TypeTree/cursor state; Inspector presentation is not involved.
    const auto makeStreamProperty = [this](const std::string_view candidate) -> SerializedProperty
    {
        if (PropertyPath(std::string(candidate)).IsArraySize())
        {
            constexpr std::string_view suffix = ".Array.size";
            const auto parentPath = candidate.substr(0u, candidate.size() - suffix.size());
            const auto parent = NativeSerializedStreamCodec::ResolveCursor(m_state->stagedStream, parentPath);
            if (!parent.has_value() || m_state->stagedStream->GetSchema() == nullptr)
                return {};
            const auto* parentNode = m_state->stagedStream->GetSchema()->FindNode(parent->GetNodeId());
            if (parentNode == nullptr || !Engine::Serialize::HasSerializedNodeFlag(
                    parentNode->flags, Engine::Serialize::SerializedNodeFlags::Array))
            {
                return {};
            }
            return SerializedProperty(
                m_state,
                std::string(candidate),
                m_state->version.schemaGeneration,
                m_state->version.structureGeneration,
                0u,
                parent->GetNodeId());
        }

        const auto cursor = NativeSerializedStreamCodec::ResolveCursor(m_state->stagedStream, candidate);
        if (!cursor.has_value())
            return {};
        return SerializedProperty(
            m_state,
            std::string(candidate),
            m_state->version.schemaGeneration,
            m_state->version.structureGeneration,
            0u,
            cursor->GetNodeId());
    };
    return makeStreamProperty(path);
}

SerializedProperty SerializedObject::GetIterator() const
{
    if (!m_state || !m_state->valid || m_state->stagedStream == nullptr ||
        m_state->stagedStream->GetSchema() == nullptr)
        return {};
    // external editor returns a cursor positioned on the serialized-object root; the
    // TypeTree is the only traversal authority.
    const auto schema = m_state->stagedStream->GetSchema();
    const auto rootId = schema->GetRoot();
    const std::string rootPath;
    return SerializedProperty(
        m_state,
        rootPath,
        m_state->version.schemaGeneration,
        m_state->version.structureGeneration,
        std::numeric_limits<size_t>::max(),
        rootId);
}

bool SerializedObject::CopyFromSerializedProperty(const SerializedProperty& source)
{
    if (!source || !m_state || source.GetValue().IsValid() == false)
        return false;
    const auto destination = FindProperty(source.GetPropertyPath());
    return destination && destination.SetValue(source.GetValue());
}

bool SerializedObject::CopyFromSerializedPropertyIfDifferent(const SerializedProperty& source)
{
    if (!source || !m_state)
        return false;
    const auto sourceValue = source.GetValue();
    if (!sourceValue.IsValid())
        return false;
    const auto destination = FindProperty(source.GetPropertyPath());
    if (!destination)
        return false;
    if (EquivalentVariant(destination.GetValue(), sourceValue))
        return false;
    return destination.SetValue(sourceValue);
}

const SerializedSchema& SerializedObject::GetSchema() const
{
    return m_state ? m_state->schema : EmptySchema();
}

bool SerializedObject::HasModifiedProperties() const
{
    return m_state != nullptr && m_state->modified && !m_state->modifiedPaths.empty();
}

bool SerializedObject::ApplyModifiedProperties()
{
    if (!m_state || !m_state->valid || m_state->modifiedPaths.empty())
        return false;
    m_state->lastError.clear();
    auto changes = BuildStreamChangeSet(*m_state);
    if (changes.Empty())
    {
        // A stream path can fail to resolve in one of several targets after a
        // schema/layout change. This is not a no-op: keep staged state intact
        // so the caller can refresh, inspect the error, or retry safely.
        if (!m_state->lastError.empty())
            return false;
        m_state->modifiedPaths.clear();
        m_state->modified = false;
        return false;
    }
    auto info = BuildCommandInfo(*m_state, changes);
    auto command = std::make_unique<SerializedStreamChangeCommand>(
        info,
        m_state->backend,
        changes);

    bool success = false;
    auto* undo = NLS::Core::ServiceLocator::Contains<Undo::EditorUndoService>()
        ? &NLS::Core::ServiceLocator::Get<Undo::EditorUndoService>()
        : nullptr;
    if (undo != nullptr)
    {
        const auto execution = undo->Execute(std::move(command));
        success = execution.Succeeded();
        if (!success)
            m_state->lastError = execution.message;
    }
    else
    {
        std::string error;
        success = ApplyDirect(m_state->backend, changes, false, error);
        if (!success)
            m_state->lastError = std::move(error);
    }

    if (!success)
        return false;
    Update();
    m_state->lastError.clear();
    m_state->version = ComputeVersion(*m_state);
    return true;
}

bool SerializedObject::ApplyModifiedPropertiesWithoutUndo()
{
    if (!m_state || !m_state->valid || m_state->modifiedPaths.empty())
        return false;
    m_state->lastError.clear();
    const auto changes = BuildStreamChangeSet(*m_state);
    if (changes.Empty())
    {
        if (!m_state->lastError.empty())
            return false;
        m_state->modifiedPaths.clear();
        m_state->modified = false;
        return false;
    }
    std::string error;
    if (!ApplyDirect(m_state->backend, changes, true, error))
    {
        m_state->lastError = std::move(error);
        return false;
    }
    Update();
    m_state->lastError.clear();
    m_state->version = ComputeVersion(*m_state);
    return true;
}

void SerializedObject::DiscardModifiedProperties()
{
    if (!m_state)
        return;
    if (!m_state->targetStreams.empty())
        m_state->stagedStream = CloneStream(m_state->targetStreams.front());
    m_state->modifiedPaths.clear();
    m_state->modified = false;
}

void SerializedObject::SetIsDifferentCacheDirty() const
{
    // Mixed values are calculated lazily from the staged vectors.  Clearing the
    // cached dirty bit is therefore equivalent to invalidating a cache entry.
}

bool SerializedObject::IsEditingMultipleObjects() const
{
    return m_state != nullptr && m_state->targets.targets.size() > 1u;
}

size_t SerializedObject::GetMaxArraySizeForMultiEditing() const
{
    return m_state ? m_state->maxArraySizeForMultiEditing : 64u;
}

void SerializedObject::SetMaxArraySizeForMultiEditing(const size_t maxSize)
{
    if (m_state)
        m_state->maxArraySizeForMultiEditing = maxSize;
}

void SerializedObject::SetInspectorMode(const SerializedInspectorMode mode)
{
    if (m_state == nullptr || m_state->inspectorMode == mode)
        return;
    m_state->inspectorMode = mode;
    if (m_state->backend != nullptr)
    {
        const auto treeResult = m_state->backend->BuildTypeTree(m_state->targets, mode);
        if (!treeResult.Succeeded())
        {
            m_state->lastError = treeResult.error;
            return;
        }
        const auto targets = m_state->targets;
        const auto backend = m_state->backend;
        const auto context = m_state->context;
        const auto resolver = m_state->exposedReferenceResolver;
        Reset(targets, backend, mode, context, resolver);
    }
}

SerializedInspectorMode SerializedObject::GetInspectorMode() const
{
    return m_state ? m_state->inspectorMode : SerializedInspectorMode::Normal;
}

std::string SerializedObject::GetLastError() const
{
    return m_state ? m_state->lastError : std::string {};
}
} // namespace NLS::Editor::Serialization

namespace NLS::Editor::Serialization
{
namespace
{

struct RuntimeTraversalEntry
{
    std::string path;
    SerializedNodeId nodeId = 0u;
    bool visible = true;
};

enum class RuntimeTraversalVisitResult : uint8_t
{
    Continue,
    SkipChildren,
    Stop
};

template <typename TVisitor>
bool VisitRuntimeTraversalNode(
    const std::shared_ptr<SerializedByteStream>& stream,
    const SerializedNodeId nodeId,
    const std::string& path,
    TVisitor&& visitor,
    bool& stopped)
{
    if (stream == nullptr || stream->GetSchema() == nullptr)
        return false;
    const auto& schema = *stream->GetSchema();
    const auto* node = schema.FindNode(nodeId);
    if (node == nullptr)
        return false;

    const bool visible = !Engine::Serialize::HasSerializedNodeFlag(
        node->flags,
        Engine::Serialize::SerializedNodeFlags::HideInInspector);
    auto dispatch = [&visitor, &stopped](RuntimeTraversalEntry entry)
    {
        switch (visitor(std::move(entry)))
        {
        case RuntimeTraversalVisitResult::Stop:
            stopped = true;
            return RuntimeTraversalVisitResult::Stop;
        case RuntimeTraversalVisitResult::SkipChildren:
            return RuntimeTraversalVisitResult::SkipChildren;
        case RuntimeTraversalVisitResult::Continue:
        default:
            return RuntimeTraversalVisitResult::Continue;
        }
    };
    if (!path.empty())
    {
        const auto result = dispatch({path, node->id, visible});
        if (result != RuntimeTraversalVisitResult::Continue)
            return true;
    }

    if (Engine::Serialize::HasSerializedNodeFlag(node->flags, SerializedNodeFlags::Array))
    {
        if (path.empty())
            return false;
        const auto sizeResult = dispatch({path + ".Array.size", node->id, visible});
        if (sizeResult != RuntimeTraversalVisitResult::Continue)
            return true;
        const auto arrayCursor = NativeSerializedStreamCodec::ResolveCursor(stream, path);
        uint32_t count = 0u;
        if (!arrayCursor.has_value() || !arrayCursor->Read(&count, sizeof(count)))
            return false;
        const auto* element = schema.GetFirstChild(node->id);
        if (element == nullptr)
            return false;
        for (uint32_t index = 0u; index < count; ++index)
        {
            if (!VisitRuntimeTraversalNode(
                    stream,
                    element->id,
                    path + ".Array.data[" + std::to_string(index) + "]",
                    visitor,
                    stopped))
            {
                return false;
            }
            if (stopped)
                return true;
        }
        return true;
    }

    for (auto childId = node->firstChild; childId != 0u;)
    {
        const auto* child = schema.FindNode(childId);
        if (child == nullptr)
            return false;
        const auto childPath = path.empty()
            ? child->serializedName
            : path + "." + child->serializedName;
        if (!VisitRuntimeTraversalNode(stream, child->id, childPath, visitor, stopped))
            return false;
        if (stopped)
            return true;
        childId = child->nextSibling;
    }
    return true;
}

template <typename TVisitor>
bool VisitRuntimeTraversal(
    const std::shared_ptr<SerializedByteStream>& stream,
    TVisitor&& visitor)
{
    if (stream == nullptr || stream->GetSchema() == nullptr)
        return false;
    const auto& schema = *stream->GetSchema();
    const auto* root = schema.FindNode(schema.GetRoot());
    if (root == nullptr)
        return false;
    bool stopped = false;
    // Most reflected schemas use an unnamed structural root, but asset-backed
    // backends intentionally expose a named root (for example
    // "Material.Uniforms" and "Importer.Settings") as part of their stable
    // public property path. Traverse that root itself so iterator output stays
    // resolvable by the same cursor rules as FindProperty().
    if (!root->serializedName.empty())
    {
        return VisitRuntimeTraversalNode(
            stream,
            root->id,
            root->serializedName,
            visitor,
            stopped);
    }

    for (auto childId = root->firstChild; childId != 0u;)
    {
        const auto* child = schema.FindNode(childId);
        if (child == nullptr || !VisitRuntimeTraversalNode(
                stream,
                child->id,
                child->serializedName,
                visitor,
                stopped))
        {
            return false;
        }
        if (stopped)
            break;
        childId = child->nextSibling;
    }
    return true;
}

std::optional<RuntimeTraversalEntry> FindNextRuntimeTraversalEntry(
    const std::shared_ptr<SerializedByteStream>& stream,
    const std::string_view currentPath,
    const bool enterChildren,
    const bool visibleOnly)
{
    bool currentSeen = currentPath.empty();
    std::optional<RuntimeTraversalEntry> next;
    const bool valid = VisitRuntimeTraversal(stream, [&](RuntimeTraversalEntry entry)
    {
        if (currentSeen)
        {
            if (!visibleOnly || entry.visible)
            {
                next = std::move(entry);
                return RuntimeTraversalVisitResult::Stop;
            }
            return RuntimeTraversalVisitResult::Continue;
        }
        if (entry.path != currentPath)
            return RuntimeTraversalVisitResult::Continue;
        currentSeen = true;
        return enterChildren
            ? RuntimeTraversalVisitResult::Continue
            : RuntimeTraversalVisitResult::SkipChildren;
    });
    if (!valid || !currentSeen)
        return std::nullopt;
    return next;
}

bool IsRuntimeDescendant(const std::string_view path, const std::string_view parent)
{
    return !parent.empty() && path.size() > parent.size() &&
        path.compare(0u, parent.size(), parent) == 0 &&
        (path[parent.size()] == '.' || path[parent.size()] == '[');
}


std::optional<meta::Variant> DecodeStreamValue(
    const SerializedObject::State& owner,
    const std::string_view path)
{
    if (owner.stagedStream == nullptr || owner.stagedStream->GetSchema() == nullptr)
        return std::nullopt;
    const auto cursor = NativeSerializedStreamCodec::ResolveCursor(owner.stagedStream, path);
    if (!cursor.has_value())
        return std::nullopt;
    const auto* node = owner.stagedStream->GetSchema()->FindNode(cursor->GetNodeId());
    if (node == nullptr)
        return std::nullopt;

    // A cursor is the canonical source for scalar data. Besides avoiding a
    // needless one-node reflection round trip, this keeps hand-authored
    // TypeTrees (whose scalar typeId may intentionally be omitted) readable.
    // Composite values still go through the codec below because their layout
    // needs the full reflected type information.
    const auto readScalar = [&]<typename TValue>() -> std::optional<meta::Variant>
    {
        TValue value {};
        return cursor->Read(&value, sizeof(value))
            ? std::optional<meta::Variant>(meta::Variant(value))
            : std::nullopt;
    };
    switch (node->numericKind)
    {
    case Engine::Serialize::SerializedNumericKind::Int8: return readScalar.operator()<int8_t>();
    case Engine::Serialize::SerializedNumericKind::UInt8: return readScalar.operator()<uint8_t>();
    case Engine::Serialize::SerializedNumericKind::Int16: return readScalar.operator()<int16_t>();
    case Engine::Serialize::SerializedNumericKind::UInt16: return readScalar.operator()<uint16_t>();
    case Engine::Serialize::SerializedNumericKind::Int32: return readScalar.operator()<int32_t>();
    case Engine::Serialize::SerializedNumericKind::UInt32: return readScalar.operator()<uint32_t>();
    case Engine::Serialize::SerializedNumericKind::Int64: return readScalar.operator()<int64_t>();
    case Engine::Serialize::SerializedNumericKind::UInt64: return readScalar.operator()<uint64_t>();
    case Engine::Serialize::SerializedNumericKind::Float32: return readScalar.operator()<float>();
    case Engine::Serialize::SerializedNumericKind::Float64: return readScalar.operator()<double>();
    case Engine::Serialize::SerializedNumericKind::Character: return readScalar.operator()<uint32_t>();
    case Engine::Serialize::SerializedNumericKind::Unknown: break;
    }
    if (node->kind == Engine::Serialize::SerializedNodeKind::Boolean)
    {
        uint8_t value = 0u;
        return cursor->Read(&value, sizeof(value))
            ? std::optional<meta::Variant>(meta::Variant(value != 0u))
            : std::nullopt;
    }
    if (node->typeId == meta::InvalidTypeID)
        return std::nullopt;
    const meta::Type type(node->typeId);
    const auto schema = NativeSerializedStreamCodec::BuildTypeTree(type, {"value", false});
    if (schema == nullptr)
        return std::nullopt;
    std::vector<uint8_t> bytes(cursor->GetByteSize());
    if (!cursor->Read(bytes.data(), bytes.size()))
        return std::nullopt;
    const auto references = owner.stagedStream->GetReferences() != nullptr
        ? *owner.stagedStream->GetReferences() : Engine::Serialize::SerializedReferenceTable {};
    std::string error;
    return NativeSerializedStreamCodec::DecodeVariant(
        bytes, *schema, type, references, &error, cursor->GetByteOffset());
}

bool ReplaceStreamValue(
    SerializedObject::State& owner,
    const std::string_view path,
    const meta::Variant& value)
{
    if (owner.stagedStream == nullptr || !value.IsValid())
        return false;
    const auto cursor = NativeSerializedStreamCodec::ResolveCursor(owner.stagedStream, path);
    if (!cursor.has_value())
        return false;
    const auto* node = owner.stagedStream->GetSchema() != nullptr
        ? owner.stagedStream->GetSchema()->FindNode(cursor->GetNodeId()) : nullptr;
    if (node == nullptr)
    {
        return false;
    }

    // Scalar leaves are encoded directly by their TypeTree numeric layout.
    // This deliberately takes precedence over reflection TypeId: a stream
    // property may have a reflected runtime type while its staged byte buffer
    // has no standalone reflected root value to feed EncodeVariant (notably
    // hand-authored array elements). The typed setters already select the
    // declared scalar Variant, and the exact compatibility check below keeps
    // this boundary type-safe.
    if (node->numericKind != Engine::Serialize::SerializedNumericKind::Unknown ||
        node->kind == Engine::Serialize::SerializedNodeKind::Boolean)
    {
        const auto writeScalar = [&]<typename TValue>()
        {
            if (!IsCompatibleType(NLS_TYPEOF(TValue), value.GetType()))
                return false;
            const auto scalar = value.GetValue<TValue>();
            return owner.stagedStream->Write(cursor->GetByteOffset(), &scalar, sizeof(TValue));
        };
        // Booleans use an UInt8 physical layout, but their public Variant type
        // is bool. Handle that semantic type before the raw numeric kind.
        if (node->kind == Engine::Serialize::SerializedNodeKind::Boolean)
            return writeScalar.operator()<bool>();
        switch (node->numericKind)
        {
        case Engine::Serialize::SerializedNumericKind::Int8: return writeScalar.operator()<int8_t>();
        case Engine::Serialize::SerializedNumericKind::UInt8: return writeScalar.operator()<uint8_t>();
        case Engine::Serialize::SerializedNumericKind::Int16: return writeScalar.operator()<int16_t>();
        case Engine::Serialize::SerializedNumericKind::UInt16: return writeScalar.operator()<uint16_t>();
        case Engine::Serialize::SerializedNumericKind::Int32: return writeScalar.operator()<int32_t>();
        case Engine::Serialize::SerializedNumericKind::UInt32: return writeScalar.operator()<uint32_t>();
        case Engine::Serialize::SerializedNumericKind::Int64: return writeScalar.operator()<int64_t>();
        case Engine::Serialize::SerializedNumericKind::UInt64: return writeScalar.operator()<uint64_t>();
        case Engine::Serialize::SerializedNumericKind::Float32: return writeScalar.operator()<float>();
        case Engine::Serialize::SerializedNumericKind::Float64: return writeScalar.operator()<double>();
        case Engine::Serialize::SerializedNumericKind::Character: return writeScalar.operator()<uint32_t>();
        case Engine::Serialize::SerializedNumericKind::Unknown: break;
        }
        return false;
    }
    if (!IsCompatibleType(meta::Type(node->typeId), value.GetType()))
        return false;
    const auto schema = NativeSerializedStreamCodec::BuildTypeTree(value.GetType(), {"value", false});
    if (schema == nullptr)
        return false;
    Engine::Serialize::SerializedReferenceTable references;
    std::vector<uint8_t> bytes;
    std::string error;
    if (!NativeSerializedStreamCodec::EncodeVariant(
            value, *schema, references, bytes, &error, cursor->GetByteOffset()))
        return false;
    if (owner.stagedStream->GetReferences() == nullptr)
        owner.stagedStream->SetReferences(std::make_shared<Engine::Serialize::SerializedReferenceTable>());
    if (!references.GetEntries().empty() && !NativeSerializedStreamCodec::RelocateReferenceSlots(
            bytes, *schema, references, *owner.stagedStream->GetReferences(), &error))
    {
        return false;
    }
    return bytes.size() == cursor->GetByteSize()
        ? owner.stagedStream->Write(cursor->GetByteOffset(), bytes.data(), bytes.size())
        : owner.stagedStream->Replace(cursor->GetByteOffset(), cursor->GetByteSize(), bytes);
}

} // namespace

bool SerializedProperty::SyncStreamCursor() const
{
    if (m_owner == nullptr || m_owner->stagedStream == nullptr)
        return false;

    const auto layoutRevision = m_owner->stagedStream->GetLayoutRevision();
    const auto structureRevision = m_owner->stagedStream->GetStructureRevision();
    if (m_streamNodeId != 0u && m_observedLayoutRevision == layoutRevision &&
        m_observedStreamStructureRevision == structureRevision)
    {
        return true;
    }

    if (m_path.empty() && m_owner->stagedStream->GetSchema() != nullptr)
    {
        m_streamNodeId = m_owner->stagedStream->GetSchema()->GetRoot();
        m_cachedByteOffset = 0u;
        m_observedLayoutRevision = layoutRevision;
        m_observedStreamStructureRevision = structureRevision;
        return true;
    }

    // Array.size is a external editor-visible virtual child. It is backed by the count
    // prefix of the array parent rather than by a TypeTree node of its own.
    if (PropertyPath(m_path).IsArraySize())
    {
        constexpr std::string_view suffix = ".Array.size";
        const auto parentPath = std::string_view(m_path).substr(0u, m_path.size() - suffix.size());
        const auto cursor = NativeSerializedStreamCodec::ResolveCursor(m_owner->stagedStream, parentPath);
        if (!cursor.has_value() || m_owner->stagedStream->GetSchema() == nullptr)
            return false;
        const auto* parent = m_owner->stagedStream->GetSchema()->FindNode(cursor->GetNodeId());
        if (parent == nullptr || !Engine::Serialize::HasSerializedNodeFlag(
                parent->flags, Engine::Serialize::SerializedNodeFlags::Array))
        {
            return false;
        }
        m_streamNodeId = parent->id;
        m_cachedByteOffset = cursor->GetByteOffset();
        m_observedLayoutRevision = layoutRevision;
        m_observedStreamStructureRevision = structureRevision;
        return true;
    }

    const auto cursor = NativeSerializedStreamCodec::ResolveCursor(m_owner->stagedStream, m_path);
    if (!cursor.has_value())
        return false;
    m_streamNodeId = cursor->GetNodeId();
    m_cachedByteOffset = cursor->GetByteOffset();
    m_observedLayoutRevision = layoutRevision;
    m_observedStreamStructureRevision = structureRevision;
    return true;
}

namespace
{
SerializedPropertyType ToSerializedPropertyType(const Engine::Serialize::SerializedTypeTreeNode& node)
{
    using Kind = Engine::Serialize::SerializedNodeKind;
    switch (node.kind)
    {
    case Kind::Boolean: return SerializedPropertyType::Boolean;
    case Kind::Integer: return SerializedPropertyType::Integer;
    case Kind::Enum: return SerializedPropertyType::Enum;
    case Kind::Float: return SerializedPropertyType::Float;
    case Kind::String: return SerializedPropertyType::String;
    case Kind::Color: return SerializedPropertyType::Color;
    case Kind::ObjectReference: return SerializedPropertyType::ObjectReference;
    case Kind::LayerMask: return SerializedPropertyType::LayerMask;
    case Kind::Vector2: return SerializedPropertyType::Vector2;
    case Kind::Vector3: return SerializedPropertyType::Vector3;
    case Kind::Vector4: return SerializedPropertyType::Vector4;
    case Kind::Rect: return SerializedPropertyType::Rect;
    case Kind::Character: return SerializedPropertyType::Character;
    case Kind::AnimationCurve: return SerializedPropertyType::AnimationCurve;
    case Kind::Bounds: return SerializedPropertyType::Bounds;
    case Kind::Gradient: return SerializedPropertyType::Gradient;
    case Kind::Quaternion: return SerializedPropertyType::Quaternion;
    case Kind::ExposedReference: return SerializedPropertyType::ExposedReference;
    case Kind::FixedBufferSize: return SerializedPropertyType::FixedBufferSize;
    case Kind::Vector2Int: return SerializedPropertyType::Vector2Int;
    case Kind::Vector3Int: return SerializedPropertyType::Vector3Int;
    case Kind::RectInt: return SerializedPropertyType::RectInt;
    case Kind::BoundsInt: return SerializedPropertyType::BoundsInt;
    default: return SerializedPropertyType::Generic;
    }
}

SerializedNumericType ToSerializedNumericType(const Engine::Serialize::SerializedNumericKind numeric)
{
    using Source = Engine::Serialize::SerializedNumericKind;
    switch (numeric)
    {
    case Source::Int8: return SerializedNumericType::Int8;
    case Source::UInt8: return SerializedNumericType::UInt8;
    case Source::Int16: return SerializedNumericType::Int16;
    case Source::UInt16: return SerializedNumericType::UInt16;
    case Source::Int32: return SerializedNumericType::Int32;
    case Source::UInt32: return SerializedNumericType::UInt32;
    case Source::Int64: return SerializedNumericType::Int64;
    case Source::UInt64: return SerializedNumericType::UInt64;
    case Source::Float32: return SerializedNumericType::Float;
    case Source::Float64: return SerializedNumericType::Double;
    case Source::Character: return SerializedNumericType::Char;
    default: return SerializedNumericType::Unknown;
    }
}
} // namespace

bool SerializedProperty::IsValid() const
{
    // A SerializedProperty is a cursor into the staged byte stream. Its
    // validity is determined by owner generations and a bounded stream lookup,
    // never by a projection node. Dynamic Array.data[n] paths have no one-to-one
    // PropertyNode counterpart.
    if (m_owner == nullptr || !m_owner->valid || m_owner->stagedStream == nullptr ||
        m_owner->stagedStream->GetSchema() == nullptr ||
        m_owner->version.schemaGeneration != m_schemaGeneration)
    {
        return false;
    }
    if (!m_path.empty() &&
        m_owner->version.structureGeneration != m_structureGeneration &&
        m_owner->stalePropertyPaths.contains(m_path))
    {
        return false;
    }
    if (SyncStreamCursor())
        return true;
    return false;
}

std::string_view SerializedProperty::GetPropertyPath() const
{
    return IsValid() ? std::string_view(m_path) : std::string_view {};
}

std::string_view SerializedProperty::GetName() const
{
    if (ParseArrayElementPath(m_path).has_value())
        return "data";
    if (PropertyPath(m_path).IsArraySize())
        return "size";
    if (m_owner != nullptr && SyncStreamCursor() &&
        m_owner->stagedStream != nullptr && m_owner->stagedStream->GetSchema() != nullptr)
    {
        const auto* node = m_owner->stagedStream->GetSchema()->FindNode(m_streamNodeId);
        return node != nullptr ? std::string_view(node->serializedName) : std::string_view {};
    }
    return {};
}

std::string_view SerializedProperty::GetDisplayName() const
{
    if (ParseArrayElementPath(m_path).has_value())
        return "Element";
    if (PropertyPath(m_path).IsArraySize())
        return "Size";
    return GetName();
}

std::string_view SerializedProperty::GetTooltip() const
{
    return {};
}

SerializedPropertyType SerializedProperty::GetPropertyType() const
{
    if (PropertyPath(m_path).IsArraySize())
        return SerializedPropertyType::ArraySize;
    if (m_owner != nullptr && SyncStreamCursor() &&
        m_owner->stagedStream != nullptr && m_owner->stagedStream->GetSchema() != nullptr)
    {
        const auto* streamNode = m_owner->stagedStream->GetSchema()->FindNode(m_streamNodeId);
        return streamNode != nullptr ? ToSerializedPropertyType(*streamNode) : SerializedPropertyType::Generic;
    }
    return SerializedPropertyType::Generic;
}

SerializedNumericType SerializedProperty::GetNumericType() const
{
    if (PropertyPath(m_path).IsArraySize())
        return SerializedNumericType::Int32;
    if (m_owner != nullptr && SyncStreamCursor() &&
        m_owner->stagedStream != nullptr && m_owner->stagedStream->GetSchema() != nullptr)
    {
        const auto* streamNode = m_owner->stagedStream->GetSchema()->FindNode(m_streamNodeId);
        return streamNode != nullptr ? ToSerializedNumericType(streamNode->numericKind) : SerializedNumericType::Unknown;
    }
    return SerializedNumericType::Unknown;
}

std::string_view SerializedProperty::GetExactTypeName() const
{
    if (m_owner != nullptr && SyncStreamCursor() &&
        m_owner->stagedStream != nullptr && m_owner->stagedStream->GetSchema() != nullptr)
    {
        const auto* streamNode = m_owner->stagedStream->GetSchema()->FindNode(m_streamNodeId);
        m_exactTypeNameCache = streamNode != nullptr ? streamNode->typeName : std::string {};
        return m_exactTypeNameCache;
    }
    m_exactTypeNameCache.clear();
    return {};
}

uint64_t SerializedProperty::GetExactTypeId() const
{
    if (m_owner != nullptr && SyncStreamCursor() &&
        m_owner->stagedStream != nullptr && m_owner->stagedStream->GetSchema() != nullptr)
    {
        const auto* streamNode = m_owner->stagedStream->GetSchema()->FindNode(m_streamNodeId);
        return streamNode != nullptr ? static_cast<uint64_t>(streamNode->typeId) : 0u;
    }
    return 0u;
}

size_t SerializedProperty::GetDepth() const
{
    if (!IsValid())
        return 0u;
    size_t depth = 0u;
    for (const auto character : m_path)
    {
        if (character == '.')
            ++depth;
    }
    return depth;
}

bool SerializedProperty::IsExpanded() const
{
    if (!IsValid() || m_owner == nullptr)
        return false;
    const auto found = m_owner->expanded.find(m_path);
    return found != m_owner->expanded.end() && found->second;
}

void SerializedProperty::SetExpanded(const bool expanded) const
{
    if (IsValid() && m_owner != nullptr)
        m_owner->expanded[m_path] = expanded;
}

bool SerializedProperty::IsEditable() const
{
    if (m_owner != nullptr && SyncStreamCursor() &&
        m_owner->stagedStream != nullptr && m_owner->stagedStream->GetSchema() != nullptr)
    {
        const auto* streamNode = m_owner->stagedStream->GetSchema()->FindNode(m_streamNodeId);
        return streamNode != nullptr && !Engine::Serialize::HasSerializedNodeFlag(
            streamNode->flags, Engine::Serialize::SerializedNodeFlags::ReadOnly);
    }
    return false;
}

bool SerializedProperty::HasChildren() const
{
    if (m_owner != nullptr && SyncStreamCursor() &&
        m_owner->stagedStream != nullptr && m_owner->stagedStream->GetSchema() != nullptr)
    {
        const auto* streamNode = m_owner->stagedStream->GetSchema()->FindNode(m_streamNodeId);
        return streamNode != nullptr && streamNode->firstChild != 0u;
    }
    return false;
}

bool SerializedProperty::HasVisibleChildren() const
{
    if (m_owner != nullptr && SyncStreamCursor() &&
        m_owner->stagedStream != nullptr && m_owner->stagedStream->GetSchema() != nullptr)
    {
        const auto* streamNode = m_owner->stagedStream->GetSchema()->FindNode(m_streamNodeId);
        if (streamNode == nullptr)
            return false;
        for (auto child = m_owner->stagedStream->GetSchema()->GetFirstChild(streamNode->id);
             child != nullptr; child = m_owner->stagedStream->GetSchema()->GetNextSibling(child->id))
        {
            if (!Engine::Serialize::HasSerializedNodeFlag(
                    child->flags, Engine::Serialize::SerializedNodeFlags::HideInInspector))
                return true;
        }
        return false;
    }
    return false;
}

bool SerializedProperty::IsArray() const
{
    if (PropertyPath(m_path).IsArraySize())
        return false;
    if (m_owner != nullptr && SyncStreamCursor() &&
        m_owner->stagedStream != nullptr && m_owner->stagedStream->GetSchema() != nullptr)
    {
        const auto* streamNode = m_owner->stagedStream->GetSchema()->FindNode(m_streamNodeId);
        return streamNode != nullptr && Engine::Serialize::HasSerializedNodeFlag(
            streamNode->flags, Engine::Serialize::SerializedNodeFlags::Array);
    }
    return false;
}

std::string SerializedProperty::GetArrayElementType() const
{
    if (!IsArray() || m_owner == nullptr || !SyncStreamCursor() ||
        m_owner->stagedStream == nullptr || m_owner->stagedStream->GetSchema() == nullptr)
    {
        return {};
    }
    const auto* array = m_owner->stagedStream->GetSchema()->FindNode(m_streamNodeId);
    const auto* element = array != nullptr
        ? m_owner->stagedStream->GetSchema()->GetFirstChild(array->id)
        : nullptr;
    return element != nullptr ? element->typeName : std::string {};
}

size_t SerializedProperty::GetArraySize() const
{
    if (m_owner != nullptr && IsArray() && SyncStreamCursor())
    {
        uint32_t count = 0u;
        return m_owner->stagedStream != nullptr &&
            m_owner->stagedStream->Read(m_cachedByteOffset, &count, sizeof(count))
            ? static_cast<size_t>(count) : 0u;
    }
    return 0u;
}

size_t SerializedProperty::GetMinArraySize() const
{
    if (m_owner != nullptr && IsArray())
    {
        size_t result = std::numeric_limits<size_t>::max();
        for (const auto& stream : m_owner->targetStreams)
        {
            const auto cursor = NativeSerializedStreamCodec::ResolveCursor(stream, m_path);
            uint32_t count = 0u;
            if (!cursor.has_value() || !cursor->Read(&count, sizeof(count)))
                return 0u;
            result = (std::min)(result, static_cast<size_t>(count));
        }
        return result == std::numeric_limits<size_t>::max() ? 0u : result;
    }
    return 0u;
}

size_t SerializedProperty::GetFixedBufferSize() const
{
    if (!IsFixedBuffer())
        return 0u;
    const auto* node = m_owner->stagedStream->GetSchema()->FindNode(m_streamNodeId);
    const auto* element = m_owner->stagedStream->GetSchema()->GetFirstChild(node->id);
    return element != nullptr && element->fixedByteSize > 0 && node->fixedByteSize > 0
        ? static_cast<size_t>(node->fixedByteSize / element->fixedByteSize) : 0u;
}

bool SerializedProperty::IsFixedBuffer() const
{
    if (!SyncStreamCursor() || m_owner == nullptr || m_owner->stagedStream == nullptr ||
        m_owner->stagedStream->GetSchema() == nullptr)
        return false;
    const auto* node = m_owner->stagedStream->GetSchema()->FindNode(m_streamNodeId);
    return node != nullptr && Engine::Serialize::HasSerializedNodeFlag(
        node->flags, Engine::Serialize::SerializedNodeFlags::FixedBuffer);
}

SerializedProperty SerializedProperty::GetFixedBufferElementAtIndex(const size_t index) const
{
    if (!IsFixedBuffer() || index >= GetFixedBufferSize())
        return {};
    return GetArrayElementAtIndex(index);
}

bool SerializedProperty::HasMultipleDifferentValues() const
{
    if (!IsValid() || m_owner == nullptr || m_owner->targetStreams.size() < 2u)
        return false;

    // A staged edit is propagated to every target by BuildStreamChangeSet.
    // Treat that pending delta as the common value instead of comparing the
    // primary staged stream to the still-unmodified secondary base streams.
    const auto modifiedAncestor = std::any_of(
        m_owner->modifiedPaths.begin(),
        m_owner->modifiedPaths.end(),
        [this](const std::string& modified)
        {
            return modified == m_path || IsDescendant(PropertyPath(m_path), PropertyPath(modified));
        });
    if (modifiedAncestor)
        return false;

    const auto readArraySize = [this](const std::shared_ptr<SerializedByteStream>& stream)
    {
        constexpr std::string_view suffix = ".Array.size";
        const auto parentPath = std::string_view(m_path).substr(0u, m_path.size() - suffix.size());
        const auto cursor = NativeSerializedStreamCodec::ResolveCursor(stream, parentPath);
        uint32_t count = 0u;
        return cursor.has_value() && cursor->Read(&count, sizeof(count))
            ? std::optional<uint32_t>(count)
            : std::optional<uint32_t> {};
    };

    if (PropertyPath(m_path).IsArraySize())
    {
        const auto first = readArraySize(m_owner->targetStreams.front());
        if (!first.has_value())
            return false;
        return std::any_of(
            std::next(m_owner->targetStreams.begin()),
            m_owner->targetStreams.end(),
            [&readArraySize, first](const auto& stream)
            {
                const auto next = readArraySize(stream);
                return !next.has_value() || *next != *first;
            });
    }

    const auto firstCursor = NativeSerializedStreamCodec::ResolveCursor(m_owner->targetStreams.front(), m_path);
    if (!firstCursor.has_value())
        return false;
    const auto signature = m_owner->targetStreams.front()->GetSchema()->GetTypeSignature();
    const auto first = firstCursor->Capture(signature);
    if (!first.has_value())
        return false;
    for (size_t index = 1u; index < m_owner->targetStreams.size(); ++index)
    {
        const auto cursor = NativeSerializedStreamCodec::ResolveCursor(m_owner->targetStreams[index], m_path);
        const auto next = cursor.has_value() ? cursor->Capture(signature) : std::optional<Engine::Serialize::SerializedByteSlice> {};
        if (!next.has_value() || !first->Equivalent(*next))
            return true;
    }
    return false;
}

bool SerializedProperty::IsAnimated() const
{
    if (m_owner != nullptr && SyncStreamCursor() &&
        m_owner->stagedStream != nullptr && m_owner->stagedStream->GetSchema() != nullptr)
    {
        const auto* streamNode = m_owner->stagedStream->GetSchema()->FindNode(m_streamNodeId);
        return streamNode != nullptr && Engine::Serialize::HasSerializedNodeFlag(
            streamNode->flags,
            Engine::Serialize::SerializedNodeFlags::Animated);
    }
    return false;
}

bool SerializedProperty::IsPrefabOverride() const
{
    if (!IsValid() || m_owner == nullptr || m_owner->backend == nullptr ||
        m_owner->targets.targets.empty())
    {
        return false;
    }
    return m_owner->backend->QueryPrefabState(
        m_owner->targets.targets.front(), PropertyPath(m_path)).state != Inspector::PrefabOverrideState::None;
}

bool SerializedProperty::IsDefaultOverride() const
{
    return IsValid() && m_owner != nullptr && m_owner->backend != nullptr &&
        !m_owner->targets.targets.empty() &&
        m_owner->backend->QueryPrefabState(
            m_owner->targets.targets.front(), PropertyPath(m_path)).defaultOverride;
}

bool SerializedProperty::IsInstantiatedPrefab() const
{
    return IsValid() && m_owner != nullptr && m_owner->backend != nullptr &&
        !m_owner->targets.targets.empty() &&
        m_owner->backend->QueryPrefabState(
            m_owner->targets.targets.front(), PropertyPath(m_path)).instantiatedPrefab;
}

meta::Variant SerializedProperty::GetValue() const
{
    if (!IsValid() || m_owner == nullptr)
        return {};
    if (PropertyPath(m_path).IsArraySize())
    {
        constexpr std::string_view suffix = ".Array.size";
        const SerializedProperty parent(
            m_owner,
            m_path.substr(0u, m_path.size() - suffix.size()),
            m_schemaGeneration,
            m_structureGeneration);
        return meta::Variant(static_cast<int32_t>(parent.GetArraySize()));
    }
    return DecodeStreamValue(*m_owner, m_path).value_or(meta::Variant {});
}

bool SerializedProperty::SetValue(const meta::Variant& value) const
{
    if (!IsValid() || m_owner == nullptr || !IsEditable() || !value.IsValid())
        return false;
    if (PropertyPath(m_path).IsArraySize())
    {
        constexpr std::string_view suffix = ".Array.size";
        const SerializedProperty parent(
            m_owner,
            m_path.substr(0u, m_path.size() - suffix.size()),
            m_schemaGeneration,
            m_structureGeneration);
        return parent.SetArraySize(static_cast<size_t>(std::max<int64_t>(0, value.ToInt())));
    }
    const auto before = GetValue();
    if (!ReplaceStreamValue(*m_owner, m_path, value))
        return false;
    const auto after = GetValue();
    if (EquivalentVariant(before, after))
        return false;
    for (auto iterator = m_owner->modifiedPaths.begin(); iterator != m_owner->modifiedPaths.end();)
    {
        if (*iterator != m_path && IsDescendant(PropertyPath(*iterator), PropertyPath(m_path)))
            iterator = m_owner->modifiedPaths.erase(iterator);
        else
            ++iterator;
    }
    m_owner->modifiedPaths.insert(m_path);
    m_owner->modified = true;
    return true;
}

bool SerializedProperty::GetBoolValue() const
{
    const auto value = GetValue();
    return value.IsValid() ? value.ToBool() : false;
}

int64_t SerializedProperty::GetLongValue() const
{
    if (PropertyPath(m_path).IsArraySize())
    {
        const auto suffix = std::string_view(".Array.size");
        const auto parent = m_path.substr(0u, m_path.size() - suffix.size());
        const SerializedProperty property(
            m_owner,
            parent,
            m_schemaGeneration,
            m_structureGeneration);
        return static_cast<int64_t>(property.GetArraySize());
    }
    const auto value = GetValue();
    if (!value.IsValid())
        return 0;
    switch (GetNumericType())
    {
    case SerializedNumericType::Int8: return value.GetValue<int8_t>();
    case SerializedNumericType::Int16: return value.GetValue<int16_t>();
    case SerializedNumericType::Int32: return value.GetValue<int32_t>();
    case SerializedNumericType::Int64: return value.GetValue<int64_t>();
    default: return static_cast<int64_t>(value.ToDouble());
    }
}

uint64_t SerializedProperty::GetUnsignedLongValue() const
{
    const auto value = GetValue();
    if (!value.IsValid())
        return 0u;
    switch (GetNumericType())
    {
    case SerializedNumericType::UInt8: return value.GetValue<uint8_t>();
    case SerializedNumericType::UInt16: return value.GetValue<uint16_t>();
    case SerializedNumericType::UInt32: return value.GetValue<uint32_t>();
    case SerializedNumericType::UInt64: return value.GetValue<uint64_t>();
    default: return static_cast<uint64_t>(value.ToDouble());
    }
}

double SerializedProperty::GetDoubleValue() const
{
    const auto value = GetValue();
    return value.IsValid() ? value.ToDouble() : 0.0;
}

float SerializedProperty::GetFloatValue() const
{
    return static_cast<float>(GetDoubleValue());
}

std::string SerializedProperty::GetStringValue() const
{
    const auto value = GetValue();
    return value.IsValid() ? value.ToString() : std::string {};
}

Maths::Vector2 SerializedProperty::GetVector2Value() const
{
    const auto value = GetValue();
    return value.IsValid() ? value.GetValue<Maths::Vector2>() : Maths::Vector2 {};
}

Maths::Vector3 SerializedProperty::GetVector3Value() const
{
    const auto value = GetValue();
    return value.IsValid() ? value.GetValue<Maths::Vector3>() : Maths::Vector3 {};
}

Maths::Vector4 SerializedProperty::GetVector4Value() const
{
    const auto value = GetValue();
    return value.IsValid() ? value.GetValue<Maths::Vector4>() : Maths::Vector4 {};
}

Maths::Quaternion SerializedProperty::GetQuaternionValue() const
{
    const auto value = GetValue();
    return value.IsValid() ? value.GetValue<Maths::Quaternion>() : Maths::Quaternion {};
}

Maths::Color SerializedProperty::GetColorValue() const
{
    const auto value = GetValue();
    return value.IsValid() ? value.GetValue<Maths::Color>() : Maths::Color {};
}

Maths::Rect SerializedProperty::GetRectValue() const
{
    const auto value = GetValue();
    return value.IsValid() ? value.GetValue<Maths::Rect>() : Maths::Rect {};
}

Maths::Vector2Int SerializedProperty::GetVector2IntValue() const
{
    const auto value = GetValue();
    return value.IsValid() ? value.GetValue<Maths::Vector2Int>() : Maths::Vector2Int {};
}

Maths::Vector3Int SerializedProperty::GetVector3IntValue() const
{
    const auto value = GetValue();
    return value.IsValid() ? value.GetValue<Maths::Vector3Int>() : Maths::Vector3Int {};
}

Maths::RectInt SerializedProperty::GetRectIntValue() const
{
    const auto value = GetValue();
    return value.IsValid() ? value.GetValue<Maths::RectInt>() : Maths::RectInt {};
}

Maths::Bounds SerializedProperty::GetBoundsValue() const
{
    const auto value = GetValue();
    return value.IsValid() ? value.GetValue<Maths::Bounds>() : Maths::Bounds {};
}

Maths::BoundsInt SerializedProperty::GetBoundsIntValue() const
{
    const auto value = GetValue();
    return value.IsValid() ? value.GetValue<Maths::BoundsInt>() : Maths::BoundsInt {};
}

Engine::Serialize::AnimationCurve SerializedProperty::GetAnimationCurveValue() const
{
    const auto value = GetValue();
    return value.IsValid() && value.GetType() == NLS_TYPEOF(Engine::Serialize::AnimationCurve)
        ? value.GetValue<Engine::Serialize::AnimationCurve>()
        : Engine::Serialize::AnimationCurve {};
}

Engine::Serialize::Gradient SerializedProperty::GetGradientValue() const
{
    const auto value = GetValue();
    return value.IsValid() && value.GetType() == NLS_TYPEOF(Engine::Serialize::Gradient)
        ? value.GetValue<Engine::Serialize::Gradient>()
        : Engine::Serialize::Gradient {};
}

Engine::Serialize::ExposedReference SerializedProperty::GetExposedReferenceValue() const
{
    const auto value = GetValue();
    return value.IsValid() && value.GetType() == NLS_TYPEOF(Engine::Serialize::ExposedReference)
        ? value.GetValue<Engine::Serialize::ExposedReference>()
        : Engine::Serialize::ExposedReference {};
}

int SerializedProperty::GetEnumValueIndex() const
{
    return static_cast<int>(GetLongValue());
}

std::vector<std::string> SerializedProperty::GetEnumDisplayNames() const
{
    if (!IsValid() || m_owner == nullptr || !SyncStreamCursor() ||
        m_owner->stagedStream == nullptr || m_owner->stagedStream->GetSchema() == nullptr)
    {
        return {};
    }
    const auto* node = m_owner->stagedStream->GetSchema()->FindNode(m_streamNodeId);
    const meta::Type type(node != nullptr ? node->typeId : meta::InvalidTypeID);
    return type.IsValid() && type.IsEnum() ? type.GetEnum().GetKeys() : std::vector<std::string> {};
}

bool SerializedProperty::SetBoolValue(const bool value) const
{
    return SetValue(meta::Variant(value));
}

bool SerializedProperty::SetLongValue(const int64_t value) const
{
    if (PropertyPath(m_path).IsArraySize())
    {
        constexpr std::string_view suffix = ".Array.size";
        const SerializedProperty parent(
            m_owner,
            m_path.substr(0u, m_path.size() - suffix.size()),
            m_schemaGeneration,
            m_structureGeneration);
        return parent.SetArraySize(static_cast<size_t>(std::max<int64_t>(0, value)));
    }
    switch (GetNumericType())
    {
    case SerializedNumericType::Int8: return SetValue(meta::Variant(static_cast<int8_t>(value)));
    case SerializedNumericType::Int16: return SetValue(meta::Variant(static_cast<int16_t>(value)));
    case SerializedNumericType::Int32: return SetValue(meta::Variant(static_cast<int32_t>(value)));
    case SerializedNumericType::Int64: return SetValue(meta::Variant(value));
    default: return SetValue(meta::Variant(value));
    }
}

bool SerializedProperty::SetUnsignedLongValue(const uint64_t value) const
{
    switch (GetNumericType())
    {
    case SerializedNumericType::UInt8: return SetValue(meta::Variant(static_cast<uint8_t>(value)));
    case SerializedNumericType::UInt16: return SetValue(meta::Variant(static_cast<uint16_t>(value)));
    case SerializedNumericType::UInt32: return SetValue(meta::Variant(static_cast<uint32_t>(value)));
    case SerializedNumericType::UInt64: return SetValue(meta::Variant(value));
    default: return SetValue(meta::Variant(value));
    }
}

bool SerializedProperty::SetDoubleValue(const double value) const
{
    return GetNumericType() == SerializedNumericType::Float
        ? SetValue(meta::Variant(static_cast<float>(value)))
        : SetValue(meta::Variant(value));
}

bool SerializedProperty::SetFloatValue(const float value) const
{
    return SetValue(meta::Variant(value));
}

bool SerializedProperty::SetStringValue(const std::string_view value) const
{
    return SetValue(meta::Variant(std::string(value)));
}

bool SerializedProperty::SetVector2Value(const Maths::Vector2& value) const
{
    return SetValue(meta::Variant(value));
}

bool SerializedProperty::SetVector3Value(const Maths::Vector3& value) const
{
    return SetValue(meta::Variant(value));
}

bool SerializedProperty::SetVector4Value(const Maths::Vector4& value) const
{
    return SetValue(meta::Variant(value));
}

bool SerializedProperty::SetQuaternionValue(const Maths::Quaternion& value) const
{
    return SetValue(meta::Variant(value));
}

bool SerializedProperty::SetColorValue(const Maths::Color& value) const
{
    return SetValue(meta::Variant(value));
}

bool SerializedProperty::SetRectValue(const Maths::Rect& value) const
{
    return SetValue(meta::Variant(value));
}

bool SerializedProperty::SetVector2IntValue(const Maths::Vector2Int& value) const
{
    return SetValue(meta::Variant(value));
}

bool SerializedProperty::SetVector3IntValue(const Maths::Vector3Int& value) const
{
    return SetValue(meta::Variant(value));
}

bool SerializedProperty::SetRectIntValue(const Maths::RectInt& value) const
{
    return SetValue(meta::Variant(value));
}

bool SerializedProperty::SetBoundsValue(const Maths::Bounds& value) const
{
    return SetValue(meta::Variant(value));
}

bool SerializedProperty::SetBoundsIntValue(const Maths::BoundsInt& value) const
{
    return SetValue(meta::Variant(value));
}

bool SerializedProperty::SetAnimationCurveValue(const Engine::Serialize::AnimationCurve& value) const
{
    return SetValue(meta::Variant(value));
}

bool SerializedProperty::SetGradientValue(const Engine::Serialize::Gradient& value) const
{
    return SetValue(meta::Variant(value));
}

bool SerializedProperty::SetExposedReferenceValue(const Engine::Serialize::ExposedReference& value) const
{
    return SetValue(meta::Variant(value));
}

bool SerializedProperty::SetEnumValueIndex(const int value) const
{
    if (!IsValid() || m_owner == nullptr || !SyncStreamCursor() || value < 0 ||
        m_owner->stagedStream == nullptr || m_owner->stagedStream->GetSchema() == nullptr)
        return SetLongValue(value);
    const auto* node = m_owner->stagedStream->GetSchema()->FindNode(m_streamNodeId);
    const meta::Type type(node != nullptr ? node->typeId : meta::InvalidTypeID);
    if (!type.IsValid() || !type.IsEnum())
        return SetLongValue(value);
    const auto values = type.GetEnum().GetValues();
    if (static_cast<size_t>(value) >= values.size())
        return false;
    return SetValue(values[static_cast<size_t>(value)]);
}

SerializedProperty SerializedProperty::FindPropertyRelative(const std::string_view relativePath) const
{
    if (!IsValid() || m_owner == nullptr || relativePath.empty() ||
        m_owner->stagedStream == nullptr)
        return {};
    const auto make = [this](const std::string& path) -> SerializedProperty
    {
        if (m_owner == nullptr || m_owner->stagedStream == nullptr)
            return {};
        if (PropertyPath(path).IsArraySize())
        {
            constexpr std::string_view suffix = ".Array.size";
            const auto parentPath = std::string_view(path).substr(0u, path.size() - suffix.size());
            const auto parent = NativeSerializedStreamCodec::ResolveCursor(m_owner->stagedStream, parentPath);
            if (!parent.has_value() || m_owner->stagedStream->GetSchema() == nullptr)
                return {};
            const auto* parentNode = m_owner->stagedStream->GetSchema()->FindNode(parent->GetNodeId());
            if (parentNode == nullptr || !Engine::Serialize::HasSerializedNodeFlag(
                    parentNode->flags, Engine::Serialize::SerializedNodeFlags::Array))
            {
                return {};
            }
            return SerializedProperty(
                m_owner,
                path,
                m_owner->version.schemaGeneration,
                m_owner->version.structureGeneration,
                0u,
                parent->GetNodeId());
        }
        const auto cursor = NativeSerializedStreamCodec::ResolveCursor(m_owner->stagedStream, path);
        return cursor.has_value()
            ? SerializedProperty(
                m_owner,
                path,
                m_owner->version.schemaGeneration,
                m_owner->version.structureGeneration,
                0u,
                cursor->GetNodeId())
            : SerializedProperty {};
    };
    const auto nested = make(m_path.empty()
        ? std::string(relativePath)
        : m_path + "." + std::string(relativePath));
    return nested ? nested : make(std::string(relativePath));
}

SerializedProperty SerializedProperty::GetArrayElementAtIndex(const size_t index) const
{
    if (!IsArray() || m_owner == nullptr)
        return {};
    if (!SyncStreamCursor() || index >= GetArraySize() || m_owner->stagedStream == nullptr)
        return {};
    const auto path = m_path + ".Array.data[" + std::to_string(index) + "]";
    const auto cursor = NativeSerializedStreamCodec::ResolveCursor(m_owner->stagedStream, path);
    return cursor.has_value()
        ? SerializedProperty(
            m_owner,
            path,
            m_owner->version.schemaGeneration,
            m_owner->version.structureGeneration,
            0u,
            cursor->GetNodeId())
        : SerializedProperty {};
}

bool SerializedProperty::InsertArrayElementAtIndex(const size_t index) const
{
    if (!IsArray() || !IsEditable() || m_owner == nullptr || (m_owner->targets.targets.size() > 1u &&
        GetArraySize() > m_owner->maxArraySizeForMultiEditing))
        return false;
    const auto size = GetArraySize();
    if (index > size)
        return false;
    return MutateStreamArray(*m_owner, PropertyPath(m_path), true, [index, size](
        std::vector<std::vector<uint8_t>>& elements,
        const SerializedTypeTree& tree,
        const SerializedTypeTreeNode& element)
    {
        std::vector<uint8_t> inserted;
        if (index < size)
            inserted = elements[index];
        else if (!AppendDefaultStreamValue(tree, element, inserted))
            return false;
        elements.insert(elements.begin() + static_cast<std::ptrdiff_t>(index), std::move(inserted));
        return true;
    });
}

bool SerializedProperty::DeleteArrayElementAtIndex(const size_t index) const
{
    if (!IsArray() || !IsEditable() || m_owner == nullptr || index >= GetArraySize())
        return false;
    return MutateStreamArray(*m_owner, PropertyPath(m_path), true, [index](
        std::vector<std::vector<uint8_t>>& elements,
        const SerializedTypeTree&,
        const SerializedTypeTreeNode& element)
    {
        if (element.kind == SerializedNodeKind::ObjectReference &&
            elements[index].size() == sizeof(uint32_t))
        {
            uint32_t slot = 0u;
            std::memcpy(&slot, elements[index].data(), sizeof(slot));
            if (slot != 0u)
            {
                slot = 0u;
                std::memcpy(elements[index].data(), &slot, sizeof(slot));
                return true;
            }
        }
        elements.erase(elements.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    });
}

bool SerializedProperty::MoveArrayElement(const size_t sourceIndex, const size_t destinationIndex) const
{
    if (!IsArray() || !IsEditable() || m_owner == nullptr || sourceIndex == destinationIndex)
        return sourceIndex == destinationIndex && sourceIndex < GetArraySize();
    if (sourceIndex >= GetArraySize() || destinationIndex >= GetArraySize())
        return false;
    return MutateStreamArray(*m_owner, PropertyPath(m_path), true, [sourceIndex, destinationIndex](
        std::vector<std::vector<uint8_t>>& elements,
        const SerializedTypeTree&,
        const SerializedTypeTreeNode&)
    {
        auto element = std::move(elements[sourceIndex]);
        elements.erase(elements.begin() + static_cast<std::ptrdiff_t>(sourceIndex));
        elements.insert(elements.begin() + static_cast<std::ptrdiff_t>(destinationIndex), std::move(element));
        return true;
    });
}

bool SerializedProperty::ClearArray() const
{
    return SetArraySize(0u);
}

bool SerializedProperty::SetArraySize(const size_t size) const
{
    if (!IsArray() || !IsEditable() || m_owner == nullptr ||
        (m_owner->targets.targets.size() > 1u && size > m_owner->maxArraySizeForMultiEditing))
        return false;
    if (size == GetArraySize())
        return false;
    return MutateStreamArray(*m_owner, PropertyPath(m_path), true, [size](
        std::vector<std::vector<uint8_t>>& elements,
        const SerializedTypeTree& tree,
        const SerializedTypeTreeNode& element)
    {
        if (size < elements.size())
        {
            elements.resize(size);
            return true;
        }
        while (elements.size() < size)
        {
            std::vector<uint8_t> value;
            if (!AppendDefaultStreamValue(tree, element, value))
                return false;
            elements.push_back(std::move(value));
        }
        return true;
    });
}

SerializedProperty SerializedProperty::Copy() const
{
    return *this;
}

void SerializedProperty::Reset() const
{
    if (m_owner == nullptr || m_owner->stagedStream == nullptr ||
        m_owner->stagedStream->GetSchema() == nullptr)
    {
        return;
    }
    m_path.clear();
    m_streamNodeId = m_owner->stagedStream->GetSchema()->GetRoot();
    m_iteratorIndex = std::numeric_limits<size_t>::max();
    m_observedLayoutRevision = m_owner->stagedStream->GetLayoutRevision();
    m_observedStreamStructureRevision = m_owner->stagedStream->GetStructureRevision();
}

bool SerializedProperty::DataEquals(
    const SerializedProperty& left,
    const SerializedProperty& right)
{
    if (!left.IsValid() || !right.IsValid() || left.m_owner == nullptr || right.m_owner == nullptr ||
        !left.SyncStreamCursor() || !right.SyncStreamCursor() ||
        left.m_owner->stagedStream == nullptr || right.m_owner->stagedStream == nullptr)
    {
        return false;
    }
    const auto* leftSchema = left.m_owner->stagedStream->GetSchema().get();
    const auto* rightSchema = right.m_owner->stagedStream->GetSchema().get();
    const auto* leftNode = leftSchema != nullptr ? leftSchema->FindNode(left.m_streamNodeId) : nullptr;
    const auto* rightNode = rightSchema != nullptr ? rightSchema->FindNode(right.m_streamNodeId) : nullptr;
    if (leftNode == nullptr || rightNode == nullptr || leftNode->typeId != rightNode->typeId ||
        leftNode->kind != rightNode->kind || leftNode->numericKind != rightNode->numericKind)
    {
        return false;
    }
    const auto leftCursor = NativeSerializedStreamCodec::ResolveCursor(left.m_owner->stagedStream, left.m_path);
    const auto rightCursor = NativeSerializedStreamCodec::ResolveCursor(right.m_owner->stagedStream, right.m_path);
    if (!leftCursor.has_value() || !rightCursor.has_value())
        return false;
    const auto leftSlice = leftCursor->Capture(leftNode->typeId);
    const auto rightSlice = rightCursor->Capture(rightNode->typeId);
    return leftSlice.has_value() && rightSlice.has_value() && leftSlice->Equivalent(*rightSlice);
}

bool SerializedProperty::DeleteCommand() const
{
    const auto element = ParseArrayElementPath(m_path);
    if (!element.has_value() || m_owner == nullptr)
        return false;
    const SerializedProperty parent(
        m_owner,
        element->parent.ToString(),
        m_schemaGeneration,
        m_structureGeneration);
    return parent.DeleteArrayElementAtIndex(element->index);
}

bool SerializedProperty::DuplicateCommand() const
{
    const auto element = ParseArrayElementPath(m_path);
    if (!element.has_value() || m_owner == nullptr)
        return false;
    const SerializedProperty parent(
        m_owner,
        element->parent.ToString(),
        m_schemaGeneration,
        m_structureGeneration);
    return parent.InsertArrayElementAtIndex(element->index);
}

bool SerializedProperty::Next(const bool enterChildren) const
{
    if (m_owner == nullptr || m_owner->stagedStream == nullptr ||
        m_owner->stagedStream->GetSchema() == nullptr || !IsValid())
        return false;
    const auto next = FindNextRuntimeTraversalEntry(
        m_owner->stagedStream,
        m_path,
        enterChildren,
        false);
    if (!next.has_value())
    {
        m_path.clear();
        m_streamNodeId = 0u;
        m_iteratorIndex = std::numeric_limits<size_t>::max();
        return false;
    }
    m_path = next->path;
    m_streamNodeId = 0u;
    m_cachedByteOffset = 0u;
    m_iteratorIndex = 0u;
    m_observedLayoutRevision = 0u;
    m_observedStreamStructureRevision = 0u;
    return SyncStreamCursor();
}

bool SerializedProperty::NextVisible(const bool enterChildren) const
{
    if (m_owner == nullptr || m_owner->stagedStream == nullptr ||
        m_owner->stagedStream->GetSchema() == nullptr)
        return false;
    if (!IsValid())
        return false;
    const auto next = FindNextRuntimeTraversalEntry(
        m_owner->stagedStream,
        m_path,
        enterChildren,
        true);
    if (!next.has_value())
    {
        m_path.clear();
        m_streamNodeId = 0u;
        m_iteratorIndex = std::numeric_limits<size_t>::max();
        return false;
    }
    m_path = next->path;
    m_streamNodeId = 0u;
    m_cachedByteOffset = 0u;
    m_iteratorIndex = 0u;
    m_observedLayoutRevision = 0u;
    m_observedStreamStructureRevision = 0u;
    return SyncStreamCursor();
}

SerializedProperty SerializedProperty::GetEndProperty(const bool includeInvisible) const
{
    if (m_owner == nullptr || m_owner->stagedStream == nullptr ||
        m_owner->stagedStream->GetSchema() == nullptr)
        return {};
    if (!SyncStreamCursor())
        return {};
    const auto next = FindNextRuntimeTraversalEntry(
        m_owner->stagedStream,
        m_path,
        false,
        !includeInvisible);
    if (!next.has_value())
        return {};
    return SerializedProperty(
        m_owner,
        next->path,
        m_schemaGeneration,
        m_structureGeneration,
        0u,
        next->nodeId);
}

size_t SerializedProperty::CountInProperty() const
{
    if (m_owner == nullptr || !IsValid())
        return 0u;
    bool currentSeen = false;
    size_t count = 0u;
    const bool valid = VisitRuntimeTraversal(m_owner->stagedStream, [&](RuntimeTraversalEntry entry)
    {
        if (!currentSeen)
        {
            if (entry.path == m_path)
            {
                currentSeen = true;
                if (entry.visible)
                    ++count;
            }
            return RuntimeTraversalVisitResult::Continue;
        }
        if (!IsRuntimeDescendant(entry.path, m_path))
            return RuntimeTraversalVisitResult::Stop;
        if (entry.visible)
            ++count;
        return RuntimeTraversalVisitResult::Continue;
    });
    return valid && currentSeen ? count : 0u;
}

size_t SerializedProperty::CountRemaining() const
{
    if (m_owner == nullptr || m_owner->stagedStream == nullptr ||
        m_owner->stagedStream->GetSchema() == nullptr)
        return 0u;
    if (!IsValid())
        return 0u;
    bool currentSeen = false;
    size_t count = 0u;
    const bool valid = VisitRuntimeTraversal(m_owner->stagedStream, [&](RuntimeTraversalEntry entry)
    {
        if (currentSeen)
        {
            if (entry.visible)
                ++count;
            return RuntimeTraversalVisitResult::Continue;
        }
        if (entry.path == m_path)
            currentSeen = true;
        return RuntimeTraversalVisitResult::Continue;
    });
    return valid && currentSeen ? count : 0u;
}

bool SerializedProperty::EqualContents(
    const SerializedProperty& left,
    const SerializedProperty& right)
{
    return left.m_owner == right.m_owner && left.m_streamNodeId == right.m_streamNodeId &&
        left.m_path == right.m_path && left.m_iteratorIndex == right.m_iteratorIndex;
}

const Inspector::EditorPropertyMetadata& SerializedProperty::GetMetadata() const
{
    // Draw metadata is a projection-side concern. The canonical TypeTree only
    // carries serialization semantics, so a SerializedProperty never reaches
    // back into an Inspector presentation to obtain UI callbacks or bindings.
    return EmptyMetadata();
}

Inspector::PropertyValueState SerializedProperty::GetValueState() const
{
    if (!IsValid() || m_owner == nullptr || m_owner->targetStreams.empty())
        return PropertyValueState::Invalid;
    if (PropertyPath(m_path).IsArraySize())
    {
        constexpr std::string_view suffix = ".Array.size";
        const auto parent = std::string_view(m_path).substr(0u, m_path.size() - suffix.size());
        for (const auto& stream : m_owner->targetStreams)
        {
            const auto cursor = NativeSerializedStreamCodec::ResolveCursor(stream, parent);
            uint32_t count = 0u;
            if (!cursor.has_value() || !cursor->Read(&count, sizeof(count)))
                return PropertyValueState::Invalid;
        }
    }

    for (const auto& stream : m_owner->targetStreams)
    {
        if (!PropertyPath(m_path).IsArraySize() &&
            !NativeSerializedStreamCodec::ResolveCursor(stream, m_path).has_value())
        {
            return PropertyValueState::Invalid;
        }
    }
    return HasMultipleDifferentValues() ? PropertyValueState::Mixed : PropertyValueState::Valid;
}

size_t SerializedProperty::GetTargetCount() const
{
    if (m_owner == nullptr)
        return 0u;
    return m_owner->targets.targets.size();
}

SerializedEditSession SerializedObject::BeginEditSession(const std::string_view path)
{
    return SerializedEditSession(this, FindProperty(path));
}

bool SerializedEditSession::Preview(const meta::Variant& value)
{
    return IsActive() && m_property.SetValue(value);
}

bool SerializedEditSession::Commit()
{
    if (!IsActive())
        return false;
    if (!m_owner->HasModifiedProperties())
    {
        m_active = false;
        return false;
    }
    const bool result = m_owner->ApplyModifiedProperties();
    if (result)
        m_active = false;
    return result;
}

bool SerializedEditSession::Cancel()
{
    if (!IsActive())
        return false;
    const bool hadChanges = m_owner->HasModifiedProperties();
    m_owner->DiscardModifiedProperties();
    m_active = false;
    return hadChanges;
}
} // namespace NLS::Editor::Serialization
