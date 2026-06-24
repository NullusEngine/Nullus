#include "Core/RecentBackgroundWorkGate.h"

#include <utility>

namespace NLS::Editor::Core
{
    RecentBackgroundWorkGate::Completion::Completion(RecentBackgroundWorkGate& gate, std::string key)
        : m_gate(&gate), m_key(std::move(key))
    {
    }

    RecentBackgroundWorkGate::Completion::~Completion()
    {
        Release();
    }

    RecentBackgroundWorkGate::Completion::Completion(Completion&& other) noexcept
        : m_gate(other.m_gate), m_key(std::move(other.m_key))
    {
        other.m_gate = nullptr;
    }

    RecentBackgroundWorkGate::Completion& RecentBackgroundWorkGate::Completion::operator=(Completion&& other) noexcept
    {
        if (this != &other)
        {
            Release();
            m_gate = other.m_gate;
            m_key = std::move(other.m_key);
            other.m_gate = nullptr;
        }
        return *this;
    }

    void RecentBackgroundWorkGate::Completion::Release()
    {
        if (m_gate != nullptr)
        {
            m_gate->End(m_key);
            m_gate = nullptr;
        }
    }

    RecentBackgroundWorkGate::RecentBackgroundWorkGate(const size_t capacity, Clock::duration ttl)
        : m_capacity(capacity), m_ttl(ttl)
    {
    }

    bool RecentBackgroundWorkGate::TryBegin(const std::string& key, Clock::time_point now)
    {
        if (key.empty() || m_capacity == 0u)
            return false;

        std::lock_guard lock(m_mutex);
        PruneExpired(now);

        if (auto entry = m_entries.find(key); entry != m_entries.end())
        {
            if (entry->second.inFlight)
                return false;
            if (now - entry->second.lastAttempt < m_ttl)
                return false;

            entry->second.lastAttempt = now;
            entry->second.sequence = ++m_nextSequence;
            entry->second.inFlight = true;
            return true;
        }

        if (m_entries.size() >= m_capacity && !EvictOldestCompleted())
            return false;

        m_entries.emplace(
            key,
            Entry
            {
                .lastAttempt = now,
                .sequence = ++m_nextSequence,
                .inFlight = true,
            });
        return true;
    }

    void RecentBackgroundWorkGate::End(const std::string& key)
    {
        std::lock_guard lock(m_mutex);
        if (auto entry = m_entries.find(key); entry != m_entries.end())
            entry->second.inFlight = false;
    }

    bool RecentBackgroundWorkGate::IsInFlight(const std::string& key) const
    {
        if (key.empty())
            return false;
        std::lock_guard lock(m_mutex);
        if (auto entry = m_entries.find(key); entry != m_entries.end())
            return entry->second.inFlight;
        return false;
    }

    RecentBackgroundWorkGate::Completion RecentBackgroundWorkGate::CompleteOnScopeExit(std::string key)
    {
        return Completion(*this, std::move(key));
    }

    size_t RecentBackgroundWorkGate::EntryCountForTesting() const
    {
        std::lock_guard lock(m_mutex);
        return m_entries.size();
    }

    void RecentBackgroundWorkGate::PruneExpired(Clock::time_point now)
    {
        for (auto entry = m_entries.begin(); entry != m_entries.end();)
        {
            if (!entry->second.inFlight && now - entry->second.lastAttempt >= m_ttl)
                entry = m_entries.erase(entry);
            else
                ++entry;
        }
    }

    bool RecentBackgroundWorkGate::EvictOldestCompleted()
    {
        auto oldest = m_entries.end();
        for (auto entry = m_entries.begin(); entry != m_entries.end(); ++entry)
        {
            if (entry->second.inFlight)
                continue;
            if (oldest == m_entries.end() || entry->second.sequence < oldest->second.sequence)
                oldest = entry;
        }
        if (oldest == m_entries.end())
            return false;
        m_entries.erase(oldest);
        return true;
    }
}
