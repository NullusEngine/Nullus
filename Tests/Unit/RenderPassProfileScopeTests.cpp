#include <gtest/gtest.h>

#include "Rendering/Context/DriverInternal.h"

TEST(RenderPassProfileScopeTests, ResolvePassProfileScopeNameUsesDebugName)
{
    NLS::Render::Context::RenderPassCommandInput input;
    input.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
    input.debugName = "SelectionOutlinePass";

    EXPECT_STREQ(
        NLS::Render::Context::Detail::ResolvePassProfileScopeName(input),
        "SelectionOutlinePass");
}

TEST(RenderPassProfileScopeTests, ResolvePassProfileScopeNameFallsBackToPassKind)
{
    NLS::Render::Context::RenderPassCommandInput input;
    input.kind = NLS::Render::Context::RenderPassCommandKind::Lighting;

    EXPECT_STREQ(
        NLS::Render::Context::Detail::ResolvePassProfileScopeName(input),
        "ThreadedLightingPass");
}
