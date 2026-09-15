#pragma once

#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include <Object/Object.h>
#include <Serialize/PPtr.h>

#include "Undo/EditorUndoService.h"

namespace NLS::Editor::Undo
{
// A durable address for an editor-owned Object. Commands retain this value
// object only; the current Object pointer is resolved through PersistentManager
// at execution time and is never captured by a command closure.
class ObjectUndoTarget final
{
public:
    ObjectUndoTarget() = default;

    [[nodiscard]] static std::optional<ObjectUndoTarget> Capture(
        NLS::Object& object,
        UndoScopeId scope)
    {
        if (!scope.IsValid() || object.GetInstanceID() == NLS::InstanceID_None)
            return std::nullopt;

        auto& persistentManager = Engine::Serialize::PersistentManager::Instance();
        Engine::Serialize::ObjectIdentifier identifier;
        if (!persistentManager.InstanceIDToObjectIdentifier(object.GetInstanceID(), identifier))
            identifier = persistentManager.GetOrCreateLocalObjectIdentifier(object);

        if (!identifier.IsValid())
            return std::nullopt;

        return ObjectUndoTarget(std::move(scope), std::move(identifier));
    }

    [[nodiscard]] bool IsValid() const noexcept
    {
        return m_scope.IsValid() && m_identifier.IsValid();
    }

    [[nodiscard]] const UndoScopeId& GetScope() const noexcept
    {
        return m_scope;
    }

    [[nodiscard]] const Engine::Serialize::ObjectIdentifier& GetObjectIdentifier() const noexcept
    {
        return m_identifier;
    }

    [[nodiscard]] std::string GetAffectedKey() const
    {
        if (m_identifier.IsLocalObject())
            return "object:" + std::to_string(m_identifier.localIdentifierInFile);

        return "asset-object:" + m_identifier.guid.ToString() + ":" +
            std::to_string(m_identifier.localIdentifierInFile);
    }

    [[nodiscard]] NLS::Object* Resolve() const
    {
        if (!IsValid())
            return nullptr;

        NLS::InstanceID instanceId = NLS::InstanceID_None;
        if (!Engine::Serialize::PersistentManager::Instance().TryGetInstanceID(m_identifier, instanceId))
            return nullptr;

        return NLS::Object::IDToPointer(instanceId);
    }

    template <typename T>
    [[nodiscard]] T* ResolveAs() const
    {
        using CleanType = std::remove_cv_t<T>;
        static_assert(std::is_base_of_v<NLS::Object, CleanType>,
            "Undo targets may only resolve Object-derived types.");
        return dynamic_cast<T*>(Resolve());
    }

    [[nodiscard]] bool Matches(const NLS::Object& object) const
    {
        return Resolve() == &object;
    }

    [[nodiscard]] bool HasSameObjectIdentifier(const ObjectUndoTarget& other) const noexcept
    {
        return m_identifier == other.m_identifier;
    }

    [[nodiscard]] bool operator==(const ObjectUndoTarget& other) const noexcept
    {
        return m_scope == other.m_scope && HasSameObjectIdentifier(other);
    }

    [[nodiscard]] bool operator!=(const ObjectUndoTarget& other) const noexcept
    {
        return !(*this == other);
    }

private:
    ObjectUndoTarget(
        UndoScopeId scope,
        Engine::Serialize::ObjectIdentifier identifier)
        : m_scope(std::move(scope))
        , m_identifier(std::move(identifier))
    {
    }

    UndoScopeId m_scope;
    Engine::Serialize::ObjectIdentifier m_identifier;
};
} // namespace NLS::Editor::Undo
