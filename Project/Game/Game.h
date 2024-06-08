#pragma once
namespace NLS
{
namespace Engine
{
class Game
{
public:
    Game();
    ~Game();

    virtual void UpdateGame(float dt);

};
} // namespace Engine
} // namespace NLS
