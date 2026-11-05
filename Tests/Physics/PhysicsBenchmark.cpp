#include <Physics/PhysicsWorld.h>
#include <Physics/PhysicsWorld2D.h>
#include <TaskGraph/TaskGraph.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>
#endif

namespace
{
using namespace NLS;
using Clock = std::chrono::steady_clock;
constexpr int Warmup = 30, Samples = 120, Queries = 1000;
constexpr float Interval = 1.0f/60;
struct Result
{
    int dimension = 0, requested = 0;
    uint32_t bodies = 0, shapes = 0, active = 0, sleeping = 0;
    std::vector<double> milliseconds;
    double queryMilliseconds = 0;
    uint64_t queryHits = 0;
    std::optional<uint64_t> processWorkingSet;
};
std::optional<uint64_t> WorkingSet()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) return counters.WorkingSetSize;
#endif
    return {};
}
double Elapsed(Clock::time_point start)
{ return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
Result Spatial(int count, const std::shared_ptr<Base::TaskGraph::TaskGraph>& tasks)
{
    Physics::WorldSettings settings; settings.tasks=tasks;
    std::string error; auto world=Physics::PhysicsWorld::Create(settings,error);
    if (!world) throw std::runtime_error(error);
    Physics::ShapeSettings shape; shape.restitution=0;
    if (!world->AttachBox(world->CreateBody(),{100,0.5f,100},shape).IsValid()) throw std::runtime_error("Cannot create 3D floor");
    for (int i=0;i<count;++i)
    {
        Physics::BodySettings body; body.dynamic=true; body.kinematic=false;
        body.position={float(i%32)*1.1f,1+float(i/1024)*1.1f,float(i/32%32)*1.1f};
        if (!world->AttachBox(world->CreateBody(body),{0.49f,0.49f,0.49f},shape).IsValid()) throw std::runtime_error("Cannot create 3D body");
    }
    Result result; result.dimension=3; result.requested=count; result.milliseconds.reserve(Samples);
    for (int i=0;i<Warmup+Samples;++i)
    {
        const auto start=Clock::now(); world->Step(Interval); const double elapsed=Elapsed(start);
        if (i>=Warmup) result.milliseconds.push_back(elapsed);
        (void)world->TakeContactEvents();
    }
    {
        auto read=world->Read(); Physics::QueryHit hit;
        const auto start=Clock::now();
        for (int i=0;i<Queries;++i) result.queryHits+=read->Raycast({float(i%32)*1.1f,200,float(i/32%32)*1.1f},{0,-1,0},250,hit);
        result.queryMilliseconds=Elapsed(start);
    }
    const auto stats=world->GetStatistics(); result.bodies=stats.bodyCount; result.shapes=stats.shapeCount;
    result.active=stats.activeBodyCount; result.sleeping=stats.sleepingBodyCount; result.processWorkingSet=WorkingSet();
    return result;
}
Result Planar(int count, const std::shared_ptr<Base::TaskGraph::TaskGraph>& tasks)
{
    Physics::WorldSettings2D settings; settings.tasks=tasks;
    std::string error; auto world=Physics::PhysicsWorld2D::Create(settings,error);
    if (!world) throw std::runtime_error(error);
    if (!world->AddBox(world->CreateBody(),{200,0.5f}).IsValid()) throw std::runtime_error("Cannot create 2D floor");
    for (int i=0;i<count;++i)
    {
        Physics::BodySettings2D body; body.type=Physics::BodyType2D::Dynamic;
        body.position={float(i%100)*1.1f,1+float(i/100)*1.1f};
        if (!world->AddBox(world->CreateBody(body),{0.49f,0.49f}).IsValid()) throw std::runtime_error("Cannot create 2D body");
    }
    Result result; result.dimension=2; result.requested=count; result.milliseconds.reserve(Samples);
    for (int i=0;i<Warmup+Samples;++i)
    {
        const auto start=Clock::now(); world->Step(Interval); const double elapsed=Elapsed(start);
        if (i>=Warmup) result.milliseconds.push_back(elapsed);
    }
    {
        auto read=world->Read(); Physics::QueryHit2D hit;
        const auto start=Clock::now();
        for (int i=0;i<Queries;++i) result.queryHits+=read->Raycast({float(i%100)*1.1f,200},{0,-1},250,hit);
        result.queryMilliseconds=Elapsed(start);
    }
    const auto stats=world->GetStatistics(); result.bodies=stats.bodyCount; result.shapes=stats.shapeCount;
    result.active=stats.activeBodyCount; result.sleeping=stats.sleepingBodyCount; result.processWorkingSet=WorkingSet();
    return result;
}
void Write(std::ostream& output, std::vector<Result>& results)
{
    output<<"{\n  \"schema\": 1, \"configuration\": \""<<NLS_PHYSICS_BENCH_CONFIG<<"\",\n"
        <<"  \"scenario\": \"grid_drop_boxes\", \"physx\": \"5.6.1\", \"box2d\": \"3.2.0\",\n"
        <<"  \"logical_processors\": "<<std::thread::hardware_concurrency()<<", \"task_workers\": 2,\n"
        <<"  \"fixed_delta\": "<<Interval<<", \"box2d_solver_substeps\": 4, \"warmup_steps\": "<<Warmup<<", \"sample_steps\": "<<Samples<<",\n"
        <<"  \"step_metric\": \"PhysicsWorld::Step wall time; 3D event collection included, dispatch excluded\",\n"
        <<"  \"backend_memory_bytes\": null, \"backend_memory_reason\": \"not exposed by both adapters\",\n  \"results\": [\n";
    for (size_t i=0;i<results.size();++i)
    {
        auto& r=results[i]; std::sort(r.milliseconds.begin(),r.milliseconds.end());
        const auto percentile=[&](double p) { return r.milliseconds[static_cast<size_t>(p*(r.milliseconds.size()-1))]; };
        output<<"    {\"dimension\": "<<r.dimension<<", \"dynamic_bodies_requested\": "<<r.requested<<", \"body_count\": "<<r.bodies
            <<", \"shape_count\": "<<r.shapes<<", \"active_bodies\": "<<r.active<<", \"sleeping_bodies\": "<<r.sleeping
            <<", \"step_ms_p50\": "<<percentile(0.50)<<", \"step_ms_p95\": "<<percentile(0.95)<<", \"step_ms_p99\": "<<percentile(0.99)
            <<", \"queries\": "<<Queries<<", \"query_batch_ms\": "<<r.queryMilliseconds<<", \"query_hits\": "<<r.queryHits<<", \"process_working_set_bytes\": ";
        if(r.processWorkingSet) output<<*r.processWorkingSet; else output<<"null";
        output<<"}"<<(i+1==results.size()?"\n":",\n");
    }
    output<<"  ]\n}\n";
}
}
int main(int argc, char** argv)
{
    try
    {
        if(argc!=2) { std::cerr<<"Usage: NullusPhysicsBenchmark output.json\n"; return 2; }
        Base::TaskGraph::TaskGraphConfig config; config.workerCount=2;
        auto tasks=std::make_shared<Base::TaskGraph::TaskGraph>(config);
        if(!tasks->Start()) throw std::runtime_error("Cannot start benchmark tasks");
        std::vector<Result> results;
        for(int count:{1000,5000,10000})
        { std::cout<<"3D "<<count<<std::endl; results.push_back(Spatial(count,tasks)); std::cout<<"2D "<<count<<std::endl; results.push_back(Planar(count,tasks)); }
        tasks->Shutdown();
        std::ofstream output(argv[1]); if(!output) throw std::runtime_error("Cannot open benchmark output");
        Write(output,results); output.close(); if(!output) throw std::runtime_error("Cannot write benchmark output");
    }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
