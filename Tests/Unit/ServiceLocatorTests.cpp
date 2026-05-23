#include <gtest/gtest.h>

#include "Core/ServiceLocator.h"

namespace
{
struct ServiceLocatorTestService
{
    int value = 0;
};
}

TEST(ServiceLocatorTests, RemoveClearsRegisteredService)
{
    ServiceLocatorTestService service;
    service.value = 42;

    NLS::Core::ServiceLocator::Provide(service);
    ASSERT_TRUE(NLS::Core::ServiceLocator::Contains<ServiceLocatorTestService>());
    EXPECT_EQ(NLS::Core::ServiceLocator::Get<ServiceLocatorTestService>().value, 42);

    NLS::Core::ServiceLocator::Remove<ServiceLocatorTestService>();

    EXPECT_FALSE(NLS::Core::ServiceLocator::Contains<ServiceLocatorTestService>());
}

TEST(ServiceLocatorTests, ProvideCanRestorePreviousServiceAfterTemporaryOverride)
{
    ServiceLocatorTestService original;
    original.value = 7;
    ServiceLocatorTestService temporary;
    temporary.value = 99;

    NLS::Core::ServiceLocator::Provide(original);
    ASSERT_EQ(NLS::Core::ServiceLocator::Get<ServiceLocatorTestService>().value, 7);

    NLS::Core::ServiceLocator::Provide(temporary);
    EXPECT_EQ(NLS::Core::ServiceLocator::Get<ServiceLocatorTestService>().value, 99);

    NLS::Core::ServiceLocator::Provide(original);
    EXPECT_EQ(NLS::Core::ServiceLocator::Get<ServiceLocatorTestService>().value, 7);

    NLS::Core::ServiceLocator::Remove<ServiceLocatorTestService>();
}
