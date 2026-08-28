#pragma once

#include "EngineDef.h"
#include "UIDataContext.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace NLS::Engine::UI
{
    struct NLS_ENGINE_API UISignalSubscriptionHandle
    {
        uint64_t value = 0u;
        [[nodiscard]] bool IsValid() const { return value != 0u; }
    };

    class NLS_ENGINE_API UISignalBus final
    {
    public:
        using Handler = std::function<void(const std::string&, const std::vector<UIValue>&)>;

        UISignalSubscriptionHandle Connect(std::string name, Handler handler);
        bool Disconnect(UISignalSubscriptionHandle handle);
        uint32_t Emit(const std::string& name, const std::vector<UIValue>& arguments = {});
        void Clear();

    private:
        struct Subscription
        {
            UISignalSubscriptionHandle handle;
            std::string name;
            Handler handler;
        };

        std::vector<Subscription> m_subscriptions;
        uint64_t m_nextHandle = 1u;
    };
}
