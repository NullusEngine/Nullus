#include <gtest/gtest.h>

#include "Core/LauncherTheme.h"

namespace
{
float ProjectColumnTotal(const NLS::HubLayout::ProjectTableColumns& columns)
{
    return NLS::HubLayout::kProjectTablePadding + columns.name + columns.modified + columns.backend + columns.actions;
}

float WizardColumnTotal(const NLS::HubLayout::WizardColumns& columns)
{
    return columns.category + columns.templates + columns.preview;
}
}

TEST(LauncherHubLayoutTests, ProjectTableColumnsFitDefaultHubContentWidth)
{
    const auto columns = NLS::HubLayout::CalculateProjectTableColumns(960.0f);

    EXPECT_GE(columns.name, 360.0f);
    EXPECT_GE(columns.modified, 150.0f);
    EXPECT_GE(columns.backend, 150.0f);
    EXPECT_LE(ProjectColumnTotal(columns), 960.0f);
}

TEST(LauncherHubLayoutTests, ProjectTableColumnsFitMinimumHubContentWidth)
{
    const auto columns = NLS::HubLayout::CalculateProjectTableColumns(720.0f);

    EXPECT_GE(columns.name, 280.0f);
    EXPECT_LE(ProjectColumnTotal(columns), 720.0f);
}

TEST(LauncherHubLayoutTests, WizardColumnsKeepTemplateAndPreviewAreasVisible)
{
    const auto columns = NLS::HubLayout::CalculateWizardColumns(1000.0f);

    EXPECT_GE(columns.category, 150.0f);
    EXPECT_GE(columns.templates, 500.0f);
    EXPECT_GE(columns.preview, 280.0f);
    EXPECT_LE(WizardColumnTotal(columns), 1000.0f);
}

TEST(LauncherHubLayoutTests, WizardColumnsFitExpandedHubWidth)
{
    const auto columns = NLS::HubLayout::CalculateWizardColumns(1260.0f);

    EXPECT_GE(columns.category, 180.0f);
    EXPECT_GE(columns.templates, 680.0f);
    EXPECT_GE(columns.preview, 340.0f);
    EXPECT_LE(WizardColumnTotal(columns), 1260.0f);
}
