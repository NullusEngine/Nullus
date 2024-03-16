#pragma once
#include <stack>
#include "EngineDef.h"
namespace NLS
{
namespace Engine
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
} // namespace Engine
} // namespace NLS
