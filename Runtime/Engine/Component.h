#pragma once
#include "EngineDef.h"
namespace NLS
{
namespace Engine
{
class GameObject;
class NLS_ENGINE_API Component
{
public:
    void CreateBy(GameObject* owner);

    virtual void OnCreate() {}
    virtual void OnEnabled() {}
    virtual void OnInit() {}
    virtual void OnStart() {}
    virtual void OnDisabled() {}
    virtual void OnUpdate() {}
    virtual void OnLateUpdate() {}
    virtual void OnDestroy() {}

    bool IsSelfEnabled() const { return m_enabled; }

    GameObject* gameobject() const { return mOwner; }

protected:
private:
    /// @brief 组件的拥有者
    GameObject* mOwner = nullptr;
    /// @brief 组件本身是否启用
    bool m_enabled = true;
};
} // namespace Engine
} // namespace NLS