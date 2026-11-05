#include "Properties/SpritePropertyDrawers.h"
#include "Properties/PropertyDrawerRegistry.h"
#include "Undo/EditorUndoService.h"
#include "Inspector/NativeReflectionAdapter.h"
#include <Components/SpriteRendererComponent.h>
#include <Assets/SpriteProjectSettings.h>
#include <Assets/SpriteSheet.h>
#include <Core/ResourceManagement/ShaderManager.h>
#include <Core/ServiceLocator.h>
#include <Guid.h>
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#ifdef _WIN32
#include <windows.h>
#endif

namespace NLS::Editor::Properties
{
namespace
{
using Table=NLS::Core::Assets::SpriteSortingLayers;
std::optional<std::string> Read(const std::filesystem::path& path)
{
    std::ifstream input(path,std::ios::binary);if(!input)return {};
    return std::string(std::istreambuf_iterator<char>(input),{});
}
bool Write(const std::filesystem::path& path,const std::optional<std::string>& text,std::string& error)
{
    std::error_code ec;
    if(!text){std::filesystem::remove(path,ec);if(ec){error=ec.message();return false;}return true;}
    std::filesystem::create_directories(path.parent_path(),ec);if(ec){error=ec.message();return false;}
    auto temporary=path;temporary+=".tmp-"+Guid::New().ToString();
    {std::ofstream output(temporary,std::ios::binary);output.write(text->data(),text->size());output.flush();
        if(!output){error="Cannot save sorting layers";output.close();std::filesystem::remove(temporary,ec);return false;}}
#ifdef _WIN32
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))ec=std::error_code(GetLastError(),std::system_category());
#else
    std::filesystem::rename(temporary,path,ec);
#endif
    if(ec){error=ec.message();std::filesystem::remove(temporary,ec);return false;}return true;
}
class TableCommand final:public Undo::IEditorUndoCommand
{
public:
    std::filesystem::path path;std::optional<std::string> before,after;
    Undo::UndoCommandInfo info;
    Undo::UndoCommandInfo GetInfo()const override{return info;}
    Undo::UndoValidationResult Validate(Undo::UndoDirection direction,const Undo::UndoExecutionContext&)const override
    {
        if(path!=std::filesystem::path(NLS::Core::ResourceManagement::ShaderManager::ProjectAssetsRoot())/Table::RelativePath)
            return Undo::UndoValidationResult::Stale("Sorting layer project is closed");
        if(Read(path)!=(direction==Undo::UndoDirection::Undo?after:before))return Undo::UndoValidationResult::Conflict("Sorting layers changed outside Undo");
        return Undo::UndoValidationResult::Ready();
    }
    Undo::UndoStartResult Apply(Undo::UndoDirection direction,const Undo::UndoExecutionContext&,Undo::UndoCompletion)override
    {
        std::string error;if(!Write(path,direction==Undo::UndoDirection::Undo?before:after,error))return Undo::UndoStartResult::Failed(error);
        Engine::Assets::InvalidateSpriteSortingLayers();return Undo::UndoStartResult::Completed();
    }
    bool TryMergeFrom(const Undo::IEditorUndoCommand&)override{return false;}
};
bool SaveTable(const Table& table,std::string label,std::string& error)
{
    Table checked;const auto after=table.Serialize();if(!Table::Parse(after,checked,error))return false;
    const auto path=std::filesystem::path(NLS::Core::ResourceManagement::ShaderManager::ProjectAssetsRoot())/Table::RelativePath;
    auto command=std::make_unique<TableCommand>();command->path=path;command->before=Read(path);command->after=after;
    command->info.label=std::move(label);command->info.sourceModule="Sprite";
    command->info.scopes={{Undo::UndoScopeKind::Asset,"sprite-sorting:"+path.generic_string(),1}};
    command->info.estimatedBytes=after.size()+(command->before?command->before->size():0);
    if(!NLS::Core::ServiceLocator::Contains<Undo::EditorUndoService>()){error="Editor Undo service is unavailable";return false;}
    const auto result=NLS_SERVICE(Undo::EditorUndoService).Execute(std::move(command));error=result.message;
    return result.state==Undo::UndoRequestState::Completed;
}
struct LayerState {std::string selected,error;std::array<char,257> name{};};
class SpriteRendererEditor final:public Inspector::ComponentEditor
{
public:
    void OnInspectorGUI()override
    {
        serializedObject().Update();DrawDefaultInspector();(void)ApplyModifiedProperties();
        for(const auto& handle:targets().targets)
            if(auto* component=dynamic_cast<Engine::Components::SpriteRendererComponent*>(Inspector::NativeReflectionAdapter::ResolveObject(handle)))
            {
                component->CaptureSprite();
                if(!component->LastError().empty())HelpBox(component->LastError(),Inspector::ComponentEditorMessageKind::Warning);
            }
    }
};
bool DrawLayer(PropertyDrawerContext& c)
{
    auto& state=c.GetState<LayerState>();std::string loadError;const auto layers=Engine::Assets::CaptureSpriteSortingLayers(&loadError);
    if(!loadError.empty())state.error=loadError;
    const auto value=c.value.GetValue<std::string>();std::string label="Missing (Default fallback)";
    for(const auto& layer:layers->layers)if(layer.id==value||(value=="Default"&&layer.id==Table::DefaultId))label=layer.name;
    ImGui::BeginDisabled(c.readOnly||c.property.readOnly||c.property.metadata.readOnly);
    bool openTable=false;
    if(ImGui::BeginCombo(c.widgetId.c_str(),label.c_str()))
    {
        for(const auto& layer:layers->layers)
            if(ImGui::Selectable(layer.name.c_str(),layer.id==value))
            {c.interaction={true,false,true,true,false,1};c.changed=c.preview(meta::Variant(layer.id),"Change sorting layer");}
        ImGui::Separator();if(ImGui::Selectable("Edit sorting layers..."))openTable=true;
        ImGui::EndCombo();
    }
    // A separate button keeps the table accessible after the combo closes.
    if(ImGui::SmallButton(("Layers##"+c.widgetId).c_str()))openTable=true;
    if(openTable)ImGui::OpenPopup(("Sorting layers##"+c.widgetId).c_str());
    if(ImGui::BeginPopup(("Sorting layers##"+c.widgetId).c_str()))
    {
        ImGui::TextUnformatted("Sorting layers (back to front)");ImGui::Separator();
        for(const auto& layer:layers->layers)
            if(ImGui::Selectable(layer.name.c_str(),state.selected==layer.id))
            {state.selected=layer.id;state.name={};std::copy_n(layer.name.data(),(std::min)(layer.name.size(),state.name.size()-1),state.name.data());}
        if(ImGui::Button("Add layer"))
        {
            auto updated=*layers;const auto id=Guid::New().ToString();size_t number=1;std::string name;
            do{name="Layer "+std::to_string(number++);}while(std::any_of(updated.layers.begin(),updated.layers.end(),[&](const auto& layer){return layer.name==name;}));
            updated.layers.push_back({id,name});if(SaveTable(updated,"Add sorting layer",state.error))state.selected=id;
        }
        const auto selected=std::find_if(layers->layers.begin(),layers->layers.end(),[&](const auto& layer){return layer.id==state.selected;});
        if(selected!=layers->layers.end())
        {
            const auto index=size_t(selected-layers->layers.begin());
            ImGui::InputText("Name",state.name.data(),state.name.size());
            if(ImGui::Button("Rename")){auto updated=*layers;updated.layers[index].name=state.name.data();SaveTable(updated,"Rename sorting layer",state.error);}
            ImGui::SameLine();ImGui::BeginDisabled(index==0);
            if(ImGui::Button("Move back")){auto updated=*layers;std::swap(updated.layers[index],updated.layers[index-1]);SaveTable(updated,"Reorder sorting layer",state.error);}ImGui::EndDisabled();
            ImGui::SameLine();ImGui::BeginDisabled(index+1==layers->layers.size());
            if(ImGui::Button("Move front")){auto updated=*layers;std::swap(updated.layers[index],updated.layers[index+1]);SaveTable(updated,"Reorder sorting layer",state.error);}ImGui::EndDisabled();
            ImGui::BeginDisabled(selected->id==Table::DefaultId);
            if(ImGui::Button("Remove layer")){auto updated=*layers;updated.layers.erase(updated.layers.begin()+index);SaveTable(updated,"Remove sorting layer",state.error);}ImGui::EndDisabled();
        }
        if(!state.error.empty())ImGui::TextWrapped("%s",state.error.c_str());
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();return true;
}
}
void RegisterSpritePropertyDrawers()
{
    static auto tokens=[]
    {
        std::vector<DrawerRegistrationToken> registrations;
        Inspector::ComponentEditorRegistration editor;editor.inspectedType=NLS_TYPEOF(Engine::Components::SpriteRendererComponent);
        editor.identifier="builtin.sprite.renderer";editor.supportsMultiObject=true;
        editor.create=[] {return std::make_unique<SpriteRendererEditor>();};
        Inspector::InspectorRegistry::Get().RegisterComponentEditor(std::move(editor));
        PropertyDrawerRegistration registration;registration.scope=DrawerScope::Field;registration.identifier="builtin.sprite.sorting-layer";
        registration.ownerTypeName="NLS::Engine::Components::SpriteRendererComponent";registration.fieldPath=Inspector::PropertyPath("root.sortingLayer");
        registration.draw=DrawLayer;registrations.push_back(PropertyDrawerRegistry::Get().Register(std::move(registration)));
        registration={};registration.scope=DrawerScope::Field;registration.identifier="builtin.sprite.persistent-id";
        registration.ownerTypeName="ImporterSettings";
        registration.fieldPath=Inspector::PropertyPath("Importer.Settings.SPRITE_ID");
        registration.draw=[](auto&){ImGui::TextDisabled("Stable Sprite identity");return true;};
        registrations.push_back(PropertyDrawerRegistry::Get().Register(std::move(registration)));
        registration={};registration.scope=DrawerScope::Field;registration.identifier="builtin.sprite.slice-summary";
        registration.ownerTypeName="ImporterSettings";
        registration.fieldPath=Inspector::PropertyPath("Importer.Settings.SPRITE_SLICES");
        registration.draw=[](auto& c)
        {
            std::string error;
            const auto slices=NLS::Core::Assets::DeserializeSpriteSlices(c.value.template GetValue<std::string>(),error);
            if(slices)ImGui::TextDisabled("%zu Sprites",slices->size());
            else ImGui::TextWrapped("%s",error.c_str());
            return true;
        };
        registrations.push_back(PropertyDrawerRegistry::Get().Register(std::move(registration)));
        return registrations;
    }();
}
}
