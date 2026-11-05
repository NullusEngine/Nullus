#include <gtest/gtest.h>
#include <Engine/GameObject.h>

#include <array>
#include <functional>

namespace
{
struct LifecycleSnapshotProbe : NLS::Engine::Components::Component
{
    std::function<void(LifecycleSnapshotProbe&)> update;
    void OnUpdate(float) override
    {
        // Keep the callable alive when it removes its owning component.
        const auto callback = update;
        if (callback) callback(*this);
    }
};

TEST(GameObjectLifecycleSnapshot, InlineAndOverflowVisitsKeepMutationAndDeferredAdditionSemantics)
{
    for (int count : {2,12})
    {
        SCOPED_TRACE(count);
        NLS::Engine::GameObject owner("Snapshot mutation");
        std::array<int,13> calls{};
        std::vector<LifecycleSnapshotProbe*> probes;
        for (int index = 0; index < count; ++index)
        {
            auto* probe = owner.AddComponent<LifecycleSnapshotProbe>();
            probe->update = [&,index](auto&) { ++calls[index]; };
            probes.push_back(probe);
        }
        const auto removedId = probes.back()->GetInstanceID();
        const auto selfId = probes.front()->GetInstanceID();
        probes.front()->update = [&](auto& self)
        {
            ++calls[0];
            EXPECT_TRUE(owner.RemoveComponent(probes.back()));
            auto* added = owner.AddComponent<LifecycleSnapshotProbe>();
            added->update = [&](auto&) { ++calls[12]; };
            EXPECT_TRUE(owner.RemoveComponent(&self));
        };
        owner.OnUpdate(1.0f/60);
        EXPECT_EQ(calls[0],1); EXPECT_EQ(calls[count-1],0); EXPECT_EQ(calls[12],0);
        for (int index = 1; index < count-1; ++index) EXPECT_EQ(calls[index],1);
        EXPECT_EQ(NLS::Object::IDToPointerNoThreadCheck(removedId),nullptr);
        EXPECT_EQ(NLS::Object::IDToPointerNoThreadCheck(selfId),nullptr);
        owner.OnUpdate(1.0f/60);
        EXPECT_EQ(calls[0],1); EXPECT_EQ(calls[count-1],0); EXPECT_EQ(calls[12],1);
        for (int index = 1; index < count-1; ++index) EXPECT_EQ(calls[index],2);
    }
}
}
