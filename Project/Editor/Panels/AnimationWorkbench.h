#pragma once
#include <UI/Panels/PanelWindow.h>
#include <filesystem>
#include <memory>
namespace NLS::Editor::AnimationTools {class AnimationPreview;}
namespace NLS::Editor::Panels
{
class AnimationWorkbench final:public UI::PanelWindow
{
public:
    AnimationWorkbench(const std::string& title,bool opened,const UI::PanelWindowSettings& settings,
        std::filesystem::path projectRoot,AnimationTools::AnimationPreview& preview);
    ~AnimationWorkbench()override;
    bool OpenDocument(const std::filesystem::path& path);
    bool OpenImportedAsset(const std::filesystem::path& path,std::string_view subAssetKey);
    bool OwnsShortcuts()const;
protected:
    void OnBeforeDrawWidgets()override;
private:
    struct State;
    std::unique_ptr<State> m;
};
}
