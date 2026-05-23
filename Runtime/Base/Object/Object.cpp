#include "Object/Object.h"

#include "Reflection/Type.h"

#include <limits>
#include <mutex>
#include <unordered_set>

namespace NLS
{
namespace
{
struct ObjectRegistryState
{
    InstanceID nextInstanceID = 1;
    std::unordered_map<InstanceID, Object*> objectByInstanceID;
    std::unordered_set<InstanceID> reservedInstanceIDs;
    std::recursive_mutex mutex;
};

ObjectRegistryState& Registry()
{
    static ObjectRegistryState registry;
    return registry;
}
}

Object::Object()
    : Object(AllocateInstanceID())
{
}

Object::Object(const InstanceID instanceID)
    : m_instanceID(instanceID != InstanceID_None ? instanceID : AllocateInstanceID())
{
    RegisterInstance(*this);
}

Object::~Object()
{
    UnregisterInstance(*this);
}

InstanceID Object::GetInstanceID() const
{
    return m_instanceID;
}

bool Object::IsInstanceIDCreated() const
{
    return m_instanceID != InstanceID_None;
}

meta::Type Object::GetType() const
{
    return meta::Type::GetFromName(GetObjectTypeName());
}

const char* Object::GetObjectTypeName() const
{
    return "NLS::Object";
}

void Object::MainThreadCleanup()
{
}

void Object::ThreadedCleanup()
{
}

void Object::AwakeFromLoad(AwakeFromLoadMode)
{
}

void Object::AwakeFromLoadThreaded()
{
}

void Object::CheckConsistency()
{
}

void Object::Reset()
{
}

void Object::SmartReset()
{
}

Object* Object::IDToPointer(const InstanceID instanceID)
{
    return IDToPointerThreadSafe(instanceID);
}

Object* Object::IDToPointerThreadSafe(const InstanceID instanceID)
{
    if (instanceID == InstanceID_None)
        return nullptr;

    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    return IDToPointerNoThreadCheck(instanceID);
}

Object* Object::IDToPointerNoThreadCheck(const InstanceID instanceID)
{
    if (instanceID == InstanceID_None)
        return nullptr;

    auto& registry = Registry();
    const auto found = registry.objectByInstanceID.find(instanceID);
    return found != registry.objectByInstanceID.end() ? found->second : nullptr;
}

void Object::RegisterInstanceID(Object* object)
{
    if (object == nullptr)
        return;

    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    RegisterInstanceIDNoLock(object);
}

void Object::RegisterInstanceIDNoLock(Object* object)
{
    if (object == nullptr || object->m_instanceID == InstanceID_None)
        return;

    auto& registry = Registry();
    registry.objectByInstanceID[object->m_instanceID] = object;
    registry.reservedInstanceIDs.erase(object->m_instanceID);
    if (object->m_instanceID >= registry.nextInstanceID)
    {
        if (object->m_instanceID == std::numeric_limits<InstanceID>::max())
            registry.nextInstanceID = 1;
        else
            registry.nextInstanceID = object->m_instanceID + 1;
    }
    if (registry.nextInstanceID == InstanceID_None)
        registry.nextInstanceID = 1;
}

InstanceID Object::ReserveInstanceID()
{
    return AllocateInstanceID();
}

void Object::ReserveInstanceID(const InstanceID instanceID)
{
    if (instanceID == InstanceID_None)
        return;

    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    if (registry.objectByInstanceID.contains(instanceID))
        return;

    registry.reservedInstanceIDs.insert(instanceID);
    if (instanceID >= registry.nextInstanceID)
    {
        if (instanceID == std::numeric_limits<InstanceID>::max())
            registry.nextInstanceID = 1;
        else
            registry.nextInstanceID = instanceID + 1;
        if (registry.nextInstanceID == InstanceID_None)
            registry.nextInstanceID = 1;
    }
}

void Object::ReleaseReservedInstanceID(const InstanceID instanceID)
{
    if (instanceID == InstanceID_None)
        return;

    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    registry.reservedInstanceIDs.erase(instanceID);
}

void Object::AssignInstanceID(Object* object, const InstanceID instanceID)
{
    if (object == nullptr)
        return;

    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    AssignInstanceIDNoLock(object, instanceID);
}

void Object::AssignInstanceIDNoLock(Object* object, const InstanceID instanceID)
{
    if (object == nullptr || instanceID == InstanceID_None)
        return;

    object->SetInstanceID(instanceID);
    RegisterInstanceIDNoLock(object);
}

Object* Object::AllocateAndAssignInstanceID(Object* object)
{
    if (object == nullptr)
        return nullptr;

    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    return AllocateAndAssignInstanceIDNoLock(object);
}

Object* Object::AllocateAndAssignInstanceIDNoLock(Object* object)
{
    if (object == nullptr)
        return nullptr;

    AssignInstanceIDNoLock(object, AllocateInstanceID());
    return object;
}

void Object::LockObjectCreation()
{
    Registry().mutex.lock();
}

void Object::UnlockObjectCreation()
{
    Registry().mutex.unlock();
}

size_t Object::GetLoadedObjectCount()
{
    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    return registry.objectByInstanceID.size();
}

#if defined(NLS_ENABLE_TEST_HOOKS)
void ObjectTestAccess::ClearObjectRegistry()
{
    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    registry.nextInstanceID = 1;
    registry.objectByInstanceID.clear();
    registry.reservedInstanceIDs.clear();
}

bool ObjectTestAccess::IsInstanceIDReserved(const InstanceID instanceID)
{
    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    return registry.reservedInstanceIDs.contains(instanceID);
}
#endif

InstanceID Object::AllocateInstanceID()
{
    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    while (registry.nextInstanceID == InstanceID_None ||
           registry.objectByInstanceID.contains(registry.nextInstanceID) ||
           registry.reservedInstanceIDs.contains(registry.nextInstanceID))
    {
        if (registry.nextInstanceID == std::numeric_limits<InstanceID>::max())
            registry.nextInstanceID = 1;
        else
            ++registry.nextInstanceID;
    }

    const InstanceID allocated = registry.nextInstanceID;
    if (registry.nextInstanceID == std::numeric_limits<InstanceID>::max())
        registry.nextInstanceID = 1;
    else
        ++registry.nextInstanceID;
    registry.reservedInstanceIDs.insert(allocated);
    return allocated;
}

void Object::RegisterInstance(Object& object)
{
    if (object.m_instanceID == InstanceID_None)
        return;

    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    RegisterInstanceIDNoLock(&object);
}

void Object::UnregisterInstance(const Object& object)
{
    if (object.m_instanceID == InstanceID_None)
        return;

    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    const auto found = registry.objectByInstanceID.find(object.m_instanceID);
    if (found != registry.objectByInstanceID.end() && found->second == &object)
        registry.objectByInstanceID.erase(found);
}

void Object::SetInstanceID(const InstanceID instanceID)
{
    UnregisterInstance(*this);
    m_instanceID = instanceID;
}

NamedObject::NamedObject() = default;

NamedObject::NamedObject(std::string name)
    : m_name(std::move(name))
{
}

NamedObject::NamedObject(const InstanceID instanceID)
    : Object(instanceID)
{
}

NamedObject::NamedObject(const InstanceID instanceID, std::string name)
    : Object(instanceID)
    , m_name(std::move(name))
{
}

const std::string& NamedObject::GetName() const
{
    return m_name;
}

void NamedObject::SetName(std::string name)
{
    m_name = std::move(name);
}
}
