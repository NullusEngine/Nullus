#include <ECS/Core/World.h>
#include <iostream>

namespace
{
struct Position { float x = 0, y = 0, z = 0; };
struct Velocity { float x = 0, y = 0, z = 0; };
}
int main()
{
    using namespace NLS::ECS;
    World world;
    world.RegisterComponent<Position>("sample.Position");
    world.RegisterComponent<Velocity>("sample.Velocity");
    auto entity = world.CreateEntity();
    world.AddComponent<Position>(entity);
    world.AddComponent<Velocity>(entity, {2, 0, 0});
    auto movement = world.Query<Write<Position>, Read<Velocity>>();
    for (int frame = 0; frame < 60; ++frame)
        movement.Each([](Entity, Position& position, const Velocity& velocity) { position.x += velocity.x / 60.0f; });
    std::cout << "Position after one second: " << world.ReadComponent<Position>(entity).x << '\n';
    world.DestroyEntity(entity);
    std::cout << "Old handle alive: " << std::boolalpha << world.IsAlive(entity) << '\n';
    return world.GetEntityCount() == 0 ? 0 : 1;
}
