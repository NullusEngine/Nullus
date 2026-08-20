#pragma once

#include "ScriptRuntime.h"

#include <deque>

namespace NLS::Scripting
{
class NLS_SCRIPTING_API ScriptErrorConsole final
{
public:
    explicit ScriptErrorConsole(size_t capacity = 256)
        : m_capacity(capacity)
    {
    }

    void Attach(ScriptRuntime& runtime)
    {
        runtime.SetErrorSink([this](const ScriptError& error) { Push(error); });
    }

    void Push(const ScriptError& error)
    {
        if (m_capacity == 0)
            return;
        if (m_errors.size() == m_capacity)
            m_errors.pop_front();
        m_errors.push_back(error);
    }

    const std::deque<ScriptError>& GetErrors() const { return m_errors; }
    void Clear() { m_errors.clear(); }

private:
    size_t m_capacity;
    std::deque<ScriptError> m_errors;
};
}
