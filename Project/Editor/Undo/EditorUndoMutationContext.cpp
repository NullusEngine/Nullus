#include "Undo/EditorUndoMutationContext.h"

#include <vector>

#if !defined(NDEBUG)
#include <Debug/Assertion.h>
#endif

namespace NLS::Editor::Undo
{
namespace
{
#if !defined(NDEBUG)
thread_local std::vector<UndoExecutionContext> g_mutationContexts;
#endif
} // namespace

ScopedEditorUndoMutationContext::ScopedEditorUndoMutationContext(const UndoExecutionContext& context)
{
#if !defined(NDEBUG)
    g_mutationContexts.push_back(context);
    m_active = true;
#else
    static_cast<void>(context);
#endif
}

ScopedEditorUndoMutationContext::~ScopedEditorUndoMutationContext()
{
#if !defined(NDEBUG)
    if (m_active && !g_mutationContexts.empty())
        g_mutationContexts.pop_back();
#endif
}

ScopedEditorUndoTransientMutationContext::ScopedEditorUndoTransientMutationContext(
    std::vector<UndoScopeId> scopes,
    const UndoDirection direction)
{
#if !defined(NDEBUG)
    UndoExecutionContext context;
    context.commandId = 0u;
    context.direction = direction;
    context.scopes = std::move(scopes);
    g_mutationContexts.push_back(std::move(context));
    m_active = true;
#else
    static_cast<void>(scopes);
    static_cast<void>(direction);
#endif
}

ScopedEditorUndoTransientMutationContext::ScopedEditorUndoTransientMutationContext(
    UndoScopeId scope,
    const UndoDirection direction)
    : ScopedEditorUndoTransientMutationContext(
        std::vector<UndoScopeId> { std::move(scope) },
        direction)
{
}

ScopedEditorUndoTransientMutationContext::~ScopedEditorUndoTransientMutationContext()
{
#if !defined(NDEBUG)
    if (m_active && !g_mutationContexts.empty())
        g_mutationContexts.pop_back();
#endif
}

bool HasActiveEditorUndoMutationContext() noexcept
{
#if !defined(NDEBUG)
    return !g_mutationContexts.empty();
#else
    return false;
#endif
}

std::optional<UndoExecutionContext> GetActiveEditorUndoMutationContext()
{
#if !defined(NDEBUG)
    if (!g_mutationContexts.empty())
        return g_mutationContexts.back();
#endif
    return std::nullopt;
}

void AssertEditorUndoMutationContext(const std::string_view operation)
{
#if !defined(NDEBUG)
    NLS_ASSERT(
        HasActiveEditorUndoMutationContext(),
        "Editor mutation bypassed the global Undo service: " + std::string(operation));
#else
    static_cast<void>(operation);
#endif
}
} // namespace NLS::Editor::Undo
