#include "UIDataContext.h"

#include <algorithm>

namespace NLS::Engine::UI
{
    bool UIDataContext::Set(std::string path, UIValue value, const bool notify)
    {
        if (path.empty())
            return false;
        const auto iterator = m_values.find(path);
        if (iterator != m_values.end() && iterator->second == value)
            return false;
        auto [stored, inserted] = m_values.insert_or_assign(std::move(path), std::move(value));
        if (notify)
        {
            const auto copy = m_subscriptions;
            for (const auto& subscription : copy)
            {
                if (subscription.path == stored->first && subscription.handler)
                    subscription.handler(stored->first, stored->second);
            }
        }
        (void)inserted;
        return true;
    }

    bool UIDataContext::Remove(const std::string& path)
    {
        return m_values.erase(path) != 0u;
    }

    const UIValue* UIDataContext::Get(const std::string& path) const
    {
        const auto iterator = m_values.find(path);
        return iterator == m_values.end() ? nullptr : &iterator->second;
    }

    bool UIDataContext::Contains(const std::string& path) const
    {
        return m_values.contains(path);
    }

    UIDataSubscriptionHandle UIDataContext::Subscribe(std::string path, ChangedHandler handler)
    {
        if (path.empty() || !handler)
            return {};
        const UIDataSubscriptionHandle handle { m_nextSubscription++ };
        m_subscriptions.push_back({ handle, std::move(path), std::move(handler) });
        return handle;
    }

    bool UIDataContext::Unsubscribe(const UIDataSubscriptionHandle handle)
    {
        const auto iterator = std::find_if(
            m_subscriptions.begin(), m_subscriptions.end(),
            [handle](const Subscription& subscription) { return subscription.handle.value == handle.value; });
        if (iterator == m_subscriptions.end())
            return false;
        m_subscriptions.erase(iterator);
        return true;
    }

    void UIDataContext::Clear()
    {
        m_values.clear();
        m_subscriptions.clear();
    }
}
