#include "HeapAllocationCounter.h"
#include <Input/InputDomain.h>
#include <Json/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace NLS;
using namespace NLS::Input;
using namespace InputBenchmark;
namespace
{
using Clock = std::chrono::steady_clock;
std::atomic<uint64_t> checksum{0};
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
Guid Id(std::string_view name) { return Guid::NewDeterministic(name); }

InputDefinition Buttons(size_t count, bool sharedControl = false)
{
    InputDefinition data; data.id = Id("benchmark");
    ContextDefinition context; context.id = Id("benchmark.context"); context.name = "benchmark";
    context.consumption = Consumption::Never;
    for (size_t i = 0; i < count; ++i)
    {
        const auto name = "action" + std::to_string(i);
        ActionDefinition action; action.id = Id(name); action.name = name;
        BindingDefinition binding; binding.id = Id(name + ".key"); binding.action = action.id;
        binding.parts = {{{DeviceKind::Keyboard, ControlKind::Button, static_cast<uint16_t>(sharedControl ? 32 : 32 + i)}, {1}}};
        data.actions.push_back(action); context.bindings.push_back(binding);
    }
    data.contexts.push_back(std::move(context)); return data;
}
std::shared_ptr<const CompiledInput> Compile(InputDefinition data)
{
    std::vector<std::string> diagnostics;
    auto compiled = CompiledInput::Compile(std::move(data), diagnostics);
    Require(compiled != nullptr && diagnostics.empty(), "Invalid benchmark definition");
    return compiled;
}
struct Fixture
{
    double time = 0;
    InputSystem system{[this] { return time; }};
    InputDomain domain{system, "benchmark"};
    std::shared_ptr<const CompiledInput> definition;
    std::vector<std::shared_ptr<InputUser>> users;
    std::vector<InputSubscription> subscriptions;
    uint64_t callbacks = 0;
    uint64_t tick = 0;
    uint64_t frame = 0;
    uint64_t dynamicEvents = 0;
    uint64_t fixedEvents = 0;
    explicit Fixture(InputDefinition data, size_t count = 1, bool fixed = false) : definition(Compile(std::move(data)))
    {
        for (size_t i = 0; i < count; ++i)
        {
            auto user = domain.CreateUser(); user->SetDefinition(definition);
            user->PushContext(definition->GetDefinition().contexts[0].id); user->SetFixedBufferEnabled(fixed);
            users.push_back(std::move(user));
        }
    }
    InputUser& User() { return *users[0]; }
    ActionId Action(size_t index = 0) { return definition->GetDefinition().actions[index].id; }
    void Event(DeviceId device, InputEventType type, uint16_t control = 0, double x = 0, double y = 0)
    {
        InputEvent event; event.device = device; event.type = type; event.control = control; event.x = x; event.y = y;
        Require(system.SubmitEvent(event), "Benchmark event was rejected");
    }
    void Key(bool down, uint16_t control = 32) { Event(system.GetKeyboard(), down ? InputEventType::ButtonDown : InputEventType::ButtonUp, control); }
    void Update(size_t fixedSteps = 0)
    {
        time += 1.0 / 120.0;
        system.FinishEventPump(); domain.Update(time);
        for (auto& user : users) dynamicEvents += user->GetActionEvents().size();
        for (size_t i = 0; i < fixedSteps; ++i)
        {
            domain.BeginFixedTick(++tick, 1.0 / 60.0);
            for (auto& user : users) fixedEvents += user->GetFixedFrame().events.size();
        }
        system.EndFrame(); ++frame;
    }
    void SubscribeAll()
    {
        for (auto& user : users)
            for (const auto& action : definition->GetDefinition().actions)
                subscriptions.push_back(user->BindAction(action.id, ActionPhase::Performed, [this](const auto&) { ++callbacks; }));
    }
};
struct Options { size_t warmup = 256, samples = 64, batch = 32; bool allowAllocations = false; std::string output; };
struct Runner
{
    Options options;
    nlohmann::json results = nlohmann::json::array();
    bool passed = true;
    template<class Function>
    void Measure(const char* name, const char* workload, Function&& step, bool structural = false, size_t queries = 0)
    {
        for (size_t i = 0; i < options.warmup; ++i) step();
        std::vector<double> samples(options.samples);
        for (auto& sample : samples)
        {
            const auto start = Clock::now();
            for (size_t i = 0; i < options.batch; ++i) step();
            sample = std::chrono::duration<double, std::micro>(Clock::now() - start).count() / options.batch;
        }
        // A separate pass prevents allocation instrumentation from distorting
        // the reported timing. No reporting/sample-vector allocations included.
        AllocationCounts counts;
        const size_t iterations = options.samples * options.batch;
        {
            AllocationScope scope(counts);
            for (size_t i = 0; i < iterations; ++i) step();
        }
        std::sort(samples.begin(), samples.end());
        const auto percentile = [&](double p) { return samples[static_cast<size_t>(std::ceil(p * samples.size())) - 1]; };
        const bool clean = structural || counts.allocations == 0;
        passed &= clean;
        auto entry = nlohmann::json{{"name", name}, {"workload", workload}, {"iterations_per_pass", iterations},
            {"warmup", options.warmup}, {"samples", options.samples}, {"batch_size", options.batch},
            {"p50_us", percentile(.50)}, {"p95_us", percentile(.95)}, {"p99_us", percentile(.99)},
            {"min_us", samples.front()}, {"max_us", samples.back()}, {"allocations", counts.allocations},
            {"allocated_bytes", counts.bytes}, {"frees", counts.frees}, {"structural_allocations_allowed", structural},
            {"allocation_gate_passed", clean}};
        if (queries) entry["p50_ns_per_query"] = percentile(.50) * 1000 / queries;
        results.push_back(std::move(entry));
        std::cout << name << ": p50=" << percentile(.50) << " us p95=" << percentile(.95)
            << " us, allocations=" << counts.allocations << " bytes=" << counts.bytes
            << (structural ? " (structural)" : clean ? " PASS" : " FAIL") << '\n';
    }
};

void Run(Runner& runner)
{
    runner.Measure("empty_harness", "Loop/call/checksum baseline; no input", [] { checksum.fetch_add(1, std::memory_order_relaxed); });
    for (const bool held : {false, true})
    {
        Fixture fixture(Buttons(128, true));
        if (held) { fixture.Key(true); fixture.Update(); }
        runner.Measure(held ? "held_128" : "idle_128", "1 user, 128 actions/bindings, 0 raw events, fixed buffering off", [&] { fixture.Update(); });
        Require(fixture.User().GetActionState(fixture.User().FindAction(fixture.Action(127))).actuated == held, "Held snapshot mismatch");
        Require(fixture.User().GetPendingFixedEventCount() == 0 && fixture.User().GetActionEvents().empty(), "Idle queues grew");
    }
    {
        Fixture fixture(Buttons(128)); fixture.SubscribeAll();
        runner.Measure("four_taps_128", "1 user, 128 actions/subscriptions, 8 ordered raw events, 4 short taps/update", [&]
        {
            const auto before = fixture.callbacks;
            for (size_t i = 0; i < 4; ++i) { fixture.Key(true); fixture.Key(false); }
            fixture.Update(); Require(fixture.callbacks == before + 4, "A same-frame tap was lost/duplicated");
        });
    }
    {
        Fixture fixture(Buttons(128, true)); fixture.SubscribeAll();
        runner.Measure("fanout_128", "1 user, 128 actions/subscriptions driven by 1 control, 128 callbacks/update", [&]
        {
            const auto before = fixture.callbacks; fixture.Key(true); fixture.Key(false); fixture.Update();
            Require(fixture.callbacks == before + 128, "Callback fanout mismatch");
        });
    }
    for (const auto consumption : {Consumption::OnActuated, Consumption::OnStarted, Consumption::OnPerformed})
    {
        auto data = Buttons(128, true); data.contexts[0].consumption = consumption;
        Fixture fixture(std::move(data)); fixture.SubscribeAll();
        const auto* name = consumption == Consumption::OnActuated ? "consume_actuated_128" :
            consumption == Consumption::OnStarted ? "consume_started_128" : "consume_performed_128";
        runner.Measure(name, "128 actions/subscriptions, 2 raw events, context consumption policy", [&]
        {
            const auto before = fixture.callbacks; fixture.Key(true); fixture.Key(false); fixture.Update();
            Require(fixture.callbacks == before + 128, "Context consumption changed same-group action fanout");
        });
    }
    {
        auto data = Buttons(128); data.contexts[0].consumption = Consumption::OnActuated;
        for (auto& binding : data.contexts[0].bindings)
        {
            binding.chord.matchModifiers = true;
            binding.chord.modifierMatch = ModifierMatch::Exact;
            binding.chord.modifiers = Modifier::Primary;
        }
        Fixture fixture(std::move(data)); fixture.SubscribeAll();
#ifdef __APPLE__
        fixture.Key(true, 343);
#else
        fixture.Key(true, 341);
#endif
        fixture.Update();
        runner.Measure("editor_exact_primary_128", "128 exact Primary-modifier bindings/subscriptions, 2 short taps/update", [&]
        {
            const auto before = fixture.callbacks;
            for (size_t i = 0; i < 2; ++i) { fixture.Key(true); fixture.Key(false); }
            fixture.Update(); Require(fixture.callbacks == before + 2, "Exact Primary chord did not perform once per tap");
        });
    }
    {
        auto data = Buttons(1); auto alias = data.contexts[0].bindings[0]; alias.id = Id("alias"); alias.parts[0].path.control = 257;
        data.contexts[0].bindings.push_back(alias); Fixture fixture(std::move(data)); fixture.SubscribeAll();
        runner.Measure("two_bindings", "1 action, 2 simultaneously held bindings; partial release must not release action", [&]
        {
            const auto before = fixture.callbacks;
            fixture.Key(true); fixture.Key(true, 257); fixture.Key(false); fixture.Update();
            Require(fixture.User().GetActionState(fixture.User().FindAction(fixture.Action())).actuated, "Partial binding release released action");
            fixture.Key(false, 257); fixture.Update();
            Require(fixture.callbacks == before + 1, "Multiple bindings performed twice");
        });
    }
    {
        Fixture fixture(Buttons(128), 4, true); fixture.SubscribeAll();
        runner.Measure("four_users_128_fixed", "4 users, shared definition/devices, 128 actions/user, 2 raw events, 1 fixed step", [&]
        {
            const auto before = fixture.callbacks; fixture.Key(true); fixture.Key(false); fixture.Update(1);
            Require(fixture.callbacks == before + 4, "User callback mismatch");
        });
        Require(fixture.dynamicEvents == fixture.fixedEvents, "Fixed events were lost/duplicated between users");
    }
    {
        auto data = Buttons(3);
        auto& look = data.actions[1]; look.type = ValueType::Vector2; look.nature = ValueNature::Delta;
        auto& mouse = data.contexts[0].bindings[1]; mouse.nature = ValueNature::Delta;
        mouse.parts = {{{DeviceKind::Mouse, ControlKind::PointerDelta}, {1, 1}}};
        auto& rate = data.actions[2]; rate.type = ValueType::Vector2; rate.nature = ValueNature::Rate;
        auto& stick = data.contexts[0].bindings[2]; stick.composite = Composite::Vector2; stick.nature = ValueNature::Rate;
        stick.parts = {{{DeviceKind::Gamepad, ControlKind::Axis, 0}, {1, 0}}, {{DeviceKind::Gamepad, ControlKind::Axis, 1}, {0, 1}}};
        stick.processors.push_back({ProcessorKind::Deadzone});
        Fixture fixture(std::move(data), 1, true);
        const auto gamepad = fixture.system.RegisterDevice({DeviceKind::Gamepad, "Virtual benchmark gamepad"}); fixture.User().PairDevice(gamepad);
        runner.Measure("mixed_delta_rate_fixed_0_1_3", "3 actions, 1 user, pointer delta + gamepad rate + short tap, cyclic 0/1/3 fixed steps", [&]
        {
            fixture.Key(true); fixture.Key(false);
            fixture.Event(fixture.system.GetMouse(), InputEventType::PointerDelta, 0, 2, -1);
            fixture.Event(gamepad, InputEventType::Axis, 0, fixture.frame % 2 ? .8 : -.8);
            fixture.Event(gamepad, InputEventType::Axis, 1, .4);
            constexpr size_t steps[]{0, 1, 3}; fixture.Update(steps[fixture.frame % 3]);
            const auto delta = fixture.User().GetActionValue(fixture.User().FindAction(fixture.Action(1)));
            Require(delta == InputValue(2, -1), "Delta was mixed with rate or dropped");
        });
        fixture.Update(1);
        Require(fixture.dynamicEvents == fixture.fixedEvents && fixture.User().GetPendingFixedEventCount() == 0, "Fixed 0/1/3 event consumption mismatch");
    }
    {
        auto data = Buttons(3, true);
        data.actions[0].interaction = {InteractionKind::Hold, .05};
        data.actions[1].interaction = {InteractionKind::Tap, .05};
        data.actions[2].interaction = {InteractionKind::Repeat, .05, .01};
        Fixture fixture(std::move(data)); fixture.SubscribeAll(); fixture.Key(true); fixture.Update();
        runner.Measure("timers_without_events", "Hold/Tap/Repeat, 0 new raw events; Repeat continues on active real time", [&] { fixture.Update(); });
        Require(fixture.callbacks > 2 && fixture.User().GetPendingFixedEventCount() == 0, "Timers stopped without raw events");
    }
    {
        Fixture fixture(Buttons(1));
        fixture.subscriptions.push_back(fixture.User().BindAction(fixture.Action(), ActionPhase::Performed, [&](const auto&)
        {
            ++fixture.callbacks;
            fixture.User().SetDeviceBlocked(DeviceKind::Mouse, true);
            fixture.User().SetDeviceBlocked(DeviceKind::Mouse, false);
        }));
        runner.Measure("callback_mutations", "Performed callback queues two routing mutations; safe boundary applies both", [&]
        {
            const auto before = fixture.callbacks; fixture.Key(true); fixture.Key(false); fixture.Update();
            Require(fixture.callbacks == before + 1, "Callback mutation changed dispatch count");
        });
    }
    {
        Fixture fixture(Buttons(1)); size_t changes = 0;
        runner.Measure("domain_changes", "1 small queued domain change/update, retaining boundary semantics", [&]
        {
            const auto before = changes;
            fixture.domain.EnqueueChange([&] { ++changes; }); fixture.Update();
            Require(changes == before + 1, "Queued domain change was lost");
        });
    }
    {
        Fixture fixture(Buttons(1)); fixture.SubscribeAll();
        const auto context = fixture.definition->GetDefinition().contexts[0].id;
        const auto action = fixture.User().FindAction(fixture.Action());
        runner.Measure("context_require_release", "Disable/re-enable held action, cancellation, RequireRelease then new press; 6 updates/operation", [&]
        {
            const auto before = fixture.callbacks;
            fixture.Key(true); fixture.Update();
            fixture.User().DisableContext(context); fixture.Update();
            auto state = fixture.User().GetActionState(action);
            Require(state.canceled && !state.released && !state.actuated, "Context disable fabricated release or lost cancellation");
            fixture.User().EnableContext(context); fixture.Update();
            Require(!fixture.User().GetActionState(action).actuated, "Held key reactivated without release");
            fixture.Key(false); fixture.Update(); fixture.Key(true); fixture.Update(); fixture.Key(false); fixture.Update();
            Require(fixture.callbacks == before + 2, "Context recovery lost/duplicated a press");
        });
    }
    {
        Fixture fixture(Buttons(128)); const auto id = fixture.Action(127); const auto handle = fixture.User().FindAction(id);
        const auto name = fixture.definition->GetDefinition().actions.back().name;
        for (size_t lookup = 0; lookup < 3; ++lookup)
            runner.Measure(lookup == 0 ? "query_handle" : lookup == 1 ? "query_id" : "query_name",
                "1024 queries, 128-action definition; handle vs stable ID vs initialization-time name lookup", [&]
            {
                uint64_t value = 0;
                for (size_t i = 0; i < 1024; ++i)
                {
                    const auto resolved = lookup == 0 ? handle : lookup == 1 ? fixture.User().FindAction(id) : fixture.User().FindAction(name);
                    value += resolved.index + fixture.User().GetActionState(resolved).actuated;
                }
                checksum.fetch_add(value, std::memory_order_relaxed);
            }, false, 1024);
    }
    {
        const auto data = Buttons(128);
        runner.Measure("compile_128", "Cold definition compile; allocations expected and outside input update", [&]
        { const auto compiled = Compile(data); checksum.fetch_add(compiled->GetDefinition().actions.size(), std::memory_order_relaxed); }, true);
        Fixture fixture(data); BindingOverride binding;
        binding.binding = data.contexts[0].bindings[0].id; binding.state = OverrideState::Override;
        binding.parts = {{{DeviceKind::Keyboard, ControlKind::Button, 257}, {1}}};
        runner.Measure("profile_override", "Apply user binding override; structural copies/cancellation, not steady input", [&]
        { fixture.User().SetBindingOverride(binding); fixture.Update(); }, true);
    }
}
}

int main(int argc, char** argv)
{
    try
    {
#ifndef NDEBUG
        throw std::runtime_error("Use an optimized Release/RelWithDebInfo build for this benchmark");
#endif
        Runner runner;
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view argument(argv[i]);
            if (argument == "--allow-allocations") runner.options.allowAllocations = true;
            else
            {
                Require(i + 1 < argc, "Missing argument value");
                if (argument == "--output") runner.options.output = argv[++i];
                else if (argument == "--samples") runner.options.samples = std::stoull(argv[++i]);
                else if (argument == "--batch") runner.options.batch = std::stoull(argv[++i]);
                else if (argument == "--warmup") runner.options.warmup = std::stoull(argv[++i]);
                else throw std::runtime_error("Unknown benchmark argument");
            }
        }
        Require(runner.options.samples > 0 && runner.options.samples <= 4096 && runner.options.batch > 0 && runner.options.batch <= 4096,
            "Sample/batch count out of range");
        VerifyAllocationCounter(); Run(runner);
        nlohmann::json report{{"configuration", NLS_INPUT_BENCH_CONFIG},
            {"pointer_bits", sizeof(void*) * 8}, {"counter_calibration_passed", true},
            {"timing", "steady_clock; per-operation batch percentiles; separate uninstrumented pass; no absolute timing gate"},
            {"allocation_scope", "current-thread C++ new/new[] including aligned/nothrow/sized delete; static production core; excludes C/OS heaps and external DLLs"},
            {"passed", runner.passed}, {"checksum", checksum.load()}, {"results", runner.results}};
#ifdef _MSC_VER
        report["msvc_version"] = _MSC_FULL_VER;
        report["cxx_standard"] = _MSVC_LANG;
#else
        report["cxx_standard"] = __cplusplus;
#endif
        if (!runner.options.output.empty())
        {
            std::ofstream output(runner.options.output); output << report.dump(2) << '\n';
            Require(output.good(), "Could not write benchmark report");
        }
        return runner.passed || runner.options.allowAllocations ? 0 : 1;
    }
    catch (const std::exception& error) { std::cerr << "Input benchmark failed: " << error.what() << '\n'; return 2; }
}
