#pragma once
#include "CookTypes.h"

namespace NLS::Cook::Detail
{
// Leaves the existing asset ordering intact, then orders ECS inputs so a cold
// cook publishes referenced ECS artifacts before their consumers.
bool OrderECSInputs(std::vector<std::filesystem::path>& paths, CookSummary& summary, const CookOptions& options);
}
