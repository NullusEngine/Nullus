#include "SpriteDocument.h"

#include "Undo/EditorUndoService.h"
#include <Image.h>
#include <algorithm>
#include <cctype>

namespace NLS::Editor::Sprites
{
namespace A = NLS::Core::Assets;

SpriteDocument::SpriteDocument(std::filesystem::path root,Undo::EditorUndoService* undo)
    : m_root(std::move(root)),m_undo(undo) {}

bool SpriteDocument::Open(const std::filesystem::path& requested)
{
    std::error_code ec;
    const auto assets = std::filesystem::weakly_canonical(m_root/"Assets",ec);
    if (ec) { m_error = "The project Assets folder is unavailable."; return false; }
    auto path = requested;
    if (!path.is_absolute()) path = path.begin()!=path.end() && *path.begin()=="Assets" ? m_root/path : assets/path;
    path = std::filesystem::weakly_canonical(path,ec);
    const auto relative = path.lexically_relative(assets);
    if (ec || relative.empty() || relative.is_absolute() || *relative.begin()==".." || !std::filesystem::is_regular_file(path,ec))
    { m_error = "Choose a texture inside this project's Assets folder."; return false; }
    const auto meta = A::AssetMeta::Load(A::GetAssetMetaPath(path));
    if (!meta || meta->assetType != A::AssetType::Texture)
    { m_error = "Import the texture before opening Sprite Editor."; return false; }
    NLS::Image image(path.string(),false);
    const int channels = image.GetChannels();
    const auto pixels = uint64_t(std::max(0,image.GetWidth()))*std::max(0,image.GetHeight());
    if (!image.GetData() || !pixels || pixels > 64ull*1024*1024 || channels < 1 || channels > 4)
    { m_error = "Cannot decode this texture, or its image exceeds 64 million pixels."; return false; }
    std::vector<uint8_t> rgba(size_t(pixels)*4);
    const auto* source = image.GetData();
    for (size_t index = 0; index < pixels; ++index)
    {
        const auto* sample = source+index*channels;
        rgba[index*4] = sample[0]; rgba[index*4+1] = channels < 3 ? sample[0] : sample[1];
        rgba[index*4+2] = channels < 3 ? sample[0] : sample[2];
        rgba[index*4+3] = channels == 2 ? sample[1] : channels == 4 ? sample[3] : 255;
    }
    const auto bytes = std::filesystem::file_size(path,ec); if(ec){m_error="Cannot inspect source texture.";return false;}
    const auto time = std::filesystem::last_write_time(path,ec); if(ec){m_error="Cannot inspect source texture.";return false;}
    m_path = std::move(path); m_meta = *meta; m_width = image.GetWidth(); m_height = image.GetHeight();
    m_rgba = std::move(rgba); m_sourceBytes = bytes; m_sourceTime = time;
    m_target = Inspector::InspectorTargetHandle::FromAsset(Inspector::InspectorTargetKind::AssetImporter,
        "Assets/"+relative.generic_string(),m_meta.id.ToString());
    m_error.clear(); return true;
}

bool SpriteDocument::SourceChanged() const
{
    if (m_path.empty()) return false;
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(m_path,ec); if(ec || bytes != m_sourceBytes)return true;
    const auto time = std::filesystem::last_write_time(m_path,ec); return ec || time != m_sourceTime;
}

std::optional<SpriteDocumentView> SpriteDocument::Read()
{
    if (m_path.empty()) { m_error = "Open a Sprite texture."; return {}; }
    const auto draft = Inspector::ReadImporterEditingState(m_target);
    if (!draft.available) { m_error = "The texture importer is no longer available."; return {}; }
    auto meta = m_meta; meta.settings = draft.settings; meta.settings["TEXTURE_TYPE"] = "sprite";
    std::string readError;
    const auto sprites = A::BuildSpriteArtifacts(meta,m_width,m_height,"texture:main",m_path.stem().string(),readError);
    if (!sprites) { m_error = std::move(readError); return {}; }
    SpriteDocumentView view; view.dirty = draft.dirty; view.reimporting = draft.reimporting; view.revision = draft.revision;
    auto mode = meta.settings.contains(A::SpriteImportModeSetting) ? meta.settings.at(A::SpriteImportModeSetting) : "single";
    std::transform(mode.begin(),mode.end(),mode.begin(),[](unsigned char c){return char(std::tolower(c));});
    view.multiple = mode == "multiple";
    for (const auto& sprite : *sprites)
        view.slices.push_back({sprite.id,sprite.name,sprite.x,sprite.y,sprite.width,sprite.height,sprite.pivot,sprite.pixelsPerUnit,sprite.border});
    if (!draft.dirty && !draft.reimporting)
        if (const auto current = A::AssetMeta::Load(A::GetAssetMetaPath(m_path));
            current && current->id == m_meta.id && current->settings == draft.settings)m_meta = *current;
    if (!draft.error.empty()) m_error = draft.error;
    return view;
}

bool SpriteDocument::Commit(std::map<std::string,std::string> expected,std::map<std::string,std::string> replacement,
    std::string label,std::string mergeKey)
{
    if (!m_undo) { m_error = "The Editor Undo service is unavailable."; return false; }
    if (SourceChanged()) { m_error = "The source image changed. Reload it before editing slices."; return false; }
    if (const auto current=A::AssetMeta::Load(A::GetAssetMetaPath(m_path));!current||*current!=m_meta)
    { m_error="Texture metadata changed outside this edit. Revert and reload before editing slices."; return false; }
    auto candidate = m_meta; candidate.settings = replacement;
    if (!A::BuildSpriteArtifacts(candidate,m_width,m_height,"texture:main",m_path.stem().string(),m_error))return false;
    return Inspector::EditImporterSettings(*m_undo,m_target,expected,std::move(replacement),
        std::move(label),std::move(mergeKey),m_error);
}

bool SpriteDocument::EditSlices(std::vector<A::SpriteSliceDefinition> slices,std::string label,std::string mergeKey)
{
    const auto encoded = A::SerializeSpriteSlices(slices,m_error); if(encoded.empty())return false;
    const auto current = Inspector::ReadImporterEditingState(m_target);
    auto next = current.settings; next["TEXTURE_TYPE"] = "sprite";
    next[A::SpriteImportModeSetting] = "multiple"; next[A::SpriteSlicesSetting] = encoded;
    return Commit(current.settings,std::move(next),std::move(label),std::move(mergeKey));
}

bool SpriteDocument::EditSingle(std::array<float,2> pivot,float ppu,std::string mergeKey,
    std::optional<std::array<uint32_t,4>> border)
{
    const auto current = Inspector::ReadImporterEditingState(m_target);
    auto next = current.settings; next["TEXTURE_TYPE"] = "sprite"; next[A::SpriteImportModeSetting] = "single";
    next["SPRITE_PIVOT_X"] = std::to_string(pivot[0]); next["SPRITE_PIVOT_Y"] = std::to_string(pivot[1]);
    next["SPRITE_PIXELS_PER_UNIT"] = std::to_string(ppu);
    if(border)
    {
        const char* names[]{"SPRITE_BORDER_LEFT","SPRITE_BORDER_BOTTOM","SPRITE_BORDER_RIGHT","SPRITE_BORDER_TOP"};
        for(size_t index=0;index<4;++index)next[names[index]]=std::to_string((*border)[index]);
    }
    return Commit(current.settings,std::move(next),"Edit Single Sprite",std::move(mergeKey));
}

bool SpriteDocument::Grid(const A::SpriteGridSettings& settings)
{
    const auto current = Read(); if(!current)return false;
    auto slices = A::GenerateGridSpriteSlices(m_width,m_height,settings,current->slices,m_error); if(!slices)return false;
    // Converting the original full-image Sprite keeps its reference on the
    // first generated cell. Later edits/reorders always retain explicit UUIDs.
    if (!current->multiple && !slices->empty() && !current->slices.empty())
    { slices->front().id = current->slices.front().id; slices->front().name = current->slices.front().name; }
    return EditSlices(std::move(*slices),"Grid slice Sprite sheet");
}

bool SpriteDocument::Automatic(uint8_t threshold,uint32_t minimumPixels)
{
    const auto current = Read(); if(!current)return false;
    A::SpriteGridSettings defaults; defaults.namePrefix = m_path.stem().string();
    if(!current->slices.empty()){defaults.pivot=current->slices.front().pivot;defaults.pixelsPerUnit=current->slices.front().pixelsPerUnit;}
    auto slices = A::GenerateAlphaSpriteSlices(m_rgba,m_width,m_height,threshold,minimumPixels,defaults,current->slices,m_error);
    if(!slices)return false;
    if (!current->multiple && !slices->empty() && !current->slices.empty())
    { slices->front().id = current->slices.front().id; slices->front().name = current->slices.front().name; }
    return EditSlices(std::move(*slices),"Automatically slice Sprite sheet");
}

bool SpriteDocument::Apply()
{
    const auto view = Read(); if(!view)return false;
    if (view->reimporting) { m_error = "Wait for the current import to complete."; return false; }
    if (!view->dirty) { m_error.clear(); return true; }
    if (SourceChanged()) { m_error = "The source image changed. Reload it before applying slices."; return false; }
    const auto current = A::AssetMeta::Load(A::GetAssetMetaPath(m_path));
    if (!current || *current != m_meta) { m_error = "Texture metadata changed outside this edit. Reload before applying."; return false; }
    m_error.clear();return Inspector::ApplyImporterEditingState(m_target,m_error);
}
bool SpriteDocument::Revert()
{ m_error.clear();return Inspector::RevertImporterEditingState(m_target,m_error); }
}
