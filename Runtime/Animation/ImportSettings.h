#pragma once
#include <memory>
#include <string>
#include <vector>
#include "Animation.h"

namespace NLS::Animation
{
enum class FbxReaderSelection {Autodesk,Assimp,AutodeskWithAssimpFallback};
struct ImportSettings
{
    bool importSkeleton=true;
    bool importAnimations=true;
    double sampleRate=0; // Source frame rate, or 30 FPS when the source does not specify one.
    double globalScale=1;
    std::string sourceId;
    std::string axisConversion,unitConversion;
    std::vector<std::string> takes;
    std::shared_ptr<const Skeleton> targetSkeleton;
    FbxReaderSelection fbxReader=FbxReaderSelection::AutodeskWithAssimpFallback;
};
}
