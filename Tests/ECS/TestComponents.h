#pragma once
#include <ECS/Core/World.h>
#include <gtest/gtest.h>
#include <array>
#include <string>

namespace ECSTest
{
struct Position { float x = 0, y = 0, z = 0; bool operator==(const Position&) const = default; };
struct Velocity { float x = 0, y = 0, z = 0; };
struct Tag {};
struct Group { int value = 0; bool operator==(const Group&) const = default; };
struct Active { int value = 0; };
struct alignas(256) Aligned { int value = 0; };
struct Large { std::array<std::byte, 32 * 1024> bytes{}; };
struct Tracked
{
    inline static int alive = 0, moves = 0, copies = 0;
    inline static bool failConstruction = false, failCopy = false;
    std::string text = "owned";
    std::vector<std::string> values{"buffer"};
    Tracked() { if (failConstruction) throw std::runtime_error("construction failure"); ++alive; }
    explicit Tracked(std::string value) : text(std::move(value)) { ++alive; }
    Tracked(const Tracked& other) : text(other.text), values(other.values)
    { if (failCopy) throw std::runtime_error("copy failure"); ++alive; ++copies; }
    Tracked(Tracked&& other) noexcept : text(std::move(other.text)), values(std::move(other.values)) { ++alive; ++moves; }
    ~Tracked() { --alive; }
};
inline void Register(NLS::ECS::World& world)
{
    using namespace NLS::ECS;
    world.RegisterComponent<Position>("tests.Position"); world.RegisterComponent<Velocity>("tests.Velocity");
    world.RegisterComponent<Tag>("tests.Tag", ComponentKind::Tag);
    world.RegisterComponent<Group>("tests.Group", ComponentKind::Shared);
    world.RegisterComponent<Active>("tests.Active", ComponentKind::Enableable);
    world.RegisterComponent<DynamicBuffer<std::string>>("tests.StringBuffer", ComponentKind::Buffer);
    world.RegisterComponent<Tracked>("tests.Tracked");
    world.RegisterComponent<Aligned>("tests.Aligned"); world.RegisterComponent<Large>("tests.Large");
}
template<class... T> auto Signature(const NLS::ECS::World& world)
{ return std::array<NLS::ECS::ComponentId, sizeof...(T)>{world.GetComponentType<T>().id...}; }
}
