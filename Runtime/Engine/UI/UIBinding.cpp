#include "UIBinding.h"

#include <algorithm>

namespace NLS::Engine::UI
{
    namespace
    {
        bool TryGetNumber(const UIValue& value, float& output)
        {
            if (const auto* integer = std::get_if<int64_t>(&value))
            {
                output = static_cast<float>(*integer);
                return true;
            }
            if (const auto* real = std::get_if<double>(&value))
            {
                output = static_cast<float>(*real);
                return true;
            }
            return false;
        }
    }

    UIBindingHandle UIBindingEngine::Bind(
        UIDataContext& context,
        UIElement& element,
        std::string path,
        const UIBindingProperty property,
        const UIBindingMode mode,
        const UIBindingUpdateStrategy updateStrategy)
    {
        if (path.empty())
            return {};

        Binding binding;
        binding.handle = { m_nextHandle++ };
        binding.context = &context;
        binding.element = &element;
        binding.path = std::move(path);
        binding.property = property;
        binding.mode = mode;
        binding.updateStrategy = updateStrategy;

        if (const auto* value = context.Get(binding.path))
            ApplyValue(element, property, *value);

        if (mode != UIBindingMode::OneTime)
        {
            const auto handle = binding.handle;
            binding.dataSubscription = context.Subscribe(
                binding.path,
                [this, handle](const std::string&, const UIValue& value)
                {
                    const auto iterator = std::find_if(
                        m_bindings.begin(), m_bindings.end(),
                        [handle](const Binding& candidate) { return candidate.handle.value == handle.value; });
                    if (iterator != m_bindings.end() && iterator->element)
                        ApplyValue(*iterator->element, iterator->property, value);
                });
        }

        if (mode == UIBindingMode::TwoWay)
        {
            const auto handle = binding.handle;
            const auto eventType = updateStrategy == UIBindingUpdateStrategy::Commit
                ? UIEventType::Submit
                : UIEventType::ValueChanged;
            binding.eventSubscription = element.AddEventListener(
                eventType,
                [this, handle](UIEvent&)
                {
                    const auto iterator = std::find_if(
                        m_bindings.begin(), m_bindings.end(),
                        [handle](const Binding& candidate) { return candidate.handle.value == handle.value; });
                    if (iterator != m_bindings.end() && iterator->context && iterator->element)
                        iterator->context->Set(
                            iterator->path,
                            ReadValue(*iterator->element, iterator->property));
                });
        }

        m_bindings.push_back(std::move(binding));
        return m_bindings.back().handle;
    }

    bool UIBindingEngine::Unbind(const UIBindingHandle handle)
    {
        const auto iterator = std::find_if(
            m_bindings.begin(), m_bindings.end(),
            [handle](const Binding& candidate) { return candidate.handle.value == handle.value; });
        if (iterator == m_bindings.end())
            return false;
        if (iterator->context && iterator->dataSubscription.IsValid())
            iterator->context->Unsubscribe(iterator->dataSubscription);
        if (iterator->element && iterator->eventSubscription.IsValid())
            iterator->element->RemoveEventListener(iterator->eventSubscription);
        m_bindings.erase(iterator);
        return true;
    }

    void UIBindingEngine::Clear()
    {
        while (!m_bindings.empty())
            Unbind(m_bindings.back().handle);
    }

    void UIBindingEngine::ApplyValue(
        UIElement& element,
        const UIBindingProperty property,
        const UIValue& value)
    {
        switch (property)
        {
        case UIBindingProperty::Text:
            if (const auto* text = std::get_if<std::string>(&value))
                element.SetText(*text);
            break;
        case UIBindingProperty::Value:
        {
            float number = 0.0f;
            if (TryGetNumber(value, number))
                element.SetValue(number);
            break;
        }
        case UIBindingProperty::Checked:
            if (const auto* checked = std::get_if<bool>(&value))
                element.SetChecked(*checked);
            break;
        case UIBindingProperty::Visible:
            if (const auto* visible = std::get_if<bool>(&value))
                element.SetVisible(*visible);
            break;
        case UIBindingProperty::Enabled:
            if (const auto* enabled = std::get_if<bool>(&value))
                element.SetEnabled(*enabled);
            break;
        }
    }

    UIValue UIBindingEngine::ReadValue(
        const UIElement& element,
        const UIBindingProperty property)
    {
        switch (property)
        {
        case UIBindingProperty::Text: return element.GetText();
        case UIBindingProperty::Value: return static_cast<double>(element.GetValue());
        case UIBindingProperty::Checked: return element.IsChecked();
        case UIBindingProperty::Visible: return element.GetStyle().visible;
        case UIBindingProperty::Enabled: return element.GetStyle().enabled;
        }
        return {};
    }

    void UIBindingEngine::ApplyInitial(Binding& binding)
    {
        if (binding.context && binding.element)
            if (const auto* value = binding.context->Get(binding.path))
                ApplyValue(*binding.element, binding.property, *value);
    }
}
