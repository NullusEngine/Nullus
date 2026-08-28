#include "UISignal.h"

#include <algorithm>

namespace NLS::Engine::UI
{
    UISignalSubscriptionHandle UISignalBus::Connect(std::string name, Handler handler)
    {
        if (name.empty() || !handler)
            return {};
        const UISignalSubscriptionHandle handle { m_nextHandle++ };
        m_subscriptions.push_back({ handle, std::move(name), std::move(handler) });
        return handle;
    }

    bool UISignalBus::Disconnect(const UISignalSubscriptionHandle handle)
    {
        if (!handle.IsValid())
            return false;
        const auto iterator = std::find_if(
            m_subscriptions.begin(), m_subscriptions.end(),
            [handle](const Subscription& subscription) { return subscription.handle.value == handle.value; });
        if (iterator == m_subscriptions.end())
            return false;
        m_subscriptions.erase(iterator);
        return true;
    }

    uint32_t UISignalBus::Emit(
        const std::string& name,
        const std::vector<UIValue>& arguments)
    {
        if (name.empty())
            return 0u;
        const auto copy = m_subscriptions;
        uint32_t invoked = 0u;
        for (const auto& subscription : copy)
        {
            if (subscription.name == name && subscription.handler)
            {
                subscription.handler(name, arguments);
                ++invoked;
            }
        }
        return invoked;
    }

    void UISignalBus::Clear()
    {
        m_subscriptions.clear();
    }
}
