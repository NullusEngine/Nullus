#include "StateGameObject.h"
#include "AI/StateTransition.h"
#include "AI/StateMachine.h"
#include "AI/State.h"
using namespace NLS;
using namespace Engine;

StateGameObject::StateGameObject(string name)
{
    counter = 0.0f;
    stateMachine = new StateMachine();
    State* stateA = new State([&](float dt) -> void
                              { this->MoveLeft(dt); });

    State* stateB = new State([&](float dt) -> void
                              { this->MoveRight(dt); });
    stateMachine->AddState(stateA);
    stateMachine->AddState(stateB);

    stateMachine->AddTransition(new StateTransition(stateA, stateB, [&]() -> bool
                                                    { return this->counter > 3.0f; }));

    stateMachine->AddTransition(new StateTransition(stateB, stateA, [&]() -> bool
                                                    { return this->counter < 0.0f; }));
}

StateGameObject ::~StateGameObject()
{
    delete stateMachine;
}

void StateGameObject::Update(float dt)
{
    stateMachine->Update(dt);
}

void StateGameObject::MoveLeft(float dt)
{
    GetPhysicsObject()->AddForce({-100, 0, 0});
    counter += dt;
}

void StateGameObject::MoveRight(float dt)
{
    GetPhysicsObject()->AddForce({100, 0, 0});
    counter -= dt;
}