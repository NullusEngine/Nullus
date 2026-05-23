#include <gtest/gtest.h>

#include "Eventing/Event.h"

TEST(EventTests, ListenerIdZeroIsReservedAsInvalidSentinel)
{
    NLS::Event<> event;
    int calls = 0;

    const NLS::ListenerID firstListener = event.AddListener([&calls]
    {
        ++calls;
    });

    EXPECT_NE(firstListener, 0u);
    EXPECT_FALSE(event.RemoveListener(0u));
    EXPECT_EQ(event.GetListenerCount(), 1u);

    EXPECT_TRUE(event.RemoveListener(firstListener));
    EXPECT_EQ(event.GetListenerCount(), 0u);

    event.Invoke();
    EXPECT_EQ(calls, 0);
}
