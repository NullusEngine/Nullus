#include "Profiling/Profiler.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace NLS::Base::Profiling
{
class ProfilerDestinationRegistration final
{
public:
    explicit ProfilerDestinationRegistration(IProfilerDestination& destination)
        : m_identity(&destination), m_destination(&destination)
    {
    }

    bool Matches(const IProfilerDestination& destination) const
    {
        return m_identity == &destination;
    }

    IProfilerDestination* TryAcquire()
    {
        std::lock_guard lock(m_mutex);
        if (!m_registered || m_destination == nullptr)
            return nullptr;

        ++m_activeCallbackCount;
        return m_destination;
    }

    void Release()
    {
        {
            std::lock_guard lock(m_mutex);
            if (m_activeCallbackCount > 0u)
                --m_activeCallbackCount;
            if (!m_registered && m_activeCallbackCount == 0u)
                m_destination = nullptr;
        }
        m_callbacksFinished.notify_all();
    }

    void Retire()
    {
        {
            std::lock_guard lock(m_mutex);
            m_registered = false;
            if (m_activeCallbackCount == 0u)
                m_destination = nullptr;
        }
        m_callbacksFinished.notify_all();
    }

    void WaitForCallbacks()
    {
        std::unique_lock lock(m_mutex);
        m_callbacksFinished.wait(lock, [&]() {
            return m_activeCallbackCount == 0u;
        });
        m_destination = nullptr;
    }

    bool IsRetiredAndDrained()
    {
        std::lock_guard lock(m_mutex);
        return !m_registered && m_activeCallbackCount == 0u;
    }

private:
    const IProfilerDestination* const m_identity = nullptr;
    IProfilerDestination* m_destination = nullptr;
    std::mutex m_mutex;
    std::condition_variable m_callbacksFinished;
    size_t m_activeCallbackCount = 0u;
    bool m_registered = true;
};

namespace
{
struct ActiveDestinationLeaseNode
{
    ProfilerDestinationRegistration* registration = nullptr;
    ActiveDestinationLeaseNode* previous = nullptr;
};

thread_local ActiveDestinationLeaseNode* g_activeDestinationLease = nullptr;

class ProfilerDestinationLease final
{
public:
    explicit ProfilerDestinationLease(
        const std::shared_ptr<ProfilerDestinationRegistration>& registration)
        : m_registration(registration)
    {
        if (m_registration == nullptr)
            return;

        m_destination = m_registration->TryAcquire();
        if (m_destination == nullptr)
        {
            m_registration.reset();
            return;
        }

        m_activeNode.registration = m_registration.get();
        m_activeNode.previous = g_activeDestinationLease;
        g_activeDestinationLease = &m_activeNode;
    }

    ~ProfilerDestinationLease()
    {
        if (m_destination == nullptr)
            return;

        auto** current = &g_activeDestinationLease;
        while (*current != nullptr && *current != &m_activeNode)
            current = &((*current)->previous);
        if (*current == &m_activeNode)
            *current = m_activeNode.previous;

        m_registration->Release();
    }

    ProfilerDestinationLease(const ProfilerDestinationLease&) = delete;
    ProfilerDestinationLease& operator=(const ProfilerDestinationLease&) = delete;

    explicit operator bool() const
    {
        return m_destination != nullptr;
    }

    IProfilerDestination* operator->() const
    {
        return m_destination;
    }

private:
    std::shared_ptr<ProfilerDestinationRegistration> m_registration;
    IProfilerDestination* m_destination = nullptr;
    ActiveDestinationLeaseNode m_activeNode;
};

using ProfilerDestinationRegistrations =
    std::vector<std::shared_ptr<ProfilerDestinationRegistration>>;

std::mutex g_profilerMutex;
std::atomic_bool g_enabled = false;
ProfilerDestinationRegistrations g_destinations;
ProfilerDestinationRegistrations g_retiredDestinations;
ProfilerSessionStats g_stats;
ProfilerGpuContextEvent g_lastGpuContext;
bool g_hasGpuContext = false;
thread_local uint32_t g_scopeDepth = 0u;
thread_local uint32_t g_gpuScopeDepth = 0u;
thread_local std::string g_threadName;

bool AcceptsCPUScopes(const ProfilerDestinationState& state)
{
    return state.enabled &&
        state.availability == ProfilerAvailability::Available &&
        (state.capabilities & ProfilerCapability_CPUScopes) != 0u;
}

bool AcceptsGPUScopes(const ProfilerDestinationState& state)
{
    return state.enabled &&
        state.availability == ProfilerAvailability::Available &&
        (state.capabilities & ProfilerCapability_GPUScopes) != 0u;
}

bool AcceptsGPUContextInitialization(const ProfilerDestinationState& state)
{
    return state.enabled &&
        state.availability == ProfilerAvailability::Available &&
        ((state.capabilities & ProfilerCapability_GPUScopes) != 0u ||
            (state.capabilities & ProfilerCapability_EditorTimeline) != 0u);
}

ProfilerDestinationRegistrations GetRegisteredDestinations()
{
    std::lock_guard lock(g_profilerMutex);
    g_retiredDestinations.erase(
        std::remove_if(
            g_retiredDestinations.begin(),
            g_retiredDestinations.end(),
            [](const auto& registration) {
                return registration->IsRetiredAndDrained();
            }),
        g_retiredDestinations.end());
    return g_destinations;
}

void PruneRetiredDestinationsLocked()
{
    g_retiredDestinations.erase(
        std::remove_if(
            g_retiredDestinations.begin(),
            g_retiredDestinations.end(),
            [](const auto& registration) {
                return registration->IsRetiredAndDrained();
            }),
        g_retiredDestinations.end());
}

void RetainRetiredDestinationLocked(
    const std::shared_ptr<ProfilerDestinationRegistration>& registration)
{
    const auto found = std::find(
        g_retiredDestinations.begin(),
        g_retiredDestinations.end(),
        registration);
    if (found == g_retiredDestinations.end())
        g_retiredDestinations.push_back(registration);
}

bool RegistrationAccepts(
    const std::shared_ptr<ProfilerDestinationRegistration>& registration,
    bool (*accepts)(const ProfilerDestinationState&))
{
    ProfilerDestinationLease lease(registration);
    return lease && accepts(lease->GetState());
}

void WaitForRetiredRegistrations(const ProfilerDestinationRegistrations& registrations)
{
    // Waiting from inside any destination callback can deadlock with another callback
    // unregistering this thread's destination. Callback-originated unregister is revoke-only;
    // an external caller provides the final destruction barrier.
    if (g_activeDestinationLease != nullptr)
        return;

    for (const auto& registration : registrations)
        registration->WaitForCallbacks();
}

void RetireRegistrations(ProfilerDestinationRegistrations& registrations)
{
    for (const auto& registration : registrations)
        registration->Retire();
}
}

void IProfilerDestination::BeginGpuScope(const ProfilerGpuScopeEvent& event)
{
    (void)event;
}

void IProfilerDestination::EndGpuScope(const ProfilerGpuScopeEvent& event)
{
    (void)event;
}

void IProfilerDestination::InitializeGpuContext(const ProfilerGpuContextEvent& event)
{
    (void)event;
}

void IProfilerDestination::SubmitGpuCommandLists(const ProfilerGpuCommandListSubmitEvent& event)
{
    (void)event;
}

void Profiler::SetEnabled(const bool enabled)
{
    g_enabled.store(enabled, std::memory_order_release);
}

bool Profiler::IsEnabled()
{
    return g_enabled.load(std::memory_order_acquire);
}

void Profiler::RegisterDestination(IProfilerDestination& destination)
{
    std::shared_ptr<ProfilerDestinationRegistration> registration;
    std::optional<ProfilerGpuContextEvent> contextToReplay;
    {
        std::lock_guard lock(g_profilerMutex);
        PruneRetiredDestinationsLocked();
        const auto found = std::find_if(
            g_destinations.begin(),
            g_destinations.end(),
            [&](const auto& candidate) { return candidate->Matches(destination); });
        if (found == g_destinations.end())
        {
            registration = std::make_shared<ProfilerDestinationRegistration>(destination);
            g_destinations.push_back(registration);
        }
        else
        {
            registration = *found;
        }

        if (g_hasGpuContext)
            contextToReplay = g_lastGpuContext;
    }

    if (contextToReplay.has_value() &&
        RegistrationAccepts(registration, AcceptsGPUContextInitialization))
    {
        ProfilerDestinationLease lease(registration);
        if (lease)
            lease->InitializeGpuContext(contextToReplay.value());
    }
}

void Profiler::UnregisterDestination(IProfilerDestination& destination)
{
    ProfilerDestinationRegistrations retiredRegistrations;
    {
        std::lock_guard lock(g_profilerMutex);
        PruneRetiredDestinationsLocked();
        const auto firstRemoved = std::remove_if(
            g_destinations.begin(),
            g_destinations.end(),
            [&](const auto& registration) {
                if (!registration->Matches(destination))
                    return false;

                retiredRegistrations.push_back(registration);
                return true;
            });
        g_destinations.erase(firstRemoved, g_destinations.end());
        RetireRegistrations(retiredRegistrations);
        for (const auto& registration : retiredRegistrations)
            RetainRetiredDestinationLocked(registration);

        for (const auto& registration : g_retiredDestinations)
        {
            if (!registration->Matches(destination) ||
                std::find(
                    retiredRegistrations.begin(),
                    retiredRegistrations.end(),
                    registration) != retiredRegistrations.end())
            {
                continue;
            }

            retiredRegistrations.push_back(registration);
        }
    }

    WaitForRetiredRegistrations(retiredRegistrations);

    {
        std::lock_guard lock(g_profilerMutex);
        PruneRetiredDestinationsLocked();
    }
}

void Profiler::ReplayGpuContextIfAvailable(IProfilerDestination& destination)
{
    std::shared_ptr<ProfilerDestinationRegistration> registration;
    std::optional<ProfilerGpuContextEvent> contextToReplay;
    {
        std::lock_guard lock(g_profilerMutex);
        const auto found = std::find_if(
            g_destinations.begin(),
            g_destinations.end(),
            [&](const auto& candidate) { return candidate->Matches(destination); });
        if (found != g_destinations.end())
            registration = *found;
        if (registration != nullptr && g_hasGpuContext)
            contextToReplay = g_lastGpuContext;
    }

    if (contextToReplay.has_value() &&
        RegistrationAccepts(registration, AcceptsGPUContextInitialization))
    {
        ProfilerDestinationLease lease(registration);
        if (lease)
            lease->InitializeGpuContext(contextToReplay.value());
    }
}

void Profiler::ClearGpuContext(void* nativeDevice)
{
    std::lock_guard lock(g_profilerMutex);
    if (!g_hasGpuContext)
        return;
    if (nativeDevice != nullptr && g_lastGpuContext.nativeDevice != nativeDevice)
        return;

    g_lastGpuContext = {};
    g_hasGpuContext = false;
}

void Profiler::ResetForTesting()
{
    ProfilerDestinationRegistrations retiredRegistrations;
    {
        std::lock_guard lock(g_profilerMutex);
        PruneRetiredDestinationsLocked();
        g_enabled.store(false, std::memory_order_release);
        auto activeRegistrations = std::move(g_destinations);
        RetireRegistrations(activeRegistrations);
        for (const auto& registration : activeRegistrations)
            RetainRetiredDestinationLocked(registration);
        retiredRegistrations = g_retiredDestinations;
        g_stats = {};
        g_lastGpuContext = {};
        g_hasGpuContext = false;
        g_scopeDepth = 0u;
        g_gpuScopeDepth = 0u;
        g_threadName.clear();
    }

    WaitForRetiredRegistrations(retiredRegistrations);

    {
        std::lock_guard lock(g_profilerMutex);
        PruneRetiredDestinationsLocked();
    }
}

size_t Profiler::GetDestinationCountForTesting()
{
    std::lock_guard lock(g_profilerMutex);
    PruneRetiredDestinationsLocked();
    return g_destinations.size();
}

void Profiler::RegisterThread(const std::string_view name)
{
    g_threadName = std::string(name);
}

ProfilerScopeEvent Profiler::BeginScope(const std::string_view name, const std::string_view sourceFunction)
{
    ProfilerScopeEvent event;

    const bool enabled = IsEnabled();
    if (!enabled)
        return event;

    auto registrations = GetRegisteredDestinations();
    if (registrations.empty())
    {
        std::lock_guard lock(g_profilerMutex);
        ++g_stats.droppedEventCount;
        return event;
    }

    event.name = name.empty() ? std::string(sourceFunction) : std::string(name);
    event.sourceFunction = std::string(sourceFunction);
    event.threadName = g_threadName;
    event.depth = g_scopeDepth;

    if (event.name.empty())
        event.name = "Unnamed Scope";

    uint64_t skippedDestinationCount = 0u;
    for (const auto& registration : registrations)
    {
        if (!RegistrationAccepts(registration, AcceptsCPUScopes))
        {
            ++skippedDestinationCount;
            continue;
        }

        ProfilerDestinationLease lease(registration);
        if (!lease)
        {
            ++skippedDestinationCount;
            continue;
        }

        event.destinationRegistrations.push_back(registration);
        event.active = true;
        lease->BeginScope(event);
    }

    if (!event.active)
    {
        std::lock_guard lock(g_profilerMutex);
        ++g_stats.droppedEventCount;
        g_stats.droppedEventCount += skippedDestinationCount;
        return event;
    }

    {
        std::lock_guard lock(g_profilerMutex);
        ++g_stats.acceptedEventCount;
        g_stats.droppedEventCount += skippedDestinationCount;
    }

    ++g_scopeDepth;
    return event;
}

void Profiler::EndScope(const ProfilerScopeEvent& event)
{
    if (!event.active)
        return;

    for (const auto& registration : event.destinationRegistrations)
    {
        ProfilerDestinationLease lease(registration);
        if (lease)
            lease->EndScope(event);
    }

    if (g_scopeDepth > 0u)
        --g_scopeDepth;
}

ProfilerGpuScopeEvent Profiler::BeginGpuScope(
    void* nativeCommandBuffer,
    const std::string_view name,
    const std::string_view sourceFunction)
{
    ProfilerGpuScopeEvent event;

    const bool enabled = IsEnabled();
    if (!enabled)
        return event;

    auto registrations = GetRegisteredDestinations();
    if (registrations.empty())
    {
        std::lock_guard lock(g_profilerMutex);
        ++g_stats.droppedEventCount;
        return event;
    }

    event.nativeCommandBuffer = nativeCommandBuffer;
    event.name = name.empty() ? std::string(sourceFunction) : std::string(name);
    event.sourceFunction = std::string(sourceFunction);
    event.threadName = g_threadName;
    event.depth = g_gpuScopeDepth;

    if (event.name.empty())
        event.name = "Unnamed GPU Scope";

    uint64_t skippedDestinationCount = 0u;
    for (const auto& registration : registrations)
    {
        if (!RegistrationAccepts(registration, AcceptsGPUScopes))
        {
            ++skippedDestinationCount;
            continue;
        }

        ProfilerDestinationLease lease(registration);
        if (!lease)
        {
            ++skippedDestinationCount;
            continue;
        }

        event.destinationRegistrations.push_back(registration);
        event.active = true;
        lease->BeginGpuScope(event);
    }

    if (!event.active)
    {
        std::lock_guard lock(g_profilerMutex);
        ++g_stats.droppedEventCount;
        g_stats.droppedEventCount += skippedDestinationCount;
        return event;
    }

    {
        std::lock_guard lock(g_profilerMutex);
        ++g_stats.acceptedEventCount;
        g_stats.droppedEventCount += skippedDestinationCount;
    }

    ++g_gpuScopeDepth;
    return event;
}

void Profiler::EndGpuScope(const ProfilerGpuScopeEvent& event)
{
    if (!event.active)
        return;

    for (const auto& registration : event.destinationRegistrations)
    {
        ProfilerDestinationLease lease(registration);
        if (lease)
            lease->EndGpuScope(event);
    }

    if (g_gpuScopeDepth > 0u)
        --g_gpuScopeDepth;
}

void Profiler::InitializeGpuContext(const ProfilerGpuContextEvent& event)
{
    {
        std::lock_guard lock(g_profilerMutex);
        g_lastGpuContext = event;
        g_hasGpuContext = true;
    }

    if (!IsEnabled())
        return;

    auto registrations = GetRegisteredDestinations();
    for (const auto& registration : registrations)
    {
        if (!RegistrationAccepts(registration, AcceptsGPUContextInitialization))
            continue;

        ProfilerDestinationLease lease(registration);
        if (lease)
            lease->InitializeGpuContext(event);
    }
}

void Profiler::SubmitGpuCommandLists(const ProfilerGpuCommandListSubmitEvent& event)
{
    if (!IsEnabled())
        return;

    auto registrations = GetRegisteredDestinations();
    for (const auto& registration : registrations)
    {
        if (!RegistrationAccepts(registration, AcceptsGPUScopes))
            continue;

        ProfilerDestinationLease lease(registration);
        if (lease)
            lease->SubmitGpuCommandLists(event);
    }
}

ProfilerSessionStats Profiler::GetSessionStats()
{
    std::lock_guard lock(g_profilerMutex);
    return g_stats;
}
}
