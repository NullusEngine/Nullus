#pragma once

#include "PhysicsDef.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>

namespace NLS::Physics
{
namespace Detail
{
// The outer recursive gate groups public mutation calls into a single commit.
// Read views release the gate once their shared state lock has been acquired,
// so independent readers still run concurrently. Mutations always take gate
// before state; a callback must never re-enter a world during its solver call.
class WorldAccessMutex final
{
public:
    void lock() { m_gate.lock(); try { m_state.lock(); } catch (...) { m_gate.unlock(); throw; } }
    void unlock() { m_state.unlock(); m_revision.fetch_add(1, std::memory_order_release); m_gate.unlock(); }
    bool try_lock()
    {
        if (!m_gate.try_lock()) return false;
        if (m_state.try_lock()) return true;
        m_gate.unlock(); return false;
    }
    void lock_shared() { std::lock_guard gate(m_gate); m_state.lock_shared(); }
    void unlock_shared() { m_state.unlock_shared(); }
    bool try_lock_shared()
    {
        std::unique_lock gate(m_gate, std::try_to_lock);
        return gate.owns_lock() && m_state.try_lock_shared();
    }
    std::unique_lock<std::recursive_mutex> Batch() { return std::unique_lock(m_gate); }
    uint64_t Revision() const noexcept { return m_revision.load(std::memory_order_acquire); }
private:
    std::recursive_mutex m_gate;
    std::shared_mutex m_state;
    std::atomic<uint64_t> m_revision{0};
};
}

class PhysicsWorld;
class PhysicsWorld2D;
class NLS_PHYSICS_API PhysicsMutationBatch final
{
public:
    ~PhysicsMutationBatch();
    PhysicsMutationBatch(const PhysicsMutationBatch&) = delete;
    PhysicsMutationBatch& operator=(const PhysicsMutationBatch&) = delete;
private:
    PhysicsMutationBatch(std::shared_ptr<void> owner, Detail::WorldAccessMutex& access);
    std::shared_ptr<void> m_owner;
    std::unique_lock<std::recursive_mutex> m_lock;
    friend class PhysicsWorld;
    friend class PhysicsWorld2D;
};
}
