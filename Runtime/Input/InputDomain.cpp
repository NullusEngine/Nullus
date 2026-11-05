#include "InputDomain.h"
#include <algorithm>
#include <stdexcept>
namespace NLS::Input
{
InputDomain::InputDomain(InputSystem& system, std::string name) : m_system(system), m_name(std::move(name)) {}
InputDomain::~InputDomain() { SetSuspended(true, CancelReason::Stop); }
std::shared_ptr<InputUser> InputDomain::CreateUser()
{
    auto user = std::make_shared<InputUser>(m_system, m_system.AllocateUserId());
    m_nextUser = user->GetId();
    user->SetNextFixedTick(m_nextTick);
    user->SetSuspended(m_suspended);
    m_users.push_back(user);
    return user;
}
bool InputDomain::RemoveUser(uint64_t id)
{
    auto it = std::find_if(m_users.begin(), m_users.end(), [id](const auto& user) { return user->GetId() == id; });
    if (it == m_users.end()) return false;
    (*it)->SetSuspended(true, CancelReason::Stop);
    m_users.erase(it);
    return true;
}
void InputDomain::EnqueueChange(std::function<void()> change) { if (change) m_changes.push_back(std::move(change)); }
void InputDomain::Update(double realTime, bool dispatchDynamic)
{
    if (!m_changes.empty())
    {
        auto changes = std::move(m_changes); m_changes.clear();
        // Detached storage keeps enqueued/nested changes at the next boundary.
        // Reuse it only after execution, including an exception's unwind.
        struct RecycleCapacity
        {
            decltype(m_changes)& pending;
            decltype(m_changes)& executing;
            ~RecycleCapacity()
            {
                executing.clear();
                if (pending.empty() && pending.capacity() < executing.capacity()) pending.swap(executing);
            }
        } recycle{m_changes, changes};
        for (auto& change : changes) change();
    }
    const auto lastUser = m_nextUser;
    // Copying the pointer, not the collection, keeps callback-driven removal safe.
    for (size_t i = 0; i < m_users.size() && m_users[i]->GetId() <= lastUser;)
    {
        auto user = m_users[i];
        user->Update(realTime, false);
        if (i < m_users.size() && m_users[i] == user) ++i;
    }
    if (dispatchDynamic) DispatchDynamicEvents();
}
void InputDomain::DispatchDynamicEvents()
{
    const auto lastUser = m_nextUser;
    for (size_t i = 0; i < m_users.size() && m_users[i]->GetId() <= lastUser;)
    {
        auto user = m_users[i];
        user->DispatchDynamicEvents();
        if (i < m_users.size() && m_users[i] == user) ++i;
    }
}
void InputDomain::BeginFixedTick(uint64_t tick, double deltaTime)
{
    if (tick != m_nextTick) throw std::invalid_argument("Domain fixed input tick is out of order");
    m_nextTick = tick + 1;
    const auto lastUser = m_nextUser;
    for (size_t i = 0; i < m_users.size() && m_users[i]->GetId() <= lastUser;)
    {
        auto user = m_users[i];
        user->BeginFixedTick(tick, deltaTime);
        if (i < m_users.size() && m_users[i] == user) ++i;
    }
}
void InputDomain::SetSuspended(bool suspended, CancelReason reason)
{
    m_suspended = suspended;
    for (const auto& user : m_users) user->SetSuspended(suspended, reason);
}
void InputDomain::Cancel(CancelReason reason) { for (const auto& user : m_users) user->Cancel(reason); }
}
