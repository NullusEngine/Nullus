#pragma once
#include "GameObject.h"
namespace NLS
{
namespace Engine
{
class StateMachine;
class StateGameObject : public GameObject
{
public:
    StateGameObject(string name = "");
    ~StateGameObject();
    virtual void Update(float dt);

protected:
    void MoveLeft(float dt);
    void MoveRight(float dt);

    StateMachine* stateMachine;
    float counter;
};
} // namespace Engine
} // namespace NLS
