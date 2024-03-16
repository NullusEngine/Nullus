#pragma once
#include <vector>
#include <map>
#include "EngineDef.h"
namespace NLS
{
namespace Engine
{

class State;
class StateTransition;

typedef std::multimap<State*, StateTransition*> TransitionContainer;
typedef TransitionContainer::iterator TransitionIterator;

class NLS_ENGINE_API StateMachine
{
public:
    StateMachine();
    ~StateMachine();

    void AddState(State* s);
    void AddTransition(StateTransition* t);

    void Update(float dt);

protected:
    State* activeState;

    std::vector<State*> allStates;

    TransitionContainer allTransitions;
};
} // namespace Engine
} // namespace NLS