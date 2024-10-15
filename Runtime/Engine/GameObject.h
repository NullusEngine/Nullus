#pragma once
#include <vector>
#include "EngineDef.h"
#include "UTemplate/Type.hpp"
#include "UDRefl/Object.hpp"
#include "Eventing/Event.h"
#include "Components/Component.h"
namespace NLS
{
namespace Engine
{
namespace Components
{
    class TransformComponent;
}
class NLS_ENGINE_API GameObject
{
public:
    static void Bind();

public:
    GameObject(int64_t p_actorID, const std::string& p_name, const std::string& p_tag, bool& p_playing);
    ~GameObject();
    template<typename T>
    T* AddComponent(const std::function<void(Components::Component*)>& func = {});
    template<typename T>
    T* GetComponent(bool includeSubType = true) const;
    /**
     * Returns a reference to the vector of components
     */
    std::vector<UDRefl::SharedObject>& GetComponents()
    {
        return m_vComponents;
    }
    bool RemoveComponent(UDRefl::SharedObject component);

    UDRefl::SharedObject AddComponent(Type type, const std::function<void(Components::Component*)>& func = {});
    UDRefl::SharedObject GetComponent(Type type, bool includeSubType = true) const;
    /**
     * Enable or disable the actor
     * @param p_active
     */
    void SetActive(bool p_active);

    /**
     * Returns true if the actor is active, ignoring his parent (if any) active state
     */
    bool IsSelfActive() const;

    /**
     * Returns true if the actor is and his recursive parents (if any) are active
     */
    bool IsActive() const;

    Components::TransformComponent* GetTransform() const
    {
        return m_transform;
    }

    const std::string& GetName() const
    {
        return m_name;
    }
    /**
     * Defines a new name for the actor
     * @param p_name
     */
    void SetName(const std::string& p_name)
    {
        m_name = p_name;
    }
    /**
     * Returns true if the actor is not marked as destroyed
     */
    bool IsAlive() const
    {
        return !m_destroyed;
    }
    /**
     * Defines a new tag for the actor
     * @param p_tag
     */
    void SetTag(const std::string& p_tag)
    {
        m_tag = p_tag;
    }
    /**
     * Return the current tag of the actor
     */
    const std::string& GetTag() const
    {
        return m_tag;
    }
    /**
     * Called when the actor enter in collision with another physical object
     * @param p_otherObject
     */
    void OnCollisionEnter(GameObject& p_otherObject);

    /**
     * Called when the actor is in collision with another physical object
     * @param p_otherObject
     */
    void OnCollisionStay(GameObject& p_otherObject);

    /**
     * Called when the actor exit from collision with another physical object
     * @param p_otherObject
     */
    void OnCollisionExit(GameObject& p_otherObject);

    /**
     * Called when the actor enter in trigger with another physical object
     * @param p_otherObject
     */
    void OnTriggerEnter(GameObject& p_otherObject);

    /**
     * Called when the actor is in trigger with another physical object
     * @param p_otherObject
     */
    void OnTriggerStay(GameObject& p_otherObject);

    /**
     * Called when the actor exit from trigger with another physical object
     * @param p_otherObject
     */
    void OnTriggerExit(GameObject& p_otherObject);

    void SetWorldID(int newID)
    {
        m_worldID = newID;
    }

    int GetWorldID() const
    {
        return m_worldID;
    }

    /**
     * Set an actor as the parent of this actor
     * @param p_parent
     */
    void SetParent(GameObject& p_parent);

    /**
     * Detach from the parent
     */
    void DetachFromParent();

    /**
     * Returns true if this actor transform is descendant of the actor
     * @param p_actor
     */
    bool IsDescendantOf(const GameObject* p_actor) const;

    /**
     * Returns true if the actor has a parent
     */
    bool HasParent() const;

    /**
     * Returns the parents of this actor (Or nullptr if no parent)
     */
    GameObject* GetParent() const;

    /**
     * Returns the ID of the parent of this actor
     */
    int64_t GetParentID() const;

    /**
     * Returns the children of this actor
     */
    std::vector<GameObject*>& GetChildren();

    
	/**
     * Mark the Actor as "Destroyed". A "Destroyed" actor will be removed from the scene by the scene itself
     */
    void MarkAsDestroy();

    /**
     * Defines if the actor is sleeping or not.
     * A sleeping actor will not trigger methods suchs as OnEnable, OnDisable and OnDestroyed
     * @param p_sleeping
     */
    void SetSleeping(bool p_sleeping) { m_sleeping = p_sleeping; }
    /**
     * Called when the scene start or when the actor gets enabled for the first time during play mode
     * This method will always be called in an ordered triple:
     * - OnAwake()
     * - OnEnable()
     * - OnStart()
     */
    void OnAwake();

    /**
     * Called when the scene start or when the actor gets enabled for the first time during play mode
     * This method will always be called in an ordered triple:
     * - OnAwake()
     * - OnEnable()
     * - OnStart()
     */
    void OnStart();

    /**
     * Called when the actor gets enabled (SetActive set to true) or at scene start if the actor is hierarchically active.
     * This method can be called in an ordered triple at scene start:
     * - OnAwake()
     * - OnEnable()
     * - OnStart()
     * Or can be called solo if the actor hierarchical active state changed to true and the actor already gets awaked
     * Conditions:
     * - Play mode only
     */
    void OnEnable();

    /**
     * Called when the actor hierarchical active state changed to false or gets destroyed while being hierarchically active
     * Conditions:
     * - Play mode only
     */
    void OnDisable();

    /**
     * Called when the actor gets destroyed if it has been awaked
     * Conditions:
     * - Play mode only
     */
    void OnDestroy();

    /**
     * Called every frame
     * @param p_deltaTime
     */
    void OnUpdate(float p_deltaTime);

    /**
     * Called every physics frame
     * @param p_deltaTime
     */
    void OnFixedUpdate(float p_deltaTime);

    /**
     * Called every frame after OnUpdate
     * @param p_deltaTime
     */
    void OnLateUpdate(float p_deltaTime);

private:
    /**
     * @brief Deleted copy constructor
     * @param p_actor
     */
    GameObject(const GameObject& p_actor) = delete;

    void RecursiveActiveUpdate();
    void RecursiveWasActiveUpdate();

public:
    /* Some events that are triggered when an action occur on the actor instance */
    Event<UDRefl::SharedObject> ComponentAddedEvent;
    Event<UDRefl::SharedObject> ComponentRemovedEvent;

    /* Some events that are triggered when an action occur on any actor */
    static Event<GameObject&> DestroyedEvent;
    static Event<GameObject&> CreatedEvent;
    static Event<GameObject&, GameObject&> AttachEvent;
    static Event<GameObject&> DettachEvent;

protected:
    std::vector<UDRefl::SharedObject> m_vComponents;
    bool m_active;
    int m_worldID;
    std::string m_name;
    /* Settings */
    std::string m_tag;

    /* Internal settings */
    bool m_destroyed = false;
    bool m_sleeping = true;
    bool m_awaked = false;
    bool m_started = false;
    bool m_wasActive = false;
    bool& m_playing;
    /* Parenting system stuff */
    int64_t m_parentID = 0;
    GameObject* m_parent = nullptr;
    std::vector<GameObject*> m_children;
    Components::TransformComponent* m_transform = nullptr;
};

} // namespace Engine
} // namespace NLS

#include "GameObject.inl"
