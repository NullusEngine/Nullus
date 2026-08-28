#include "UIScriptEventBinding.h"

#include "ScriptRuntime.h"

#include "Math/Vector2.h"

#include <array>

namespace NLS::Scripting
{
    UIScriptEventBinding::~UIScriptEventBinding()
    {
        Unbind();
    }

    bool UIScriptEventBinding::Bind(
        NLS::Engine::UI::UIElement& element,
        const NLS::Engine::UI::UIEventType eventType,
        ScriptRuntime& runtime,
        const ScriptInstanceHandle instance,
        const ScriptMemberId member)
    {
        Unbind();
        ScriptCallableHandle callable;
        if (!runtime.CreateCallable(instance, member, callable).Succeeded())
            return false;

        m_element = &element;
        m_runtime = &runtime;
        m_instance = instance;
        m_callable = callable;
        m_listener = element.AddEventListener(
            eventType,
            [this](NLS::Engine::UI::UIEvent& event) { OnEvent(event); });
        if (!m_listener.IsValid())
        {
            runtime.ReleaseCallable(callable);
            m_element = nullptr;
            m_runtime = nullptr;
            m_instance = {};
            m_callable = {};
            return false;
        }
        return true;
    }

    void UIScriptEventBinding::Unbind()
    {
        if (m_element && m_listener.IsValid())
            m_element->RemoveEventListener(m_listener);
        if (m_runtime && m_callable.IsValid())
            m_runtime->ReleaseCallable(m_callable);
        m_element = nullptr;
        m_runtime = nullptr;
        m_instance = {};
        m_callable = {};
        m_listener = {};
    }

    void UIScriptEventBinding::OnEvent(NLS::Engine::UI::UIEvent& event)
    {
        if (!m_runtime || !m_callable.IsValid() || !m_element)
            return;

        const auto objectHandle = NativeObjectHandle::FromInstanceId(
            static_cast<int32_t>(m_element->GetInstanceID()));
        const NLS::Maths::Vector2 position { event.position.x, event.position.y };
        const NLS::Maths::Vector2 delta { event.delta.x, event.delta.y };
        const std::array<ScriptValue, 7u> arguments {
            objectHandle,
            position,
            delta,
            event.wheel,
            static_cast<uint32_t>(event.keyCode),
            static_cast<uint32_t>(event.modifiers),
            event.text };
        // A failed callable is reported by ScriptRuntime and deliberately does
        // not stop the remaining native UI listeners from running.
        (void)m_runtime->InvokeCallable(m_callable, arguments);
    }
}
