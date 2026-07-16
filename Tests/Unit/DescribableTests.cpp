#include <gtest/gtest.h>

#include <utility>

#include "Rendering/Data/Describable.h"

namespace
{
struct CopyTrackedDescriptor
{
	CopyTrackedDescriptor() = default;
	CopyTrackedDescriptor(const CopyTrackedDescriptor& other)
		: value(other.value)
	{
		++copyCount;
	}

	CopyTrackedDescriptor& operator=(const CopyTrackedDescriptor& other)
	{
		value = other.value;
		++copyCount;
		return *this;
	}

	int value = 0;
	static inline int copyCount = 0;
};
}

TEST(DescribableTests, ReadOnlyLookupReturnsStoredDescriptorWithoutCopying)
{
	NLS::Render::Data::Describable describable;
	CopyTrackedDescriptor descriptor;
	descriptor.value = 42;
	describable.AddDescriptor(std::move(descriptor));
	CopyTrackedDescriptor::copyCount = 0;

	const auto* stored = describable.TryGetDescriptor<CopyTrackedDescriptor>();

	ASSERT_NE(stored, nullptr);
	EXPECT_EQ(stored->value, 42);
	EXPECT_EQ(CopyTrackedDescriptor::copyCount, 0);
	EXPECT_EQ(describable.TryGetDescriptor<int>(), nullptr);
}

TEST(DescribableTests, CopyingLookupPreservesExistingBehavior)
{
	NLS::Render::Data::Describable describable;
	CopyTrackedDescriptor descriptor;
	descriptor.value = 7;
	describable.AddDescriptor(std::move(descriptor));
	CopyTrackedDescriptor::copyCount = 0;
	CopyTrackedDescriptor copy;

	ASSERT_TRUE(describable.TryGetDescriptor(copy));
	EXPECT_EQ(copy.value, 7);
	EXPECT_EQ(CopyTrackedDescriptor::copyCount, 1);
}
