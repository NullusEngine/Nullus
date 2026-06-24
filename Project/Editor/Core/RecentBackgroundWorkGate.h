#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

namespace NLS::Editor::Core
{
    class RecentBackgroundWorkGate
    {
    public:
        using Clock = std::chrono::steady_clock;

        class Completion
        {
        public:
            Completion() = default;
            Completion(RecentBackgroundWorkGate& gate, std::string key);
            ~Completion();

            Completion(const Completion&) = delete;
            Completion& operator=(const Completion&) = delete;

            Completion(Completion&& other) noexcept;
            Completion& operator=(Completion&& other) noexcept;

            void Release();

        private:
            RecentBackgroundWorkGate* m_gate = nullptr;
            std::string m_key;
        };

        RecentBackgroundWorkGate(size_t capacity, Clock::duration ttl);

        bool TryBegin(const std::string& key, Clock::time_point now);
        bool IsInFlight(const std::string& key) const;
        Completion CompleteOnScopeExit(std::string key);
        void End(const std::string& key);
        size_t EntryCountForTesting() const;

    private:
        struct Entry
        {
            Clock::time_point lastAttempt {};
            size_t sequence = 0u;
            bool inFlight = false;
        };

        void PruneExpired(Clock::time_point now);
        bool EvictOldestCompleted();

        size_t m_capacity = 0u;
        Clock::duration m_ttl {};
        size_t m_nextSequence = 0u;
        std::unordered_map<std::string, Entry> m_entries;
        mutable std::mutex m_mutex;
    };
}
