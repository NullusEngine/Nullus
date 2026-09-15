#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "Undo/EditorUndoService.h"
#include "Undo/EditorUndoMutationContext.h"

namespace NLS::Editor::Undo::Tests
{
namespace
{
struct ValueState
{
    int value = 0;
    bool alive = true;
    bool conflict = false;
};

struct AsyncControl
{
    UndoCompletion completion;
};

class ValueCommand final : public IEditorUndoCommand
{
public:
    ValueCommand(
        std::shared_ptr<ValueState> state,
        const int before,
        const int after,
        UndoCommandInfo info,
        std::shared_ptr<AsyncControl> async = {},
        const bool declaresAsync = true)
        : m_state(std::move(state))
        , m_before(before)
        , m_after(after)
        , m_info(std::move(info))
        , m_async(std::move(async))
        , m_declaresAsync(static_cast<bool>(m_async) && declaresAsync)
    {
    }

    [[nodiscard]] UndoCommandInfo GetInfo() const override
    {
        return m_info;
    }

    [[nodiscard]] UndoValidationResult Validate(
        const UndoDirection,
        const UndoExecutionContext&) const override
    {
        if (!m_state || !m_state->alive)
            return UndoValidationResult::Stale("Target is gone.");
        if (m_state->conflict)
            return UndoValidationResult::Conflict("Target changed outside the editor.");
        return UndoValidationResult::Ready();
    }

    UndoStartResult Apply(
        const UndoDirection direction,
        const UndoExecutionContext&,
        UndoCompletion completion) override
    {
        if (m_failNextApply)
        {
            m_failNextApply = false;
            return UndoStartResult::Failed("Forced command failure.", m_failRollback);
        }

        if (direction == UndoDirection::Redo && m_async)
        {
            m_async->completion = std::move(completion);
            return UndoStartResult::Pending();
        }

        m_state->value = direction == UndoDirection::Redo ? m_after : m_before;
        return UndoStartResult::Completed();
    }

    [[nodiscard]] bool MayCompleteAsynchronously() const override
    {
        return m_declaresAsync;
    }

    bool TryMergeFrom(const IEditorUndoCommand& newer) override
    {
        const auto* valueCommand = dynamic_cast<const ValueCommand*>(&newer);
        if (!valueCommand || valueCommand->m_state != m_state || m_async || valueCommand->m_async)
            return false;
        m_after = valueCommand->m_after;
        m_info = valueCommand->m_info;
        return true;
    }

    void FailNextApply(const bool rollbackFails = false)
    {
        m_failNextApply = true;
        m_failRollback = rollbackFails;
    }

private:
    std::shared_ptr<ValueState> m_state;
    int m_before = 0;
    int m_after = 0;
    UndoCommandInfo m_info;
    std::shared_ptr<AsyncControl> m_async;
    bool m_declaresAsync = false;
    bool m_failNextApply = false;
    bool m_failRollback = false;
};

class MetadataMergingValueCommand final : public IEditorUndoCommand
{
public:
    MetadataMergingValueCommand(
        std::shared_ptr<ValueState> state,
        const int before,
        const int after,
        UndoCommandInfo info,
        const size_t mergedMemoryBytes,
        const size_t mergedBlobBytes)
        : m_state(std::move(state))
        , m_before(before)
        , m_after(after)
        , m_info(std::move(info))
        , m_mergedMemoryBytes(mergedMemoryBytes)
        , m_mergedBlobBytes(mergedBlobBytes)
    {
    }

    [[nodiscard]] UndoCommandInfo GetInfo() const override
    {
        return m_info;
    }

    [[nodiscard]] UndoValidationResult Validate(
        const UndoDirection,
        const UndoExecutionContext&) const override
    {
        return m_state && m_state->alive
            ? UndoValidationResult::Ready()
            : UndoValidationResult::Stale("Target is gone.");
    }

    UndoStartResult Apply(
        const UndoDirection direction,
        const UndoExecutionContext&,
        UndoCompletion) override
    {
        if (!m_state)
            return UndoStartResult::Failed("Target is gone.");
        m_state->value = direction == UndoDirection::Redo ? m_after : m_before;
        return UndoStartResult::Completed();
    }

    bool TryMergeFrom(const IEditorUndoCommand& newer) override
    {
        const auto* valueCommand = dynamic_cast<const MetadataMergingValueCommand*>(&newer);
        if (valueCommand == nullptr || valueCommand->m_state != m_state || m_after != valueCommand->m_before)
            return false;

        m_after = valueCommand->m_after;
        // A real snapshot command can retain the original before-state while
        // replacing only its after-state. Its retained size is not necessarily
        // the size reported by the transient newer command.
        m_info.estimatedBytes = m_mergedMemoryBytes;
        m_info.estimatedBlobBytes = m_mergedBlobBytes;
        return true;
    }

private:
    std::shared_ptr<ValueState> m_state;
    int m_before = 0;
    int m_after = 0;
    UndoCommandInfo m_info;
    size_t m_mergedMemoryBytes = 0u;
    size_t m_mergedBlobBytes = 0u;
};

struct DeferredWriteControl
{
    std::function<void()> finish;
    UndoExecutionContext context;
    bool capturedContext = false;
};

class DeferredWriteCommand final : public IEditorUndoCommand
{
public:
    DeferredWriteCommand(
        std::shared_ptr<ValueState> state,
        const int before,
        const int after,
        UndoCommandInfo info,
        std::shared_ptr<DeferredWriteControl> control)
        : m_state(std::move(state))
        , m_before(before)
        , m_after(after)
        , m_info(std::move(info))
        , m_control(std::move(control))
    {
    }

    [[nodiscard]] UndoCommandInfo GetInfo() const override { return m_info; }

    [[nodiscard]] UndoValidationResult Validate(
        const UndoDirection,
        const UndoExecutionContext&) const override
    {
        return m_state && m_control ? UndoValidationResult::Ready() : UndoValidationResult::Stale("Deferred target is gone.");
    }

    UndoStartResult Apply(
        const UndoDirection direction,
        const UndoExecutionContext& context,
        UndoCompletion completion) override
    {
        if (direction == UndoDirection::Undo)
        {
            m_state->value = m_before;
            return UndoStartResult::Completed();
        }

        m_control->context = context;
        m_control->capturedContext = true;
        m_control->finish = [state = m_state, after = m_after, context, completion = std::move(completion)]() mutable
        {
            auto mutationLease = context.TryAcquireOperationMutation();
            const bool stillCurrent = static_cast<bool>(mutationLease);
            if (stillCurrent)
                state->value = after;
            mutationLease.Release();
            if (completion)
            {
                completion(stillCurrent
                    ? UndoCompletionResult::Succeeded()
                    : UndoCompletionResult::Failed("Deferred operation was invalidated."));
            }
        };
        return UndoStartResult::Pending();
    }

    [[nodiscard]] bool MayCompleteAsynchronously() const override { return true; }
    bool TryMergeFrom(const IEditorUndoCommand&) override { return false; }

private:
    std::shared_ptr<ValueState> m_state;
    int m_before = 0;
    int m_after = 0;
    UndoCommandInfo m_info;
    std::shared_ptr<DeferredWriteControl> m_control;
};

struct BlockingMutationLeaseControl
{
    std::mutex mutex;
    std::condition_variable condition;
    std::function<void()> finish;
    bool leaseAcquired = false;
    bool delayedWriteFinished = false;
};

class BlockingMutationLeaseCommand final : public IEditorUndoCommand
{
public:
    BlockingMutationLeaseCommand(
        std::shared_ptr<ValueState> state,
        const int before,
        const int after,
        UndoCommandInfo info,
        std::shared_ptr<BlockingMutationLeaseControl> control)
        : m_state(std::move(state))
        , m_before(before)
        , m_after(after)
        , m_info(std::move(info))
        , m_control(std::move(control))
    {
    }

    [[nodiscard]] UndoCommandInfo GetInfo() const override { return m_info; }

    [[nodiscard]] UndoValidationResult Validate(
        const UndoDirection,
        const UndoExecutionContext&) const override
    {
        return m_state && m_control
            ? UndoValidationResult::Ready()
            : UndoValidationResult::Stale("Delayed target is gone.");
    }

    UndoStartResult Apply(
        const UndoDirection direction,
        const UndoExecutionContext& context,
        UndoCompletion completion) override
    {
        if (direction == UndoDirection::Undo)
        {
            m_state->value = m_before;
            return UndoStartResult::Completed();
        }

        m_control->finish = [
            state = m_state,
            after = m_after,
            control = m_control,
            context,
            completion = std::move(completion)]() mutable
        {
            const ScopedEditorUndoMutationContext mutationContext(context);
            auto mutationLease = context.TryAcquireOperationMutation();
            if (!mutationLease)
            {
                if (completion)
                    completion(UndoCompletionResult::Failed("Delayed operation was invalidated."));
                return;
            }

            {
                std::lock_guard lock(control->mutex);
                control->leaseAcquired = true;
            }
            control->condition.notify_all();

            // ClearAll/InvalidateScope set the gate before waiting for this
            // lease. A pre-lease implementation would return from the caller
            // first and leave this delayed mutation running behind it.
            while (context.IsOperationCurrent())
                std::this_thread::yield();

            state->value = after;
            {
                std::lock_guard lock(control->mutex);
                control->delayedWriteFinished = true;
            }
            control->condition.notify_all();

            mutationLease.Release();
            if (completion)
                completion(UndoCompletionResult::Failed("Delayed operation was invalidated."));
        };
        return UndoStartResult::Pending();
    }

    [[nodiscard]] bool MayCompleteAsynchronously() const override { return true; }
    bool TryMergeFrom(const IEditorUndoCommand&) override { return false; }

private:
    std::shared_ptr<ValueState> m_state;
    int m_before = 0;
    int m_after = 0;
    UndoCommandInfo m_info;
    std::shared_ptr<BlockingMutationLeaseControl> m_control;
};

class ValueEdit final : public IEditorUndoEdit
{
public:
    ValueEdit(
        std::shared_ptr<ValueState> state,
        const int before,
        const int previewValue,
        UndoCommandInfo commandInfo)
        : m_state(std::move(state))
        , m_before(before)
        , m_previewValue(previewValue)
        , m_commandInfo(std::move(commandInfo))
    {
    }

    [[nodiscard]] UndoValidationResult Validate(const UndoExecutionContext&) const override
    {
        return !m_state || !m_state->alive
            ? UndoValidationResult::Stale("Target is gone.")
            : UndoValidationResult::Ready();
    }

    UndoStartResult Preview(const UndoExecutionContext&, UndoCompletion) override
    {
        m_state->value = m_previewValue;
        return UndoStartResult::Completed();
    }

    UndoStartResult Cancel(const UndoExecutionContext&, UndoCompletion) override
    {
        m_state->value = m_before;
        return UndoStartResult::Completed();
    }

    [[nodiscard]] bool HasChanges() const override
    {
        return m_state && m_state->value != m_before;
    }

    [[nodiscard]] UndoCommandPtr BuildCommand() override
    {
        if (!HasChanges())
            return {};
        return std::make_unique<ValueCommand>(m_state, m_before, m_state->value, m_commandInfo);
    }

private:
    std::shared_ptr<ValueState> m_state;
    int m_before = 0;
    int m_previewValue = 0;
    UndoCommandInfo m_commandInfo;
};

struct MutationContextProbe
{
    std::vector<UndoDirection> directions;
    std::vector<CommandId> commandIds;
};

#if !defined(NDEBUG)
TEST(EditorUndoCoreTests, TransientMutationContextIsExplicitAndNonHistorical)
{
    UndoScopeId scope;
    scope.kind = UndoScopeKind::Scene;
    scope.stableKey = "scene:transient-preview";
    scope.generation = 7u;

    EXPECT_FALSE(HasActiveEditorUndoMutationContext());
    {
        const ScopedEditorUndoTransientMutationContext mutationContext(scope, UndoDirection::Redo);
        ASSERT_TRUE(HasActiveEditorUndoMutationContext());
        const auto active = GetActiveEditorUndoMutationContext();
        ASSERT_TRUE(active.has_value());
        EXPECT_EQ(active->commandId, 0u);
        EXPECT_EQ(active->direction, UndoDirection::Redo);
        ASSERT_EQ(active->scopes.size(), 1u);
        EXPECT_EQ(active->scopes.front(), scope);
    }
    EXPECT_FALSE(HasActiveEditorUndoMutationContext());
}
#endif

void ObserveMutationContext(
    MutationContextProbe& probe,
    const UndoExecutionContext& context,
    const std::string_view operation)
{
#if !defined(NDEBUG)
    NLS_EDITOR_UNDO_REQUIRE_MUTATION_CONTEXT(operation.data());
    const auto active = GetActiveEditorUndoMutationContext();
    ASSERT_TRUE(active.has_value());
    ASSERT_EQ(active->commandId, context.commandId);
    ASSERT_EQ(active->direction, context.direction);
    probe.directions.push_back(active->direction);
    probe.commandIds.push_back(active->commandId);
#else
    static_cast<void>(probe);
    static_cast<void>(context);
    static_cast<void>(operation);
#endif
}

class MutationContextProbeCommand final : public IEditorUndoCommand
{
public:
    MutationContextProbeCommand(UndoCommandInfo info, std::shared_ptr<MutationContextProbe> probe)
        : m_info(std::move(info))
        , m_probe(std::move(probe))
    {
    }

    [[nodiscard]] UndoCommandInfo GetInfo() const override { return m_info; }

    [[nodiscard]] UndoValidationResult Validate(
        const UndoDirection,
        const UndoExecutionContext&) const override
    {
        return m_probe ? UndoValidationResult::Ready() : UndoValidationResult::Stale("Probe no longer exists.");
    }

    UndoStartResult Apply(
        const UndoDirection,
        const UndoExecutionContext& context,
        UndoCompletion) override
    {
        ObserveMutationContext(*m_probe, context, "applying a probe command");
        return UndoStartResult::Completed();
    }

    bool TryMergeFrom(const IEditorUndoCommand&) override { return false; }

private:
    UndoCommandInfo m_info;
    std::shared_ptr<MutationContextProbe> m_probe;
};

class MutationContextProbeEdit final : public IEditorUndoEdit
{
public:
    explicit MutationContextProbeEdit(std::shared_ptr<MutationContextProbe> probe)
        : m_probe(std::move(probe))
    {
    }

    [[nodiscard]] UndoValidationResult Validate(const UndoExecutionContext&) const override
    {
        return m_probe ? UndoValidationResult::Ready() : UndoValidationResult::Stale("Probe no longer exists.");
    }

    UndoStartResult Preview(const UndoExecutionContext& context, UndoCompletion) override
    {
        ObserveMutationContext(*m_probe, context, "previewing a probe edit");
        m_changed = true;
        return UndoStartResult::Completed();
    }

    UndoStartResult Cancel(const UndoExecutionContext& context, UndoCompletion) override
    {
        ObserveMutationContext(*m_probe, context, "cancelling a probe edit");
        m_changed = false;
        return UndoStartResult::Completed();
    }

    [[nodiscard]] bool HasChanges() const override { return m_changed; }
    [[nodiscard]] UndoCommandPtr BuildCommand() override { return {}; }

private:
    std::shared_ptr<MutationContextProbe> m_probe;
    bool m_changed = false;
};

UndoScopeId Scope(const UndoScopeKind kind, std::string key, const uint64_t generation = 1u)
{
    return {kind, std::move(key), generation};
}

UndoCommandInfo Info(
    std::string label,
    std::vector<UndoScopeId> scopes,
    std::string mergeKey = {},
    const size_t bytes = 0u,
    const size_t blobBytes = 0u)
{
    UndoCommandInfo info;
    info.label = std::move(label);
    info.sourceModule = "EditorUndoCoreTests";
    info.scopes = std::move(scopes);
    info.affected = {"stable-target/property"};
    info.mergeKey = std::move(mergeKey);
    info.allowMerge = !info.mergeKey.empty();
    info.estimatedBytes = bytes;
    info.estimatedBlobBytes = blobBytes;
    return info;
}

UndoCommandPtr Command(
    const std::shared_ptr<ValueState>& state,
    const int before,
    const int after,
    UndoCommandInfo info,
    std::shared_ptr<AsyncControl> async = {},
    const bool declaresAsync = true)
{
    return std::make_unique<ValueCommand>(
        state,
        before,
        after,
        std::move(info),
        std::move(async),
        declaresAsync);
}
} // namespace

TEST(EditorUndoCoreTests, GlobalTimelineKeepsCrossWindowCommandsChronological)
{
    EditorUndoService undo;
    const auto scene = Scope(UndoScopeKind::Scene, "scene:main");
    const auto document = Scope(UndoScopeKind::UIDocument, "ui:main");
    const auto sceneValue = std::make_shared<ValueState>();
    const auto documentValue = std::make_shared<ValueState>();
    documentValue->value = 10;

    ASSERT_TRUE(undo.Execute(Command(sceneValue, 0, 1, Info("Move Object", {scene}))).Succeeded());
    ASSERT_TRUE(undo.Execute(Command(documentValue, 10, 20, Info("Resize UI", {document}))).Succeeded());
    EXPECT_EQ(sceneValue->value, 1);
    EXPECT_EQ(documentValue->value, 20);
    EXPECT_EQ(undo.GetUndoLabel(), "Resize UI");

    EXPECT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_EQ(documentValue->value, 10);
    EXPECT_EQ(sceneValue->value, 1);
    EXPECT_EQ(undo.GetUndoLabel(), "Move Object");

    EXPECT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_EQ(sceneValue->value, 0);
    EXPECT_EQ(undo.RequestRedo().state, UndoRequestState::Completed);
    EXPECT_EQ(sceneValue->value, 1);
    EXPECT_EQ(undo.RequestRedo().state, UndoRequestState::Completed);
    EXPECT_EQ(documentValue->value, 20);
}

TEST(EditorUndoCoreTests, NestedTransactionsCommitAndUndoAsOneAtomicTimelineEntry)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Scene, "scene:transaction");
    const auto first = std::make_shared<ValueState>();
    const auto second = std::make_shared<ValueState>();
    const auto third = std::make_shared<ValueState>();

    auto outer = undo.BeginTransaction({"Create Composite", "Hierarchy", {scope}, {"root"}});
    ASSERT_TRUE(outer.result.Succeeded());
    ASSERT_TRUE(undo.Execute(Command(first, 0, 1, Info("Create Root", {scope}))).Succeeded());

    auto inner = undo.BeginTransaction({"Configure Composite", "Inspector", {scope}, {"root/component"}});
    ASSERT_TRUE(inner.result.Succeeded());
    ASSERT_TRUE(undo.Execute(Command(second, 0, 2, Info("Add Component", {scope}))).Succeeded());
    ASSERT_EQ(undo.CommitTransaction(inner.transaction).state, UndoRequestState::Completed);
    ASSERT_TRUE(undo.Execute(Command(third, 0, 3, Info("Move Root", {scope}))).Succeeded());
    ASSERT_EQ(undo.CommitTransaction(outer.transaction).state, UndoRequestState::Completed);

    ASSERT_EQ(undo.GetTransactionCount(), 1u);
    EXPECT_EQ(undo.GetUndoLabel(), "Create Composite");
    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_EQ(first->value, 0);
    EXPECT_EQ(second->value, 0);
    EXPECT_EQ(third->value, 0);
    ASSERT_EQ(undo.RequestRedo().state, UndoRequestState::Completed);
    EXPECT_EQ(first->value, 1);
    EXPECT_EQ(second->value, 2);
    EXPECT_EQ(third->value, 3);
}

TEST(EditorUndoCoreTests, RecordAppliedMergesContinuousEditsAndTruncatesRedo)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Scene, "scene:merge");
    const auto value = std::make_shared<ValueState>();

    value->value = 4;
    ASSERT_TRUE(undo.RecordApplied(Command(value, 0, 4, Info("Change Value", {scope}, "property:value"))).Succeeded());
    value->value = 8;
    ASSERT_TRUE(undo.RecordApplied(Command(value, 0, 8, Info("Change Value", {scope}, "property:value"))).Succeeded());
    ASSERT_EQ(undo.GetTransactionCount(), 1u);

    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_EQ(value->value, 0);
    ASSERT_EQ(undo.RequestRedo().state, UndoRequestState::Completed);
    EXPECT_EQ(value->value, 8);

    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    value->value = 12;
    ASSERT_TRUE(undo.RecordApplied(Command(value, 0, 12, Info("Replace Value", {scope}))).Succeeded());
    EXPECT_FALSE(undo.CanRedo());
    ASSERT_EQ(undo.GetTransactionCount(), 1u);
    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_EQ(value->value, 0);
}

TEST(EditorUndoCoreTests, MergeUsesTheRetainedCommandsBudgetMetadata)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::UIDocument, "ui:merge-budget");
    const auto value = std::make_shared<ValueState>();

    constexpr size_t retainedMemoryBytes = 37u;
    constexpr size_t retainedBlobBytes = 71u;
    ASSERT_TRUE(undo.Execute(std::make_unique<MetadataMergingValueCommand>(
        value,
        0,
        1,
        Info("Resize UI", {scope}, "ui:node:size", 101u, 103u),
        retainedMemoryBytes,
        retainedBlobBytes)).Succeeded());
    ASSERT_TRUE(undo.Execute(std::make_unique<MetadataMergingValueCommand>(
        value,
        1,
        2,
        Info("Resize UI", {scope}, "ui:node:size", 107u, 109u),
        retainedMemoryBytes,
        retainedBlobBytes)).Succeeded());

    const auto history = undo.GetHistoryState();
    EXPECT_EQ(history.transactionCount, 1u);
    EXPECT_EQ(history.retainedMemoryBytes, retainedMemoryBytes);
    EXPECT_EQ(history.retainedBlobBytes, retainedBlobBytes);
    EXPECT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_EQ(value->value, 0);
    EXPECT_EQ(undo.RequestRedo().state, UndoRequestState::Completed);
    EXPECT_EQ(value->value, 2);
}

TEST(EditorUndoCoreTests, ActiveTransactionsBlockGlobalUndoAndRedoUntilTheyClose)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Scene, "scene:transaction-gate");
    const auto value = std::make_shared<ValueState>();

    ASSERT_TRUE(undo.Execute(Command(value, 0, 1, Info("Baseline", {scope}))).Succeeded());
    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    ASSERT_TRUE(undo.CanRedo());

    auto transaction = undo.BeginTransaction({"Open Transaction", "EditorUndoCoreTests", {scope}, {}});
    ASSERT_TRUE(transaction.result.Succeeded());
    EXPECT_FALSE(undo.CanUndo());
    EXPECT_FALSE(undo.CanRedo());
    EXPECT_EQ(undo.RequestUndo().state, UndoRequestState::Busy);
    EXPECT_EQ(undo.RequestRedo().state, UndoRequestState::Busy);
    EXPECT_EQ(undo.GetCursor(), 0u);

    ASSERT_EQ(undo.CancelTransaction(transaction.transaction).state, UndoRequestState::Completed);
    EXPECT_TRUE(undo.CanRedo());
}

TEST(EditorUndoCoreTests, FrozenTransactionsCannotBeCommitted)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Scene, "scene:frozen-transaction");
    const auto value = std::make_shared<ValueState>();

    auto transaction = undo.BeginTransaction({"Frozen Commit", "EditorUndoCoreTests", {scope}, {}});
    ASSERT_TRUE(transaction.result.Succeeded());
    ASSERT_TRUE(undo.Execute(Command(value, 0, 1, Info("Child", {scope}))).Succeeded());

    undo.Freeze(UndoFreezeReason::PlayMode);
    EXPECT_EQ(undo.CommitTransaction(transaction.transaction).state, UndoRequestState::Frozen);
    EXPECT_TRUE(transaction.transaction.IsValid());
    EXPECT_EQ(undo.GetTransactionCount(), 0u);

    undo.Unfreeze(UndoFreezeReason::PlayMode);
    ASSERT_EQ(undo.CommitTransaction(transaction.transaction).state, UndoRequestState::Completed);
    EXPECT_EQ(undo.GetTransactionCount(), 1u);
}

TEST(EditorUndoCoreTests, ActiveEditDisablesRedoWithoutSuppressingItsCancelUndo)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Scene, "scene:active-edit-redo");
    const auto value = std::make_shared<ValueState>();

    ASSERT_TRUE(undo.Execute(Command(value, 0, 1, Info("Baseline", {scope}))).Succeeded());
    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    ASSERT_TRUE(undo.CanRedo());

    auto edit = undo.BeginEditSession({
        Info("Preview", {scope}, "property:value"),
        std::make_unique<ValueEdit>(value, 0, 3, Info("Preview", {scope}, "property:value"))});
    ASSERT_TRUE(edit.result.Succeeded());
    EXPECT_FALSE(undo.CanRedo());
    EXPECT_EQ(undo.RequestRedo().state, UndoRequestState::Busy);

    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_TRUE(undo.CanRedo());
}

TEST(EditorUndoCoreTests, MarkSavedIgnoresUnstableMutationContexts)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Scene, "scene:mark-saved-gate");
    const auto value = std::make_shared<ValueState>();

    ASSERT_TRUE(undo.Execute(Command(value, 0, 1, Info("Baseline", {scope}))).Succeeded());
    undo.MarkSaved(scope, "durable-save");
    const auto saved = undo.GetScopeState(scope);

    auto edit = undo.BeginEditSession({
        Info("Preview", {scope}, "property:value"),
        std::make_unique<ValueEdit>(value, 1, 2, Info("Preview", {scope}, "property:value"))});
    ASSERT_TRUE(edit.result.Succeeded());
    undo.MarkSaved(scope, "edit-save");
    EXPECT_EQ(undo.GetScopeState(scope).savedRevision, saved.savedRevision);
    EXPECT_EQ(undo.GetScopeState(scope).savedFingerprint, "durable-save");
    ASSERT_EQ(undo.CancelEditSession(edit.session).state, UndoRequestState::Completed);

    auto transaction = undo.BeginTransaction({"Transaction", "EditorUndoCoreTests", {scope}, {}});
    ASSERT_TRUE(transaction.result.Succeeded());
    undo.MarkSaved(scope, "transaction-save");
    EXPECT_EQ(undo.GetScopeState(scope).savedRevision, saved.savedRevision);
    EXPECT_EQ(undo.GetScopeState(scope).savedFingerprint, "durable-save");
    ASSERT_EQ(undo.CancelTransaction(transaction.transaction).state, UndoRequestState::Completed);

    const auto async = std::make_shared<AsyncControl>();
    const auto pending = undo.Execute(Command(value, 1, 2, Info("Async", {scope}), async));
    ASSERT_EQ(pending.state, UndoRequestState::Pending);
    undo.MarkSaved(scope, "pending-save");
    EXPECT_EQ(undo.GetScopeState(scope).savedRevision, saved.savedRevision);
    EXPECT_EQ(undo.GetScopeState(scope).savedFingerprint, "durable-save");
    value->value = 2;
    ASSERT_EQ(undo.CompletePending(pending.pendingToken, UndoCompletionResult::Succeeded()).state, UndoRequestState::Completed);
}

TEST(EditorUndoCoreTests, EditSessionCancelsPreviewOnFirstUndoAndSkipsNoOpCommits)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Scene, "scene:edit-session");
    const auto value = std::make_shared<ValueState>();

    ASSERT_TRUE(undo.Execute(Command(value, 0, 1, Info("Baseline", {scope}))).Succeeded());
    auto edit = undo.BeginEditSession({
        Info("Preview Value", {scope}, "property:value"),
        std::make_unique<ValueEdit>(value, 1, 5, Info("Preview Value", {scope}, "property:value"))});
    ASSERT_TRUE(edit.result.Succeeded());
    ASSERT_EQ(undo.PreviewEditSession(edit.session).state, UndoRequestState::Completed);
    EXPECT_EQ(value->value, 5);
    EXPECT_TRUE(undo.HasActiveEditSession());

    // The first Undo is reserved for cancelling the in-progress drag/preview.
    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_EQ(value->value, 1);
    EXPECT_FALSE(undo.HasActiveEditSession());
    EXPECT_EQ(undo.GetTransactionCount(), 1u);
    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_EQ(value->value, 0);

    auto noOp = undo.BeginEditSession({
        Info("No-op Preview", {scope}, "property:value"),
        std::make_unique<ValueEdit>(value, 0, 0, Info("No-op Preview", {scope}, "property:value"))});
    ASSERT_TRUE(noOp.result.Succeeded());
    ASSERT_EQ(undo.PreviewEditSession(noOp.session).state, UndoRequestState::Completed);
    ASSERT_EQ(undo.CommitEditSession(noOp.session).state, UndoRequestState::Completed);
    EXPECT_EQ(undo.GetTransactionCount(), 1u);
    EXPECT_FALSE(noOp.session.IsValid());

    auto committed = undo.BeginEditSession({
        Info("Committed Preview", {scope}, "property:value"),
        std::make_unique<ValueEdit>(value, 0, 9, Info("Committed Preview", {scope}, "property:value"))});
    ASSERT_TRUE(committed.result.Succeeded());
    ASSERT_EQ(undo.PreviewEditSession(committed.session).state, UndoRequestState::Completed);
    ASSERT_EQ(undo.CommitEditSession(committed.session).state, UndoRequestState::Completed);
    EXPECT_EQ(value->value, 9);
    // The baseline is currently in Redo after the second Undo, so committing
    // a new preview follows normal timeline semantics and truncates it.
    EXPECT_EQ(undo.GetTransactionCount(), 1u);
    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_EQ(value->value, 0);
}

TEST(EditorUndoCoreTests, StaleTopEntryIsRemovedWithoutSkippingOlderUndo)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Scene, "scene:stale");
    const auto older = std::make_shared<ValueState>();
    const auto newer = std::make_shared<ValueState>();

    ASSERT_TRUE(undo.Execute(Command(older, 0, 1, Info("Older", {scope}))).Succeeded());
    ASSERT_TRUE(undo.Execute(Command(newer, 0, 2, Info("Newer", {scope}))).Succeeded());
    newer->alive = false;

    const auto stale = undo.RequestUndo();
    EXPECT_EQ(stale.state, UndoRequestState::Stale);
    EXPECT_EQ(older->value, 1);
    EXPECT_EQ(newer->value, 2);
    EXPECT_EQ(undo.GetTransactionCount(), 1u);
    EXPECT_EQ(undo.GetCursor(), 1u);

    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_EQ(older->value, 0);
}

TEST(EditorUndoCoreTests, BudgetsEvictOldestCompleteTransactionsButKeepOneOversizedEntry)
{
    UndoBudget budget;
    budget.maxTransactions = 2u;
    budget.maxMemoryBytes = 100u;
    budget.maxBlobBytes = 100u;
    EditorUndoService undo(budget);
    const auto scope = Scope(UndoScopeKind::Asset, "asset:budget");
    const auto first = std::make_shared<ValueState>();
    const auto second = std::make_shared<ValueState>();
    const auto third = std::make_shared<ValueState>();

    ASSERT_TRUE(undo.Execute(Command(first, 0, 1, Info("First", {scope}, {}, 40u, 40u))).Succeeded());
    ASSERT_TRUE(undo.Execute(Command(second, 0, 2, Info("Second", {scope}, {}, 40u, 40u))).Succeeded());
    ASSERT_TRUE(undo.Execute(Command(third, 0, 3, Info("Third", {scope}, {}, 40u, 40u))).Succeeded());
    EXPECT_EQ(undo.GetTransactionCount(), 2u);
    EXPECT_EQ(undo.GetHistoryState().retainedMemoryBytes, 80u);
    EXPECT_EQ(undo.GetHistoryState().retainedBlobBytes, 80u);

    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_EQ(undo.RequestUndo().state, UndoRequestState::Empty);
    EXPECT_EQ(first->value, 1);
    EXPECT_EQ(second->value, 0);
    EXPECT_EQ(third->value, 0);

    const auto oversized = std::make_shared<ValueState>();
    ASSERT_TRUE(undo.Execute(Command(oversized, 0, 9, Info("Oversized", {scope}, {}, 500u, 500u))).Succeeded());
    EXPECT_EQ(undo.GetTransactionCount(), 1u);
    EXPECT_EQ(undo.GetHistoryState().retainedMemoryBytes, 500u);
    EXPECT_EQ(undo.GetHistoryState().retainedBlobBytes, 500u);
}

TEST(EditorUndoCoreTests, PendingCommandsLockHistoryUntilMainThreadCompletion)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Asset, "asset:pending");
    const auto value = std::make_shared<ValueState>();
    const auto async = std::make_shared<AsyncControl>();

    const auto started = undo.Execute(Command(value, 0, 7, Info("Reimport", {scope}), async));
    ASSERT_EQ(started.state, UndoRequestState::Pending);
    ASSERT_NE(started.pendingToken, 0u);
    EXPECT_TRUE(undo.IsPending());
    EXPECT_EQ(undo.RequestUndo().state, UndoRequestState::Busy);

    value->value = 7;
    ASSERT_EQ(
        undo.CompletePending(started.pendingToken, UndoCompletionResult::Succeeded()).state,
        UndoRequestState::Completed);
    EXPECT_FALSE(undo.IsPending());
    EXPECT_EQ(undo.GetTransactionCount(), 1u);
    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_EQ(value->value, 0);
}

TEST(EditorUndoCoreTests, WorkerCompletionIsAppliedOnlyWhenTheEditorThreadDrainsIt)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Asset, "asset:worker-completion");
    const auto value = std::make_shared<ValueState>();
    const auto async = std::make_shared<AsyncControl>();

    const auto started = undo.Execute(Command(value, 0, 4, Info("Async Import", {scope}), async));
    ASSERT_EQ(started.state, UndoRequestState::Pending);
    ASSERT_TRUE(static_cast<bool>(async->completion));

    auto completion = async->completion;
    std::thread worker([value, completion = std::move(completion)]() mutable
    {
        value->value = 4;
        completion(UndoCompletionResult::Succeeded());
    });
    worker.join();

    EXPECT_TRUE(undo.IsPending());
    EXPECT_EQ(undo.GetTransactionCount(), 0u);
    EXPECT_EQ(value->value, 4);
    EXPECT_EQ(undo.DrainPendingCompletions(), 1u);
    EXPECT_FALSE(undo.IsPending());
    EXPECT_EQ(undo.GetTransactionCount(), 1u);
    EXPECT_EQ(value->value, 4);
}

TEST(EditorUndoCoreTests, DirectWorkerCompletePendingQueuesUntilTheOwnerDrainsIt)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Asset, "asset:direct-worker-completion");
    const auto value = std::make_shared<ValueState>();
    const auto async = std::make_shared<AsyncControl>();

    const auto pending = undo.Execute(Command(value, 0, 4, Info("Async Import", {scope}), async));
    ASSERT_EQ(pending.state, UndoRequestState::Pending);

    UndoRequestResult workerResult;
    std::thread worker([&undo, &workerResult, value, token = pending.pendingToken]()
    {
        value->value = 4;
        workerResult = undo.CompletePending(token, UndoCompletionResult::Succeeded());
    });
    worker.join();

    EXPECT_EQ(workerResult.state, UndoRequestState::Pending);
    EXPECT_TRUE(undo.IsPending());
    EXPECT_EQ(undo.GetTransactionCount(), 0u);
    EXPECT_EQ(undo.DrainPendingCompletions(), 1u);
    EXPECT_FALSE(undo.IsPending());
    EXPECT_EQ(undo.GetTransactionCount(), 1u);
}

TEST(EditorUndoCoreTests, ClearAllReleasesAPendingOperationAndDropsLateCompletion)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Asset, "asset:clear-pending");
    const auto value = std::make_shared<ValueState>();
    const auto async = std::make_shared<AsyncControl>();

    const auto started = undo.Execute(Command(value, 0, 4, Info("Async Import", {scope}), async));
    ASSERT_EQ(started.state, UndoRequestState::Pending);
    ASSERT_TRUE(static_cast<bool>(async->completion));

    undo.ClearAll(UndoClearReason::SceneLoaded);
    EXPECT_FALSE(undo.IsPending());
    async->completion(UndoCompletionResult::Succeeded());
    EXPECT_FALSE(undo.IsPending());
    EXPECT_EQ(undo.GetTransactionCount(), 0u);
}

TEST(EditorUndoCoreTests, TransactionCancelReportsPoisonedWhenItsRollbackCannotBeRestored)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Scene, "scene:cancel-transaction-poison");
    const auto first = std::make_shared<ValueState>();
    const auto second = std::make_shared<ValueState>();

    auto transaction = undo.BeginTransaction({"Composite Edit", "EditorUndoCoreTests", {scope}, {}});
    ASSERT_TRUE(transaction.result.Succeeded());

    auto failing = std::make_unique<ValueCommand>(first, 0, 1, Info("First", {scope}));
    auto* failingCommand = failing.get();
    ASSERT_TRUE(undo.Execute(std::move(failing)).Succeeded());
    ASSERT_TRUE(undo.Execute(Command(second, 0, 2, Info("Second", {scope}))).Succeeded());
    ASSERT_EQ(first->value, 1);
    ASSERT_EQ(second->value, 2);

    // Cancel applies children in reverse. The second child is restored after
    // the first child reports an unrecoverable failure, so the transaction is
    // left intact but the global history must become a poisoned barrier.
    failingCommand->FailNextApply(true);
    const auto result = undo.CancelTransaction(transaction.transaction);
    EXPECT_EQ(result.state, UndoRequestState::Poisoned);
    EXPECT_TRUE(undo.IsPoisoned());
    EXPECT_TRUE(transaction.transaction.IsValid());
    EXPECT_EQ(first->value, 1);
    EXPECT_EQ(second->value, 2);
    EXPECT_EQ(undo.Execute(Command(first, 1, 3, Info("Blocked", {scope}))).state, UndoRequestState::Poisoned);
}

TEST(EditorUndoCoreTests, SavedScopesTrackDirtyStateAndInvalidationPrunesCrossScopeDependencies)
{
    EditorUndoService undo;
    const auto scene = Scope(UndoScopeKind::Scene, "scene:dependency");
    const auto asset = Scope(UndoScopeKind::Asset, "asset:dependency");
    const auto material = Scope(UndoScopeKind::Asset, "material:dependency");
    const auto sceneValue = std::make_shared<ValueState>();
    const auto assetValue = std::make_shared<ValueState>();

    ASSERT_TRUE(undo.Execute(Command(sceneValue, 0, 1, Info("Scene + Asset", {scene, asset}))).Succeeded());
    undo.MarkSaved(scene, "scene-hash");
    EXPECT_FALSE(undo.GetScopeState(scene).IsDirty());
    ASSERT_TRUE(undo.Execute(Command(assetValue, 0, 2, Info("Asset + Material", {asset, material}))).Succeeded());
    EXPECT_FALSE(undo.GetScopeState(scene).IsDirty());

    undo.InvalidateScope(scene, UndoInvalidationReason::AssetReimported);
    EXPECT_EQ(undo.GetTransactionCount(), 0u);
    EXPECT_FALSE(undo.CanUndo());
    EXPECT_FALSE(undo.GetScopeState(scene).IsDirty());
    EXPECT_FALSE(undo.GetScopeState(asset).IsDirty());
    EXPECT_FALSE(undo.GetScopeState(material).IsDirty());
}

TEST(EditorUndoCoreTests, ClosingPrefabStageGenerationPrunesOnlyTheClosedStageHistory)
{
    EditorUndoService undo;
    const auto closedStage = Scope(UndoScopeKind::PrefabStage, "Assets/Prefabs/Shared.prefab", 1u);
    const auto reopenedStage = Scope(UndoScopeKind::PrefabStage, "Assets/Prefabs/Shared.prefab", 2u);
    const auto closedValue = std::make_shared<ValueState>();
    const auto reopenedValue = std::make_shared<ValueState>();

    ASSERT_TRUE(undo.Execute(Command(closedValue, 0, 1, Info("Closed Stage Edit", {closedStage}))).Succeeded());
    ASSERT_TRUE(undo.Execute(Command(reopenedValue, 0, 2, Info("Reopened Stage Edit", {reopenedStage}))).Succeeded());

    undo.InvalidateScope(closedStage, UndoInvalidationReason::ScopeClosed);

    EXPECT_EQ(undo.GetTransactionCount(), 1u);
    EXPECT_TRUE(undo.CanUndo());
    EXPECT_FALSE(undo.GetScopeState(closedStage).IsDirty());
    EXPECT_TRUE(undo.GetScopeState(reopenedStage).IsDirty());
    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_EQ(reopenedValue->value, 0);
    EXPECT_EQ(closedValue->value, 1);
    ASSERT_EQ(undo.RequestRedo().state, UndoRequestState::Completed);
    EXPECT_EQ(reopenedValue->value, 2);
}

TEST(EditorUndoCoreTests, StaleAppliedHistoryRebaselinesScopesAsDirty)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Scene, "scene:stale-savepoint");
    const auto value = std::make_shared<ValueState>();

    ASSERT_TRUE(undo.Execute(Command(value, 0, 1, Info("Saved Value", {scope}))).Succeeded());
    undo.MarkSaved(scope, "saved-hash");
    ASSERT_FALSE(undo.GetScopeState(scope).IsDirty());

    value->alive = false;
    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Stale);
    const auto state = undo.GetScopeState(scope);
    EXPECT_TRUE(state.IsDirty());
    EXPECT_NE(state.currentRevision, state.savedRevision);
    EXPECT_TRUE(state.savedFingerprint.empty());
}

TEST(EditorUndoCoreTests, InvalidatingScopeExpandsThroughAnActiveEdit)
{
    EditorUndoService undo;
    const auto scene = Scope(UndoScopeKind::Scene, "scene:active-edit-invalidation");
    const auto asset = Scope(UndoScopeKind::Asset, "asset:active-edit-invalidation");
    const auto sceneValue = std::make_shared<ValueState>();
    const auto assetValue = std::make_shared<ValueState>();

    ASSERT_TRUE(undo.Execute(Command(assetValue, 0, 1, Info("Asset Change", {asset}))).Succeeded());
    auto edit = undo.BeginEditSession({
        Info("Cross Scope Preview", {scene, asset}, "property:value"),
        std::make_unique<ValueEdit>(sceneValue, 0, 5, Info("Cross Scope Preview", {scene, asset}, "property:value"))});
    ASSERT_TRUE(edit.result.Succeeded());
    ASSERT_EQ(undo.PreviewEditSession(edit.session).state, UndoRequestState::Completed);

    undo.InvalidateScope(scene, UndoInvalidationReason::Reloaded);
    EXPECT_FALSE(undo.HasActiveEditSession());
    EXPECT_EQ(undo.GetTransactionCount(), 0u);
    EXPECT_EQ(undo.CommitEditSession(edit.session).state, UndoRequestState::InvalidTransaction);
    EXPECT_EQ(sceneValue->value, 5);
    EXPECT_FALSE(undo.GetScopeState(scene).IsDirty());
    EXPECT_FALSE(undo.GetScopeState(asset).IsDirty());
}

TEST(EditorUndoCoreTests, AsynchronousCommandsAreRejectedBeforeEnteringTransactions)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Asset, "asset:async-transaction");
    const auto value = std::make_shared<ValueState>();
    const auto async = std::make_shared<AsyncControl>();

    auto transaction = undo.BeginTransaction({"Composite Import", "EditorUndoCoreTests", {scope}, {}});
    ASSERT_TRUE(transaction.result.Succeeded());
    const auto result = undo.Execute(Command(value, 0, 1, Info("Importer", {scope}), async));
    EXPECT_EQ(result.state, UndoRequestState::Failed);
    EXPECT_FALSE(undo.IsPending());
    EXPECT_FALSE(static_cast<bool>(async->completion));
    EXPECT_EQ(value->value, 0);
    ASSERT_EQ(undo.CancelTransaction(transaction.transaction).state, UndoRequestState::Completed);
}

TEST(EditorUndoCoreTests, UndeclaredPendingTransactionChildrenBecomePoisonedInsteadOfHistoryEntries)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Asset, "asset:undeclared-async-transaction");
    const auto value = std::make_shared<ValueState>();
    const auto async = std::make_shared<AsyncControl>();

    auto transaction = undo.BeginTransaction({"Composite Import", "EditorUndoCoreTests", {scope}, {}});
    ASSERT_TRUE(transaction.result.Succeeded());
    const auto pending = undo.Execute(Command(value, 0, 1, Info("Bad Importer", {scope}), async, false));
    ASSERT_EQ(pending.state, UndoRequestState::Pending);
    value->value = 1;

    EXPECT_EQ(
        undo.CompletePending(pending.pendingToken, UndoCompletionResult::Succeeded()).state,
        UndoRequestState::Poisoned);
    EXPECT_TRUE(undo.IsPoisoned());
    EXPECT_EQ(undo.GetTransactionCount(), 0u);
    EXPECT_TRUE(undo.GetScopeState(scope).IsDirty());
}

TEST(EditorUndoCoreTests, FailedTransactionChildRollsBackEarlierChildrenBeforeDiscarding)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Scene, "scene:failed-transaction-child");
    const auto firstValue = std::make_shared<ValueState>();
    const auto secondValue = std::make_shared<ValueState>();

    auto transaction = undo.BeginTransaction({"Atomic Failure", "EditorUndoCoreTests", {scope}, {}});
    ASSERT_TRUE(transaction.result.Succeeded());
    ASSERT_TRUE(undo.Execute(Command(firstValue, 0, 1, Info("First Child", {scope}))).Succeeded());
    EXPECT_EQ(firstValue->value, 1);

    auto failing = std::make_unique<ValueCommand>(
        secondValue,
        0,
        2,
        Info("Failing Child", {scope}));
    failing->FailNextApply();
    const auto result = undo.Execute(std::move(failing));

    EXPECT_EQ(result.state, UndoRequestState::Failed);
    EXPECT_EQ(firstValue->value, 0);
    EXPECT_EQ(secondValue->value, 0);
    EXPECT_EQ(undo.GetTransactionCount(), 0u);
    EXPECT_FALSE(undo.IsPoisoned());
    EXPECT_FALSE(undo.IsPending());
}

TEST(EditorUndoCoreTests, InvalidTransactionChildValidationRollsBackEarlierChildren)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Scene, "scene:invalid-transaction-child");
    const auto firstValue = std::make_shared<ValueState>();
    const auto invalidValue = std::make_shared<ValueState>();
    invalidValue->conflict = true;

    auto transaction = undo.BeginTransaction({"Validation Failure", "EditorUndoCoreTests", {scope}, {}});
    ASSERT_TRUE(transaction.result.Succeeded());
    ASSERT_TRUE(undo.Execute(Command(firstValue, 0, 1, Info("First Child", {scope}))).Succeeded());

    const auto result = undo.Execute(Command(invalidValue, 0, 2, Info("Invalid Child", {scope})));

    EXPECT_EQ(result.state, UndoRequestState::Conflict);
    EXPECT_EQ(firstValue->value, 0);
    EXPECT_EQ(undo.GetTransactionCount(), 0u);
    EXPECT_FALSE(undo.IsPoisoned());
}

TEST(EditorUndoCoreTests, UnexpectedPendingTransactionChildRollsBackItselfAndEarlierChildren)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Asset, "asset:unexpected-pending-rollback");
    const auto firstValue = std::make_shared<ValueState>();
    const auto pendingValue = std::make_shared<ValueState>();
    const auto async = std::make_shared<AsyncControl>();

    auto transaction = undo.BeginTransaction({"Atomic Pending Failure", "EditorUndoCoreTests", {scope}, {}});
    ASSERT_TRUE(transaction.result.Succeeded());
    ASSERT_TRUE(undo.Execute(Command(firstValue, 0, 1, Info("First Child", {scope}))).Succeeded());

    const auto pending = undo.Execute(Command(
        pendingValue,
        0,
        2,
        Info("Unexpected Pending Child", {scope}),
        async,
        false));
    ASSERT_EQ(pending.state, UndoRequestState::Pending);
    pendingValue->value = 2;

    EXPECT_EQ(
        undo.CompletePending(pending.pendingToken, UndoCompletionResult::Succeeded()).state,
        UndoRequestState::Poisoned);
    EXPECT_EQ(firstValue->value, 0);
    EXPECT_EQ(pendingValue->value, 0);
    EXPECT_EQ(undo.GetTransactionCount(), 0u);
    EXPECT_TRUE(undo.IsPoisoned());
    EXPECT_TRUE(undo.GetScopeState(scope).IsDirty());
}

TEST(EditorUndoCoreTests, FailedUnexpectedPendingCompletionRollsBackItselfAndEarlierChildren)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Asset, "asset:unexpected-pending-failure");
    const auto firstValue = std::make_shared<ValueState>();
    const auto pendingValue = std::make_shared<ValueState>();
    const auto async = std::make_shared<AsyncControl>();

    auto transaction = undo.BeginTransaction({"Pending Failure", "EditorUndoCoreTests", {scope}, {}});
    ASSERT_TRUE(transaction.result.Succeeded());
    ASSERT_TRUE(undo.Execute(Command(firstValue, 0, 1, Info("First Child", {scope}))).Succeeded());

    const auto pending = undo.Execute(Command(
        pendingValue,
        0,
        2,
        Info("Unexpected Pending Child", {scope}),
        async,
        false));
    ASSERT_EQ(pending.state, UndoRequestState::Pending);
    pendingValue->value = 2;

    EXPECT_EQ(
        undo.CompletePending(
            pending.pendingToken,
            UndoCompletionResult::Failed("Importer completion failed."))
            .state,
        UndoRequestState::Poisoned);
    EXPECT_EQ(firstValue->value, 0);
    EXPECT_EQ(pendingValue->value, 0);
    EXPECT_EQ(undo.GetTransactionCount(), 0u);
    EXPECT_TRUE(undo.IsPoisoned());
    EXPECT_TRUE(undo.GetScopeState(scope).IsDirty());
}

TEST(EditorUndoCoreTests, SavedScopeDoesNotBecomeCleanWhenUndoCreatesANewBranch)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Scene, "scene:savepoint-branch");
    const auto value = std::make_shared<ValueState>();

    ASSERT_TRUE(undo.Execute(Command(value, 0, 1, Info("First Value", {scope}))).Succeeded());
    undo.MarkSaved(scope, "saved-at-one");
    const auto savedRevision = undo.GetScopeState(scope).savedRevision;
    EXPECT_FALSE(undo.GetScopeState(scope).IsDirty());

    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_TRUE(undo.GetScopeState(scope).IsDirty());

    ASSERT_TRUE(undo.Execute(Command(value, 0, 2, Info("Branched Value", {scope}))).Succeeded());
    EXPECT_EQ(value->value, 2);
    EXPECT_NE(undo.GetScopeState(scope).currentRevision, savedRevision);
    EXPECT_TRUE(undo.GetScopeState(scope).IsDirty());

    undo.MarkSaved(scope, "saved-at-two");
    EXPECT_FALSE(undo.GetScopeState(scope).IsDirty());
    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_TRUE(undo.GetScopeState(scope).IsDirty());
    ASSERT_EQ(undo.RequestRedo().state, UndoRequestState::Completed);
    EXPECT_FALSE(undo.GetScopeState(scope).IsDirty());
}

TEST(EditorUndoCoreTests, FreezeAndPoisonStatesPreventFurtherHistoryMutationAndPublishEvents)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::ProjectSettings, "project:settings");
    const auto value = std::make_shared<ValueState>();
    std::vector<UndoHistoryEventKind> events;
    const auto subscription = undo.SubscribeHistoryChanged([&events](const UndoHistoryEvent& event)
    {
        events.push_back(event.kind);
    });

    undo.Freeze(UndoFreezeReason::PlayMode);
    EXPECT_EQ(undo.Execute(Command(value, 0, 1, Info("Blocked", {scope}))).state, UndoRequestState::Frozen);
    undo.Unfreeze(UndoFreezeReason::PlayMode);

    auto poison = std::make_unique<ValueCommand>(value, 0, 1, Info("Poison", {scope}));
    poison->FailNextApply(true);
    EXPECT_EQ(undo.Execute(std::move(poison)).state, UndoRequestState::Poisoned);
    EXPECT_TRUE(undo.IsPoisoned());
    EXPECT_EQ(undo.Execute(Command(value, 0, 1, Info("Blocked By Poison", {scope}))).state, UndoRequestState::Poisoned);
    EXPECT_NE(std::find(events.begin(), events.end(), UndoHistoryEventKind::CommandFailed), events.end());
    undo.UnsubscribeHistoryChanged(subscription);
}

TEST(EditorUndoCoreTests, AvailabilityReasonsDescribeTheGlobalHistoryBlocker)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Scene, "scene:availability-reason");
    const auto value = std::make_shared<ValueState>();

    EXPECT_EQ(undo.GetUndoUnavailableReason(), "There is no command to undo.");
    EXPECT_EQ(undo.GetRedoUnavailableReason(), "There is no command to redo.");

    undo.Freeze(UndoFreezeReason::PlayMode);
    EXPECT_EQ(undo.GetUndoUnavailableReason(), "Undo is unavailable while Play mode is active.");
    EXPECT_EQ(undo.GetRedoUnavailableReason(), "Redo is unavailable while Play mode is active.");
    undo.Unfreeze(UndoFreezeReason::PlayMode);

    auto transaction = undo.BeginTransaction({"Active", "EditorUndoCoreTests", {scope}, {}});
    ASSERT_TRUE(transaction.result.Succeeded());
    EXPECT_EQ(undo.GetUndoUnavailableReason(), "Finish or cancel the active editor transaction first.");
    EXPECT_EQ(undo.GetRedoUnavailableReason(), "Finish or cancel the active editor transaction first.");
    ASSERT_EQ(undo.CancelTransaction(transaction.transaction).state, UndoRequestState::Completed);

    auto poison = std::make_unique<ValueCommand>(value, 0, 1, Info("Poison", {scope}));
    poison->FailNextApply(true);
    ASSERT_EQ(undo.Execute(std::move(poison)).state, UndoRequestState::Poisoned);
    EXPECT_NE(undo.GetUndoUnavailableReason().find("requires recovery"), std::string::npos);
    EXPECT_NE(undo.GetRedoUnavailableReason().find("requires recovery"), std::string::npos);
}

TEST(EditorUndoCoreTests, ExecuteAndHistoryApplyEstablishMutationContexts)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Scene, "scene:mutation-context-command");
    const auto probe = std::make_shared<MutationContextProbe>();

    ASSERT_TRUE(undo.Execute(std::make_unique<MutationContextProbeCommand>(
        Info("Probe Mutation", {scope}), probe)).Succeeded());
    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    ASSERT_EQ(undo.RequestRedo().state, UndoRequestState::Completed);

#if !defined(NDEBUG)
    ASSERT_EQ(probe->directions.size(), 3u);
    EXPECT_EQ(probe->directions[0], UndoDirection::Redo);
    EXPECT_EQ(probe->directions[1], UndoDirection::Undo);
    EXPECT_EQ(probe->directions[2], UndoDirection::Redo);
    EXPECT_NE(probe->commandIds[0], 0u);
    EXPECT_EQ(probe->commandIds[0], probe->commandIds[1]);
    EXPECT_EQ(probe->commandIds[1], probe->commandIds[2]);
#endif
    EXPECT_FALSE(HasActiveEditorUndoMutationContext());
}

TEST(EditorUndoCoreTests, EditPreviewAndCancelEstablishMutationContexts)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Scene, "scene:mutation-context-edit");
    const auto probe = std::make_shared<MutationContextProbe>();

    auto begun = undo.BeginEditSession({
        Info("Probe Continuous Mutation", {scope}, "probe:context"),
        std::make_unique<MutationContextProbeEdit>(probe)});
    ASSERT_TRUE(begun.result.Succeeded());
    ASSERT_EQ(undo.PreviewEditSession(begun.session).state, UndoRequestState::Completed);
    ASSERT_EQ(undo.CancelEditSession(begun.session).state, UndoRequestState::Completed);

#if !defined(NDEBUG)
    ASSERT_EQ(probe->directions.size(), 2u);
    EXPECT_EQ(probe->directions[0], UndoDirection::Redo);
    EXPECT_EQ(probe->directions[1], UndoDirection::Undo);
    EXPECT_NE(probe->commandIds[0], 0u);
    EXPECT_EQ(probe->commandIds[0], probe->commandIds[1]);
#endif
    EXPECT_FALSE(HasActiveEditorUndoMutationContext());
}

TEST(EditorUndoCoreTests, TransactionChildrenObserveTheirOwnMutationContexts)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Scene, "scene:mutation-context-transaction");
    const auto probe = std::make_shared<MutationContextProbe>();

    auto transaction = undo.BeginTransaction({"Probe Transaction", "EditorUndoCoreTests", {scope}, {"target"}});
    ASSERT_TRUE(transaction.result.Succeeded());
    ASSERT_TRUE(undo.Execute(std::make_unique<MutationContextProbeCommand>(
        Info("Probe Child Mutation", {scope}), probe)).Succeeded());
    ASSERT_EQ(undo.CommitTransaction(transaction.transaction).state, UndoRequestState::Completed);
    ASSERT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    ASSERT_EQ(undo.RequestRedo().state, UndoRequestState::Completed);

#if !defined(NDEBUG)
    ASSERT_EQ(probe->directions.size(), 3u);
    EXPECT_EQ(probe->directions[0], UndoDirection::Redo);
    EXPECT_EQ(probe->directions[1], UndoDirection::Undo);
    EXPECT_EQ(probe->directions[2], UndoDirection::Redo);
    EXPECT_NE(probe->commandIds[0], 0u);
    EXPECT_EQ(probe->commandIds[0], probe->commandIds[1]);
    EXPECT_EQ(probe->commandIds[1], probe->commandIds[2]);
#endif
    EXPECT_FALSE(HasActiveEditorUndoMutationContext());
}

TEST(EditorUndoCoreTests, FirstUndoCancelsAnActivePreviewInsideATransaction)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Scene, "scene:transaction-preview-cancel");
    const auto value = std::make_shared<ValueState>();

    auto transaction = undo.BeginTransaction({"Preview Transaction", "EditorUndoCoreTests", {scope}, {}});
    ASSERT_TRUE(transaction.result.Succeeded());
    auto edit = undo.BeginEditSession({
        Info("Preview Value", {scope}, "property:value"),
        std::make_unique<ValueEdit>(value, 0, 9, Info("Preview Value", {scope}, "property:value"))});
    ASSERT_TRUE(edit.result.Succeeded());
    ASSERT_EQ(undo.PreviewEditSession(edit.session).state, UndoRequestState::Completed);
    ASSERT_EQ(value->value, 9);

    EXPECT_TRUE(undo.CanUndo());
    EXPECT_EQ(undo.RequestUndo().state, UndoRequestState::Completed);
    EXPECT_FALSE(undo.HasActiveEditSession());
    EXPECT_EQ(value->value, 0);
    ASSERT_EQ(undo.CancelTransaction(transaction.transaction).state, UndoRequestState::Completed);
}

TEST(EditorUndoCoreTests, FailedEditCommitKeepsThePreviewCancellable)
{
    EditorUndoService undo;
    const auto scope = Scope(UndoScopeKind::Scene, "scene:failed-edit-commit");
    const auto value = std::make_shared<ValueState>();

    auto edit = undo.BeginEditSession({
        Info("Preview Value", {scope}, "property:value"),
        std::make_unique<ValueEdit>(value, 0, 7, Info("Preview Value", {scope}, "property:value"))});
    ASSERT_TRUE(edit.result.Succeeded());
    ASSERT_EQ(undo.PreviewEditSession(edit.session).state, UndoRequestState::Completed);
    ASSERT_EQ(value->value, 7);

    value->conflict = true;
    EXPECT_EQ(undo.CommitEditSession(edit.session).state, UndoRequestState::Conflict);
    EXPECT_TRUE(edit.session.IsValid());
    EXPECT_TRUE(undo.HasActiveEditSession());

    value->conflict = false;
    ASSERT_EQ(undo.CancelEditSession(edit.session).state, UndoRequestState::Completed);
    EXPECT_EQ(value->value, 0);
}

TEST(EditorUndoCoreTests, InvalidatedOrClearedPendingCommandsCannotApplyLateMutations)
{
    const auto run = [](const bool clearAll)
    {
        EditorUndoService undo;
        const auto scope = Scope(UndoScopeKind::Asset, clearAll ? "asset:clear-gate" : "asset:invalidate-gate");
        const auto value = std::make_shared<ValueState>();
        const auto control = std::make_shared<DeferredWriteControl>();
        const auto started = undo.Execute(std::make_unique<DeferredWriteCommand>(
            value,
            0,
            5,
            Info("Deferred Import", {scope}),
            control));
        ASSERT_EQ(started.state, UndoRequestState::Pending);
        ASSERT_TRUE(control->capturedContext);
        ASSERT_TRUE(control->context.IsOperationCurrent());

        if (clearAll)
            undo.ClearAll(UndoClearReason::ProjectChanged);
        else
            undo.InvalidateScope(scope, UndoInvalidationReason::AssetReimported);

        EXPECT_FALSE(control->context.IsOperationCurrent());
        ASSERT_TRUE(static_cast<bool>(control->finish));
        control->finish();
        EXPECT_EQ(value->value, 0);
        EXPECT_FALSE(undo.IsPending());
        EXPECT_EQ(undo.GetTransactionCount(), 0u);
    };

    run(false);
    run(true);
}

TEST(EditorUndoCoreTests, ScopeInvalidationWaitsForAnInFlightDelayedMutationLease)
{
    const auto run = [](const bool clearAll)
    {
        EditorUndoService undo;
        const auto scope = Scope(
            UndoScopeKind::Asset,
            clearAll ? "asset:clear-mutation-lease" : "asset:invalidate-mutation-lease");
        const auto value = std::make_shared<ValueState>();
        const auto control = std::make_shared<BlockingMutationLeaseControl>();
        const auto started = undo.Execute(std::make_unique<BlockingMutationLeaseCommand>(
            value,
            0,
            5,
            Info("Blocking Delayed Mutation", {scope}),
            control));
        ASSERT_EQ(started.state, UndoRequestState::Pending);
        ASSERT_TRUE(static_cast<bool>(control->finish));

        std::thread worker([finish = control->finish]
        {
            finish();
        });

        bool acquired = false;
        {
            std::unique_lock lock(control->mutex);
            acquired = control->condition.wait_for(lock, std::chrono::seconds(5), [&control]
            {
                return control->leaseAcquired;
            });
        }
        if (!acquired)
        {
            // Do not leave a worker waiting on a test failure.
            undo.ClearAll(UndoClearReason::ProjectChanged);
            worker.join();
            ADD_FAILURE() << "The delayed mutation did not acquire its validity lease.";
            return;
        }

        if (clearAll)
            undo.ClearAll(UndoClearReason::ProjectChanged);
        else
            undo.InvalidateScope(scope, UndoInvalidationReason::AssetReimported);

        bool delayedWriteFinished = false;
        {
            std::lock_guard lock(control->mutex);
            delayedWriteFinished = control->delayedWriteFinished;
        }
        // Invalidation cannot return until a callback that acquired the lease
        // has completed its mutation and released the lease.
        EXPECT_TRUE(delayedWriteFinished);
        EXPECT_EQ(value->value, 5);

        worker.join();
        static_cast<void>(undo.DrainPendingCompletions());
        EXPECT_FALSE(undo.IsPending());
        EXPECT_EQ(undo.GetTransactionCount(), 0u);
    };

    run(false);
    run(true);
}
} // namespace NLS::Editor::Undo::Tests
