#include "Undo/EditorUndoService.h"
#include "Undo/EditorUndoMutationContext.h"

#include "Guid.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace NLS::Editor::Undo
{
namespace
{
constexpr size_t SaturatingAdd(const size_t left, const size_t right)
{
    return right > (std::numeric_limits<size_t>::max)() - left
        ? (std::numeric_limits<size_t>::max)()
        : left + right;
}

constexpr std::string_view kBlobSessionPrefix = ".s-";
constexpr std::string_view kBlobSessionLeaseName = ".lease";
constexpr size_t kBlobSessionIdLength = 16u;
constexpr auto kBlobSessionLeaseTtl = std::chrono::hours(24);
constexpr auto kBlobSessionLeaseHeartbeatInterval = std::chrono::minutes(1);
constexpr auto kBlobSessionLeaseRetryInterval = std::chrono::seconds(5);

[[nodiscard]] bool IsLowerHex(const std::string_view value)
{
    return std::all_of(value.begin(), value.end(), [](const unsigned char character)
    {
        return (character >= '0' && character <= '9') ||
            (character >= 'a' && character <= 'f');
    });
}

[[nodiscard]] bool IsManagedBlobSessionDirectoryName(const std::string_view name)
{
    return name.size() == kBlobSessionPrefix.size() + kBlobSessionIdLength &&
        name.starts_with(kBlobSessionPrefix) &&
        IsLowerHex(name.substr(kBlobSessionPrefix.size()));
}

[[nodiscard]] UndoBlobStoreResult TouchBlobSessionLease(const std::filesystem::path& leasePath)
{
    std::error_code error;
    auto status = std::filesystem::symlink_status(leasePath, error);
    const bool missing = error == std::errc::no_such_file_or_directory;
    if (error && !missing)
    {
        return {
            UndoBlobStoreStatus::IoError,
            "Could not inspect the undo blob session lease: " + error.message()};
    }
    if (missing)
    {
        error.clear();
        status = std::filesystem::file_status(std::filesystem::file_type::not_found);
    }

    if (std::filesystem::exists(status))
    {
        if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))
        {
            return {
                UndoBlobStoreStatus::IoError,
                "The undo blob session lease is not a regular file."};
        }
    }
    {
        // Rewrite a fixed one-byte marker rather than relying on platform-
        // specific timestamp setters. The file content is intentionally opaque.
        std::ofstream output(leasePath, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            return {
                UndoBlobStoreStatus::IoError,
                "Could not create the undo blob session lease."};
        }
        output.put('1');
        if (!output.good())
        {
            return {
                UndoBlobStoreStatus::IoError,
                "Could not update the undo blob session lease."};
        }
    }
    return {};
}

[[nodiscard]] bool IsStaleBlobSessionLease(const std::filesystem::path& leasePath)
{
    std::error_code error;
    const auto status = std::filesystem::symlink_status(leasePath, error);
    if (error || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))
        return false;

    const auto modified = std::filesystem::last_write_time(leasePath, error);
    if (error)
        return false;

    const auto now = std::filesystem::file_time_type::clock::now();
    const auto ttl = std::chrono::duration_cast<std::filesystem::file_time_type::duration>(
        kBlobSessionLeaseTtl);
    return modified < now && now - modified > ttl;
}

void CleanupStaleBlobSessionDirectories(
    const std::filesystem::path& parentRoot,
    const std::filesystem::path& currentSessionRoot)
{
    std::error_code error;
    std::filesystem::directory_iterator sessions(parentRoot, error);
    const std::filesystem::directory_iterator end;
    if (error)
        return;

    while (sessions != end)
    {
        const auto entry = *sessions;
        const auto sessionPath = entry.path();
        const auto status = entry.symlink_status(error);
        if (!error &&
            !std::filesystem::is_symlink(status) &&
            std::filesystem::is_directory(status) &&
            sessionPath.lexically_normal() != currentSessionRoot.lexically_normal() &&
            IsManagedBlobSessionDirectoryName(sessionPath.filename().string()))
        {
            const auto leasePath = sessionPath / kBlobSessionLeaseName;
            if (IsStaleBlobSessionLease(leasePath))
            {
                // Recheck immediately before removal so a lease refreshed during
                // enumeration is never reclaimed as a stale session.
                error.clear();
                const auto currentStatus = std::filesystem::symlink_status(sessionPath, error);
                if (!error &&
                    !std::filesystem::is_symlink(currentStatus) &&
                    std::filesystem::is_directory(currentStatus) &&
                    IsStaleBlobSessionLease(leasePath))
                {
                    std::filesystem::remove_all(sessionPath, error);
                }
            }
        }
        error.clear();
        sessions.increment(error);
        if (error)
            return;
    }
}

bool ContainsScope(
    const std::vector<UndoScopeId>& scopes,
    const std::unordered_set<UndoScopeId, UndoScopeIdHash>& candidates)
{
    return std::any_of(scopes.begin(), scopes.end(), [&candidates](const UndoScopeId& scope)
    {
        return candidates.contains(scope);
    });
}

void AddScopes(
    const std::vector<UndoScopeId>& scopes,
    std::unordered_set<UndoScopeId, UndoScopeIdHash>& destination,
    bool& changed)
{
    for (const auto& scope : scopes)
        changed = destination.insert(scope).second || changed;
}

class TransactionUndoCommand final : public IEditorUndoCommand
{
public:
    struct Child
    {
        UndoCommandPtr command;
        UndoCommandInfo info;
    };

    TransactionUndoCommand(UndoCommandInfo info, std::vector<Child> children)
        : m_info(std::move(info))
        , m_children(std::move(children))
    {
    }

    [[nodiscard]] UndoCommandInfo GetInfo() const override
    {
        return m_info;
    }

    [[nodiscard]] UndoValidationResult Validate(
        const UndoDirection direction,
        const UndoExecutionContext& context) const override
    {
        const auto validateChild = [direction, &context](const Child& child)
        {
            if (!child.command)
                return UndoValidationResult::Stale("Transaction child no longer exists.");
            if (child.command->MayCompleteAsynchronously())
            {
                return UndoValidationResult::Failed(
                    "Asynchronous child commands are not supported inside a transaction.");
            }

            UndoExecutionContext childContext = context;
            childContext.commandId = child.info.id;
            childContext.scopes = child.info.scopes;
            return child.command->Validate(direction, childContext);
        };

        if (direction == UndoDirection::Redo)
        {
            for (const auto& child : m_children)
            {
                const auto validation = validateChild(child);
                if (validation.state != UndoValidationState::Ready)
                    return validation;
            }
        }
        else
        {
            for (auto iterator = m_children.rbegin(); iterator != m_children.rend(); ++iterator)
            {
                const auto validation = validateChild(*iterator);
                if (validation.state != UndoValidationState::Ready)
                    return validation;
            }
        }

        return UndoValidationResult::Ready();
    }

    UndoStartResult Apply(
        const UndoDirection direction,
        const UndoExecutionContext& context,
        UndoCompletion) override
    {
        if (std::any_of(m_children.begin(), m_children.end(), [](const Child& child)
            {
                return child.command && child.command->MayCompleteAsynchronously();
            }))
        {
            return UndoStartResult::Failed(
                "Asynchronous child commands are not supported inside a transaction.");
        }

        std::vector<Child*> applied;
        applied.reserve(m_children.size());

        const auto applyChild = [direction, &context, &applied](Child& child)
        {
            if (!child.command)
                return UndoStartResult::Failed("Transaction child no longer exists.");

            UndoExecutionContext childContext = context;
            childContext.commandId = child.info.id;
            childContext.scopes = child.info.scopes;
            const ScopedEditorUndoMutationContext childMutationContext(childContext);
            const auto result = child.command->Apply(direction, childContext, {});
            if (result.state == UndoStartState::Completed)
                applied.push_back(&child);
            return result;
        };

        UndoStartResult failure = UndoStartResult::Completed();
        if (direction == UndoDirection::Redo)
        {
            for (auto& child : m_children)
            {
                const auto result = applyChild(child);
                if (result.state != UndoStartState::Completed)
                {
                    failure = result;
                    break;
                }
            }
        }
        else
        {
            for (auto iterator = m_children.rbegin(); iterator != m_children.rend(); ++iterator)
            {
                const auto result = applyChild(*iterator);
                if (result.state != UndoStartState::Completed)
                {
                    failure = result;
                    break;
                }
            }
        }

        if (failure.state == UndoStartState::Completed)
            return failure;

        // A committed transaction must be atomic. Child asynchronous work is
        // rejected here because the command protocol cannot safely interleave
        // another editor operation between transaction children.
        if (failure.state == UndoStartState::Pending)
            failure = UndoStartResult::Failed("Pending child commands are not supported inside a transaction.");

        const UndoDirection rollbackDirection =
            direction == UndoDirection::Redo ? UndoDirection::Undo : UndoDirection::Redo;
        bool rollbackFailed = failure.rollbackFailed;
        for (auto iterator = applied.rbegin(); iterator != applied.rend(); ++iterator)
        {
            Child& child = **iterator;
            if (!child.command)
            {
                rollbackFailed = true;
                continue;
            }

            UndoExecutionContext childContext = context;
            childContext.commandId = child.info.id;
            childContext.direction = rollbackDirection;
            childContext.scopes = child.info.scopes;
            const ScopedEditorUndoMutationContext childMutationContext(childContext);
            const auto rollback = child.command->Apply(rollbackDirection, childContext, {});
            if (rollback.state != UndoStartState::Completed)
                rollbackFailed = true;
        }

        failure.rollbackFailed = rollbackFailed;
        return failure;
    }

    bool TryMergeFrom(const IEditorUndoCommand&) override
    {
        return false;
    }

private:
    UndoCommandInfo m_info;
    std::vector<Child> m_children;
};

UndoCommandInfo BuildTransactionInfo(
    const UndoTransactionDesc& description,
    const std::vector<TransactionUndoCommand::Child>& children)
{
    UndoCommandInfo info;
    info.label = description.label;
    info.sourceModule = description.sourceModule;
    info.scopes = description.scopes;
    info.affected = description.affected;

    for (const auto& child : children)
    {
        info.scopes.insert(info.scopes.end(), child.info.scopes.begin(), child.info.scopes.end());
        info.affected.insert(info.affected.end(), child.info.affected.begin(), child.info.affected.end());
        info.writeThroughScopes.insert(
            info.writeThroughScopes.end(),
            child.info.writeThroughScopes.begin(),
            child.info.writeThroughScopes.end());
        info.estimatedBytes = SaturatingAdd(info.estimatedBytes, child.info.estimatedBytes);
        info.estimatedBlobBytes = SaturatingAdd(info.estimatedBlobBytes, child.info.estimatedBlobBytes);
        if (info.label.empty())
            info.label = child.info.label;
        if (info.sourceModule.empty())
            info.sourceModule = child.info.sourceModule;
    }

    if (info.label.empty())
        info.label = "Transaction";
    return info;
}

[[nodiscard]] bool RollbackCommand(
    IEditorUndoCommand& command,
    const UndoCommandInfo& info,
    const UndoPendingToken pendingToken,
    std::shared_ptr<const UndoOperationValidity> operationValidity,
    std::string& error)
{
    const UndoExecutionContext context {
        info.id,
        UndoDirection::Undo,
        pendingToken,
        info.scopes,
        std::move(operationValidity)};
    const auto validation = command.Validate(UndoDirection::Undo, context);
    if (validation.state != UndoValidationState::Ready)
    {
        if (error.empty())
        {
            error = validation.message.empty()
                ? "The undo command could not be validated for rollback."
                : validation.message;
        }
        return false;
    }

    const ScopedEditorUndoMutationContext mutationContext(context);
    const auto result = command.Apply(UndoDirection::Undo, context, {});
    if (result.state == UndoStartState::Completed)
        return true;
    if (error.empty())
    {
        error = result.message.empty()
            ? "The undo command could not be rolled back."
            : result.message;
    }
    return false;
}
} // namespace

struct EditorUndoService::HistoryEntry
{
    struct ScopeRevisionTransition
    {
        UndoScopeId scope;
        uint64_t beforeRevision = 0u;
        uint64_t afterRevision = 0u;
    };

    UndoCommandPtr command;
    UndoCommandInfo info;
    std::chrono::steady_clock::time_point completedAt;
    std::vector<ScopeRevisionTransition> scopeRevisions;
};

struct EditorUndoService::TransactionFrame
{
    uint64_t id = 0u;
    UndoTransactionDesc description;
    std::vector<HistoryEntry> entries;
};

struct EditorUndoService::PendingOperation
{
    enum class Kind : uint8_t
    {
        Execute,
        Undo,
        Redo
    };

    Kind kind = Kind::Execute;
    UndoDirection direction = UndoDirection::Redo;
    UndoPendingToken token = 0u;
    uint64_t transactionId = 0u;
    size_t historyIndex = 0u;
    CommandId historyCommandId = 0u;
    UndoCommandPtr command;
    UndoCommandInfo info;
    std::shared_ptr<UndoOperationValidity> validity;
    bool invalidated = false;
    // A command that unexpectedly returned Pending while an outer transaction
    // was open cannot be inserted into the eventual composite command. Its
    // completion is still tracked, then conservatively turned into a barrier.
    bool pendingTransactionChild = false;
};

struct EditorUndoService::ActiveEdit
{
    uint64_t id = 0u;
    UndoCommandInfo info;
    UndoEditPtr edit;
    // Preserve the user's interaction timing even when BuildCommand performs
    // expensive serialization or blob-store writes during Commit.
    std::chrono::steady_clock::time_point startedAt;
    bool previewed = false;
};

struct EditorUndoService::LifetimeState
{
    std::mutex mutex;
    EditorUndoService* owner = nullptr;
};

bool UndoScopeId::operator==(const UndoScopeId& other) const noexcept
{
    return kind == other.kind && generation == other.generation && stableKey == other.stableKey;
}

size_t UndoScopeIdHash::operator()(const UndoScopeId& scope) const noexcept
{
    const auto kindHash = std::hash<uint8_t> {}(static_cast<uint8_t>(scope.kind));
    const auto keyHash = std::hash<std::string> {}(scope.stableKey);
    const auto generationHash = std::hash<uint64_t> {}(scope.generation);
    return kindHash ^ (keyHash << 1u) ^ (generationHash << 2u);
}

UndoValidationResult UndoValidationResult::Ready()
{
    return {};
}

UndoValidationResult UndoValidationResult::Stale(std::string message)
{
    return {UndoValidationState::Stale, std::move(message)};
}

UndoValidationResult UndoValidationResult::Conflict(std::string message)
{
    return {UndoValidationState::Conflict, std::move(message)};
}

UndoValidationResult UndoValidationResult::Failed(std::string message)
{
    return {UndoValidationState::Failed, std::move(message)};
}

UndoStartResult UndoStartResult::Completed()
{
    return {};
}

UndoStartResult UndoStartResult::Pending()
{
    return {UndoStartState::Pending, {}, false};
}

UndoStartResult UndoStartResult::Failed(std::string message, const bool rollbackFailed)
{
    return {UndoStartState::Failed, std::move(message), rollbackFailed};
}

UndoCompletionResult UndoCompletionResult::Succeeded()
{
    return {};
}

UndoCompletionResult UndoCompletionResult::Failed(std::string message, const bool rollbackFailed)
{
    return {false, std::move(message), rollbackFailed};
}

UndoOperationValidity::MutationLease::MutationLease(
    std::shared_ptr<const UndoOperationValidity> validity,
    const bool acquired)
    : m_validity(std::move(validity))
    , m_acquired(acquired)
{
}

UndoOperationValidity::MutationLease::~MutationLease()
{
    Release();
}

UndoOperationValidity::MutationLease::MutationLease(MutationLease&& other) noexcept
    : m_validity(std::move(other.m_validity))
    , m_acquired(std::exchange(other.m_acquired, false))
{
}

UndoOperationValidity::MutationLease& UndoOperationValidity::MutationLease::operator=(MutationLease&& other) noexcept
{
    if (this != &other)
    {
        Release();
        m_validity = std::move(other.m_validity);
        m_acquired = std::exchange(other.m_acquired, false);
    }
    return *this;
}

void UndoOperationValidity::MutationLease::Release() noexcept
{
    if (!m_acquired)
        return;

    m_acquired = false;
    const auto validity = std::move(m_validity);
    if (validity)
        validity->ReleaseMutationLease();
}

UndoOperationValidity::MutationLease UndoOperationValidity::TryAcquireMutationLease(
    std::shared_ptr<const UndoOperationValidity> owner) const
{
    if (!owner)
        return {};

    std::lock_guard lock(m_mutationLeaseMutex);
    if (!m_current.load(std::memory_order_acquire))
        return {};

    ++m_activeMutationLeases;
    return MutationLease(std::move(owner), true);
}

void UndoOperationValidity::Invalidate() noexcept
{
    std::unique_lock lock(m_mutationLeaseMutex);
    m_current.store(false, std::memory_order_release);
    m_mutationLeasesDrained.wait(lock, [this]
    {
        return m_activeMutationLeases == 0u;
    });
}

void UndoOperationValidity::ReleaseMutationLease() const noexcept
{
    std::lock_guard lock(m_mutationLeaseMutex);
    if (m_activeMutationLeases == 0u)
        return;

    --m_activeMutationLeases;
    if (m_activeMutationLeases == 0u)
        m_mutationLeasesDrained.notify_all();
}

EditorUndoService::EditorUndoService(UndoBudget budget)
    : m_budget(std::move(budget))
    , m_lifetime(std::make_shared<LifetimeState>())
    , m_ownerThread(std::this_thread::get_id())
{
    m_lifetime->owner = this;
}

EditorUndoService::~EditorUndoService()
{
    // Commands may own references to project-local snapshot blobs. Destroy
    // them before the store so their reference releases remain meaningful even
    // when a service is used outside the Editor lifetime in a focused test.
    if (m_pending && m_pending->validity)
        m_pending->validity->Invalidate();
    m_pending.reset();
    m_activeEdit.reset();
    m_transactions.clear();
    m_history.clear();
    if (m_blobStore)
        static_cast<void>(m_blobStore->CleanupUnreferenced());
    if (!m_blobSessionLease.empty())
    {
        std::error_code error;
        std::filesystem::remove(m_blobSessionLease, error);
    }

    if (m_lifetime)
    {
        std::lock_guard lock(m_lifetime->mutex);
        m_lifetime->owner = nullptr;
    }
}

ExecuteResult EditorUndoService::Execute(UndoCommandPtr command)
{
    if (m_activeEdit)
        return {UndoRequestState::Busy, "A continuous edit must be committed or cancelled first."};
    const auto availability = GetMutationAvailability();
    if (availability.state != UndoRequestState::Completed)
        return availability;
    if (!command)
        return {UndoRequestState::Failed, "Cannot execute a null undo command."};

    auto info = NormalizeInfo(command->GetInfo());
    if (!m_transactions.empty() && command->MayCompleteAsynchronously())
    {
        return {
            UndoRequestState::Failed,
            "Asynchronous undo commands cannot be executed inside an active transaction."};
    }
    AssignCommandId(info);
    const uint64_t transactionId = m_transactions.empty() ? 0u : m_transactions.back().id;
    return BeginApply(std::move(command), std::move(info), UndoDirection::Redo, transactionId);
}

RecordResult EditorUndoService::RecordApplied(UndoCommandPtr command)
{
    if (m_activeEdit)
        return {UndoRequestState::Busy, "A continuous edit must be committed or cancelled first."};
    return RecordAppliedInternal(std::move(command));
}

RecordResult EditorUndoService::RecordAppliedInternal(
    UndoCommandPtr command,
    const std::optional<std::chrono::steady_clock::time_point> mergeStartedAt)
{
    const auto availability = GetMutationAvailability();
    if (availability.state != UndoRequestState::Completed)
        return availability;
    if (!command)
        return {UndoRequestState::Failed, "Cannot record a null undo command."};

    auto info = NormalizeInfo(command->GetInfo());
    if (!m_transactions.empty() && command->MayCompleteAsynchronously())
    {
        return {
            UndoRequestState::Failed,
            "Asynchronous undo commands cannot be recorded inside an active transaction."};
    }
    AssignCommandId(info);
    const UndoExecutionContext context {info.id, UndoDirection::Redo, 0u, info.scopes};
    const auto validation = command->Validate(UndoDirection::Redo, context);
    if (validation.state != UndoValidationState::Ready)
    {
        const auto state = validation.state == UndoValidationState::Stale
            ? UndoRequestState::Stale
            : validation.state == UndoValidationState::Conflict
                ? UndoRequestState::Conflict
                : UndoRequestState::Failed;
        Emit(UndoHistoryEventKind::CommandFailed, info, info.scopes, UndoDirection::Redo, validation.message);
        return {state, validation.message};
    }

    if (m_transactions.empty())
        RecordCompletedCommand(std::move(command), std::move(info), mergeStartedAt);
    else
        AppendToActiveTransaction(std::move(command), std::move(info), m_transactions.back().id);
    return {};
}

UndoEditBeginResult EditorUndoService::BeginEditSession(UndoEditDesc description)
{
    const auto availability = GetMutationAvailability();
    if (availability.state != UndoRequestState::Completed)
        return {{}, availability};
    if (m_activeEdit)
        return {{}, {UndoRequestState::Busy, "Another continuous edit is already active."}};
    if (!description.edit)
        return {{}, {UndoRequestState::Failed, "Cannot begin an edit session without an edit implementation."}};

    auto info = NormalizeInfo(std::move(description.info));
    AssignCommandId(info);
    const UndoExecutionContext context {info.id, UndoDirection::Redo, 0u, info.scopes};
    const auto validation = description.edit->Validate(context);
    if (validation.state != UndoValidationState::Ready)
    {
        const auto state = validation.state == UndoValidationState::Stale
            ? UndoRequestState::Stale
            : validation.state == UndoValidationState::Conflict
                ? UndoRequestState::Conflict
                : UndoRequestState::Failed;
        Emit(UndoHistoryEventKind::CommandFailed, info, info.scopes, UndoDirection::Redo, validation.message);
        return {{}, {state, validation.message}};
    }

    m_activeEdit = std::make_unique<ActiveEdit>();
    m_activeEdit->id = m_nextEditSessionId++;
    m_activeEdit->info = std::move(info);
    m_activeEdit->edit = std::move(description.edit);
    m_activeEdit->startedAt = description.mergeStartedAt.value_or(std::chrono::steady_clock::now());
    return {UndoEditSession(m_activeEdit->id), {}};
}

UndoRequestResult EditorUndoService::PreviewEditSession(UndoEditSession& session)
{
    if (!session.IsValid() || !m_activeEdit || m_activeEdit->id != session.m_id)
        return {UndoRequestState::InvalidTransaction, "The edit session is no longer active."};
    const auto availability = GetMutationAvailability();
    if (availability.state != UndoRequestState::Completed)
        return availability;

    const UndoExecutionContext context {
        m_activeEdit->info.id,
        UndoDirection::Redo,
        0u,
        m_activeEdit->info.scopes};
    const auto validation = m_activeEdit->edit->Validate(context);
    if (validation.state != UndoValidationState::Ready)
    {
        const auto state = validation.state == UndoValidationState::Stale
            ? UndoRequestState::Stale
            : validation.state == UndoValidationState::Conflict
                ? UndoRequestState::Conflict
                : UndoRequestState::Failed;
        Emit(UndoHistoryEventKind::CommandFailed, m_activeEdit->info, m_activeEdit->info.scopes, UndoDirection::Redo, validation.message);
        return {state, validation.message};
    }

    const ScopedEditorUndoMutationContext mutationContext(context);
    const auto result = m_activeEdit->edit->Preview(context, {});
    if (result.state == UndoStartState::Completed)
    {
        m_activeEdit->previewed = true;
        return {};
    }
    if (result.state == UndoStartState::Pending)
        return {UndoRequestState::Failed, "Continuous edit previews must complete synchronously."};

    if (result.rollbackFailed)
    {
        m_poisoned = true;
        m_poisonMessage = result.message;
    }
    Emit(UndoHistoryEventKind::CommandFailed, m_activeEdit->info, m_activeEdit->info.scopes, UndoDirection::Redo, result.message);
    return {result.rollbackFailed ? UndoRequestState::Poisoned : UndoRequestState::Failed, result.message};
}

UndoRequestResult EditorUndoService::CommitEditSession(UndoEditSession& session)
{
    if (!session.IsValid() || !m_activeEdit || m_activeEdit->id != session.m_id)
        return {UndoRequestState::InvalidTransaction, "The edit session is no longer active."};
    const auto availability = GetMutationAvailability();
    if (availability.state != UndoRequestState::Completed)
        return availability;

    if (!m_activeEdit->edit->HasChanges())
    {
        m_activeEdit.reset();
        session.m_id = 0u;
        Emit(UndoHistoryEventKind::HistoryChanged);
        return {};
    }

    auto activeEdit = std::move(m_activeEdit);
    UndoCommandPtr command = activeEdit->edit->BuildCommand();
    if (!command)
    {
        session.m_id = 0u;
        Emit(UndoHistoryEventKind::HistoryChanged);
        return {};
    }

    const auto recorded = RecordAppliedInternal(std::move(command), activeEdit->startedAt);
    if (!recorded.Succeeded())
    {
        // Keep the preview session alive after a validation/conflict failure so
        // the caller can still cancel back to the before state.
        m_activeEdit = std::move(activeEdit);
        session.m_id = m_activeEdit->id;
        return recorded;
    }

    session.m_id = 0u;
    return recorded;
}

UndoRequestResult EditorUndoService::CancelEditSession(UndoEditSession& session)
{
    if (!session.IsValid() || !m_activeEdit || m_activeEdit->id != session.m_id)
        return {UndoRequestState::InvalidTransaction, "The edit session is no longer active."};
    const auto result = CancelActiveEditSession();
    if (result.state == UndoRequestState::Completed || result.state == UndoRequestState::Stale)
        session.m_id = 0u;
    return result;
}

UndoTransactionBeginResult EditorUndoService::BeginTransaction(UndoTransactionDesc description)
{
    if (m_activeEdit)
        return {{}, {UndoRequestState::Busy, "A continuous edit must be committed or cancelled first."}};
    const auto availability = GetMutationAvailability();
    if (availability.state != UndoRequestState::Completed)
        return {{}, availability};

    TransactionFrame frame;
    frame.id = m_nextTransactionId++;
    frame.description = std::move(description);
    frame.description.scopes = UniqueScopes(std::move(frame.description.scopes));
    m_transactions.push_back(std::move(frame));
    return {UndoTransaction(m_transactions.back().id), {}};
}

UndoRequestResult EditorUndoService::CommitTransaction(UndoTransaction& transaction)
{
    if (!transaction.IsValid() || m_transactions.empty() || m_transactions.back().id != transaction.m_id)
        return {UndoRequestState::InvalidTransaction, "Only the active transaction can be committed."};
    const auto availability = GetMutationAvailability();
    if (availability.state != UndoRequestState::Completed)
        return availability;

    if (std::any_of(m_transactions.back().entries.begin(), m_transactions.back().entries.end(), [](const HistoryEntry& entry)
        {
            return entry.command && entry.command->MayCompleteAsynchronously();
        }))
    {
        return {
            UndoRequestState::Failed,
            "Asynchronous undo commands cannot be committed inside a transaction."};
    }

    TransactionFrame frame = std::move(m_transactions.back());
    m_transactions.pop_back();
    transaction.m_id = 0u;

    if (frame.entries.empty())
    {
        Emit(UndoHistoryEventKind::HistoryChanged);
        return {};
    }

    std::vector<TransactionUndoCommand::Child> children;
    children.reserve(frame.entries.size());
    for (auto& entry : frame.entries)
        children.push_back({std::move(entry.command), std::move(entry.info)});

    auto info = NormalizeInfo(BuildTransactionInfo(frame.description, children));
    AssignCommandId(info);
    auto command = std::make_unique<TransactionUndoCommand>(info, std::move(children));
    if (m_transactions.empty())
        RecordCompletedCommand(std::move(command), std::move(info));
    else
        AppendToActiveTransaction(std::move(command), std::move(info), m_transactions.back().id);
    return {};
}

UndoRequestResult EditorUndoService::CancelTransaction(UndoTransaction& transaction)
{
    if (!transaction.IsValid() || m_transactions.empty() || m_transactions.back().id != transaction.m_id)
        return {UndoRequestState::InvalidTransaction, "Only the active transaction can be cancelled."};
    if (m_pending || m_dispatchingToken != 0u)
        return {UndoRequestState::Busy, "An undo command is still pending."};
    if (m_poisoned)
        return {UndoRequestState::Poisoned, m_poisonMessage};

    auto& frame = m_transactions.back();
    if (std::any_of(frame.entries.begin(), frame.entries.end(), [](const HistoryEntry& entry)
        {
            return entry.command && entry.command->MayCompleteAsynchronously();
        }))
    {
        return {
            UndoRequestState::Failed,
            "Asynchronous undo commands cannot be cancelled inside a transaction."};
    }

    std::vector<HistoryEntry*> reverted;
    reverted.reserve(frame.entries.size());
    std::string error;
    bool rollbackFailed = false;

    for (auto iterator = frame.entries.rbegin(); iterator != frame.entries.rend(); ++iterator)
    {
        if (!iterator->command)
        {
            error = "Transaction child no longer exists.";
            break;
        }

        const UndoExecutionContext context {
            iterator->info.id,
            UndoDirection::Undo,
            0u,
            iterator->info.scopes};
        const auto validation = iterator->command->Validate(UndoDirection::Undo, context);
        if (validation.state != UndoValidationState::Ready)
        {
            error = validation.message;
            break;
        }

        const ScopedEditorUndoMutationContext mutationContext(context);
        const auto result = iterator->command->Apply(UndoDirection::Undo, context, {});
        if (result.state != UndoStartState::Completed)
        {
            error = result.message;
            rollbackFailed = result.rollbackFailed;
            break;
        }
        reverted.push_back(&*iterator);
    }

    if (!error.empty())
    {
        for (auto iterator = reverted.rbegin(); iterator != reverted.rend(); ++iterator)
        {
            auto& entry = **iterator;
            if (!entry.command)
            {
                rollbackFailed = true;
                continue;
            }
            const UndoExecutionContext context {
                entry.info.id,
                UndoDirection::Redo,
                0u,
                entry.info.scopes};
            const ScopedEditorUndoMutationContext mutationContext(context);
            const auto restore = entry.command->Apply(UndoDirection::Redo, context, {});
            if (restore.state != UndoStartState::Completed)
                rollbackFailed = true;
        }

        if (rollbackFailed)
        {
            m_poisoned = true;
            m_poisonMessage = error.empty() ? "A transaction rollback failed." : error;
        }
        Emit(UndoHistoryEventKind::CommandFailed, std::nullopt, {}, UndoDirection::Undo, error);
        Emit(UndoHistoryEventKind::HistoryChanged);
        return {
            rollbackFailed ? UndoRequestState::Poisoned : UndoRequestState::Failed,
            error.empty() && rollbackFailed ? m_poisonMessage : error};
    }

    m_transactions.pop_back();
    transaction.m_id = 0u;
    Emit(UndoHistoryEventKind::HistoryChanged);
    return {};
}

UndoRequestResult EditorUndoService::RequestUndo()
{
    // The first Undo is also the universal cancel gesture for an active drag
    // or text preview. This remains true when that preview contributes to an
    // open transaction; the transaction itself still blocks history traversal.
    if (m_activeEdit)
        return CancelActiveEditSession();
    if (!m_transactions.empty())
        return {UndoRequestState::Busy, "A transaction is active."};
    const auto availability = GetMutationAvailability();
    if (availability.state != UndoRequestState::Completed)
        return availability;
    if (m_cursor == 0u)
        return {UndoRequestState::Empty, "There is no command to undo."};
    return BeginHistoryApply(m_cursor - 1u, UndoDirection::Undo);
}

UndoRequestResult EditorUndoService::RequestRedo()
{
    if (!m_transactions.empty())
        return {UndoRequestState::Busy, "A transaction is active."};
    if (m_activeEdit)
        return {UndoRequestState::Busy, "A continuous edit is active."};
    const auto availability = GetMutationAvailability();
    if (availability.state != UndoRequestState::Completed)
        return availability;
    if (m_cursor >= m_history.size())
        return {UndoRequestState::Empty, "There is no command to redo."};
    return BeginHistoryApply(m_cursor, UndoDirection::Redo);
}

UndoRequestResult EditorUndoService::CompletePending(
    const UndoPendingToken token,
    UndoCompletionResult completion)
{
    if (std::this_thread::get_id() != m_ownerThread)
    {
        std::lock_guard lock(m_queuedCompletionMutex);
        m_queuedCompletions.emplace_back(token, std::move(completion));
        return {UndoRequestState::Pending, "Pending completion queued for the editor thread.", token};
    }

    if (m_dispatchingToken == token)
    {
        m_deferredCompletion = std::move(completion);
        return {UndoRequestState::Pending, {}, token};
    }

    if (!m_pending || m_pending->token != token)
        return {UndoRequestState::Failed, "The pending undo operation is no longer active."};

    PendingOperation operation = std::move(*m_pending);
    m_pending.reset();

    UndoRequestResult result;
    if (operation.invalidated)
    {
        result = {UndoRequestState::Stale, "The pending undo operation was invalidated."};
        Emit(UndoHistoryEventKind::CommandFailed, operation.info, operation.info.scopes, operation.direction, result.message);
    }
    else if (completion.succeeded)
    {
        result = FinalizeCompletedApply(operation);
    }
    else
    {
        if (operation.pendingTransactionChild)
        {
            std::vector<UndoScopeId> affectedScopes = operation.info.scopes;
            std::string rollbackError;
            if (!operation.command)
            {
                rollbackError = "The asynchronous transaction child no longer exists.";
            }
            else
            {
                static_cast<void>(RollbackCommand(
                    *operation.command,
                    operation.info,
                    operation.token,
                    operation.validity,
                    rollbackError));
            }

            static_cast<void>(RollbackAndDiscardOpenTransactions(rollbackError, &affectedScopes));
            operation.info.scopes = affectedScopes;
            RebaselineScopesAsDirty(affectedScopes);

            std::string message = std::move(completion.message);
            if (message.empty())
                message = "An asynchronous transaction child failed before the transaction could be committed.";
            if (!rollbackError.empty())
                message += " Transaction rollback: " + rollbackError;
            result = FinalizeFailedApply(operation, std::move(message), true);
        }
        else
        {
            result = FinalizeFailedApply(operation, std::move(completion.message), completion.rollbackFailed);
        }
    }

    if (operation.validity)
        operation.validity->Invalidate();
    Emit(UndoHistoryEventKind::PendingCompleted, operation.info, operation.info.scopes, operation.direction, result.message);
    return result;
}

size_t EditorUndoService::DrainPendingCompletions()
{
    if (std::this_thread::get_id() != m_ownerThread)
        return 0u;

    HeartbeatBlobSessionLease();

    size_t drained = 0u;
    for (;;)
    {
        std::deque<std::pair<UndoPendingToken, UndoCompletionResult>> completions;
        {
            std::lock_guard lock(m_queuedCompletionMutex);
            if (m_queuedCompletions.empty())
                break;
            completions.swap(m_queuedCompletions);
        }

        for (auto& [token, completion] : completions)
        {
            static_cast<void>(CompletePending(token, std::move(completion)));
            ++drained;
        }
    }
    return drained;
}

void EditorUndoService::HeartbeatBlobSessionLease()
{
    if (m_blobSessionLease.empty())
        return;

    const auto now = std::chrono::steady_clock::now();
    if (now < m_nextBlobSessionLeaseHeartbeat)
        return;

    const auto touched = TouchBlobSessionLease(m_blobSessionLease);
    m_nextBlobSessionLeaseHeartbeat = now +
        (touched.Succeeded() ? kBlobSessionLeaseHeartbeatInterval : kBlobSessionLeaseRetryInterval);
}

UndoBlobStoreResult EditorUndoService::ConfigureBlobStore(std::filesystem::path root)
{
    if (root.empty())
    {
        return {
            UndoBlobStoreStatus::InvalidArgument,
            "The Undo blob store root is empty."};
    }
    if (m_pending || m_dispatchingToken != 0u || m_activeEdit || !m_transactions.empty() || !m_history.empty())
    {
        return {
            UndoBlobStoreStatus::InvalidArgument,
            "Undo blob storage cannot be reconfigured while history is active."};
    }

    // Blob reference counts are process-local because history is not persisted.
    // A private session root prevents one Editor instance from cleaning another
    // instance's live snapshots when they open the same project concurrently.
    const auto parentRoot = root.lexically_normal();
    auto sessionId = NLS::Guid::New().ToString();
    sessionId.erase(std::remove(sessionId.begin(), sessionId.end(), '-'), sessionId.end());
    // Keep the session marker compact: blob publishing uses a 64-character
    // content hash plus a staging suffix and must remain below Windows'
    // legacy path limit even for deeply nested project roots.
    if (sessionId.size() > 16u)
        sessionId.resize(16u);
    const auto sessionRoot = parentRoot / (std::string(kBlobSessionPrefix) + sessionId);
    auto store = std::make_shared<UndoBlobStore>(UndoBlobStoreOptions {sessionRoot});
    const auto initialized = store->Initialize();
    if (!initialized.Succeeded())
        return initialized;

    const auto leasePath = store->GetRoot() / kBlobSessionLeaseName;
    const auto lease = TouchBlobSessionLease(leasePath);
    if (!lease.Succeeded())
        return lease;

    // Only session directories carrying an old regular lease are candidates.
    // Missing/unknown markers are deliberately retained for older editors.
    CleanupStaleBlobSessionDirectories(parentRoot, store->GetRoot());

    // A session starts without retained commands. This only clears an unlikely
    // collision/reuse residue inside the newly owned root.
    const auto cleanup = store->CleanupUnreferenced();
    if (!cleanup.Succeeded())
        return {cleanup.status, cleanup.message};

    m_blobStore = std::move(store);
    m_blobSessionLease = leasePath;
    m_nextBlobSessionLeaseHeartbeat =
        std::chrono::steady_clock::now() + kBlobSessionLeaseHeartbeatInterval;
    return {};
}

std::shared_ptr<UndoBlobStore> EditorUndoService::GetSnapshotBlobStore() const
{
    return m_blobStore;
}

void EditorUndoService::MarkSaved(const UndoScopeId& scope, std::string contentFingerprint)
{
    if (!scope.IsValid())
        return;
    // A preview, open transaction, or pending command has no stable durable
    // state to save. This API is intentionally void, so reject by no-op.
    if (m_activeEdit || !m_transactions.empty() || IsPending())
        return;

    auto& state = m_scopeStates[scope];
    state.savedRevision = state.currentRevision;
    state.savedFingerprint = std::move(contentFingerprint);
    Emit(UndoHistoryEventKind::HistoryChanged);
}

void EditorUndoService::InvalidateScope(const UndoScopeId& scope, const UndoInvalidationReason)
{
    if (!scope.IsValid())
        return;

    std::unordered_set<UndoScopeId, UndoScopeIdHash> connected;
    connected.insert(scope);

    // Scope links are transitive: a transaction touching Scene A and Asset B
    // couples later Asset B/Material C commands to that invalidated chain.
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (const auto& entry : m_history)
        {
            if (ContainsScope(entry.info.scopes, connected))
                AddScopes(entry.info.scopes, connected, changed);
        }
        for (const auto& transaction : m_transactions)
        {
            if (ContainsScope(transaction.description.scopes, connected))
                AddScopes(transaction.description.scopes, connected, changed);
            for (const auto& entry : transaction.entries)
            {
                if (ContainsScope(entry.info.scopes, connected))
                    AddScopes(entry.info.scopes, connected, changed);
            }
        }
        if (m_pending && ContainsScope(m_pending->info.scopes, connected))
            AddScopes(m_pending->info.scopes, connected, changed);
        if (m_activeEdit && ContainsScope(m_activeEdit->info.scopes, connected))
            AddScopes(m_activeEdit->info.scopes, connected, changed);
    }

    for (size_t index = m_history.size(); index > 0u; --index)
    {
        if (ContainsScope(m_history[index - 1u].info.scopes, connected))
            RemoveHistoryEntry(index - 1u);
    }

    if (std::any_of(m_transactions.begin(), m_transactions.end(), [&connected](const TransactionFrame& frame)
        {
            if (ContainsScope(frame.description.scopes, connected))
                return true;
            return std::any_of(frame.entries.begin(), frame.entries.end(), [&connected](const HistoryEntry& entry)
            {
                return ContainsScope(entry.info.scopes, connected);
            });
        }))
    {
        // Previewed edits in an invalid scope cannot be safely replayed. They
        // are already reflected in the external state, so establish a fresh
        // baseline instead of attempting an unsafe rollback.
        m_transactions.clear();
    }

    if (m_pending && ContainsScope(m_pending->info.scopes, connected))
    {
        m_pending->invalidated = true;
        if (m_pending->validity)
            m_pending->validity->Invalidate();
    }
    if (m_activeEdit && ContainsScope(m_activeEdit->info.scopes, connected))
        m_activeEdit.reset();

    std::vector<UndoScopeId> invalidated;
    invalidated.reserve(connected.size());
    for (const auto& connectedScope : connected)
    {
        m_scopeStates[connectedScope] = {};
        invalidated.push_back(connectedScope);
    }

    Emit(UndoHistoryEventKind::ScopeInvalidated, std::nullopt, std::move(invalidated));
    Emit(UndoHistoryEventKind::HistoryChanged);
}

void EditorUndoService::Freeze(const UndoFreezeReason reason)
{
    ++m_freezeCounts[reason];
    Emit(UndoHistoryEventKind::HistoryChanged);
}

void EditorUndoService::Unfreeze(const UndoFreezeReason reason)
{
    const auto iterator = m_freezeCounts.find(reason);
    if (iterator == m_freezeCounts.end())
        return;
    if (iterator->second > 1u)
        --iterator->second;
    else
        m_freezeCounts.erase(iterator);
    Emit(UndoHistoryEventKind::HistoryChanged);
}

void EditorUndoService::ClearAll(const UndoClearReason)
{
    const auto blobStore = m_blobStore;
    if (m_pending && m_pending->validity)
        m_pending->validity->Invalidate();
    m_pending.reset();
    m_dispatchingToken = 0u;
    m_deferredCompletion.reset();
    {
        std::lock_guard lock(m_queuedCompletionMutex);
        m_queuedCompletions.clear();
    }
    m_history.clear();
    m_cursor = 0u;
    m_retainedMemoryBytes = 0u;
    m_retainedBlobBytes = 0u;
    m_transactions.clear();
    m_activeEdit.reset();
    m_scopeStates.clear();
    m_nextScopeRevision = 1u;
    m_poisoned = false;
    m_poisonMessage.clear();
    if (blobStore)
        static_cast<void>(blobStore->CleanupUnreferenced());
    Emit(UndoHistoryEventKind::Cleared);
    Emit(UndoHistoryEventKind::HistoryChanged);
}

bool EditorUndoService::CanUndo() const
{
    if (m_activeEdit)
        return !IsPending() && !IsFrozen() && !IsPoisoned();
    return m_cursor > 0u && m_transactions.empty() && !IsPending() && !IsFrozen() && !IsPoisoned();
}

bool EditorUndoService::CanRedo() const
{
    return m_cursor < m_history.size() && m_transactions.empty() && !m_activeEdit &&
        !IsPending() && !IsFrozen() && !IsPoisoned();
}

bool EditorUndoService::IsPending() const
{
    return static_cast<bool>(m_pending) || m_dispatchingToken != 0u;
}

bool EditorUndoService::HasActiveEditSession() const
{
    return static_cast<bool>(m_activeEdit);
}

bool EditorUndoService::IsEditSessionActive(const UndoEditSession& session) const noexcept
{
    return session.IsValid() && m_activeEdit != nullptr && m_activeEdit->id == session.m_id;
}

bool EditorUndoService::IsFrozen() const
{
    return !m_freezeCounts.empty();
}

bool EditorUndoService::IsPoisoned() const
{
    return m_poisoned;
}

const std::string& EditorUndoService::GetPoisonMessage() const
{
    return m_poisonMessage;
}

std::string EditorUndoService::GetUndoLabel() const
{
    return m_cursor == 0u ? std::string {} : m_history[m_cursor - 1u].info.label;
}

std::string EditorUndoService::GetRedoLabel() const
{
    return m_cursor >= m_history.size() ? std::string {} : m_history[m_cursor].info.label;
}

std::string EditorUndoService::GetUndoUnavailableReason() const
{
    if (IsPending())
        return "An asynchronous editor operation is still in progress.";
    if (IsPoisoned())
    {
        return m_poisonMessage.empty()
            ? "Undo history requires recovery before it can continue."
            : "Undo history requires recovery: " + m_poisonMessage;
    }
    if (IsFrozen())
    {
        return m_freezeCounts.find(UndoFreezeReason::PlayMode) != m_freezeCounts.end()
            ? "Undo is unavailable while Play mode is active."
            : "Undo is temporarily frozen by an editor operation.";
    }
    if (m_activeEdit)
        return {};
    if (!m_transactions.empty())
        return "Finish or cancel the active editor transaction first.";
    if (m_cursor == 0u)
        return "There is no command to undo.";
    return {};
}

std::string EditorUndoService::GetRedoUnavailableReason() const
{
    if (IsPending())
        return "An asynchronous editor operation is still in progress.";
    if (IsPoisoned())
    {
        return m_poisonMessage.empty()
            ? "Undo history requires recovery before it can continue."
            : "Undo history requires recovery: " + m_poisonMessage;
    }
    if (IsFrozen())
    {
        return m_freezeCounts.find(UndoFreezeReason::PlayMode) != m_freezeCounts.end()
            ? "Redo is unavailable while Play mode is active."
            : "Redo is temporarily frozen by an editor operation.";
    }
    if (m_activeEdit)
        return "Finish or cancel the active editor edit first.";
    if (!m_transactions.empty())
        return "Finish or cancel the active editor transaction first.";
    if (m_cursor >= m_history.size())
        return "There is no command to redo.";
    return {};
}

UndoHistoryState EditorUndoService::GetHistoryState() const
{
    UndoHistoryState state;
    state.transactionCount = m_history.size();
    state.cursor = m_cursor;
    state.retainedMemoryBytes = m_retainedMemoryBytes;
    state.retainedBlobBytes = m_retainedBlobBytes;
    state.canUndo = CanUndo();
    state.canRedo = CanRedo();
    state.pending = IsPending();
    state.frozen = IsFrozen();
    state.poisoned = IsPoisoned();
    state.undoLabel = GetUndoLabel();
    state.redoLabel = GetRedoLabel();
    state.poisonMessage = m_poisonMessage;
    return state;
}

UndoScopeState EditorUndoService::GetScopeState(const UndoScopeId& scope) const
{
    const auto iterator = m_scopeStates.find(scope);
    return iterator == m_scopeStates.end() ? UndoScopeState {} : iterator->second;
}

size_t EditorUndoService::GetTransactionCount() const noexcept
{
    return m_history.size();
}

size_t EditorUndoService::GetCursor() const noexcept
{
    return m_cursor;
}

UndoListenerToken EditorUndoService::SubscribeHistoryChanged(UndoHistoryListener listener)
{
    if (!listener)
        return 0u;
    const auto token = m_nextListenerToken++;
    m_listeners.emplace(token, std::move(listener));
    return token;
}

void EditorUndoService::UnsubscribeHistoryChanged(const UndoListenerToken token)
{
    m_listeners.erase(token);
}

UndoRequestResult EditorUndoService::BeginApply(
    UndoCommandPtr command,
    UndoCommandInfo info,
    const UndoDirection direction,
    const uint64_t transactionId)
{
    if (!command)
        return {UndoRequestState::Failed, "Cannot execute a null undo command."};

    const UndoPendingToken token = m_nextPendingToken++;
    const auto validity = std::make_shared<UndoOperationValidity>();
    const UndoExecutionContext context {info.id, direction, token, info.scopes, validity};
    const auto validation = command->Validate(direction, context);
    if (validation.state != UndoValidationState::Ready)
    {
        auto state = validation.state == UndoValidationState::Stale
            ? UndoRequestState::Stale
            : validation.state == UndoValidationState::Conflict
                ? UndoRequestState::Conflict
                : UndoRequestState::Failed;
        std::string message = validation.message;
        if (transactionId != 0u)
        {
            std::vector<UndoScopeId> affectedScopes = info.scopes;
            std::string rollbackError;
            const bool rolledBack = RollbackAndDiscardOpenTransactions(rollbackError, &affectedScopes);
            info.scopes = affectedScopes;
            if (!rolledBack)
            {
                RebaselineScopesAsDirty(affectedScopes);
                state = UndoRequestState::Poisoned;
            }
            if (!rollbackError.empty())
            {
                if (!message.empty())
                    message += " ";
                message += "Transaction rollback: " + rollbackError;
            }
            Emit(UndoHistoryEventKind::HistoryChanged);
        }
        Emit(UndoHistoryEventKind::CommandFailed, info, info.scopes, direction, message);
        return {state, std::move(message)};
    }

    PendingOperation operation;
    operation.kind = PendingOperation::Kind::Execute;
    operation.direction = direction;
    operation.token = token;
    operation.transactionId = transactionId;
    operation.command = std::move(command);
    operation.info = std::move(info);
    operation.validity = validity;

    m_dispatchingToken = token;
    m_deferredCompletion.reset();
    const std::weak_ptr<LifetimeState> lifetime = m_lifetime;
    const ScopedEditorUndoMutationContext mutationContext(context);
    const auto start = operation.command->Apply(
        direction,
        context,
        [lifetime, token](UndoCompletionResult completion)
        {
            if (const auto locked = lifetime.lock())
            {
                std::lock_guard ownerLock(locked->mutex);
                if (locked->owner)
                    locked->owner->DispatchCommandCompletion(token, std::move(completion));
            }
        });
    m_dispatchingToken = 0u;

    if (start.state == UndoStartState::Completed)
    {
        m_deferredCompletion.reset();
        const auto result = FinalizeCompletedApply(operation);
        validity->Invalidate();
        return result;
    }
    if (start.state == UndoStartState::Failed)
    {
        m_deferredCompletion.reset();
        std::string message = start.message;
        bool rollbackFailed = start.rollbackFailed;
        if (transactionId != 0u)
        {
            std::vector<UndoScopeId> affectedScopes = operation.info.scopes;
            std::string rollbackError;
            const bool rolledBack = RollbackAndDiscardOpenTransactions(rollbackError, &affectedScopes);
            rollbackFailed = rollbackFailed || !rolledBack;
            operation.info.scopes = affectedScopes;
            if (rollbackFailed)
                RebaselineScopesAsDirty(affectedScopes);
            if (!rollbackError.empty())
            {
                if (!message.empty())
                    message += " ";
                message += "Transaction rollback: " + rollbackError;
            }
        }
        const auto result = FinalizeFailedApply(operation, std::move(message), rollbackFailed);
        validity->Invalidate();
        return result;
    }

    operation.pendingTransactionChild = transactionId != 0u;
    m_pending = std::make_unique<PendingOperation>(std::move(operation));
    Emit(UndoHistoryEventKind::PendingStarted, m_pending->info, m_pending->info.scopes, direction);
    if (m_deferredCompletion)
    {
        auto completion = std::move(*m_deferredCompletion);
        m_deferredCompletion.reset();
        return CompletePending(token, std::move(completion));
    }
    return {UndoRequestState::Pending, {}, token};
}

UndoRequestResult EditorUndoService::BeginHistoryApply(const size_t index, const UndoDirection direction)
{
    if (index >= m_history.size() || !m_history[index].command)
        return {UndoRequestState::Empty, "The history command no longer exists."};

    auto& entry = m_history[index];
    const UndoPendingToken token = m_nextPendingToken++;
    const auto validity = std::make_shared<UndoOperationValidity>();
    const UndoExecutionContext context {entry.info.id, direction, token, entry.info.scopes, validity};
    const auto validation = entry.command->Validate(direction, context);
    if (validation.state == UndoValidationState::Stale)
    {
        const auto info = entry.info;
        const bool entryWasApplied = index < m_cursor;
        RemoveHistoryEntry(index);
        if (entryWasApplied)
            RebaselineScopesAsDirty(info.scopes);
        Emit(UndoHistoryEventKind::CommandFailed, info, info.scopes, direction, validation.message);
        Emit(UndoHistoryEventKind::HistoryChanged);
        return {UndoRequestState::Stale, validation.message};
    }
    if (validation.state != UndoValidationState::Ready)
    {
        const auto state = validation.state == UndoValidationState::Conflict
            ? UndoRequestState::Conflict
            : UndoRequestState::Failed;
        Emit(UndoHistoryEventKind::CommandFailed, entry.info, entry.info.scopes, direction, validation.message);
        return {state, validation.message};
    }

    PendingOperation operation;
    operation.kind = direction == UndoDirection::Undo
        ? PendingOperation::Kind::Undo
        : PendingOperation::Kind::Redo;
    operation.direction = direction;
    operation.token = token;
    operation.historyIndex = index;
    operation.historyCommandId = entry.info.id;
    operation.info = entry.info;
    operation.validity = validity;

    m_dispatchingToken = token;
    m_deferredCompletion.reset();
    const std::weak_ptr<LifetimeState> lifetime = m_lifetime;
    const ScopedEditorUndoMutationContext mutationContext(context);
    const auto start = entry.command->Apply(
        direction,
        context,
        [lifetime, token](UndoCompletionResult completion)
        {
            if (const auto locked = lifetime.lock())
            {
                std::lock_guard ownerLock(locked->mutex);
                if (locked->owner)
                    locked->owner->DispatchCommandCompletion(token, std::move(completion));
            }
        });
    m_dispatchingToken = 0u;

    if (start.state == UndoStartState::Completed)
    {
        m_deferredCompletion.reset();
        const auto result = FinalizeCompletedApply(operation);
        validity->Invalidate();
        return result;
    }
    if (start.state == UndoStartState::Failed)
    {
        m_deferredCompletion.reset();
        const auto result = FinalizeFailedApply(operation, start.message, start.rollbackFailed);
        validity->Invalidate();
        return result;
    }

    m_pending = std::make_unique<PendingOperation>(std::move(operation));
    Emit(UndoHistoryEventKind::PendingStarted, m_pending->info, m_pending->info.scopes, direction);
    if (m_deferredCompletion)
    {
        auto completion = std::move(*m_deferredCompletion);
        m_deferredCompletion.reset();
        return CompletePending(token, std::move(completion));
    }
    return {UndoRequestState::Pending, {}, token};
}

UndoRequestResult EditorUndoService::FinalizeCompletedApply(PendingOperation& operation)
{
    if (operation.kind == PendingOperation::Kind::Execute)
    {
        if (operation.pendingTransactionChild)
        {
            std::vector<UndoScopeId> affectedScopes = operation.info.scopes;
            std::string rollbackError;

            if (!operation.command)
            {
                rollbackError = "The asynchronous transaction child no longer exists.";
            }
            else
            {
                static_cast<void>(RollbackCommand(
                    *operation.command,
                    operation.info,
                    operation.token,
                    operation.validity,
                    rollbackError));
            }

            static_cast<void>(RollbackAndDiscardOpenTransactions(rollbackError, &affectedScopes));
            operation.info.scopes = affectedScopes;
            RebaselineScopesAsDirty(affectedScopes);

            std::string message =
                "An asynchronous transaction child completed after execution began; the transaction cannot be made atomic.";
            if (!rollbackError.empty())
                message += " Transaction rollback: " + rollbackError;
            return FinalizeFailedApply(
                operation,
                std::move(message),
                true);
        }
        if (operation.transactionId != 0u)
        {
            const auto transaction = std::find_if(
                m_transactions.begin(),
                m_transactions.end(),
                [&operation](const TransactionFrame& frame) { return frame.id == operation.transactionId; });
            if (transaction == m_transactions.end())
                return {UndoRequestState::Stale, "The transaction was invalidated before the command completed."};
        }

        if (operation.transactionId == 0u)
            RecordCompletedCommand(std::move(operation.command), operation.info);
        else
            AppendToActiveTransaction(std::move(operation.command), operation.info, operation.transactionId);
        return {};
    }

    if (operation.historyIndex >= m_history.size() ||
        m_history[operation.historyIndex].info.id != operation.historyCommandId)
    {
        return {UndoRequestState::Stale, "The history command was invalidated before it completed."};
    }

    if (operation.kind == PendingOperation::Kind::Undo)
    {
        if (m_cursor == 0u || operation.historyIndex != m_cursor - 1u)
            return {UndoRequestState::Conflict, "The undo cursor changed while the command was pending."};
        --m_cursor;
        ApplyScopeRevisionTransition(m_history[operation.historyIndex], UndoDirection::Undo);
    }
    else
    {
        if (operation.historyIndex != m_cursor)
            return {UndoRequestState::Conflict, "The redo cursor changed while the command was pending."};
        ++m_cursor;
        ApplyScopeRevisionTransition(m_history[operation.historyIndex], UndoDirection::Redo);
    }

    MarkWriteThroughScopesSaved(m_history[operation.historyIndex].info);

    Emit(UndoHistoryEventKind::CommandApplied, operation.info, operation.info.scopes, operation.direction);
    Emit(UndoHistoryEventKind::HistoryChanged);
    return {};
}

bool EditorUndoService::RollbackAndDiscardOpenTransactions(
    std::string& error,
    std::vector<UndoScopeId>* affectedScopes)
{
    std::vector<UndoScopeId> scopes;
    for (const auto& frame : m_transactions)
    {
        scopes.insert(scopes.end(), frame.description.scopes.begin(), frame.description.scopes.end());
        for (const auto& entry : frame.entries)
            scopes.insert(scopes.end(), entry.info.scopes.begin(), entry.info.scopes.end());
    }

    bool rollbackFailed = false;
    for (auto frameIterator = m_transactions.rbegin(); frameIterator != m_transactions.rend(); ++frameIterator)
    {
        auto& frame = *frameIterator;
        for (auto entryIterator = frame.entries.rbegin(); entryIterator != frame.entries.rend(); ++entryIterator)
        {
            auto& entry = *entryIterator;
            if (!entry.command)
            {
                rollbackFailed = true;
                if (error.empty())
                    error = "An active transaction child no longer exists.";
                continue;
            }

            if (!RollbackCommand(*entry.command, entry.info, 0u, {}, error))
                rollbackFailed = true;
        }
    }

    m_transactions.clear();
    if (affectedScopes != nullptr)
    {
        affectedScopes->insert(affectedScopes->end(), scopes.begin(), scopes.end());
        *affectedScopes = UniqueScopes(std::move(*affectedScopes));
    }
    if (rollbackFailed && error.empty())
        error = "One or more active transaction children could not be rolled back.";
    return !rollbackFailed;
}

UndoRequestResult EditorUndoService::FinalizeFailedApply(
    const PendingOperation& operation,
    std::string message,
    const bool rollbackFailed)
{
    if (message.empty())
        message = "The undo command failed.";
    if (rollbackFailed)
    {
        m_poisoned = true;
        m_poisonMessage = message;
    }
    Emit(UndoHistoryEventKind::CommandFailed, operation.info, operation.info.scopes, operation.direction, message);
    Emit(UndoHistoryEventKind::HistoryChanged);
    return {rollbackFailed ? UndoRequestState::Poisoned : UndoRequestState::Failed, std::move(message)};
}

void EditorUndoService::RecordCompletedCommand(
    UndoCommandPtr command,
    UndoCommandInfo info,
    const std::optional<std::chrono::steady_clock::time_point> mergeStartedAt)
{
    if (!command)
        return;
    info = NormalizeInfo(std::move(info));
    AssignCommandId(info);

    const auto now = std::chrono::steady_clock::now();
    const auto mergeCandidateAt = mergeStartedAt.value_or(now);
    if (m_cursor == m_history.size() && !m_history.empty())
    {
        auto& previous = m_history.back();
        // History remains ordered by completion time. Merge eligibility instead
        // uses the user interaction's start, so snapshot construction does not
        // consume the keyboard-field merge window.
        const bool withinWindow = mergeCandidateAt >= previous.completedAt &&
            mergeCandidateAt - previous.completedAt <= m_budget.mergeWindow;
        if (withinWindow && SameMergeIdentity(previous.info, info) && previous.command &&
            previous.command->TryMergeFrom(*command))
        {
            const auto previousMemory = previous.info.estimatedBytes;
            const auto previousBlob = previous.info.estimatedBlobBytes;
            // A command is allowed to normalize or compact its retained
            // snapshots while merging. Its own metadata is therefore the
            // authority for the resulting memory/blob budget, rather than the
            // metadata supplied by the transient newer command.
            auto mergedInfo = NormalizeInfo(previous.command->GetInfo());
            mergedInfo.id = previous.info.id;
            previous.info = std::move(mergedInfo);
            previous.completedAt = now;
            AssignScopeRevisionTransitions(previous);
            MarkWriteThroughScopesSaved(previous.info);
            m_retainedMemoryBytes -= previousMemory;
            m_retainedBlobBytes -= previousBlob;
            m_retainedMemoryBytes = SaturatingAdd(m_retainedMemoryBytes, previous.info.estimatedBytes);
            m_retainedBlobBytes = SaturatingAdd(m_retainedBlobBytes, previous.info.estimatedBlobBytes);
            // Destroy the superseded command before cleaning its out-of-line
            // snapshots. A merge must not leave zero-reference blobs behind
            // until some unrelated history operation happens later.
            command.reset();
            if (m_blobStore)
                static_cast<void>(m_blobStore->CleanupUnreferenced());
            EnforceBudget();
            Emit(UndoHistoryEventKind::CommandApplied, previous.info, previous.info.scopes, UndoDirection::Redo);
            Emit(UndoHistoryEventKind::HistoryChanged);
            return;
        }
    }

    DiscardRedo();
    m_retainedMemoryBytes = SaturatingAdd(m_retainedMemoryBytes, info.estimatedBytes);
    m_retainedBlobBytes = SaturatingAdd(m_retainedBlobBytes, info.estimatedBlobBytes);
    m_history.push_back({std::move(command), std::move(info), now});
    AssignScopeRevisionTransitions(m_history.back());
    m_cursor = m_history.size();
    MarkWriteThroughScopesSaved(m_history.back().info);
    EnforceBudget();
    Emit(UndoHistoryEventKind::CommandApplied, m_history.back().info, m_history.back().info.scopes, UndoDirection::Redo);
    Emit(UndoHistoryEventKind::HistoryChanged);
}

void EditorUndoService::AppendToActiveTransaction(
    UndoCommandPtr command,
    UndoCommandInfo info,
    const uint64_t transactionId)
{
    if (!command)
        return;
    const auto transaction = std::find_if(
        m_transactions.begin(),
        m_transactions.end(),
        [transactionId](const TransactionFrame& frame) { return frame.id == transactionId; });
    if (transaction == m_transactions.end())
        return;
    transaction->entries.push_back({std::move(command), std::move(info), std::chrono::steady_clock::now()});
    Emit(UndoHistoryEventKind::HistoryChanged);
}

void EditorUndoService::AssignScopeRevisionTransitions(HistoryEntry& entry)
{
    std::vector<HistoryEntry::ScopeRevisionTransition> previousTransitions = std::move(entry.scopeRevisions);
    entry.scopeRevisions.clear();

    for (const auto& scope : UniqueScopes(entry.info.scopes))
    {
        auto& state = m_scopeStates[scope];
        uint64_t beforeRevision = state.currentRevision;
        const auto previous = std::find_if(
            previousTransitions.begin(),
            previousTransitions.end(),
            [&scope](const HistoryEntry::ScopeRevisionTransition& transition)
            {
                return transition.scope == scope;
            });
        if (previous != previousTransitions.end())
            beforeRevision = previous->beforeRevision;

        const uint64_t afterRevision = m_nextScopeRevision++;
        entry.scopeRevisions.push_back({scope, beforeRevision, afterRevision});
        state.currentRevision = afterRevision;
    }
}

void EditorUndoService::ApplyScopeRevisionTransition(
    const HistoryEntry& entry,
    const UndoDirection direction)
{
    for (const auto& transition : entry.scopeRevisions)
    {
        if (!transition.scope.IsValid())
            continue;
        m_scopeStates[transition.scope].currentRevision = direction == UndoDirection::Undo
            ? transition.beforeRevision
            : transition.afterRevision;
    }
}

void EditorUndoService::MarkWriteThroughScopesSaved(const UndoCommandInfo& info)
{
    for (const auto& scope : info.writeThroughScopes)
    {
        if (!scope.IsValid())
            continue;

        auto& state = m_scopeStates[scope];
        state.savedRevision = state.currentRevision;
        state.savedFingerprint.clear();
    }
}

void EditorUndoService::AssignCommandId(UndoCommandInfo& info)
{
    if (info.id == 0u)
        info.id = m_nextCommandId++;
    else if (info.id >= m_nextCommandId)
        m_nextCommandId = info.id + 1u;
}

void EditorUndoService::EnforceBudget()
{
    const auto exceeds = [this]
    {
        const bool entryExceeded = m_budget.maxTransactions != 0u && m_history.size() > m_budget.maxTransactions;
        const bool memoryExceeded = m_budget.maxMemoryBytes != 0u && m_retainedMemoryBytes > m_budget.maxMemoryBytes;
        const bool blobExceeded = m_budget.maxBlobBytes != 0u && m_retainedBlobBytes > m_budget.maxBlobBytes;
        return entryExceeded || memoryExceeded || blobExceeded;
    };

    // A single large command remains useful. Retain it alone instead of
    // rejecting a valid edit merely because its snapshot exceeds the budget.
    while (m_history.size() > 1u && exceeds())
        RemoveHistoryEntry(0u);
}

void EditorUndoService::DiscardRedo()
{
    bool removed = false;
    while (m_history.size() > m_cursor)
    {
        m_retainedMemoryBytes -= m_history.back().info.estimatedBytes;
        m_retainedBlobBytes -= m_history.back().info.estimatedBlobBytes;
        m_history.pop_back();
        removed = true;
    }
    if (removed && m_blobStore)
        static_cast<void>(m_blobStore->CleanupUnreferenced());
}

void EditorUndoService::RemoveHistoryEntry(const size_t index)
{
    if (index >= m_history.size())
        return;
    m_retainedMemoryBytes -= m_history[index].info.estimatedBytes;
    m_retainedBlobBytes -= m_history[index].info.estimatedBlobBytes;
    if (index < m_cursor)
        --m_cursor;
    m_history.erase(m_history.begin() + static_cast<std::ptrdiff_t>(index));
    if (m_blobStore)
        static_cast<void>(m_blobStore->CleanupUnreferenced());
}

void EditorUndoService::RebaselineScopesAsDirty(const std::vector<UndoScopeId>& scopes)
{
    for (const auto& scope : UniqueScopes(scopes))
    {
        auto& state = m_scopeStates[scope];
        state.currentRevision = m_nextScopeRevision++;
        state.savedFingerprint.clear();
    }
}

void EditorUndoService::Emit(
    const UndoHistoryEventKind kind,
    std::optional<UndoCommandInfo> command,
    std::vector<UndoScopeId> scopes,
    const UndoDirection direction,
    std::string message)
{
    if (m_listeners.empty())
        return;

    UndoHistoryEvent event;
    event.kind = kind;
    event.state = GetHistoryState();
    event.command = std::move(command);
    event.scopes = std::move(scopes);
    event.direction = direction;
    event.message = std::move(message);

    std::vector<UndoHistoryListener> listeners;
    listeners.reserve(m_listeners.size());
    for (const auto& [_, listener] : m_listeners)
        listeners.push_back(listener);
    for (const auto& listener : listeners)
        listener(event);
}

UndoRequestResult EditorUndoService::GetMutationAvailability() const
{
    if (m_pending || m_dispatchingToken != 0u)
        return {UndoRequestState::Busy, "An undo command is still pending."};
    if (m_poisoned)
        return {UndoRequestState::Poisoned, m_poisonMessage};
    if (IsFrozen())
        return {UndoRequestState::Frozen, "Undo history is frozen."};
    return {};
}

void EditorUndoService::DispatchCommandCompletion(
    const UndoPendingToken token,
    UndoCompletionResult completion)
{
    if (std::this_thread::get_id() == m_ownerThread)
    {
        static_cast<void>(CompletePending(token, std::move(completion)));
        return;
    }

    std::lock_guard lock(m_queuedCompletionMutex);
    m_queuedCompletions.emplace_back(token, std::move(completion));
}

UndoRequestResult EditorUndoService::CancelActiveEditSession()
{
    if (!m_activeEdit || !m_activeEdit->edit)
        return {UndoRequestState::InvalidTransaction, "The edit session is no longer active."};
    if (m_pending || m_dispatchingToken != 0u)
        return {UndoRequestState::Busy, "An undo command is still pending."};
    if (m_poisoned)
        return {UndoRequestState::Poisoned, m_poisonMessage};

    const UndoExecutionContext context {
        m_activeEdit->info.id,
        UndoDirection::Undo,
        0u,
        m_activeEdit->info.scopes};
    const auto validation = m_activeEdit->edit->Validate(context);
    if (validation.state == UndoValidationState::Stale)
    {
        const auto info = m_activeEdit->info;
        m_activeEdit.reset();
        Emit(UndoHistoryEventKind::CommandFailed, info, info.scopes, UndoDirection::Undo, validation.message);
        Emit(UndoHistoryEventKind::HistoryChanged);
        return {UndoRequestState::Stale, validation.message};
    }
    if (validation.state != UndoValidationState::Ready)
    {
        const auto state = validation.state == UndoValidationState::Conflict
            ? UndoRequestState::Conflict
            : UndoRequestState::Failed;
        Emit(UndoHistoryEventKind::CommandFailed, m_activeEdit->info, m_activeEdit->info.scopes, UndoDirection::Undo, validation.message);
        return {state, validation.message};
    }

    const ScopedEditorUndoMutationContext mutationContext(context);
    const auto result = m_activeEdit->edit->Cancel(context, {});
    if (result.state == UndoStartState::Completed)
    {
        m_activeEdit.reset();
        Emit(UndoHistoryEventKind::HistoryChanged);
        return {};
    }
    if (result.state == UndoStartState::Pending)
        return {UndoRequestState::Failed, "Continuous edit cancellation must complete synchronously."};
    if (result.rollbackFailed)
    {
        m_poisoned = true;
        m_poisonMessage = result.message;
    }
    Emit(UndoHistoryEventKind::CommandFailed, m_activeEdit->info, m_activeEdit->info.scopes, UndoDirection::Undo, result.message);
    return {result.rollbackFailed ? UndoRequestState::Poisoned : UndoRequestState::Failed, result.message};
}

bool EditorUndoService::SameMergeIdentity(
    const UndoCommandInfo& previous,
    const UndoCommandInfo& newer)
{
    return previous.allowMerge && newer.allowMerge && !previous.mergeKey.empty() &&
        previous.mergeKey == newer.mergeKey && previous.sourceModule == newer.sourceModule &&
        previous.scopes == newer.scopes && previous.affected == newer.affected;
}

std::vector<UndoScopeId> EditorUndoService::UniqueScopes(std::vector<UndoScopeId> scopes)
{
    std::vector<UndoScopeId> unique;
    unique.reserve(scopes.size());
    for (auto& scope : scopes)
    {
        if (!scope.IsValid())
            continue;
        if (std::find(unique.begin(), unique.end(), scope) == unique.end())
            unique.push_back(std::move(scope));
    }
    return unique;
}

UndoCommandInfo EditorUndoService::NormalizeInfo(UndoCommandInfo info)
{
    info.scopes = UniqueScopes(std::move(info.scopes));
    info.writeThroughScopes = UniqueScopes(std::move(info.writeThroughScopes));
    info.writeThroughScopes.erase(
        std::remove_if(
            info.writeThroughScopes.begin(),
            info.writeThroughScopes.end(),
            [&info](const UndoScopeId& scope)
            {
                return std::find(info.scopes.begin(), info.scopes.end(), scope) == info.scopes.end();
            }),
        info.writeThroughScopes.end());
    if (info.label.empty())
        info.label = "Edit";
    return info;
}
} // namespace NLS::Editor::Undo
