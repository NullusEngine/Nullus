#include "EmissionScheduler.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace NLS::VFX
{
bool EmissionScheduler::Initialize(const EmitterAsset& asset, std::string& error)
{
    if (!asset.capacity || asset.capacity > 10000000 || asset.initialCapacity > asset.capacity || !std::isfinite(asset.spawnRate) ||
        asset.spawnRate < 0 || asset.spawnRate > 10000000 || !std::isfinite(asset.duration) ||
        asset.duration < 0.0001 || asset.bursts.size() > 65536)
    { error = "Invalid emitter capacity, spawn rate, duration or burst count"; return false; }
    for (const auto& burst : asset.bursts)
        if (!std::isfinite(burst.time) || burst.time < 0 || burst.time >= asset.duration || burst.count > 10000000)
        { error = "Burst time must be inside the emitter duration"; return false; }
    m_capacity = asset.capacity; m_spawnRate = asset.spawnRate; m_duration = asset.duration;
    m_bursts = asset.bursts; m_looping = asset.looping; m_interpolated = asset.interpolatedSpawning;
    m_initialized = true;
    Reset();
    error.clear(); return true;
}

void EmissionScheduler::Reset()
{
    m_age = 0; m_remainder = 0; m_nextId = 1; m_tick = 0; m_spawning = m_initialized; m_pendingBurstCount = 0;
}

bool EmissionScheduler::SetSpawnRate(const double rate)
{
    if (!std::isfinite(rate) || rate < 0 || rate > 10000000) return false;
    m_spawnRate = rate;
    return true;
}
bool EmissionScheduler::SetDuration(const double duration)
{
    if (!std::isfinite(duration) || duration < 0.0001 || duration > 1.0e9) return false;
    m_duration = duration;
    return true;
}
bool EmissionScheduler::SetLooping(const bool looping)
{
    m_looping = looping;
    return true;
}
void EmissionScheduler::QueueBurst(const uint32_t count)
{
    if (count > 0) m_pendingBurstCount += count;
}

bool EmissionScheduler::ConsumeNextId(const uint64_t count)
{
    if (count > (std::numeric_limits<uint64_t>::max)()-m_nextId) return false;
    m_nextId += count;
    return true;
}

bool EmissionScheduler::Advance(double deltaTime, EmissionStep& output)
{
    if (!m_initialized || !std::isfinite(deltaTime) || deltaTime <= 0 || deltaTime > 1 ||
        !std::isfinite(m_age+deltaTime) || m_age+deltaTime > 1e9) return false;
    EmissionStep result;
    result.tick = m_tick; result.deltaTime = static_cast<float>(deltaTime);
    const auto nextAge = m_age+deltaTime;
    double nextRemainder = m_remainder;
    if (m_spawning)
    {
        struct Source { double time, period; uint64_t occurrences; uint32_t count, order; };
        const auto later = [](const Source& a, const Source& b)
        { return a.time != b.time ? a.time > b.time : a.order > b.order; };
        std::priority_queue<Source,std::vector<Source>,decltype(later)> sources(later);
        const auto activeDelta = m_looping ? deltaTime : (std::max)(0.0,(std::min)(nextAge,m_duration)-m_age);
        const auto requested = static_cast<uint64_t>(std::floor(m_remainder+m_spawnRate*activeDelta+1e-9));
        nextRemainder = (std::max)(0.0,m_remainder+m_spawnRate*activeDelta-static_cast<double>(requested));
        result.requested = requested;
        if (requested) sources.push({m_age+(1-m_remainder)/m_spawnRate,1/m_spawnRate,requested,1,0});
        if (m_pendingBurstCount)
        {
            if (m_pendingBurstCount > (std::numeric_limits<uint64_t>::max)()-result.requested) return false;
            result.requested += m_pendingBurstCount;
            sources.push({m_age,m_duration,1,m_pendingBurstCount,0x80000000u});
            m_pendingBurstCount = 0;
        }
        for (uint32_t index = 0; index < m_bursts.size(); ++index)
        {
            const auto& burst = m_bursts[index];
            if (!burst.count) continue;
            const auto occurrences = [&](double time)
            {
                if (time+1e-12 < burst.time) return uint64_t{0};
                return m_looping ? static_cast<uint64_t>(std::floor((time-burst.time+1e-12)/m_duration))+1 : uint64_t{1};
            };
            const auto before = m_tick ? occurrences(m_age) : uint64_t{0};
            const auto after = occurrences(nextAge);
            if (after <= before) continue;
            const auto births = (after-before)*burst.count;
            if (births > (std::numeric_limits<uint64_t>::max)()-result.requested) return false;
            result.requested += births;
            sources.push({burst.time+(m_looping ? before*m_duration : 0),m_duration,after-before,burst.count,index+1});
        }
        if (result.requested > (std::numeric_limits<uint64_t>::max)()-m_nextId) return false;
        uint32_t represented = 0;
        while (!sources.empty() && represented < m_capacity)
        {
            auto source = sources.top(); sources.pop();
            uint64_t run = 1;
            if (source.count == 1)
            {
                run = (std::min)(source.occurrences,static_cast<uint64_t>(m_capacity-represented));
                if (!sources.empty())
                {
                    const auto& next = sources.top();
                    run = (std::min)(run,static_cast<uint64_t>((std::max)(1.0,std::floor((next.time-source.time)/source.period)+1)));
                    while (run > 1 && later({source.time+(run-1)*source.period,0,0,0,source.order},next)) --run;
                }
            }
            const auto count = static_cast<uint32_t>((std::min)(run*source.count,static_cast<uint64_t>(m_capacity-represented)));
            result.batches.push_back({m_nextId+represented,count,
                m_interpolated ? static_cast<float>(std::clamp(nextAge-source.time,0.0,deltaTime)) : 0,
                m_interpolated && source.count == 1 ? static_cast<float>(source.period) : 0});
            represented += count;
            source.occurrences -= run;
            if (source.occurrences) { source.time += run*source.period; sources.push(source); }
        }
    }
    m_age = nextAge; m_remainder = nextRemainder; m_nextId += result.requested; ++m_tick; m_pendingBurstCount = 0;
    if (!m_looping && m_age >= m_duration) m_spawning = false;
    output = std::move(result); return true;
}
}
