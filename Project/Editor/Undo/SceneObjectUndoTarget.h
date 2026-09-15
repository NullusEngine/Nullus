#pragma once

#include <optional>
#include <string>
#include <utility>

#include <GameObject.h>
#include "Undo/ObjectUndoTarget.h"

namespace NLS::Editor::Undo
{
// Typed scene-object facade over ObjectUndoTarget. It keeps Scene View's
// scene-membership and editor-transient checks local while sharing the stable
// ObjectIdentifier representation used by hierarchy and component commands.
class SceneObjectUndoTarget final
{
public:
    [[nodiscard]] static std::optional<SceneObjectUndoTarget> Capture(
        Engine::GameObject& gameObject,
        UndoScopeId scope)
    {
        if (!scope.IsValid() || gameObject.GetScene() == nullptr || gameObject.IsEditorTransient())
            return std::nullopt;

        auto objectTarget = ObjectUndoTarget::Capture(gameObject, std::move(scope));
        if (!objectTarget.has_value() ||
            !objectTarget->GetObjectIdentifier().IsLocalObject())
            return std::nullopt;

        return SceneObjectUndoTarget(std::move(*objectTarget));
    }

    [[nodiscard]] bool IsValid() const noexcept
    {
        return m_target.IsValid() && m_target.GetObjectIdentifier().IsLocalObject();
    }

    [[nodiscard]] const UndoScopeId& GetScope() const noexcept
    {
        return m_target.GetScope();
    }

    [[nodiscard]] const Engine::Serialize::ObjectIdentifier& GetObjectIdentifier() const noexcept
    {
        return m_target.GetObjectIdentifier();
    }

    [[nodiscard]] std::string GetAffectedKey() const
    {
        return "scene-object:" + std::to_string(m_target.GetObjectIdentifier().localIdentifierInFile);
    }

    [[nodiscard]] Engine::GameObject* Resolve() const
    {
        if (!IsValid())
            return nullptr;

        auto* gameObject = m_target.ResolveAs<Engine::GameObject>();
        return gameObject != nullptr && gameObject->GetScene() != nullptr && !gameObject->IsEditorTransient()
            ? gameObject
            : nullptr;
    }

    [[nodiscard]] bool Matches(const Engine::GameObject& gameObject) const
    {
        return Resolve() == &gameObject;
    }

private:
    explicit SceneObjectUndoTarget(ObjectUndoTarget target)
        : m_target(std::move(target))
    {
    }

    ObjectUndoTarget m_target;
};
} // namespace NLS::Editor::Undo
