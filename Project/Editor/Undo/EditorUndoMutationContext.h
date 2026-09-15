#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "Undo/EditorUndoService.h"

namespace NLS::Editor::Undo
{
// Editor commands may only mutate durable editor state while the global undo
// service is applying, previewing, or cancelling that mutation. The context is
// thread-local because asynchronous command completions must establish their
// own context on the thread that performs the write.
class ScopedEditorUndoMutationContext final
{
public:
    explicit ScopedEditorUndoMutationContext(const UndoExecutionContext& context);
    ~ScopedEditorUndoMutationContext();

    ScopedEditorUndoMutationContext(const ScopedEditorUndoMutationContext&) = delete;
    ScopedEditorUndoMutationContext& operator=(const ScopedEditorUndoMutationContext&) = delete;
    ScopedEditorUndoMutationContext(ScopedEditorUndoMutationContext&&) = delete;
    ScopedEditorUndoMutationContext& operator=(ScopedEditorUndoMutationContext&&) = delete;

private:
    bool m_active = false;
};

// Transient editor previews (for example Scene View drag proxies) may mutate
// the live scene while they are being displayed, but they must never enter the
// durable Undo timeline. This scope carries the same debug mutation contract as
// a command context while making the non-historical nature explicit. A zero
// command id is intentional and distinguishes these writes from command Apply.
class ScopedEditorUndoTransientMutationContext final
{
public:
    explicit ScopedEditorUndoTransientMutationContext(
        std::vector<UndoScopeId> scopes,
        UndoDirection direction = UndoDirection::Redo);
    explicit ScopedEditorUndoTransientMutationContext(
        UndoScopeId scope,
        UndoDirection direction = UndoDirection::Redo);
    ~ScopedEditorUndoTransientMutationContext();

    ScopedEditorUndoTransientMutationContext(const ScopedEditorUndoTransientMutationContext&) = delete;
    ScopedEditorUndoTransientMutationContext& operator=(const ScopedEditorUndoTransientMutationContext&) = delete;
    ScopedEditorUndoTransientMutationContext(ScopedEditorUndoTransientMutationContext&&) = delete;
    ScopedEditorUndoTransientMutationContext& operator=(ScopedEditorUndoTransientMutationContext&&) = delete;

private:
    bool m_active = false;
};

[[nodiscard]] bool HasActiveEditorUndoMutationContext() noexcept;
[[nodiscard]] std::optional<UndoExecutionContext> GetActiveEditorUndoMutationContext();

// Use this at editor-side scene and filesystem mutation gateways. It is
// intentionally Debug-only: release builds retain no assertion overhead.
void AssertEditorUndoMutationContext(std::string_view operation);
} // namespace NLS::Editor::Undo

#if !defined(NDEBUG)
#define NLS_EDITOR_UNDO_REQUIRE_MUTATION_CONTEXT(operation) \
    ::NLS::Editor::Undo::AssertEditorUndoMutationContext(operation)
#else
#define NLS_EDITOR_UNDO_REQUIRE_MUTATION_CONTEXT(operation) ((void)0)
#endif
