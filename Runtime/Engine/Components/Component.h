#pragma once
#include "EngineDef.h"

namespace NLS::Engine
{
class GameObject;
namespace Components
{
class NLS_ENGINE_API Component
{
public:
    Component();
    virtual ~Component();

    void CreateBy(GameObject* owner);

    virtual void OnCreate() {}
    /**
     * Called when the scene start right before OnStart
     * It allows you to apply prioritized game logic on scene start
     */
    virtual void OnAwake() {}

    /**
     * Called when the scene start right after OnAwake
     * It allows you to apply prioritized game logic on scene start
     */
    virtual void OnStart() {}

    /**
     * Called when the components gets enabled (owner SetActive set to true) and after OnAwake() on scene starts
     */
    virtual void OnEnable() {}

    /**
     * Called when the component gets disabled (owner SetActive set to false) and before OnDestroy() when the component get destroyed
     */
    virtual void OnDisable() {}

    /**
     * Called when the components gets destroyed
     */
    virtual void OnDestroy() {}

    /**
     * Called every frame
     * @param p_deltaTime
     */
    virtual void OnUpdate(float p_deltaTime) {}

    /**
     * Called every physics frame
     * @param p_deltaTime
     */
    virtual void OnFixedUpdate(float p_deltaTime) {}

    /**
     * Called every frame after OnUpdate
     * @param p_deltaTime
     */
    virtual void OnLateUpdate(float p_deltaTime) {}

    bool IsSelfEnabled() const { return m_enabled; }

    GameObject* gameobject() const
    {
        return m_owner;
    }

protected:
    GameObject* m_owner = nullptr;
    bool m_enabled = true;
};
} // namespace Components

} // namespace NLS::Engine