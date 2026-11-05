#include "PhysicsAccess.h"
#include <utility>

namespace NLS::Physics
{
PhysicsMutationBatch::PhysicsMutationBatch(std::shared_ptr<void> owner, Detail::WorldAccessMutex& access)
    : m_owner(std::move(owner)), m_lock(access.Batch()) {}
PhysicsMutationBatch::~PhysicsMutationBatch() = default;
}
