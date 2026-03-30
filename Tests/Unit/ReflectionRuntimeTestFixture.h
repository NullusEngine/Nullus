#pragma once

#include "Assembly.h"
#include "Core/AssemblyCore.h"
#include "Engine/AssemblyEngine.h"
#include "Math/AssemblyMath.h"
#include "Rendering/AssemblyRender.h"
#include "Reflection/ReflectionDatabase.h"

#include "ReflectionTestUtils.h"

#include <gtest/gtest.h>

class ReflectionRuntimeTestFixture : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        auto& assembly = NLS::Assembly::Instance();
        assembly.Load<NLS::AssemblyCore>();
        assembly.Load<NLS::AssemblyMath>();
        assembly.Load<NLS::AssemblyRender>();
        assembly.Load<NLS::Engine::AssemblyEngine>();

        auto& db = NLS::meta::ReflectionDatabase::Instance();
        (void)db;
    }
};
