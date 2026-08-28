#pragma once

#include "EngineDef.h"
#include "UIDataContext.h"
#include "UIElement.h"

#include <cstdint>
#include <string>
#include <vector>

namespace NLS::Engine::UI
{
    enum class UIBindingMode : uint8_t
    {
        OneTime,
        OneWay,
        TwoWay
    };

    enum class UIBindingUpdateStrategy : uint8_t
    {
        Changed,
        Commit
    };

    enum class UIBindingProperty : uint8_t
    {
        Text,
        Value,
        Checked,
        Visible,
        Enabled
    };

    struct NLS_ENGINE_API UIBindingHandle
    {
        uint64_t value = 0u;
        [[nodiscard]] bool IsValid() const { return value != 0u; }
    };

    class NLS_ENGINE_API UIBindingEngine final
    {
    public:
        ~UIBindingEngine() { Clear(); }
        UIBindingHandle Bind(
            UIDataContext& context,
            UIElement& element,
            std::string path,
            UIBindingProperty property,
            UIBindingMode mode = UIBindingMode::OneWay,
            UIBindingUpdateStrategy updateStrategy = UIBindingUpdateStrategy::Changed);
        bool Unbind(UIBindingHandle handle);
        void Clear();

    private:
        struct Binding
        {
            UIBindingHandle handle;
            UIDataContext* context = nullptr;
            UIElement* element = nullptr;
            std::string path;
            UIBindingProperty property = UIBindingProperty::Text;
            UIBindingMode mode = UIBindingMode::OneWay;
            UIBindingUpdateStrategy updateStrategy = UIBindingUpdateStrategy::Changed;
            UIDataSubscriptionHandle dataSubscription;
            UIEventListenerHandle eventSubscription;
        };

        static void ApplyValue(UIElement& element, UIBindingProperty property, const UIValue& value);
        static UIValue ReadValue(const UIElement& element, UIBindingProperty property);
        void ApplyInitial(Binding& binding);

        std::vector<Binding> m_bindings;
        uint64_t m_nextHandle = 1u;
    };
}
