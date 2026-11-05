#include <gtest/gtest.h>
#include <Physics/FixedStepClock.h>
#include <Physics/PhysicsPoseHistory.h>

#include <limits>

using namespace NLS::Physics;

TEST(PhysicsClockTests, FractionalFramesAccumulateAndMultipleStepsConsumeTimeOnce)
{
    FixedStepClock clock;
    std::string error;
    ASSERT_TRUE(clock.Configure({0.02, 8, 0.25}, error)) << error;
    EXPECT_EQ(clock.Advance(0.005).stepCount, 0u);
    const auto batch = clock.Advance(0.05);
    EXPECT_EQ(batch.stepCount, 2u);
    EXPECT_NEAR(batch.interpolationAlpha, 0.75, 1e-10);
    EXPECT_DOUBLE_EQ(batch.droppedTime, 0);
    EXPECT_EQ(clock.Advance(0.005).stepCount, 1u);
}

TEST(PhysicsClockTests, LongFrameHasBoundedStepsAndReportsAllDiscardedDebt)
{
    FixedStepClock clock;
    std::string error;
    ASSERT_TRUE(clock.Configure({0.02, 8, 0.25}, error));
    const auto batch = clock.Advance(1.0);
    EXPECT_EQ(batch.stepCount, 8u);
    EXPECT_NEAR(batch.droppedTime, 0.83, 1e-10);
    EXPECT_NEAR(clock.GetAccumulatedTime(), 0.01, 1e-10);
    EXPECT_NEAR(clock.GetDroppedTime(), 0.83, 1e-10);
    EXPECT_EQ(clock.Advance(0).stepCount, 0u);
}

TEST(PhysicsClockTests, InvalidConfigurationDoesNotReplaceTheLastValidSettings)
{
    FixedStepClock clock;
    std::string error;
    for (const auto settings : {FixedStepSettings{0, 8, 0.25}, FixedStepSettings{-1, 8, 0.25},
        FixedStepSettings{0.02, 0, 0.25}, FixedStepSettings{0.02, 1025, 0.25}, FixedStepSettings{0.5, 8, 0.25},
        FixedStepSettings{std::numeric_limits<double>::infinity(), 8, 1}})
    {
        EXPECT_FALSE(clock.Configure(settings, error));
        EXPECT_FALSE(error.empty());
    }
    EXPECT_NEAR(clock.GetSettings().fixedDeltaTime, 1.0 / 60.0, 1e-12);
}

TEST(PhysicsClockTests, NonFiniteAndNegativeDeltasDoNotCreateSimulationDebt)
{
    FixedStepClock clock;
    for (const double delta : {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        EXPECT_EQ(clock.Advance(delta).stepCount, 0u);
    EXPECT_EQ(clock.Advance(1.0 / 60.0).stepCount, 1u);
    clock.Reset();
    EXPECT_DOUBLE_EQ(clock.GetAccumulatedTime(), 0);
    EXPECT_DOUBLE_EQ(clock.GetDroppedTime(), 0);
}

TEST(PhysicsClockTests, ReconfigurationRetainsRemainderAndIndependentClocksStayIsolated)
{
    FixedStepClock first, second;
    std::string error;
    first.Advance(0.01);
    ASSERT_TRUE(first.Configure({0.005, 8, 0.25}, error));
    EXPECT_EQ(first.Advance(0).stepCount, 2u);
    EXPECT_EQ(second.Advance(0.01).stepCount, 0u);
}

TEST(PhysicsPoseHistoryTests, InterpolatesTranslationAndShortestArcRotation)
{
    PhysicsPoseHistory history; PhysicsPose pose;
    history.Reset({},0,1);
    PhysicsPose current; current.position={6,0,0}; current.rotation={0,0,std::sqrt(0.5f),std::sqrt(0.5f)};
    history.Publish(current,1,1); ASSERT_TRUE(history.Interpolate(0.5,pose));
    EXPECT_NEAR(pose.position.x,3,1e-6f);
    const auto direction=pose.rotation*NLS::Maths::Vector3{1,0,0};
    EXPECT_NEAR(direction.x,std::sqrt(0.5f),1e-5f); EXPECT_NEAR(direction.y,std::sqrt(0.5f),1e-5f);
    EXPECT_TRUE(history.Tracks(1,1)); EXPECT_FALSE(history.Tracks(2,1));
}

TEST(PhysicsPoseHistoryTests, ReadingTheSameStepTwiceDoesNotConsumeThePreviousSample)
{
    PhysicsPoseHistory history; PhysicsPose pose;
    history.Reset({},0,1); history.Publish({{10,0,0}},1,1); history.Publish({{10,0,0}},1,1);
    ASSERT_TRUE(history.Interpolate(0.25,pose)); EXPECT_NEAR(pose.position.x,2.5f,1e-6f);
}

TEST(PhysicsPoseHistoryTests, TeleportRestoreAndMissingStepsResetBothSamples)
{
    PhysicsPoseHistory history; PhysicsPose pose;
    history.Reset({},0,1); history.Publish({{10,0,0}},1,1);
    history.Publish({{30,0,0}},1,2); ASSERT_TRUE(history.Interpolate(0,pose)); EXPECT_FLOAT_EQ(pose.position.x,30);
    history.Publish({{32,0,0}},2,2); ASSERT_TRUE(history.Interpolate(0.5,pose)); EXPECT_FLOAT_EQ(pose.position.x,31);
    history.Publish({{100,0,0}},8,2); ASSERT_TRUE(history.Interpolate(0.5,pose)); EXPECT_FLOAT_EQ(pose.position.x,100);
    history.Publish({{2,0,0}},1,3); ASSERT_TRUE(history.Interpolate(0.5,pose)); EXPECT_FLOAT_EQ(pose.position.x,2);
}

TEST(PhysicsPoseHistoryTests, AlphaIsClampedAndClearedHistoriesCannotPublishStalePoses)
{
    PhysicsPoseHistory history; PhysicsPose pose;
    EXPECT_FALSE(history.Interpolate(0.5,pose)); history.Reset({},0,1); history.Publish({{10,0,0}},1,1);
    ASSERT_TRUE(history.Interpolate(-1,pose)); EXPECT_FLOAT_EQ(pose.position.x,0);
    ASSERT_TRUE(history.Interpolate(2,pose)); EXPECT_FLOAT_EQ(pose.position.x,10);
    ASSERT_TRUE(history.Interpolate(std::numeric_limits<double>::quiet_NaN(),pose)); EXPECT_FLOAT_EQ(pose.position.x,10);
    history.Clear(); EXPECT_FALSE(history.Interpolate(0.5,pose)); EXPECT_FALSE(history.Tracks(1,1));
}
