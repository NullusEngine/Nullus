#pragma once

#include "IScriptBackend.h"

#include "Engine/UI/UIElement.h"

namespace NLS::Scripting
{
    class ScriptRuntime;

    // Bridges a retained UI event to one backend-neutral callable.  The
    // binding owns neither the UI element nor the ScriptRuntime; callers must
    // unbind before either owner is destroyed.
    class NLS_SCRIPTING_API UIScriptEventBinding final
    {
    public:
        UIScriptEventBinding() = default;
        ~UIScriptEventBinding();

        UIScriptEventBinding(const UIScriptEventBinding&) = delete;
        UIScriptEventBinding& operator=(const UIScriptEventBinding&) = delete;

        bool Bind(
            NLS::Engine::UI::UIElement& element,
            NLS::Engine::UI::UIEventType eventType,
            ScriptRuntime& runtime,
            ScriptInstanceHandle instance,
            ScriptMemberId member);
        void Unbind();

        [[nodiscard]] bool IsBound() const { return m_listener.IsValid(); }
        [[nodiscard]] ScriptCallableHandle GetCallable() const { return m_callable; }

    private:
        void OnEvent(NLS::Engine::UI::UIEvent& event);

        NLS::Engine::UI::UIElement* m_element = nullptr;
        ScriptRuntime* m_runtime = nullptr;
        ScriptInstanceHandle m_instance;
        ScriptCallableHandle m_callable;
        NLS::Engine::UI::UIEventListenerHandle m_listener;
    };
}
