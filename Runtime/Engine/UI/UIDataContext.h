#pragma once

#include "UICommon.h"
#include "EngineDef.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace NLS::Engine::UI
{
    using UIValue = std::variant<bool, int64_t, double, std::string, UIColor, UIVector2>;

    struct UIDataSubscriptionHandle
    {
        uint64_t value = 0u;
        [[nodiscard]] bool IsValid() const { return value != 0u; }
    };

    class NLS_ENGINE_API UIDataContext final
    {
    public:
        using ChangedHandler = std::function<void(const std::string&, const UIValue&)>;

        bool Set(std::string path, UIValue value, bool notify = true);
        bool Remove(const std::string& path);
        [[nodiscard]] const UIValue* Get(const std::string& path) const;
        [[nodiscard]] bool Contains(const std::string& path) const;

        UIDataSubscriptionHandle Subscribe(std::string path, ChangedHandler handler);
        bool Unsubscribe(UIDataSubscriptionHandle handle);
        void Clear();

    private:
        struct Subscription
        {
            UIDataSubscriptionHandle handle;
            std::string path;
            ChangedHandler handler;
        };

        std::unordered_map<std::string, UIValue> m_values;
        std::vector<Subscription> m_subscriptions;
        uint64_t m_nextSubscription = 1u;
    };
}
