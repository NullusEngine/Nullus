#pragma once
#include "State.h"
#include "EngineDef.h"
namespace NLS
{
namespace Engine
{
class NLS_ENGINE_API PushdownState : public State
{
public:
    enum class PushdownResult
    {
        Push,
        Pop,
        NoChange
    };
    PushdownState();
    ~PushdownState();

    PushdownResult PushdownUpdate(PushdownState** pushResult);

    virtual void OnAwake() {} // By default do nothing
    virtual void OnSleep() {} // By default do nothing
};
} // namespace Engine
} // namespace NLS
