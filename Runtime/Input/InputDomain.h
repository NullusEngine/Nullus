#pragma once
#include "InputUser.h"

namespace NLS::Input
{
// Host-owned devices are borrowed. Destroying a Play domain only destroys its
// users, subscriptions and command buffers; editor device state is unaffected.
class NLS_INPUT_API InputDomain final
{
public:
    explicit InputDomain(InputSystem& system, std::string name);
    ~InputDomain();
    std::shared_ptr<InputUser> CreateUser();
    bool RemoveUser(uint64_t id);
    std::span<const std::shared_ptr<InputUser>> GetUsers() const { return m_users; }
    const std::string& GetName() const { return m_name; }
    InputSystem& GetInputSystem() const { return m_system; }
    std::weak_ptr<const void> GetLifetime() const { return m_lifetime; }
    void Update(double realTime, bool dispatchDynamic = true);
    // Resource/profile adapters publish changes before the next event evaluation.
    void EnqueueChange(std::function<void()> change);
    void DispatchDynamicEvents();
    void BeginFixedTick(uint64_t tick, double deltaTime);
    void SetSuspended(bool suspended, CancelReason reason = CancelReason::Pause);
    void Cancel(CancelReason reason);
private:
    InputSystem& m_system;
    std::string m_name;
    std::vector<std::shared_ptr<InputUser>> m_users;
    std::vector<std::function<void()>> m_changes;
    uint64_t m_nextUser = 0;
    uint64_t m_nextTick = 1;
    bool m_suspended = false;
    std::shared_ptr<const void> m_lifetime = std::make_shared<int>(0);
};
}
