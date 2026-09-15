#include <gtest/gtest.h>

#include <memory>
#include <utility>

#include "Undo/EditorUndoService.h"

namespace NLS::Editor::Undo::Tests
{
namespace
{
class IntegerCommand final : public IEditorUndoCommand
{
public:
    IntegerCommand(
        std::shared_ptr<int> value,
        int before,
        int after,
        UndoCommandInfo info)
        : m_value(std::move(value))
        , m_before(before)
        , m_after(after)
        , m_info(std::move(info))
    {
    }

    [[nodiscard]] UndoCommandInfo GetInfo() const override { return m_info; }

    [[nodiscard]] UndoValidationResult Validate(
        UndoDirection,
        const UndoExecutionContext&) const override
    {
        return m_value ? UndoValidationResult::Ready() : UndoValidationResult::Stale("Value is gone.");
    }

    UndoStartResult Apply(UndoDirection direction, const UndoExecutionContext&, UndoCompletion) override
    {
        if (!m_value)
            return UndoStartResult::Failed("Value is gone.");
        *m_value = direction == UndoDirection::Redo ? m_after : m_before;
        return UndoStartResult::Completed();
    }

    bool TryMergeFrom(const IEditorUndoCommand&) override { return false; }

private:
    std::shared_ptr<int> m_value;
    int m_before = 0;
    int m_after = 0;
    UndoCommandInfo m_info;
};

UndoCommandInfo MakeInfo(
    std::string label,
    std::vector<UndoScopeId> scopes,
    std::vector<UndoScopeId> writeThroughScopes = {})
{
    UndoCommandInfo info;
    info.label = std::move(label);
    info.sourceModule = "UndoWriteThroughTests";
    info.scopes = std::move(scopes);
    info.writeThroughScopes = std::move(writeThroughScopes);
    return info;
}
} // namespace

TEST(UndoWriteThroughTests, KeepsOnlyTheDeclaredFilesystemScopeCleanAcrossTimelineMovement)
{
    const UndoScopeId scene {UndoScopeKind::Scene, "scene:write-through", 0u};
    const UndoScopeId asset {UndoScopeKind::Asset, "asset:write-through", 0u};
    auto value = std::make_shared<int>(0);
    EditorUndoService undo;

    ASSERT_TRUE(undo.Execute(std::make_unique<IntegerCommand>(
        value,
        0,
        7,
        MakeInfo("Write Asset", {scene, asset}, {asset}))).Succeeded());
    EXPECT_EQ(*value, 7);
    EXPECT_TRUE(undo.GetScopeState(scene).IsDirty());
    EXPECT_FALSE(undo.GetScopeState(asset).IsDirty());

    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_EQ(*value, 0);
    EXPECT_FALSE(undo.GetScopeState(asset).IsDirty());

    ASSERT_EQ(undo.RequestRedo().state, UndoRequestState::Completed);
    EXPECT_EQ(*value, 7);
    EXPECT_TRUE(undo.GetScopeState(scene).IsDirty());
    EXPECT_FALSE(undo.GetScopeState(asset).IsDirty());
}

TEST(UndoWriteThroughTests, CrossScopeTransactionDoesNotMarkUnrelatedScopesSaved)
{
    const UndoScopeId scene {UndoScopeKind::Scene, "scene:transaction-write-through", 0u};
    const UndoScopeId asset {UndoScopeKind::Asset, "asset:transaction-write-through", 0u};
    auto sceneValue = std::make_shared<int>(0);
    auto assetValue = std::make_shared<int>(0);
    EditorUndoService undo;

    auto transaction = undo.BeginTransaction({"Scene And Asset", "UndoWriteThroughTests", {scene, asset}, {}});
    ASSERT_TRUE(transaction.result.Succeeded());
    ASSERT_TRUE(undo.Execute(std::make_unique<IntegerCommand>(
        sceneValue,
        0,
        1,
        MakeInfo("Move Scene", {scene}))).Succeeded());
    ASSERT_TRUE(undo.Execute(std::make_unique<IntegerCommand>(
        assetValue,
        0,
        2,
        MakeInfo("Write Asset", {asset}, {asset}))).Succeeded());
    ASSERT_EQ(undo.CommitTransaction(transaction.transaction).state, UndoRequestState::Completed);

    EXPECT_TRUE(undo.GetScopeState(scene).IsDirty());
    EXPECT_FALSE(undo.GetScopeState(asset).IsDirty());
    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_EQ(*sceneValue, 0);
    EXPECT_EQ(*assetValue, 0);
    EXPECT_FALSE(undo.GetScopeState(asset).IsDirty());
}
} // namespace NLS::Editor::Undo::Tests
