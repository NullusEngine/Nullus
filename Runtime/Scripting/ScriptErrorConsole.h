#pragma once

#include "ScriptRuntime.h"
#include <Debug/Logger.h>

#include <deque>
#include <cstdint>
#include <chrono>

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
        {
            m_errors.pop_front();
            m_receiptTimes.pop_front();
        }
        auto retained = error;
        const auto level = error.severity == ScriptErrorSeverity::Info ? NLS::Debug::ELogLevel::LOG_INFO :
            error.severity == ScriptErrorSeverity::Warning ? NLS::Debug::ELogLevel::LOG_WARNING : NLS::Debug::ELogLevel::LOG_ERROR;
        const auto source = error.language == ScriptLanguage::CSharp ? NLS::Debug::ELogSource::CSharp :
            error.language == ScriptLanguage::Lua ? NLS::Debug::ELogSource::Lua : NLS::Debug::ELogSource::Build;
        const auto record = NLS::Debug::Logger::WriteWithContext(error.message, level, NLS::Debug::ELogMode::FILE,
            error.sourcePath.c_str(), error.line, nullptr, source, error.stackTrace);
        retained.stackTrace = record.scriptStack;
        retained.logDate = record.date;
        retained.nativeStack = record.nativeStack;
        m_errors.push_back(std::move(retained));
        m_receiptTimes.push_back(std::chrono::steady_clock::now());
        ++m_nextSequence;
    }

    const std::deque<ScriptError>& GetErrors() const { return m_errors; }
    // Never reset on Clear: consumers can distinguish new entries even when
    // the bounded deque has rolled over or was emptied between Editor frames.
    uint64_t GetNextSequence() const { return m_nextSequence; }
    std::chrono::steady_clock::time_point GetReceiptTime(uint64_t sequence) const
    {
        const auto first = m_nextSequence - m_errors.size();
        return sequence >= first && sequence < m_nextSequence
            ? m_receiptTimes[static_cast<size_t>(sequence - first)] : std::chrono::steady_clock::time_point{};
    }
    void Clear() { m_errors.clear(); m_receiptTimes.clear(); }

private:
    size_t m_capacity;
    std::deque<ScriptError> m_errors;
    uint64_t m_nextSequence = 0;
    std::deque<std::chrono::steady_clock::time_point> m_receiptTimes;
};
}
