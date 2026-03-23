#pragma once
#include <stack>
#include "EngineDef.h"
namespace NLS::Engine
{
class PushdownState;

class NLS_ENGINE_API PushdownMachine
{
public:
    PushdownMachine();
    ~PushdownMachine();

    void Update();

protected:
    PushdownState* activeState;

    std::stack<PushdownState*> stateStack;
};
} // namespace NLS::Engine
