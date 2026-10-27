#pragma once

#include "VfxGraph.h"

namespace NLS::VFX
{
// A chronological run of births. IDs are reserved even when capacity drops births.
struct SpawnBatch
{
    uint64_t firstId = 0;
    uint32_t count = 0;
    float initialAge = 0;
    float ageStep = 0;
};

struct EmissionStep
{
    std::vector<SpawnBatch> batches;
    uint64_t requested = 0;
    uint64_t tick = 0;
    float deltaTime = 0;
};

// CPU-owned clock shared by both simulation targets; it never reads GPU counts.
class NLS_VFX_API EmissionScheduler
{
public:
    bool Initialize(const EmitterAsset& asset, std::string& error);
    void Reset();
    void Activate() { m_spawning = m_initialized; }
    void Deactivate() { m_spawning = false; }
    bool Advance(double deltaTime, EmissionStep& output);
    bool SetSpawnRate(double rate);
    bool SetDuration(double duration);
    bool SetLooping(bool looping);
    void QueueBurst(uint32_t count);
    [[nodiscard]] double SpawnRate() const { return m_spawnRate; }
    [[nodiscard]] double Duration() const { return m_duration; }
    [[nodiscard]] bool Looping() const { return m_looping; }
    [[nodiscard]] double Age() const { return m_age; }
    [[nodiscard]] bool IsSpawning() const { return m_spawning; }
    [[nodiscard]] uint64_t SimulationSteps() const { return m_tick; }
    // Reserved-id interface for frame-driven emission (Spawn Per Frame). The
    // caller takes the next id, then consumes exactly that many so the cursor
    // stays unique across both the scheduled and the frame-based paths.
    [[nodiscard]] uint64_t NextId() const { return m_nextId; }
    bool ConsumeNextId(uint64_t count);

private:
    std::vector<Burst> m_bursts;
    uint32_t m_capacity = 0;
    double m_spawnRate = 0;
    double m_duration = 0;
    double m_age = 0;
    double m_remainder = 0;
    uint64_t m_nextId = 1;
    uint64_t m_tick = 0;
    uint32_t m_pendingBurstCount = 0;
    bool m_looping = false;
    bool m_interpolated = false;
    bool m_initialized = false;
    bool m_spawning = false;
};
}
