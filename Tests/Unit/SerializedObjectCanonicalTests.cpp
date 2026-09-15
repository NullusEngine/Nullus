#include <gtest/gtest.h>

#include <limits>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <Core/ServiceLocator.h>
#include <Math/Vector3.h>
#include <Math/BoundsInt.h>
#include <Math/RectInt.h>
#include <Math/Vector2Int.h>
#include <Math/Vector3Int.h>

#include "GameObject.h"
#include "Inspector/InspectorTypes.h"
#include "Serialize/BinaryBuffer.h"
#include "Serialize/PPtr.h"
#include "Serialize/SerializedSpecialTypes.h"
#include "Serialization/EditorSerializationAbi.h"
#include "Serialization/SerializedObject.h"
#include "Serialization/SerializedObjectHandleRegistry.h"
#include "Undo/EditorUndoService.h"

namespace NLS::Editor::Serialization::Tests
{
namespace
{
Inspector::InspectorTargetHandle MakeTarget(const char* path)
{
    return Inspector::InspectorTargetHandle::FromAsset(
        Inspector::InspectorTargetKind::Material,
        path,
        path);
}

class DynamicArrayBackend final : public ISerializedTargetBackend
{
public:
    explicit DynamicArrayBackend(
        const bool truncated = false,
        const bool hideTail = false,
        const bool animateValues = false)
        : m_truncated(truncated)
    {
        m_tree = std::make_shared<Engine::Serialize::SerializedTypeTree>();
        Engine::Serialize::SerializedTypeTreeNode root;
        root.serializedName = {};
        root.typeName = "Fixture";
        root.kind = Engine::Serialize::SerializedNodeKind::Generic;
        root.fixedByteSize = -1;
        const auto rootId = m_tree->AddNode(std::move(root));

        Engine::Serialize::SerializedTypeTreeNode values;
        values.parent = rootId;
        values.serializedName = "values";
        values.typeName = "Array<int32_t>";
        values.kind = Engine::Serialize::SerializedNodeKind::Generic;
        values.flags = Engine::Serialize::SerializedNodeFlags::Array |
            Engine::Serialize::SerializedNodeFlags::Align4After;
        if (animateValues)
            values.flags = values.flags | Engine::Serialize::SerializedNodeFlags::Animated;
        const auto valuesId = m_tree->AddNode(std::move(values));

        Engine::Serialize::SerializedTypeTreeNode element;
        element.parent = valuesId;
        element.serializedName = "data";
        element.typeName = "int32_t";
        element.typeId = NLS_TYPEOF(int32_t);
        element.kind = Engine::Serialize::SerializedNodeKind::Integer;
        element.numericKind = Engine::Serialize::SerializedNumericKind::Int32;
        element.fixedByteSize = 4;
        m_tree->AddNode(std::move(element));

        Engine::Serialize::SerializedTypeTreeNode tail;
        tail.parent = rootId;
        tail.serializedName = "tail";
        tail.typeName = "int32_t";
        tail.kind = Engine::Serialize::SerializedNodeKind::Integer;
        tail.numericKind = Engine::Serialize::SerializedNumericKind::Int32;
        tail.fixedByteSize = 4;
        if (hideTail)
            tail.flags = tail.flags | Engine::Serialize::SerializedNodeFlags::HideInInspector;
        m_tree->AddNode(std::move(tail));
        m_tree->RebuildIndexes();
    }

    Engine::Serialize::TypeTreeResult BuildTypeTree(
        const SerializedTargetSet&,
        SerializedInspectorMode) const override
    {
        return {m_tree, {}};
    }

    Engine::Serialize::StreamResult SerializeTarget(
        const SerializedTargetHandle& target,
        std::shared_ptr<const Engine::Serialize::SerializedTypeTree> schema,
        Engine::Serialize::SerializedReferenceTable& references) const override
    {
        auto stream = std::make_shared<Engine::Serialize::SerializedByteStream>(std::move(schema));
        const auto key = target.resourcePath;
        const auto stored = m_payloads.find(key);
        if (stored != m_payloads.end())
            stream->SetBytes(stored->second);
        else
            stream->SetBytes(InitialBytes(target));
        stream->SetReferences(std::make_shared<Engine::Serialize::SerializedReferenceTable>(references));
        return {std::move(stream), {}};
    }

    ValidationResult ValidateAndNormalize(
        const SerializedTargetHandle&,
        std::span<const SerializedPropertyPath>,
        Engine::Serialize::SerializedByteStream&) const override
    {
        return ValidationResult::Success();
    }

    ApplyResult DeserializeTarget(
        const SerializedTargetHandle& target,
        const Engine::Serialize::SerializedByteStream& stream,
        const Undo::UndoExecutionContext&) const override
    {
        m_payloads[target.resourcePath] = stream.GetBytes();
        return ApplyResult::Success();
    }

    SerializedVersionStamp GetVersion(const SerializedTargetHandle&) const override
    {
        return {m_dataVersion, 1u, 1u};
    }

    void SetDataVersion(const uint64_t version) { m_dataVersion = version; }

    [[nodiscard]] int32_t GetInt32(const SerializedTargetHandle& target, const size_t offset) const
    {
        const auto found = m_payloads.find(target.resourcePath);
        const auto& bytes = found != m_payloads.end() ? found->second : InitialBytes(target);
        if (offset > bytes.size() || sizeof(int32_t) > bytes.size() - offset)
            return 0;
        int32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }

private:
    [[nodiscard]] std::vector<uint8_t> InitialBytes(const SerializedTargetHandle& target) const
    {
        if (m_truncated)
            return {2u, 0u, 0u, 0u, 10u, 0u, 0u, 0u};
        const auto tail = target.resourcePath.find("second") != std::string::npos ? 77u : 99u;
        return {2u, 0u, 0u, 0u,
                10u, 0u, 0u, 0u,
                20u, 0u, 0u, 0u,
                static_cast<uint8_t>(tail), 0u, 0u, 0u};
    }

    bool m_truncated = false;
    uint64_t m_dataVersion = 1u;
    std::shared_ptr<Engine::Serialize::SerializedTypeTree> m_tree;
    mutable std::unordered_map<std::string, std::vector<uint8_t>> m_payloads;
};

// A fixed buffer is an Array-flagged node that freezes its element count:
// mutation commands are rejected while element cursors still resolve through
// the shared .Array.data[i] path.
class FixedBufferBackend final : public ISerializedTargetBackend
{
public:
    FixedBufferBackend()
    {
        m_tree = std::make_shared<Engine::Serialize::SerializedTypeTree>();
        Engine::Serialize::SerializedTypeTreeNode root;
        root.serializedName = {};
        root.typeName = "FixedBufferFixture";
        root.kind = Engine::Serialize::SerializedNodeKind::Generic;
        root.fixedByteSize = -1;
        const auto rootId = m_tree->AddNode(std::move(root));

        Engine::Serialize::SerializedTypeTreeNode buffer;
        buffer.parent = rootId;
        buffer.serializedName = "sampleBytes";
        buffer.typeName = "FixedBuffer<int32_t>";
        buffer.kind = Engine::Serialize::SerializedNodeKind::Generic;
        buffer.flags = Engine::Serialize::SerializedNodeFlags::Array |
            Engine::Serialize::SerializedNodeFlags::FixedBuffer |
            Engine::Serialize::SerializedNodeFlags::Align4After;
        buffer.fixedByteSize = 16;
        const auto bufferId = m_tree->AddNode(std::move(buffer));

        Engine::Serialize::SerializedTypeTreeNode element;
        element.parent = bufferId;
        element.serializedName = "data";
        element.typeName = "int32_t";
        element.typeId = NLS_TYPEOF(int32_t);
        element.kind = Engine::Serialize::SerializedNodeKind::Integer;
        element.numericKind = Engine::Serialize::SerializedNumericKind::Int32;
        element.fixedByteSize = 4;
        m_tree->AddNode(std::move(element));
        m_tree->RebuildIndexes();
    }

    Engine::Serialize::TypeTreeResult BuildTypeTree(
        const SerializedTargetSet&,
        SerializedInspectorMode) const override
    {
        return {m_tree, {}};
    }

    Engine::Serialize::StreamResult SerializeTarget(
        const SerializedTargetHandle&,
        std::shared_ptr<const Engine::Serialize::SerializedTypeTree> schema,
        Engine::Serialize::SerializedReferenceTable& references) const override
    {
        auto stream = std::make_shared<Engine::Serialize::SerializedByteStream>(std::move(schema));
        // uint32 count followed by four contiguous int32 elements: 1, 258, 3, 4.
        stream->SetBytes(std::vector<uint8_t> {
            4u, 0u, 0u, 0u,
            1u, 0u, 0u, 0u,
            2u, 1u, 0u, 0u,
            3u, 0u, 0u, 0u,
            4u, 0u, 0u, 0u});
        stream->SetReferences(std::make_shared<Engine::Serialize::SerializedReferenceTable>(references));
        return {std::move(stream), {}};
    }

    ValidationResult ValidateAndNormalize(
        const SerializedTargetHandle&,
        std::span<const SerializedPropertyPath>,
        Engine::Serialize::SerializedByteStream&) const override
    {
        return ValidationResult::Success();
    }

    ApplyResult DeserializeTarget(
        const SerializedTargetHandle&,
        const Engine::Serialize::SerializedByteStream&,
        const Undo::UndoExecutionContext&) const override
    {
        return ApplyResult::Success();
    }

    SerializedVersionStamp GetVersion(const SerializedTargetHandle&) const override { return {1u, 1u, 1u}; }

private:
    std::shared_ptr<Engine::Serialize::SerializedTypeTree> m_tree;
};

class ReferenceArrayBackend final : public ISerializedTargetBackend
{
public:
    ReferenceArrayBackend()
    {
        m_tree = std::make_shared<Engine::Serialize::SerializedTypeTree>();
        Engine::Serialize::SerializedTypeTreeNode root;
        root.serializedName = {};
        root.typeName = "ReferenceFixture";
        root.kind = Engine::Serialize::SerializedNodeKind::Generic;
        root.fixedByteSize = -1;
        const auto rootId = m_tree->AddNode(std::move(root));

        Engine::Serialize::SerializedTypeTreeNode references;
        references.parent = rootId;
        references.serializedName = "references";
        references.typeName = "Array<PPtr<GameObject>>";
        references.kind = Engine::Serialize::SerializedNodeKind::Generic;
        references.flags = Engine::Serialize::SerializedNodeFlags::Array |
            Engine::Serialize::SerializedNodeFlags::Align4After;
        const auto referencesId = m_tree->AddNode(std::move(references));

        Engine::Serialize::SerializedTypeTreeNode element;
        element.parent = referencesId;
        element.serializedName = "data";
        element.typeName = "PPtr<GameObject>";
        element.typeId = NLS_TYPEOF(NLS::Engine::Serialize::PPtr<NLS::Engine::GameObject>);
        element.kind = Engine::Serialize::SerializedNodeKind::ObjectReference;
        element.fixedByteSize = 4;
        element.flags = Engine::Serialize::SerializedNodeFlags::HasObjectReference;
        m_tree->AddNode(std::move(element));
        m_tree->RebuildIndexes();
    }

    Engine::Serialize::TypeTreeResult BuildTypeTree(
        const SerializedTargetSet&, SerializedInspectorMode) const override
    {
        return {m_tree, {}};
    }

    Engine::Serialize::StreamResult SerializeTarget(
        const SerializedTargetHandle& target,
        std::shared_ptr<const Engine::Serialize::SerializedTypeTree> schema,
        Engine::Serialize::SerializedReferenceTable& references) const override
    {
        auto stream = std::make_shared<Engine::Serialize::SerializedByteStream>(std::move(schema));
        const auto found = m_payloads.find(target.resourcePath);
        stream->SetBytes(found != m_payloads.end()
            ? found->second
            : std::vector<uint8_t> {2u, 0u, 0u, 0u, 1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u});
        if (references.GetEntries().empty())
        {
            Engine::Serialize::SerializedReferenceEntry entry;
            entry.resourcePath = "Assets/reference-target.prefab";
            entry.expectedTypeName = NLS_TYPEOF(NLS::Engine::GameObject).GetName();
            references.Intern(std::move(entry));
        }
        stream->SetReferences(std::make_shared<Engine::Serialize::SerializedReferenceTable>(references));
        return {std::move(stream), {}};
    }

    ValidationResult ValidateAndNormalize(
        const SerializedTargetHandle&, std::span<const SerializedPropertyPath>,
        Engine::Serialize::SerializedByteStream&) const override
    {
        return ValidationResult::Success();
    }

    ApplyResult DeserializeTarget(
        const SerializedTargetHandle& target,
        const Engine::Serialize::SerializedByteStream& stream,
        const Undo::UndoExecutionContext&) const override
    {
        m_payloads[target.resourcePath] = stream.GetBytes();
        return ApplyResult::Success();
    }

    SerializedVersionStamp GetVersion(const SerializedTargetHandle&) const override { return {1u, 1u, 1u}; }

    [[nodiscard]] uint32_t GetSlot(const SerializedTargetHandle& target, const size_t offset) const
    {
        const auto found = m_payloads.find(target.resourcePath);
        const auto& bytes = found != m_payloads.end() ? found->second : m_initial;
        uint32_t value = 0u;
        if (offset <= bytes.size() && sizeof(value) <= bytes.size() - offset)
            std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }

private:
    const std::vector<uint8_t> m_initial {2u, 0u, 0u, 0u, 1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    std::shared_ptr<Engine::Serialize::SerializedTypeTree> m_tree;
    mutable std::unordered_map<std::string, std::vector<uint8_t>> m_payloads;
};

class ScopedUndoService final
{
public:
    ScopedUndoService()
    {
        NLS::Core::ServiceLocator::Provide(m_service);
    }

    ~ScopedUndoService()
    {
        NLS::Core::ServiceLocator::Remove<Undo::EditorUndoService>();
    }

    [[nodiscard]] Undo::EditorUndoService& Get() { return m_service; }

private:
    Undo::EditorUndoService m_service;
};
} // namespace

TEST(SerializedObjectCanonicalTests, RuntimeIteratorExpandsDynamicArrayPathsAndSkipsChildren)
{
    const auto target = MakeTarget("Assets/runtime-iterator.mat");
    SerializedTargetSet targets;
    targets.targets = {target};
    SerializedObject serialized(targets, std::make_shared<DynamicArrayBackend>());
    ASSERT_TRUE(serialized.IsValid());

    auto iterator = serialized.GetIterator();
    ASSERT_TRUE(iterator);
    ASSERT_TRUE(iterator.Next(true));
    EXPECT_EQ(iterator.GetPropertyPath(), "values");
    ASSERT_TRUE(iterator.Next(false));
    EXPECT_EQ(iterator.GetPropertyPath(), "tail");

    iterator = serialized.GetIterator();
    ASSERT_TRUE(iterator.NextVisible(true));
    EXPECT_EQ(iterator.GetPropertyPath(), "values");
    ASSERT_TRUE(iterator.NextVisible(true));
    EXPECT_EQ(iterator.GetPropertyPath(), "values.Array.size");
    ASSERT_TRUE(iterator.NextVisible(true));
    EXPECT_EQ(iterator.GetPropertyPath(), "values.Array.data[0]");
    ASSERT_TRUE(iterator.NextVisible(true));
    EXPECT_EQ(iterator.GetPropertyPath(), "values.Array.data[1]");
    ASSERT_TRUE(iterator.NextVisible(true));
    EXPECT_EQ(iterator.GetPropertyPath(), "tail");
    EXPECT_FALSE(iterator.NextVisible(true));
}

TEST(SerializedObjectCanonicalTests, RuntimeIteratorEndAndCountsUseEncodedArrayLength)
{
    const auto target = MakeTarget("Assets/runtime-counts.mat");
    SerializedTargetSet targets;
    targets.targets = {target};
    SerializedObject serialized(targets, std::make_shared<DynamicArrayBackend>());
    auto iterator = serialized.GetIterator();
    ASSERT_TRUE(iterator);
    ASSERT_TRUE(iterator.Next(true));
    EXPECT_EQ(iterator.GetPropertyPath(), "values");
    EXPECT_EQ(iterator.CountInProperty(), 4u);
    EXPECT_EQ(iterator.CountRemaining(), 4u);

    const auto end = iterator.GetEndProperty();
    ASSERT_TRUE(end);
    EXPECT_EQ(end.GetPropertyPath(), "tail");
    EXPECT_EQ(end.CountRemaining(), 0u);
}

TEST(SerializedObjectCanonicalTests, RuntimeIteratorUsesEditorVisibleEndAndAnimatedSchemaFlags)
{
    SerializedTargetSet targets;
    targets.targets = {MakeTarget("Assets/runtime-visible-end.mat")};
    SerializedObject serialized(targets, std::make_shared<DynamicArrayBackend>(false, true, true));
    const auto values = serialized.FindProperty("values");
    ASSERT_TRUE(values);
    EXPECT_TRUE(values.IsAnimated());
    EXPECT_EQ(values.CountInProperty(), 4u);

    // the external editor's GetEndProperty(false) walks visible fields only. The sole next
    // sibling is hidden, so the result is the iterator end sentinel.
    EXPECT_FALSE(values.GetEndProperty(false));
    const auto includingInvisible = values.GetEndProperty(true);
    ASSERT_TRUE(includingInvisible);
    EXPECT_EQ(includingInvisible.GetPropertyPath(), "tail");
}

TEST(SerializedObjectCanonicalTests, DirectStreamRelativeAndArrayPropertiesResolveFromTypeTree)
{
    SerializedTargetSet targets;
    targets.targets = {MakeTarget("Assets/runtime-relative.mat")};
    SerializedObject serialized(targets, std::make_shared<DynamicArrayBackend>());
    ASSERT_TRUE(serialized.IsValid());

    const auto values = serialized.FindProperty("values");
    ASSERT_TRUE(values);
    EXPECT_TRUE(values.IsArray());
    EXPECT_EQ(values.GetArraySize(), 2u);

    const auto size = values.FindPropertyRelative("Array.size");
    ASSERT_TRUE(size);
    EXPECT_EQ(size.GetPropertyPath(), "values.Array.size");
    EXPECT_EQ(size.GetPropertyType(), SerializedPropertyType::ArraySize);
    EXPECT_EQ(size.GetLongValue(), 2);

    const auto first = values.GetArrayElementAtIndex(0u);
    const auto second = values.GetArrayElementAtIndex(1u);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.GetPropertyPath(), "values.Array.data[0]");
    EXPECT_EQ(second.GetPropertyPath(), "values.Array.data[1]");
    EXPECT_TRUE(first.IsEditable());
    EXPECT_EQ(first.GetLongValue(), 10);
    EXPECT_EQ(second.GetLongValue(), 20);
    EXPECT_FALSE(values.GetArrayElementAtIndex(2u));

    ASSERT_TRUE(first.SetLongValue(30));
    EXPECT_EQ(first.GetLongValue(), 30);
    EXPECT_EQ(second.GetLongValue(), 20);
    EXPECT_TRUE(serialized.HasModifiedProperties());
    ASSERT_TRUE(serialized.ApplyModifiedPropertiesWithoutUndo());
    EXPECT_FALSE(serialized.HasModifiedProperties());

    const auto root = serialized.GetIterator();
    ASSERT_TRUE(root);
    const auto relative = root.FindPropertyRelative("values.Array.data[1]");
    ASSERT_TRUE(relative);
    EXPECT_EQ(relative.GetLongValue(), 20);

    const auto directSize = serialized.FindProperty("values.Array.size");
    ASSERT_TRUE(directSize);
    EXPECT_EQ(directSize.GetLongValue(), 2);
}

TEST(SerializedObjectCanonicalTests, DirectStreamMultiTargetApplyCopiesOnlyModifiedPathsAndReplaysUndo)
{
    ScopedUndoService undo;
    auto backend = std::make_shared<DynamicArrayBackend>();
    const auto firstTarget = MakeTarget("Assets/first-array.mat");
    const auto secondTarget = MakeTarget("Assets/second-array.mat");
    SerializedTargetSet targets;
    targets.targets = {firstTarget, secondTarget};
    targets.Normalize();

    SerializedObject serialized(targets, backend);
    ASSERT_TRUE(serialized.IsValid());
    const auto values = serialized.FindProperty("values");
    ASSERT_TRUE(values);
    const auto first = values.GetArrayElementAtIndex(0u);
    ASSERT_TRUE(first);
    ASSERT_TRUE(first.SetLongValue(30));
    ASSERT_TRUE(serialized.ApplyModifiedProperties());

    EXPECT_EQ(backend->GetInt32(firstTarget, 4u), 30);
    EXPECT_EQ(backend->GetInt32(secondTarget, 4u), 30);
    EXPECT_EQ(backend->GetInt32(firstTarget, 12u), 99);
    EXPECT_EQ(backend->GetInt32(secondTarget, 12u), 77);

    ASSERT_TRUE(undo.Get().RequestUndo().Succeeded());
    EXPECT_EQ(backend->GetInt32(firstTarget, 4u), 10);
    EXPECT_EQ(backend->GetInt32(secondTarget, 4u), 10);
    EXPECT_EQ(backend->GetInt32(firstTarget, 12u), 99);
    EXPECT_EQ(backend->GetInt32(secondTarget, 12u), 77);

    ASSERT_TRUE(undo.Get().RequestRedo().Succeeded());
    EXPECT_EQ(backend->GetInt32(firstTarget, 4u), 30);
    EXPECT_EQ(backend->GetInt32(secondTarget, 4u), 30);
    EXPECT_EQ(backend->GetInt32(firstTarget, 12u), 99);
    EXPECT_EQ(backend->GetInt32(secondTarget, 12u), 77);
}

TEST(SerializedObjectCanonicalTests, DirectStreamArrayStructureUsesOnlyTheByteStream)
{
    auto backend = std::make_shared<DynamicArrayBackend>();
    SerializedTargetSet targets;
    const auto target = MakeTarget("Assets/array-structure.mat");
    targets.targets = {target};
    SerializedObject serialized(targets, backend);
    ASSERT_TRUE(serialized.IsValid());

    const auto values = serialized.FindProperty("values");
    const auto oldFirst = values.GetArrayElementAtIndex(0u);
    ASSERT_TRUE(values);
    ASSERT_TRUE(oldFirst);
    ASSERT_TRUE(values.InsertArrayElementAtIndex(1u));
    EXPECT_FALSE(oldFirst.IsValid());
    EXPECT_EQ(values.GetArraySize(), 3u);
    EXPECT_EQ(values.GetArrayElementAtIndex(0u).GetLongValue(), 10);
    EXPECT_EQ(values.GetArrayElementAtIndex(1u).GetLongValue(), 20);
    EXPECT_EQ(values.GetArrayElementAtIndex(2u).GetLongValue(), 20);

    ASSERT_TRUE(values.MoveArrayElement(2u, 0u));
    EXPECT_EQ(values.GetArrayElementAtIndex(0u).GetLongValue(), 20);
    EXPECT_EQ(values.GetArrayElementAtIndex(1u).GetLongValue(), 10);
    ASSERT_TRUE(values.DeleteArrayElementAtIndex(0u));
    EXPECT_EQ(values.GetArraySize(), 2u);
    EXPECT_EQ(values.GetArrayElementAtIndex(0u).GetLongValue(), 10);
    ASSERT_TRUE(values.SetArraySize(4u));
    EXPECT_EQ(values.GetArraySize(), 4u);
    EXPECT_EQ(values.GetArrayElementAtIndex(2u).GetLongValue(), 0);
    EXPECT_EQ(values.GetArrayElementAtIndex(3u).GetLongValue(), 0);
    ASSERT_TRUE(serialized.ApplyModifiedPropertiesWithoutUndo());
    EXPECT_EQ(backend->GetInt32(target, 20u), 99);
}

TEST(SerializedObjectCanonicalTests, DirectStreamArrayStructureAppliesAtomicallyAndReplaysUndo)
{
    ScopedUndoService undo;
    auto backend = std::make_shared<DynamicArrayBackend>();
    const auto firstTarget = MakeTarget("Assets/first-array-structure.mat");
    const auto secondTarget = MakeTarget("Assets/second-array-structure.mat");
    SerializedTargetSet targets;
    targets.targets = {firstTarget, secondTarget};
    targets.Normalize();

    SerializedObject serialized(targets, backend);
    const auto values = serialized.FindProperty("values");
    ASSERT_TRUE(values);
    ASSERT_TRUE(values.InsertArrayElementAtIndex(2u));
    ASSERT_TRUE(serialized.ApplyModifiedProperties());

    // Only the array subtree is copied to each candidate.  Each target retains
    // its own trailing field despite the variable-size splice changing its
    // byte offset.
    EXPECT_EQ(backend->GetInt32(firstTarget, 0u), 3);
    EXPECT_EQ(backend->GetInt32(secondTarget, 0u), 3);
    EXPECT_EQ(backend->GetInt32(firstTarget, 12u), 0);
    EXPECT_EQ(backend->GetInt32(secondTarget, 12u), 0);
    EXPECT_EQ(backend->GetInt32(firstTarget, 16u), 99);
    EXPECT_EQ(backend->GetInt32(secondTarget, 16u), 77);

    ASSERT_TRUE(undo.Get().RequestUndo().Succeeded());
    EXPECT_EQ(backend->GetInt32(firstTarget, 0u), 2);
    EXPECT_EQ(backend->GetInt32(secondTarget, 0u), 2);
    EXPECT_EQ(backend->GetInt32(firstTarget, 12u), 99);
    EXPECT_EQ(backend->GetInt32(secondTarget, 12u), 77);

    ASSERT_TRUE(undo.Get().RequestRedo().Succeeded());
    EXPECT_EQ(backend->GetInt32(firstTarget, 0u), 3);
    EXPECT_EQ(backend->GetInt32(secondTarget, 0u), 3);
    EXPECT_EQ(backend->GetInt32(firstTarget, 16u), 99);
    EXPECT_EQ(backend->GetInt32(secondTarget, 16u), 77);
}

TEST(SerializedObjectCanonicalTests, DirectStreamObjectReferenceArrayRequiresTwoDeletes)
{
    auto backend = std::make_shared<ReferenceArrayBackend>();
    const auto target = MakeTarget("Assets/reference-array.mat");
    SerializedTargetSet targets;
    targets.targets = {target};
    SerializedObject serialized(targets, backend);
    const auto references = serialized.FindProperty("references");
    ASSERT_TRUE(references);
    ASSERT_EQ(references.GetArraySize(), 2u);

    // the external editor's ObjectReference array deletion first clears a non-null element;
    // only the second delete removes its slot from the array.
    ASSERT_TRUE(references.DeleteArrayElementAtIndex(0u));
    EXPECT_EQ(references.GetArraySize(), 2u);
    ASSERT_TRUE(serialized.ApplyModifiedPropertiesWithoutUndo());
    EXPECT_EQ(backend->GetSlot(target, 0u), 2u);
    EXPECT_EQ(backend->GetSlot(target, 4u), 0u);

    // Array structure mutations conservatively stale element cursors. Reopen
    // the parent after applying the reference clear before removing the slot.
    serialized.Update();
    const auto refreshedReferences = serialized.FindProperty("references");
    ASSERT_TRUE(refreshedReferences);
    ASSERT_TRUE(refreshedReferences.DeleteArrayElementAtIndex(0u));
    EXPECT_EQ(refreshedReferences.GetArraySize(), 1u);
    ASSERT_TRUE(serialized.ApplyModifiedPropertiesWithoutUndo());
    EXPECT_EQ(backend->GetSlot(target, 0u), 1u);
    EXPECT_EQ(backend->GetSlot(target, 4u), 0u);
}

TEST(SerializedObjectCanonicalTests, RuntimeIteratorRejectsTruncatedDynamicArrayPayload)
{
    SerializedTargetSet targets;
    targets.targets = {MakeTarget("Assets/runtime-truncated.mat")};
    SerializedObject serialized(targets, std::make_shared<DynamicArrayBackend>(true));
    ASSERT_TRUE(serialized.IsValid());

    auto iterator = serialized.GetIterator();
    ASSERT_TRUE(iterator);
    // A malformed dynamic array invalidates its cursor before the iterator can
    // expose any partially decoded child or calculate an out-of-range offset.
    EXPECT_FALSE(iterator.NextVisible(true));
    EXPECT_TRUE(iterator.GetPropertyPath().empty());
}

TEST(SerializedObjectCanonicalTests, EditorExposedReferenceResolverUsesDurableContextAndCanClear)
{
    const auto context = MakeTarget("Assets/exposed-context.prefab");
    auto target = MakeTarget("Assets/exposed-target.prefab");
    target.instanceId = 12345;

    EditorExposedReferenceResolver resolver;
    resolver.Bind(context, "Anchor", target);
    const auto resolved = resolver.Resolve(context, "Anchor");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->resourcePath, target.resourcePath);
    EXPECT_EQ(resolved->instanceId, InstanceID_None);

    resolver.ClearContext(context);
    EXPECT_FALSE(resolver.Resolve(context, "Anchor").has_value());
}

TEST(SerializedObjectCanonicalTests, NativeHandleRegistryRejectsRecycledStaleHandles)
{
    SerializedObjectHandleRegistry registry;
    auto object = std::make_shared<SerializedObject>();
    const auto first = registry.Register(object);
    ASSERT_NE(first, 0u);
    EXPECT_EQ(registry.Resolve(first), object);
    ASSERT_TRUE(registry.Release(first));
    EXPECT_EQ(registry.Resolve(first), nullptr);

    const auto second = registry.Register(std::make_shared<SerializedObject>());
    ASSERT_NE(second, 0u);
    EXPECT_NE(second, first);
    EXPECT_EQ(registry.Resolve(second) != nullptr, true);
}

TEST(SerializedObjectCanonicalTests, SerializedValuesRoundTripSpecialSerializedValues)
{
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Engine::GameObject referenceObject("Special Value Target");

    NLS::Engine::Serialize::AnimationCurve curve;
    curve.keys = {{0.0f, 1.0f, 0.0f, 0.5f}, {1.0f, 2.0f, 0.5f, 0.0f}};
    curve.preWrapMode = 1;
    curve.postWrapMode = 2;
    NLS::Engine::Serialize::Gradient gradient;
    gradient.colorKeys = {
        {Maths::Color(1.0f, 0.0f, 0.0f, 1.0f), 0.0f},
        {Maths::Color(0.0f, 1.0f, 0.0f, 1.0f), 1.0f}};
    gradient.alphaKeys = {{0.2f, 0.0f}, {0.8f, 1.0f}};
    gradient.mode = 1;
    NLS::Engine::Serialize::ExposedReference exposed;
    exposed.defaultValue = NLS::Engine::Serialize::PPtr<NLS::Engine::GameObject>(&referenceObject);
    exposed.exposedName = "initial";
    const auto curveSnapshot = SerializedValue::Capture(meta::Variant(curve));
    const auto gradientSnapshot = SerializedValue::Capture(meta::Variant(gradient));
    const auto exposedSnapshot = SerializedValue::Capture(meta::Variant(exposed));
    ASSERT_TRUE(curveSnapshot.has_value());
    ASSERT_TRUE(gradientSnapshot.has_value());
    ASSERT_TRUE(exposedSnapshot.has_value());
    EXPECT_TRUE(curveSnapshot->Resolve(NLS_TYPEOF(NLS::Engine::Serialize::AnimationCurve)).has_value());
    EXPECT_TRUE(gradientSnapshot->Resolve(NLS_TYPEOF(NLS::Engine::Serialize::Gradient)).has_value());
    const auto restoredExposed = exposedSnapshot->Resolve(NLS_TYPEOF(NLS::Engine::Serialize::ExposedReference));
    ASSERT_TRUE(restoredExposed.has_value());
    EXPECT_EQ(restoredExposed->GetValue<NLS::Engine::Serialize::ExposedReference>().exposedName, "initial");
    EXPECT_EQ(restoredExposed->GetValue<NLS::Engine::Serialize::ExposedReference>().defaultValue.Get(), &referenceObject);
}

TEST(SerializedObjectCanonicalTests, CopyFromSerializedPropertyTransfersStagedContentsBetweenObjects)
{
    SerializedTargetSet sourceTargets;
    sourceTargets.targets = {MakeTarget("Assets/copy-source.mat")};
    SerializedObject source(sourceTargets, std::make_shared<DynamicArrayBackend>());
    const auto sourceTail = source.FindProperty("tail");
    ASSERT_TRUE(sourceTail);
    ASSERT_TRUE(sourceTail.SetLongValue(123));

    SerializedTargetSet destinationTargets;
    destinationTargets.targets = {MakeTarget("Assets/copy-destination.mat")};
    SerializedObject destination(destinationTargets, std::make_shared<DynamicArrayBackend>());
    ASSERT_TRUE(destination.IsValid());
    EXPECT_FALSE(destination.HasModifiedProperties());

    ASSERT_TRUE(destination.CopyFromSerializedProperty(sourceTail));
    EXPECT_EQ(destination.FindProperty("tail").GetLongValue(), 123);
    EXPECT_TRUE(destination.HasModifiedProperties());

    // Copying an identical value must not stage a redundant edit.
    EXPECT_FALSE(destination.CopyFromSerializedPropertyIfDifferent(sourceTail));
    ASSERT_TRUE(sourceTail.SetLongValue(124));
    EXPECT_TRUE(destination.CopyFromSerializedPropertyIfDifferent(sourceTail));
    EXPECT_EQ(destination.FindProperty("tail").GetLongValue(), 124);
}

TEST(SerializedObjectCanonicalTests, PropertyCopyEqualContentsAndDataEqualsFollowEditorSemantics)
{
    SerializedTargetSet targets;
    targets.targets = {MakeTarget("Assets/property-copy.mat")};
    SerializedObject serialized(targets, std::make_shared<DynamicArrayBackend>());
    const auto values = serialized.FindProperty("values");
    ASSERT_TRUE(values);
    const auto first = values.GetArrayElementAtIndex(0u);
    const auto second = values.GetArrayElementAtIndex(1u);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);

    // EqualContents is property identity (the editor's operator==), so a copy
    // of the same cursor matches while a sibling element does not.
    const auto copy = first.Copy();
    EXPECT_TRUE(SerializedProperty::EqualContents(first, copy));
    EXPECT_FALSE(SerializedProperty::EqualContents(first, second));

    // DataEquals compares the serialized payload instead of the cursor.
    EXPECT_TRUE(SerializedProperty::DataEquals(first, copy));
    EXPECT_FALSE(SerializedProperty::DataEquals(first, second));

    // A copy advances independently of the cursor it was copied from.
    auto advanced = first.Copy();
    ASSERT_TRUE(advanced.Next(false));
    EXPECT_EQ(first.GetPropertyPath(), "values.Array.data[0]");
    EXPECT_NE(advanced.GetPropertyPath(), first.GetPropertyPath());
}

TEST(SerializedObjectCanonicalTests, MaxArraySizeForMultiEditingLimitsMultiSelectionArrayEdits)
{
    auto backend = std::make_shared<DynamicArrayBackend>();
    SerializedTargetSet targets;
    targets.targets = {
        MakeTarget("Assets/multi-max-first.mat"),
        MakeTarget("Assets/multi-max-second.mat")};
    targets.Normalize();
    SerializedObject serialized(targets, backend);
    ASSERT_TRUE(serialized.IsEditingMultipleObjects());
    EXPECT_EQ(serialized.GetMaxArraySizeForMultiEditing(), 64u);

    const auto values = serialized.FindProperty("values");
    ASSERT_TRUE(values);
    ASSERT_EQ(values.GetArraySize(), 2u);

    serialized.SetMaxArraySizeForMultiEditing(1u);
    EXPECT_EQ(serialized.GetMaxArraySizeForMultiEditing(), 1u);
    EXPECT_FALSE(values.InsertArrayElementAtIndex(0u));
    EXPECT_FALSE(values.SetArraySize(3u));
    EXPECT_EQ(values.GetArraySize(), 2u);

    serialized.SetMaxArraySizeForMultiEditing(8u);
    EXPECT_TRUE(values.SetArraySize(3u));
    EXPECT_EQ(values.GetArraySize(), 3u);
}

TEST(SerializedObjectCanonicalTests, SetIsDifferentCacheDirtyKeepsMixedValueAnswersConsistent)
{
    auto backend = std::make_shared<DynamicArrayBackend>();
    SerializedTargetSet targets;
    targets.targets = {
        MakeTarget("Assets/mixed-first.mat"),
        MakeTarget("Assets/mixed-second.mat")};
    targets.Normalize();
    SerializedObject serialized(targets, backend);

    const auto tail = serialized.FindProperty("tail");
    ASSERT_TRUE(tail);
    const auto firstElement = serialized.FindProperty("values").GetArrayElementAtIndex(0u);
    ASSERT_TRUE(firstElement);
    EXPECT_TRUE(tail.HasMultipleDifferentValues());
    EXPECT_FALSE(firstElement.HasMultipleDifferentValues());
    EXPECT_EQ(tail.GetValueState(), Inspector::PropertyValueState::Mixed);

    serialized.SetIsDifferentCacheDirty();

    EXPECT_TRUE(tail.HasMultipleDifferentValues());
    EXPECT_FALSE(firstElement.HasMultipleDifferentValues());
    EXPECT_EQ(tail.GetValueState(), Inspector::PropertyValueState::Mixed);
}

TEST(SerializedObjectCanonicalTests, ArrayElementDeleteAndDuplicateCommandsMutateTheirParent)
{
    SerializedTargetSet targets;
    targets.targets = {MakeTarget("Assets/array-commands.mat")};
    SerializedObject serialized(targets, std::make_shared<DynamicArrayBackend>());
    const auto values = serialized.FindProperty("values");
    ASSERT_TRUE(values);
    ASSERT_EQ(values.GetArraySize(), 2u);

    const auto firstElement = values.GetArrayElementAtIndex(0u);
    ASSERT_TRUE(firstElement);
    ASSERT_TRUE(firstElement.DuplicateCommand());
    EXPECT_EQ(values.GetArraySize(), 3u);
    EXPECT_EQ(values.GetArrayElementAtIndex(0u).GetLongValue(), 10);
    EXPECT_EQ(values.GetArrayElementAtIndex(1u).GetLongValue(), 10);
    EXPECT_EQ(values.GetArrayElementAtIndex(2u).GetLongValue(), 20);

    const auto duplicated = values.GetArrayElementAtIndex(1u);
    ASSERT_TRUE(duplicated);
    ASSERT_TRUE(duplicated.DeleteCommand());
    EXPECT_EQ(values.GetArraySize(), 2u);
    EXPECT_EQ(values.GetArrayElementAtIndex(0u).GetLongValue(), 10);
    EXPECT_EQ(values.GetArrayElementAtIndex(1u).GetLongValue(), 20);

    // The commands only apply to array element cursors.
    EXPECT_FALSE(values.DuplicateCommand());
    EXPECT_FALSE(values.DeleteCommand());
}

TEST(SerializedObjectCanonicalTests, FixedBufferPropertiesResolveContiguousElements)
{
    SerializedTargetSet targets;
    targets.targets = {MakeTarget("Assets/fixed-buffer.mat")};
    SerializedObject serialized(targets, std::make_shared<FixedBufferBackend>());
    const auto buffer = serialized.FindProperty("sampleBytes");
    ASSERT_TRUE(buffer);
    EXPECT_TRUE(buffer.IsFixedBuffer());
    EXPECT_EQ(buffer.GetFixedBufferSize(), 4u);
    EXPECT_EQ(buffer.GetArraySize(), 4u);

    const auto second = buffer.GetFixedBufferElementAtIndex(1u);
    ASSERT_TRUE(second);
    EXPECT_EQ(second.GetPropertyPath(), "sampleBytes.Array.data[1]");
    EXPECT_EQ(second.GetLongValue(), 258);
    EXPECT_EQ(buffer.GetFixedBufferElementAtIndex(3u).GetLongValue(), 4);
    EXPECT_FALSE(buffer.GetFixedBufferElementAtIndex(4u));

    // A fixed buffer keeps its element count: the resize commands are refused.
    EXPECT_FALSE(buffer.SetArraySize(8u));
    EXPECT_FALSE(buffer.InsertArrayElementAtIndex(0u));
    EXPECT_FALSE(buffer.DeleteArrayElementAtIndex(0u));
    EXPECT_EQ(buffer.GetFixedBufferSize(), 4u);
}

TEST(SerializedObjectCanonicalTests, UpdateIfDirtyOrScriptMatchesUpdateIfRequiredOrScript)
{
    auto backend = std::make_shared<DynamicArrayBackend>();
    SerializedTargetSet targets;
    targets.targets = {MakeTarget("Assets/update-alias.mat")};
    SerializedObject serialized(targets, backend);
    ASSERT_TRUE(serialized.IsValid());

    // The version stamp is unchanged, so neither spelling refreshes.
    EXPECT_FALSE(serialized.UpdateIfRequiredOrScript());
    EXPECT_FALSE(serialized.UpdateIfDirtyOrScript());

    backend->SetDataVersion(2u);
    EXPECT_TRUE(serialized.UpdateIfDirtyOrScript());
    EXPECT_FALSE(serialized.UpdateIfRequiredOrScript());
}
} // namespace NLS::Editor::Serialization::Tests
