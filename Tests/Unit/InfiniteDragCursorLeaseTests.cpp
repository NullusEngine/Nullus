#include <gtest/gtest.h>

#include "UI/InfiniteDragCursorLease.h"

namespace
{
using NLS::Cursor::ECursorShape;
using NLS::UI::InfiniteDragCursorLease;
}

TEST(InfiniteDragCursorLeaseTests, ReleasesAtEndOfFirstFrameWithoutRenewedRequest)
{
    InfiniteDragCursorLease lease;

    lease.BeginFrame();
    EXPECT_TRUE(lease.Request(ECursorShape::IBEAM, ECursorShape::SLIDE_ARROW));
    EXPECT_TRUE(lease.OwnsCursor());
    EXPECT_FALSE(lease.ReleaseIfUnrequested().has_value());

    lease.BeginFrame();
    const auto release = lease.ReleaseIfUnrequested();

    ASSERT_TRUE(release.has_value());
    EXPECT_EQ(release->activeCursorShape, ECursorShape::SLIDE_ARROW);
    ASSERT_TRUE(release->previousCursorShape.has_value());
    EXPECT_EQ(*release->previousCursorShape, ECursorShape::IBEAM);
    EXPECT_FALSE(lease.OwnsCursor());
}

TEST(InfiniteDragCursorLeaseTests, RenewedRequestKeepsOriginalCursorShape)
{
    InfiniteDragCursorLease lease;

    lease.BeginFrame();
    EXPECT_TRUE(lease.Request(ECursorShape::IBEAM, ECursorShape::SLIDE_ARROW));
    EXPECT_FALSE(lease.ReleaseIfUnrequested().has_value());

    lease.BeginFrame();
    EXPECT_FALSE(lease.Request(ECursorShape::SLIDE_ARROW, ECursorShape::SLIDE_ARROW));
    EXPECT_FALSE(lease.ReleaseIfUnrequested().has_value());

    lease.BeginFrame();
    const auto release = lease.ReleaseIfUnrequested();

    ASSERT_TRUE(release.has_value());
    ASSERT_TRUE(release->previousCursorShape.has_value());
    EXPECT_EQ(*release->previousCursorShape, ECursorShape::IBEAM);
}

TEST(InfiniteDragCursorLeaseTests, RepeatedSameShapeRequestDoesNotRequireCursorReapply)
{
    InfiniteDragCursorLease lease;

    lease.BeginFrame();
    EXPECT_TRUE(lease.Request(ECursorShape::IBEAM, ECursorShape::SLIDE_ARROW));
    EXPECT_FALSE(lease.Request(ECursorShape::SLIDE_ARROW, ECursorShape::SLIDE_ARROW));
}
