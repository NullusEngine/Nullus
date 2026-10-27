#pragma once
// Internal source-format codecs shared by system and library assets.
#include "VfxAuthoring.h"
#include <Json/json.hpp>
namespace NLS::VFX::Authoring::Detail
{
nlohmann::json EncodeGraph(const Graph& graph, bool editor);
Graph DecodeGraph(const nlohmann::json& source);
nlohmann::json EncodeInput(const Input& input, bool editor);
Input DecodeInput(const nlohmann::json& source);
nlohmann::json EncodeReference(const VersionReference& reference);
VersionReference DecodeReference(const nlohmann::json& source);
bool ValidateInput(const Input& input, std::string& error);
nlohmann::json EncodeStageBindings(const SimulationStageBindings& bindings);
SimulationStageBindings DecodeStageBindings(const nlohmann::json& source);
bool HasTypedData(const nlohmann::json& source);
}
