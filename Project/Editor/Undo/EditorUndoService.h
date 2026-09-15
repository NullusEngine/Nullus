#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Undo/UndoBlobStore.h"

namespace NLS::Editor::Undo
{
using CommandId = uint64_t;
using UndoPendingToken = uint64_t;
using UndoListenerToken = uint64_t;

enum class UndoScopeKind : uint8_t
{
    Scene,
    PrefabStage,
    Asset,
    UIDocument,
    ProjectSettings
};

// A scope identifies durable editor state. Commands must use an object/file
// identifier, asset id, normalized path, or document GUID in stableKey rather
// than retaining an engine object pointer.
struct UndoScopeId
{
    UndoScopeKind kind = UndoScopeKind::Scene;
    std::string stableKey;
    uint64_t generation = 0u;

    [[nodiscard]] bool IsValid() const noexcept { return !stableKey.empty(); }
    [[nodiscard]] bool operator==(const UndoScopeId& other) const noexcept;
    [[nodiscard]] bool operator!=(const UndoScopeId& other) const noexcept
    {
        return !(*this == other);
    }
};

struct UndoScopeIdHash
{
    [[nodiscard]] size_t operator()(const UndoScopeId& scope) const noexcept;
};

// Affected identifiers are intentionally opaque to the core. A source module
// can use object file IDs, asset IDs, node GUIDs, or stable property paths.
using UndoAffectedSet = std::vector<std::string>;
using UndoMergeKey = std::string;

struct UndoCommandInfo
{
    CommandId id = 0u;
    std::string label;
    std::string sourceModule;
    std::vector<UndoScopeId> scopes;
    UndoAffectedSet affected;
    UndoMergeKey mergeKey;
    size_t estimatedBytes = 0u;
    size_t estimatedBlobBytes = 0u;
    bool allowMerge = false;
    // Filesystem-backed commands can establish a durable clean baseline after
    // every successful Execute, Undo, and Redo. Keep this explicit instead of
    // treating every scope of a cross-scope transaction as saved.
    std::vector<UndoScopeId> writeThroughScopes;
};

enum class UndoDirection : uint8_t
{
    Undo,
    Redo
};

enum class UndoValidationState : uint8_t
{
    Ready,
    Stale,
    Conflict,
    Failed
};

struct UndoValidationResult
{
    UndoValidationState state = UndoValidationState::Ready;
    std::string message;

    [[nodiscard]] static UndoValidationResult Ready();
    [[nodiscard]] static UndoValidationResult Stale(std::string message = {});
    [[nodiscard]] static UndoValidationResult Conflict(std::string message = {});
    [[nodiscard]] static UndoValidationResult Failed(std::string message = {});
};

enum class UndoStartState : uint8_t
{
    Completed,
    Pending,
    Failed
};

struct UndoStartResult
{
    UndoStartState state = UndoStartState::Completed;
    std::string message;
    // Commands that perform a multi-step operation set this when they could
    // not restore a partially applied operation. The history then becomes a
    // barrier until the user clears it or a higher-level repair succeeds.
    bool rollbackFailed = false;

    [[nodiscard]] static UndoStartResult Completed();
    [[nodiscard]] static UndoStartResult Pending();
    [[nodiscard]] static UndoStartResult Failed(std::string message = {}, bool rollbackFailed = false);
};

struct UndoCompletionResult
{
    bool succeeded = true;
    std::string message;
    bool rollbackFailed = false;

    [[nodiscard]] static UndoCompletionResult Succeeded();
    [[nodiscard]] static UndoCompletionResult Failed(std::string message = {}, bool rollbackFailed = false);
};

// A pending operation owns this gate. An asynchronous callback must acquire a
// mutation lease before it changes durable state. Scope invalidation, ClearAll,
// and command completion close the gate and wait for leases already in flight,
// so a callback cannot write after an invalidation has returned.
class UndoOperationValidity final
{
public:
    class MutationLease final
    {
    public:
        MutationLease() = default;
        ~MutationLease();

        MutationLease(const MutationLease&) = delete;
        MutationLease& operator=(const MutationLease&) = delete;
        MutationLease(MutationLease&& other) noexcept;
        MutationLease& operator=(MutationLease&& other) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept { return m_acquired; }
        void Release() noexcept;

    private:
        MutationLease(std::shared_ptr<const UndoOperationValidity> validity, bool acquired);

        std::shared_ptr<const UndoOperationValidity> m_validity;
        bool m_acquired = false;

        friend class UndoOperationValidity;
    };

    [[nodiscard]] bool IsCurrent() const noexcept
    {
        return m_current.load(std::memory_order_acquire);
    }

    [[nodiscard]] MutationLease TryAcquireMutationLease(
        std::shared_ptr<const UndoOperationValidity> owner) const;

private:
    void Invalidate() noexcept;
    void ReleaseMutationLease() const noexcept;

    std::atomic_bool m_current = true;
    mutable std::mutex m_mutationLeaseMutex;
    mutable std::condition_variable m_mutationLeasesDrained;
    mutable size_t m_activeMutationLeases = 0u;

    friend class EditorUndoService;
};

struct UndoExecutionContext
{
    CommandId commandId = 0u;
    UndoDirection direction = UndoDirection::Redo;
    UndoPendingToken pendingToken = 0u;
    std::vector<UndoScopeId> scopes;
    std::shared_ptr<const UndoOperationValidity> operationValidity;

    [[nodiscard]] bool IsOperationCurrent() const noexcept
    {
        return !operationValidity || operationValidity->IsCurrent();
    }

    // Keep the returned lease alive for the entire delayed write and release
    // it before invoking UndoCompletion. Completion can synchronously close
    // the operation gate on the editor thread.
    [[nodiscard]] UndoOperationValidity::MutationLease TryAcquireOperationMutation() const
    {
        return operationValidity
            ? operationValidity->TryAcquireMutationLease(operationValidity)
            : UndoOperationValidity::MutationLease {};
    }
};

using UndoCompletion = std::function<void(UndoCompletionResult)>;

// Commands are concrete module-owned objects. The core deliberately does not
// provide a callback-command factory: persistent commands must own durable
// identifiers and snapshots, not capture editor objects or UI state.
class IEditorUndoCommand
{
public:
    virtual ~IEditorUndoCommand() = default;

    [[nodiscard]] virtual UndoCommandInfo GetInfo() const = 0;
    [[nodiscard]] virtual UndoValidationResult Validate(
        UndoDirection direction,
        const UndoExecutionContext& context) const = 0;
    virtual UndoStartResult Apply(
        UndoDirection direction,
        const UndoExecutionContext& context,
        UndoCompletion completion) = 0;
    // Composite transactions apply children synchronously so that a failure can
    // be rolled back atomically. Commands that can return Pending must opt out
    // before they are allowed to enter a transaction.
    [[nodiscard]] virtual bool MayCompleteAsynchronously() const { return false; }
    // Called on the already-recorded command with the newer command. Return
    // true only after replacing its post-edit snapshot with the newer state.
    // A successful merge must also make GetInfo() describe the retained merged
    // snapshots, including their exact memory and blob estimates.
    virtual bool TryMergeFrom(const IEditorUndoCommand& newer) = 0;
};

using UndoCommandPtr = std::unique_ptr<IEditorUndoCommand>;

// Continuous editors (Inspector controls, gizmos, UI Designer drags) use this
// protocol instead of pushing one command per frame. Implementations own their
// durable target handles and before/after snapshots; the service owns the
// active session and commits exactly one command after Preview finishes.
class IEditorUndoEdit
{
public:
    virtual ~IEditorUndoEdit() = default;

    [[nodiscard]] virtual UndoValidationResult Validate(
        const UndoExecutionContext& context) const = 0;
    virtual UndoStartResult Preview(
        const UndoExecutionContext& context,
        UndoCompletion completion) = 0;
    virtual UndoStartResult Cancel(
        const UndoExecutionContext& context,
        UndoCompletion completion) = 0;
    [[nodiscard]] virtual bool HasChanges() const = 0;
    // Returns a command describing the already-applied edit. Returning null
    // is treated as a no-op and never creates a history entry.
    [[nodiscard]] virtual UndoCommandPtr BuildCommand() = 0;
};

using UndoEditPtr = std::unique_ptr<IEditorUndoEdit>;

struct UndoEditDesc
{
    UndoCommandInfo info;
    UndoEditPtr edit;
    // Callers that have already observed the input edge may supply it so
    // expensive snapshot construction does not consume the merge window.
    std::optional<std::chrono::steady_clock::time_point> mergeStartedAt;
};

class UndoEditSession final
{
public:
    UndoEditSession() = default;
    [[nodiscard]] bool IsValid() const noexcept { return m_id != 0u; }

private:
    explicit UndoEditSession(const uint64_t id)
        : m_id(id)
    {
    }

    uint64_t m_id = 0u;

    friend class EditorUndoService;
};

struct UndoTransactionDesc
{
    std::string label;
    std::string sourceModule;
    std::vector<UndoScopeId> scopes;
    UndoAffectedSet affected;
};

class UndoTransaction final
{
public:
    UndoTransaction() = default;
    [[nodiscard]] bool IsValid() const noexcept { return m_id != 0u; }

private:
    explicit UndoTransaction(const uint64_t id)
        : m_id(id)
    {
    }

    uint64_t m_id = 0u;

    friend class EditorUndoService;
};

enum class UndoFreezeReason : uint8_t
{
    PlayMode,
    SceneTransition,
    ExternalOperation,
    User
};

enum class UndoClearReason : uint8_t
{
    SceneLoaded,
    ProjectChanged,
    EditorShutdown,
    UserRequested,
    Recovery
};

enum class UndoInvalidationReason : uint8_t
{
    TargetDestroyed,
    ScopeClosed,
    AssetReimported,
    ExternalModification,
    Reloaded
};

struct UndoBudget
{
    static constexpr size_t kDefaultMaxTransactions = 256u;
    static constexpr size_t kDefaultMaxMemoryBytes = 256u * 1024u * 1024u;
    static constexpr size_t kDefaultMaxBlobBytes = 2u * 1024u * 1024u * 1024u;

    size_t maxTransactions = kDefaultMaxTransactions;
    size_t maxMemoryBytes = kDefaultMaxMemoryBytes;
    size_t maxBlobBytes = kDefaultMaxBlobBytes;
    std::chrono::milliseconds mergeWindow {500};
};

enum class UndoRequestState : uint8_t
{
    Completed,
    Pending,
    Empty,
    Busy,
    Frozen,
    Poisoned,
    Stale,
    Conflict,
    Failed,
    InvalidTransaction
};

struct UndoRequestResult
{
    UndoRequestState state = UndoRequestState::Completed;
    std::string message;
    UndoPendingToken pendingToken = 0u;

    [[nodiscard]] bool Succeeded() const noexcept
    {
        return state == UndoRequestState::Completed || state == UndoRequestState::Pending;
    }
};

using ExecuteResult = UndoRequestResult;
using RecordResult = UndoRequestResult;

struct UndoTransactionBeginResult
{
    UndoTransaction transaction;
    UndoRequestResult result;
};

struct UndoEditBeginResult
{
    UndoEditSession session;
    UndoRequestResult result;
};

struct UndoScopeState
{
    uint64_t currentRevision = 0u;
    uint64_t savedRevision = 0u;
    std::string savedFingerprint;

    [[nodiscard]] bool IsDirty() const noexcept { return currentRevision != savedRevision; }
};

struct UndoHistoryState
{
    size_t transactionCount = 0u;
    size_t cursor = 0u;
    size_t retainedMemoryBytes = 0u;
    size_t retainedBlobBytes = 0u;
    bool canUndo = false;
    bool canRedo = false;
    bool pending = false;
    bool frozen = false;
    bool poisoned = false;
    std::string undoLabel;
    std::string redoLabel;
    std::string poisonMessage;
};

enum class UndoHistoryEventKind : uint8_t
{
    HistoryChanged,
    CommandApplied,
    CommandFailed,
    PendingStarted,
    PendingCompleted,
    ScopeInvalidated,
    Cleared
};

struct UndoHistoryEvent
{
    UndoHistoryEventKind kind = UndoHistoryEventKind::HistoryChanged;
    UndoHistoryState state;
    std::optional<UndoCommandInfo> command;
    std::vector<UndoScopeId> scopes;
    UndoDirection direction = UndoDirection::Redo;
    std::string message;
};

using UndoHistoryListener = std::function<void(const UndoHistoryEvent&)>;

// An editor-owned, non-static, global timeline. Construct one instance in
// Editor and inject it into panels/modules; there is deliberately no Get().
class EditorUndoService final
{
public:
    explicit EditorUndoService(UndoBudget budget = {});
    ~EditorUndoService();

    EditorUndoService(const EditorUndoService&) = delete;
    EditorUndoService& operator=(const EditorUndoService&) = delete;

    [[nodiscard]] ExecuteResult Execute(UndoCommandPtr command);
    // Records a command whose effects have already been previewed/applied.
    // The caller must only use this after a successful synchronous edit.
    [[nodiscard]] RecordResult RecordApplied(UndoCommandPtr command);

    [[nodiscard]] UndoEditBeginResult BeginEditSession(UndoEditDesc desc);
    [[nodiscard]] UndoRequestResult PreviewEditSession(UndoEditSession& session);
    [[nodiscard]] UndoRequestResult CommitEditSession(UndoEditSession& session);
    [[nodiscard]] UndoRequestResult CancelEditSession(UndoEditSession& session);

    [[nodiscard]] UndoTransactionBeginResult BeginTransaction(UndoTransactionDesc desc);
    [[nodiscard]] UndoRequestResult CommitTransaction(UndoTransaction& transaction);
    [[nodiscard]] UndoRequestResult CancelTransaction(UndoTransaction& transaction);

    [[nodiscard]] UndoRequestResult RequestUndo();
    [[nodiscard]] UndoRequestResult RequestRedo();
    // Invoke this on the editor main thread for a command that returned
    // Pending. It is also the target of the completion supplied to Apply.
    [[nodiscard]] UndoRequestResult CompletePending(
        UndoPendingToken token,
        UndoCompletionResult completion);
    // Command callbacks may arrive from worker threads. The service owns the
    // queue and drains it from Editor::PreUpdate so cursor/history/listeners
    // are only touched by the editor thread.
    [[nodiscard]] size_t DrainPendingCompletions();

    // Large command snapshots are owned by this editor service. The root is
    // configured by Editor once the project Library path is known; isolated
    // unit tests may intentionally leave blob storage disabled.
    [[nodiscard]] UndoBlobStoreResult ConfigureBlobStore(std::filesystem::path root);
    [[nodiscard]] std::shared_ptr<UndoBlobStore> GetSnapshotBlobStore() const;

    void MarkSaved(const UndoScopeId& scope, std::string contentFingerprint = {});
    void InvalidateScope(const UndoScopeId& scope, UndoInvalidationReason reason);
    void Freeze(UndoFreezeReason reason);
    void Unfreeze(UndoFreezeReason reason);
    void ClearAll(UndoClearReason reason = UndoClearReason::UserRequested);

    [[nodiscard]] bool CanUndo() const;
    [[nodiscard]] bool CanRedo() const;
    [[nodiscard]] bool IsPending() const;
    [[nodiscard]] bool HasActiveEditSession() const;
    // A UI owner must check its own token rather than merely observing that
    // some other editor surface has an active continuous edit.
    [[nodiscard]] bool IsEditSessionActive(const UndoEditSession& session) const noexcept;
    [[nodiscard]] bool IsFrozen() const;
    [[nodiscard]] bool IsPoisoned() const;
    [[nodiscard]] const std::string& GetPoisonMessage() const;
    [[nodiscard]] std::string GetUndoLabel() const;
    [[nodiscard]] std::string GetRedoLabel() const;
    // UI entry points must use these instead of inferring availability from
    // selection or window focus. An empty result means the action is ready.
    [[nodiscard]] std::string GetUndoUnavailableReason() const;
    [[nodiscard]] std::string GetRedoUnavailableReason() const;
    [[nodiscard]] UndoHistoryState GetHistoryState() const;
    [[nodiscard]] UndoScopeState GetScopeState(const UndoScopeId& scope) const;
    [[nodiscard]] const UndoBudget& GetBudget() const noexcept { return m_budget; }
    [[nodiscard]] size_t GetTransactionCount() const noexcept;
    [[nodiscard]] size_t GetCursor() const noexcept;

    [[nodiscard]] UndoListenerToken SubscribeHistoryChanged(UndoHistoryListener listener);
    void UnsubscribeHistoryChanged(UndoListenerToken token);

private:
    struct HistoryEntry;
    struct TransactionFrame;
    struct PendingOperation;
    struct ActiveEdit;
    struct LifetimeState;

    [[nodiscard]] UndoRequestResult BeginApply(
        UndoCommandPtr command,
        UndoCommandInfo info,
        UndoDirection direction,
        uint64_t transactionId);
    [[nodiscard]] UndoRequestResult BeginHistoryApply(size_t index, UndoDirection direction);
    [[nodiscard]] RecordResult RecordAppliedInternal(
        UndoCommandPtr command,
        std::optional<std::chrono::steady_clock::time_point> mergeStartedAt = std::nullopt);
    [[nodiscard]] UndoRequestResult FinalizeCompletedApply(PendingOperation& operation);
    [[nodiscard]] UndoRequestResult FinalizeFailedApply(
        const PendingOperation& operation,
        std::string message,
        bool rollbackFailed);
    // A child that fails while an outer transaction is open must abort the
    // entire still-uncommitted transaction graph. Entries in nested frames are
    // already applied to the live editor state, so restore them in reverse
    // order before discarding the frames. The caller folds a false result into
    // the poisoned-barrier state and includes the affected scopes in its
    // diagnostic/rebaseline path.
    [[nodiscard]] bool RollbackAndDiscardOpenTransactions(
        std::string& error,
        std::vector<UndoScopeId>* affectedScopes = nullptr);
    void RecordCompletedCommand(
        UndoCommandPtr command,
        UndoCommandInfo info,
        std::optional<std::chrono::steady_clock::time_point> mergeStartedAt = std::nullopt);
    void AppendToActiveTransaction(UndoCommandPtr command, UndoCommandInfo info, uint64_t transactionId);
    void AssignScopeRevisionTransitions(HistoryEntry& entry);
    void ApplyScopeRevisionTransition(const HistoryEntry& entry, UndoDirection direction);
    void MarkWriteThroughScopesSaved(const UndoCommandInfo& info);
    void AssignCommandId(UndoCommandInfo& info);
    void EnforceBudget();
    void DiscardRedo();
    void RemoveHistoryEntry(size_t index);
    // History can be lost while its effects are still applied (for example a
    // stale undo target). Move those scopes to a fresh, dirty revision rather
    // than accidentally retaining a matching save point.
    void RebaselineScopesAsDirty(const std::vector<UndoScopeId>& scopes);
    void Emit(
        UndoHistoryEventKind kind,
        std::optional<UndoCommandInfo> command = std::nullopt,
        std::vector<UndoScopeId> scopes = {},
        UndoDirection direction = UndoDirection::Redo,
        std::string message = {});
    [[nodiscard]] UndoRequestResult GetMutationAvailability() const;
    [[nodiscard]] UndoRequestResult CancelActiveEditSession();
    void DispatchCommandCompletion(UndoPendingToken token, UndoCompletionResult completion);
    void HeartbeatBlobSessionLease();
    [[nodiscard]] static bool SameMergeIdentity(
        const UndoCommandInfo& previous,
        const UndoCommandInfo& newer);
    [[nodiscard]] static std::vector<UndoScopeId> UniqueScopes(std::vector<UndoScopeId> scopes);
    [[nodiscard]] static UndoCommandInfo NormalizeInfo(UndoCommandInfo info);

    UndoBudget m_budget;
    std::vector<HistoryEntry> m_history;
    size_t m_cursor = 0u;
    size_t m_retainedMemoryBytes = 0u;
    size_t m_retainedBlobBytes = 0u;
    CommandId m_nextCommandId = 1u;
    uint64_t m_nextScopeRevision = 1u;
    uint64_t m_nextTransactionId = 1u;
    uint64_t m_nextEditSessionId = 1u;
    UndoPendingToken m_nextPendingToken = 1u;
    UndoListenerToken m_nextListenerToken = 1u;
    UndoPendingToken m_dispatchingToken = 0u;
    std::optional<UndoCompletionResult> m_deferredCompletion;
    std::thread::id m_ownerThread;
    std::mutex m_queuedCompletionMutex;
    std::deque<std::pair<UndoPendingToken, UndoCompletionResult>> m_queuedCompletions;
    std::unique_ptr<PendingOperation> m_pending;
    std::unique_ptr<ActiveEdit> m_activeEdit;
    std::vector<TransactionFrame> m_transactions;
    std::unordered_map<UndoScopeId, UndoScopeState, UndoScopeIdHash> m_scopeStates;
    std::unordered_map<UndoFreezeReason, size_t> m_freezeCounts;
    std::unordered_map<UndoListenerToken, UndoHistoryListener> m_listeners;
    std::shared_ptr<LifetimeState> m_lifetime;
    std::shared_ptr<UndoBlobStore> m_blobStore;
    std::filesystem::path m_blobSessionLease;
    std::chrono::steady_clock::time_point m_nextBlobSessionLeaseHeartbeat =
        std::chrono::steady_clock::time_point::min();
    bool m_poisoned = false;
    std::string m_poisonMessage;
};
} // namespace NLS::Editor::Undo
