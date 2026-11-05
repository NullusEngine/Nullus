#include "Jobs/JobSystem.h"
#include "TaskGraph/TaskGraph.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif
namespace J = NLS::Base::Jobs;
namespace T = NLS::Base::TaskGraph;
using Clock = std::chrono::steady_clock;
template <class F> double Time(F &&f)
{
    auto t = Clock::now();
    f();
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}
template <class F> double Median(F &&f)
{
    std::vector<double> v;
    for (int i = 0; i < 5; ++i)
        v.push_back(Time(f));
    std::sort(v.begin(), v.end());
    return v[2];
}
void Emit(const char *name, uint32_t workers, uint32_t grain, double ms)
{
    std::cout << name << ',' << workers << ',' << grain << ',' << ms << std::endl;
}
void NoOp(void *)
{
}
void RequireAccepted(J::JobHandle handle)
{
    if (handle.id == 0u)
        throw std::runtime_error("Benchmark job submission was rejected");
}
void Wait(T::TaskSubmission &submission)
{
    if (!submission.completion.WaitFor(std::chrono::seconds(30)) ||
        submission.completion.GetStatus() != T::TaskStatus::Succeeded)
        throw std::runtime_error("Benchmark graph failed or timed out");
}
void Work(void *data, uint32_t i)
{
    auto &out = *static_cast<std::vector<uint64_t> *>(data);
    uint64_t x = i + 12345;
    for (int j = 0; j < 48; ++j)
        x = (x * 6364136223846793005ULL + 1442695040888963407ULL) ^ (x >> 13);
    out[i] = x;
}
double CpuMs()
{
#if defined(_WIN32)
    FILETIME create{}, exit{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user);
    auto cv = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return double(cv(kernel) + cv(user)) / 10000.;
#else
    return double(std::clock()) * 1000. / CLOCKS_PER_SEC;
#endif
}
int main()
{
    std::cout << "metric,workers,grain,milliseconds_or_count" << std::endl;
    for (uint32_t workers : {1u, 4u})
    {
        J::JobSystemConfig jc;
        jc.workerCount = workers;
        jc.backgroundWorkerCount = 1;
        jc.enableDiagnostics = false;
        if (!J::InitializeJobSystem(jc))
            return 2;
        T::TaskGraphConfig tc;
        tc.workerCount = workers;
        tc.enableDiagnostics = false;
        T::TaskGraph graph(tc);
        if (!graph.Start())
            return 3;
        graph.RegisterCurrentThread(T::NamedThread::Game);
        Emit("jobs_single_1000", workers, 0, Median([&] {
                 for (int i = 0; i < 1000; ++i)
                 {
                     J::JobScheduleDesc d;
                     d.function = NoOp;
                     auto h = J::ScheduleJob(d);
                     RequireAccepted(h);
                     J::Complete(h);
                 }
             }));
        Emit("graph_single_1000", workers, 0, Median([&] {
                 for (int i = 0; i < 1000; ++i)
                 {
                     T::TaskDesc d;
                     d.function = [] {};
                     auto h = graph.SubmitTracked(std::move(d));
                     Wait(h);
                 }
             }));
        Emit("jobs_bulk_20000", workers, 0, Median([&] {
                 std::vector<J::JobHandle> hs;
                 hs.reserve(20000);
                 for (int i = 0; i < 20000; ++i)
                 {
                     J::JobScheduleDesc d;
                     d.function = NoOp;
                     const auto handle = J::ScheduleJob(d);
                     RequireAccepted(handle);
                     hs.push_back(handle);
                 }
                 J::CompleteAll(hs.data(), hs.size());
             }));
        std::vector<uint64_t> out(1u << 20);
        Emit("serial_compute_1m", workers, 0, Median([&] {
                 for (uint32_t i = 0; i < out.size(); ++i)
                     Work(&out, i);
             }));
        for (uint32_t grain : {1u, 64u, 1024u})
            Emit("parallel_compute_1m", workers, grain, Median([&] {
                     J::JobForEachDesc d;
                     d.function = Work;
                     d.userData = &out;
                     d.iterationCount = uint32_t(out.size());
                     d.batchSize = grain;
                     auto h = J::ScheduleJobForEach(d);
                     RequireAccepted(h);
                     J::Complete(h);
                 }));
        uint64_t checksum = 0;
        for (auto x : out)
            checksum ^= x;
        Emit("compute_checksum", workers, 0, double(checksum));
        const auto chainStealsBefore = graph.GetWorkerStealCount();
        const auto chainLocalBefore = graph.GetWorkerLocalExecutionCount();
        const auto chainInjectionBefore = graph.GetWorkerInjectionExecutionCount();
        std::vector<double> releaseTimes;
        Emit("graph_chain_submit_and_release_5000", workers, 0, Median([&] {
                 T::TaskDesc root;
                 root.function = [] {};
                 root.targetThread = T::NamedThread::Game;
                 auto prev = graph.SubmitTracked(std::move(root));
                 for (int i = 0; i < 5000; ++i)
                 {
                     T::TaskDesc d;
                     d.function = [] {};
                     d.prerequisites = {prev.handle};
                     prev = graph.SubmitTracked(std::move(d));
                 }
                 releaseTimes.push_back(Time([&] { Wait(prev); }));
             }));
        std::sort(releaseTimes.begin(), releaseTimes.end());
        Emit("graph_chain_release_5000", workers, 0, releaseTimes[2]);
        Emit("graph_chain_steals_5x5000", workers, 0,
             double(graph.GetWorkerStealCount() - chainStealsBefore));
        Emit("graph_chain_local_5x5000", workers, 0,
             double(graph.GetWorkerLocalExecutionCount() - chainLocalBefore));
        Emit("graph_chain_injection_5x5000", workers, 0,
             double(graph.GetWorkerInjectionExecutionCount() - chainInjectionBefore));
        std::unordered_map<std::thread::id, size_t> counts;
        std::mutex m;
        std::vector<T::TaskSubmission> leaves;
        leaves.reserve(128);
        const auto stealsBefore = graph.GetWorkerStealCount();
        T::TaskDesc parent;
        parent.function = [&] {
            for (int i = 0; i < 128; ++i)
            {
                T::TaskDesc leaf;
                leaf.function = [&] {
                    {
                        std::lock_guard lock(m);
                        ++counts[std::this_thread::get_id()];
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                };
                leaves.push_back(graph.SubmitTracked(std::move(leaf)));
            }
        };
        auto p = graph.SubmitTracked(std::move(parent));
        Wait(p);
        for (auto &h : leaves)
            Wait(h);
        size_t minimum = 128, maximum = 0;
        for (const auto &[thread, count] : counts)
        {
            minimum = std::min(minimum, count);
            maximum = std::max(maximum, count);
        }
        Emit("graph_distribution_threads", workers, 0, double(counts.size()));
        Emit("graph_distribution_min", workers, 0, double(minimum));
        Emit("graph_distribution_max", workers, 0, double(maximum));
        Emit("graph_distribution_steals", workers, 0, double(graph.GetWorkerStealCount() - stealsBefore));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        double cpu = CpuMs();
        double wall = Time([] { std::this_thread::sleep_for(std::chrono::milliseconds(500)); });
        Emit("idle_cpu_500ms", workers, 0, CpuMs() - cpu);
        Emit("idle_wall", workers, 0, wall);
        graph.Shutdown();
        J::ShutdownJobSystem();
        J::ResetJobSystemForTesting();
    }
}
