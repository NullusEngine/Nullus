#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include "BaseDef.h"

namespace NLS::meta
{
    class Type;
}

namespace NLS
{
    using InstanceID = int32_t;
    inline constexpr InstanceID InstanceID_None = 0;

    enum class AwakeFromLoadMode : int32_t
    {
        Default = 0,
        DidLoadFromDisk = 1 << 0,
        DidLoadThreaded = 1 << 1,
        InstantiateOrCreateFromCode = 1 << 2,
        Activate = 1 << 3,
        Animation = 1 << 4,
        WillUnloadAfterWritingBuildData = 1 << 5,
        PersistentManager = DidLoadFromDisk | DidLoadThreaded,
        Invalid = -1
    };

    class NLS_BASE_API Object
    {
    public:
        Object();
        explicit Object(InstanceID instanceID);
        Object(const Object&) = delete;
        Object& operator=(const Object&) = delete;
        Object(Object&&) = delete;
        Object& operator=(Object&&) = delete;
        virtual ~Object();

        InstanceID GetInstanceID() const;
        bool IsInstanceIDCreated() const;
        meta::Type GetType() const;
        virtual const char* GetObjectTypeName() const;

        virtual void MainThreadCleanup();
        void ThreadedCleanup();
        virtual void AwakeFromLoad(AwakeFromLoadMode awakeMode);
        virtual void AwakeFromLoadThreaded();
        virtual void CheckConsistency();
        virtual void Reset();
        virtual void SmartReset();

        static Object* IDToPointer(InstanceID instanceID);
        static Object* IDToPointerThreadSafe(InstanceID instanceID);
        static Object* IDToPointerNoThreadCheck(InstanceID instanceID);
        static void RegisterInstanceID(Object* object);
        static void RegisterInstanceIDNoLock(Object* object);
        static InstanceID ReserveInstanceID();
        static void ReserveInstanceID(InstanceID instanceID);
        static void ReleaseReservedInstanceID(InstanceID instanceID);
        static void AssignInstanceID(Object* object, InstanceID instanceID);
        static void AssignInstanceIDNoLock(Object* object, InstanceID instanceID);
        static Object* AllocateAndAssignInstanceID(Object* object);
        static Object* AllocateAndAssignInstanceIDNoLock(Object* object);
        static void LockObjectCreation();
        static void UnlockObjectCreation();
        static size_t GetLoadedObjectCount();

        template <typename T>
        static T* IDToPointer(InstanceID instanceID)
        {
            static_assert(std::is_base_of_v<Object, T>, "Object registry lookups require Object-derived types.");
            return dynamic_cast<T*>(IDToPointer(instanceID));
        }

    private:
        static InstanceID AllocateInstanceID();
        static void RegisterInstance(Object& object);
        static void UnregisterInstance(const Object& object);
        void SetInstanceID(InstanceID instanceID);

        InstanceID m_instanceID = InstanceID_None;

#if defined(NLS_ENABLE_TEST_HOOKS)
        friend class ObjectTestAccess;
#endif
    };

#if defined(NLS_ENABLE_TEST_HOOKS)
    class NLS_BASE_API ObjectTestAccess final
    {
    public:
        static void ClearObjectRegistry();
        static bool IsInstanceIDReserved(InstanceID instanceID);
    };
#endif

    class NLS_BASE_API NamedObject : public Object
    {
    public:
        NamedObject();
        explicit NamedObject(std::string name);
        explicit NamedObject(InstanceID instanceID);
        NamedObject(InstanceID instanceID, std::string name);
        ~NamedObject() override = default;

        const std::string& GetName() const;
        void SetName(std::string name);

    private:
        std::string m_name;
    };
}
