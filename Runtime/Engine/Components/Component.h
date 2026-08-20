#pragma once
#include "EngineDef.h"
#include "Reflection/Macros.h"
#include "Reflection/Object.h"
#include "Components/Component.generated.h"

#include <vector>

namespace NLS::Engine::Serialize
{
struct PropertyRecord;
}

namespace NLS::Engine
{
class GameObject;
namespace Components
{
CLASS(NLS_ENGINE_API Component) : public NLS::Object
{
public:
    GENERATED_BODY()
    Component();
    virtual ~Component();

    void DestroyFromOwner();

    FUNCTION()
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

    // Optional persistence extension for components whose state is not a
    // reflected native field set (for example, script asset metadata and
    // generated field overrides).  The engine owns the graph format while
    // the component owns its payload, so the dependency stays one-way.
    virtual bool SerializeObjectGraphProperties(
        std::vector<NLS::Engine::Serialize::PropertyRecord>& properties) const
    {
        (void)properties;
        return false;
    }

    virtual bool DeserializeObjectGraphProperties(
        const std::vector<NLS::Engine::Serialize::PropertyRecord>& properties)
    {
        (void)properties;
        return false;
    }

    bool IsSelfEnabled() const { return m_enabled; }
    bool IsActiveAndEnabled() const;
    void SetEnabled(bool enabled);

    GameObject* gameobject() const
    {
        return m_owner;
    }

protected:
    // Propagate render-affecting component changes to the owning scene without
    // making the component depend on SceneSystem headers.
    void MarkRenderStateChanged();

    GameObject* m_owner = nullptr;
    bool m_enabled = true;
    bool m_destroyedFromOwner = false;
};
} // namespace Components

} // namespace NLS::Engine
