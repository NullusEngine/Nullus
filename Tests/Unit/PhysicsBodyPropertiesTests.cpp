#include <gtest/gtest.h>
#include <Physics/PhysicsWorld.h>
#include <array>
#include <cmath>
#include <limits>

namespace
{
using namespace NLS;
using namespace Physics;
std::shared_ptr<PhysicsWorld> BodyPropertyWorld()
{
    std::string error; auto world=PhysicsWorld::Create(error); EXPECT_TRUE(world) << error; return world;
}
BodySettings DynamicProperties()
{
    BodySettings settings; settings.dynamic=true; settings.kinematic=false; settings.useGravity=false; settings.angularDamping=0; return settings;
}
}

TEST(PhysicsBodyPropertiesTests, CustomMassCenterAndInertiaDriveRealOffCenterImpulse)
{
    auto world=BodyPropertyWorld(); ASSERT_TRUE(world); auto settings=DynamicProperties();
    settings.mass=2; settings.autoMassProperties=false; settings.centerOfMass={0,1,0}; settings.inertiaTensor={2,3,4};
    const auto body=world->AddBox({0.5f,0.5f,0.5f},settings); ASSERT_TRUE(body.IsValid());
    ASSERT_TRUE(world->AddForceAtPosition(body,{1,0,0},{0,0,0},ForceMode::Impulse)); world->Step(0.1f);
    BodyState state; ASSERT_TRUE(world->GetState(body,state)); EXPECT_FLOAT_EQ(state.mass,2);
    EXPECT_EQ(state.centerOfMass,Maths::Vector3(0,1,0)); EXPECT_EQ(state.inertia,Maths::Vector3(2,3,4));
    EXPECT_NEAR(state.velocity.x,0.5f,1e-5f); EXPECT_NEAR(state.angularVelocity.z,0.25f,1e-5f);
}

TEST(PhysicsBodyPropertiesTests, RotatedPrincipalAxesChangeAngularImpulseResponse)
{
    auto world=BodyPropertyWorld(); ASSERT_TRUE(world); auto settings=DynamicProperties();
    settings.autoMassProperties=false; settings.inertiaTensor={2,3,4};
    settings.massFrameRotation={0,0,std::sqrt(0.5f),std::sqrt(0.5f)};
    const auto body=world->AddSphere(0.5f,settings); ASSERT_TRUE(body.IsValid());
    ASSERT_TRUE(world->AddTorque(body,{3,0,0},ForceMode::Impulse)); world->Step(0.1f);
    BodyState state; ASSERT_TRUE(world->GetState(body,state)); EXPECT_NEAR(state.angularVelocity.x,1,1e-5f);
    EXPECT_NEAR(state.angularVelocity.y,0,1e-5f); EXPECT_NEAR(state.massFrameRotation.z,std::sqrt(0.5f),1e-5f);
}

TEST(PhysicsBodyPropertiesTests, AutomaticAndManualPropertiesSurviveShapeRemovalAndReassembly)
{
    auto world=BodyPropertyWorld(); ASSERT_TRUE(world); auto settings=DynamicProperties(); settings.mass=6;
    const auto body=world->CreateBody(settings); ShapeSettings shape; shape.localPosition={2,0,0};
    const auto first=world->AttachBox(body,{0.5f,0.5f,0.5f},shape); ASSERT_TRUE(first.IsValid());
    BodyState state; ASSERT_TRUE(world->GetState(body,state)); EXPECT_NEAR(state.centerOfMass.x,2,1e-5f); EXPECT_NEAR(state.inertia.z,1,1e-5f);
    ASSERT_TRUE(world->Remove(first)); ASSERT_TRUE(world->GetState(body,state)); EXPECT_EQ(state.centerOfMass,Maths::Vector3{});
    shape.localPosition={-3,0,0}; const auto second=world->AttachBox(body,{0.5f,0.5f,0.5f},shape); ASSERT_TRUE(second.IsValid());
    ASSERT_TRUE(world->GetState(body,state)); EXPECT_NEAR(state.centerOfMass.x,-3,1e-5f);
    settings.autoMassProperties=false; settings.centerOfMass={4,0,0}; settings.inertiaTensor={5,6,7}; ASSERT_TRUE(world->UpdateDynamics(body,settings));
    ASSERT_TRUE(world->Remove(second)); ASSERT_TRUE(world->GetState(body,state));
    EXPECT_EQ(state.centerOfMass,Maths::Vector3(4,0,0)); EXPECT_NEAR(state.inertia.x,5,1e-5f); EXPECT_NEAR(state.inertia.y,6,1e-5f); EXPECT_NEAR(state.inertia.z,7,1e-5f); EXPECT_FLOAT_EQ(state.mass,6);
    ASSERT_TRUE(world->AttachSphere(body,2).IsValid()); ASSERT_TRUE(world->GetState(body,state)); EXPECT_NEAR(state.inertia.z,7,1e-5f);
}

TEST(PhysicsBodyPropertiesTests, StartAsleepDoesNotEraseMotionWhenAnAwakeBodyRebuildsShapes)
{
    auto world=BodyPropertyWorld(); ASSERT_TRUE(world); auto settings=DynamicProperties(); settings.startAwake=false; settings.velocity={5,0,0};
    const auto body=world->CreateBody(settings); const auto shape=world->AttachBox(body,{0.5f,0.5f,0.5f}); ASSERT_TRUE(shape.IsValid());
    BodyState state; ASSERT_TRUE(world->GetState(body,state)); EXPECT_FALSE(state.awake); EXPECT_EQ(state.velocity,Maths::Vector3{});
    ASSERT_TRUE(world->SetAwake(body,true)); ASSERT_TRUE(world->SetVelocity(body,{1,0,0})); ASSERT_TRUE(world->AddForce(body,{4,0,0}));
    ASSERT_TRUE(world->AttachSphere(body,0.25f).IsValid()); ASSERT_TRUE(world->Remove(shape)); world->Step(0.1f);
    ASSERT_TRUE(world->GetState(body,state)); EXPECT_TRUE(state.awake); EXPECT_NEAR(state.velocity.x,1.4f,1e-5f);
    ASSERT_TRUE(world->SetAwake(body,false)); ASSERT_TRUE(world->AttachBox(body,{1,1,1}).IsValid());
    ASSERT_TRUE(world->GetState(body,state)); EXPECT_FALSE(state.awake); EXPECT_EQ(state.velocity,Maths::Vector3{});
}

TEST(PhysicsBodyPropertiesTests, InvalidMassPropertiesNeverModifyNativeConfiguration)
{
    auto world=BodyPropertyWorld(); ASSERT_TRUE(world); const auto settings=DynamicProperties(); const auto body=world->AddSphere(0.5f,settings);
    ASSERT_TRUE(body.IsValid()); const auto snapshot=world->CaptureState(); std::vector<BodySettings> invalid;
    auto bad=settings; bad.inertiaTensor.x=0; invalid.push_back(bad); bad=settings; bad.inertiaTensor.y=-1; invalid.push_back(bad);
    bad=settings; bad.centerOfMass.z=std::numeric_limits<float>::infinity(); invalid.push_back(bad);
    bad=settings; bad.massFrameRotation={0,0,0,0}; invalid.push_back(bad);
    bad=settings; bad.sleepThreshold=-1; invalid.push_back(bad); bad=settings; bad.sleepThreshold=std::numeric_limits<float>::quiet_NaN(); invalid.push_back(bad);
    bad=settings; bad.sleepThreshold=std::numeric_limits<float>::max(); invalid.push_back(bad);
    bad=settings; bad.inertiaTensor.x=std::numeric_limits<float>::denorm_min(); invalid.push_back(bad);
    bad=settings; bad.mass=std::numeric_limits<float>::denorm_min(); invalid.push_back(bad);
    for (const auto& value:invalid) { EXPECT_FALSE(world->CreateBody(value).IsValid()); EXPECT_FALSE(world->UpdateDynamics(body,value)); }
    EXPECT_EQ(world->GetStatistics().bodyCount,1u); std::string error; EXPECT_TRUE(world->RestoreState(snapshot,error)) << error;
    EXPECT_FALSE(world->AddForceAtPosition(body,{std::numeric_limits<float>::max(),0,0},{0,10,0},ForceMode::Force));
    world->Step(0.1f); EXPECT_EQ(world->GetVelocity(body),Maths::Vector3{});
}

TEST(PhysicsBodyPropertiesTests, TorqueAndPositionedForceModesHaveAnalyticalMassAndInertiaResponse)
{
    auto world=BodyPropertyWorld(); ASSERT_TRUE(world); auto settings=DynamicProperties(); settings.autoMassProperties=false; settings.mass=2; settings.inertiaTensor={4,4,4};
    const auto body=world->AddBox({0.5f,0.5f,0.5f},settings); ASSERT_TRUE(body.IsValid());
    const std::array modes{ForceMode::Force,ForceMode::Acceleration,ForceMode::Impulse,ForceMode::VelocityChange};
    const std::array<float,4> torqueExpected{0.1f,0.4f,1,4}, linearExpected{0.1f,0.2f,1,2}, angularExpected{-0.05f,-0.1f,-0.5f,-1};
    for (size_t i=0;i<modes.size();++i)
    {
        ASSERT_TRUE(world->SetAngularVelocity(body,{})); ASSERT_TRUE(world->AddTorque(body,{0,0,4},modes[i])); world->Step(0.1f);
        BodyState state; ASSERT_TRUE(world->GetState(body,state)); EXPECT_NEAR(state.angularVelocity.z,torqueExpected[i],1e-5f);
        ASSERT_TRUE(world->SetTransform(body,{},Maths::Quaternion::Identity)); ASSERT_TRUE(world->SetVelocity(body,{})); ASSERT_TRUE(world->SetAngularVelocity(body,{}));
        ASSERT_TRUE(world->AddForceAtPosition(body,{2,0,0},{0,1,0},modes[i])); world->Step(0.1f);
        ASSERT_TRUE(world->GetState(body,state)); EXPECT_NEAR(state.velocity.x,linearExpected[i],1e-5f); EXPECT_NEAR(state.angularVelocity.z,angularExpected[i],1e-5f);
    }
}

TEST(PhysicsBodyPropertiesTests, ConfiguredSleepThresholdChangesAutomaticSleepingBehavior)
{
    auto world=BodyPropertyWorld(); ASSERT_TRUE(world); auto moving=DynamicProperties(); moving.velocity={0.1f,0,0};
    const auto active=world->AddSphere(0.25f,moving); moving.position={0,10,0}; moving.sleepThreshold=2;
    const auto sleepy=world->AddSphere(0.25f,moving); ASSERT_TRUE(active.IsValid()); ASSERT_TRUE(sleepy.IsValid());
    for (int i=0;i<120;++i) world->Step(1.0f/60);
    BodyState state; ASSERT_TRUE(world->GetState(active,state)); EXPECT_TRUE(state.awake); EXPECT_NEAR(state.velocity.x,0.1f,1e-5f);
    ASSERT_TRUE(world->GetState(sleepy,state)); EXPECT_FALSE(state.awake); EXPECT_EQ(state.velocity,Maths::Vector3{});
}
