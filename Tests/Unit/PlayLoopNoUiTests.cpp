// Compile the shared session a second time with game UI disabled so this test
// exercises the actual conditional build path without changing the main build.
#include <Scripting/ScriptRuntime.h>
#include <Scripting/ScriptEngineApi.h>
#include <Scripting/ScriptComponent.h>
#include <Scripting/InputScriptApi.h>
#undef NLS_ENABLE_GAME_UI
// Import shared scripting APIs normally. Only the locally compiled session has
// a different name; it can coexist with the real session in the aggregate suite.
#pragma push_macro("NLS_SCRIPTING_API")
#undef NLS_SCRIPTING_API
#define NLS_SCRIPTING_API
#define RuntimePlaySession NoUiRuntimePlaySession
#include <Scripting/RuntimePlaySession.cpp>
#undef RuntimePlaySession
#pragma pop_macro("NLS_SCRIPTING_API")
#include <gtest/gtest.h>

TEST(PlayLoopNoUiTests, SimulationContinuesAndPauseDoesNotAccumulateTime)
{
    NLS::Scripting::ScriptRuntime scripts;
    ASSERT_TRUE(scripts.Initialize({}).Succeeded());
    NLS::Engine::SceneSystem::SceneManager scenes("");
    NLS::Scripting::NoUiRuntimePlaySession session;
    ASSERT_TRUE(session.Begin(scripts, scenes, 1).Succeeded());
    scenes.GetCurrentScene()->Play();
    session.Tick(0.04f);
    EXPECT_EQ(session.GetFrame().fixedFrameIndex, 2u);
    EXPECT_EQ(session.GetFrame().frameIndex, 1u);
    session.SetPaused(true);
    session.Tick(10.0f);
    EXPECT_EQ(session.GetFrame().frameIndex, 1u);
    session.SetPaused(false);
    session.Step();
    EXPECT_EQ(session.GetFrame().frameIndex, 2u);
    EXPECT_EQ(session.GetFrame().fixedFrameIndex, 3u);
    session.End();
}
