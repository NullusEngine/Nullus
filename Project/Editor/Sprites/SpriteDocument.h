#pragma once

#include <Assets/SpriteSheet.h>
#include "Inspector/AssetImporterEditing.h"

namespace NLS::Editor::Sprites
{
struct SpriteDocumentView
{
    std::vector<NLS::Core::Assets::SpriteSliceDefinition> slices;
    bool multiple = false, dirty = false, reimporting = false;
    uint64_t revision = 0;
};

class SpriteDocument
{
public:
    explicit SpriteDocument(std::filesystem::path projectRoot, Undo::EditorUndoService* undo);
    bool Open(const std::filesystem::path& path);
    std::optional<SpriteDocumentView> Read();
    bool EditSlices(std::vector<NLS::Core::Assets::SpriteSliceDefinition> slices,
        std::string label, std::string mergeKey = {});
    bool EditSingle(std::array<float,2> pivot,float pixelsPerUnit,std::string mergeKey = {},
        std::optional<std::array<uint32_t,4>> border = {});
    bool Grid(const NLS::Core::Assets::SpriteGridSettings& settings);
    bool Automatic(uint8_t threshold,uint32_t minimumPixels);
    bool Apply();
    bool Revert();
    bool SourceChanged() const;
    const std::string& Error() const { return m_error; }
    const std::filesystem::path& Path() const { return m_path; }
    const Inspector::InspectorTargetHandle& Target() const { return m_target; }
    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }
    std::span<const uint8_t> Pixels() const { return m_rgba; }
private:
    bool Commit(std::map<std::string,std::string> expected,std::map<std::string,std::string> replacement,
        std::string label,std::string mergeKey);
    std::filesystem::path m_root,m_path;
    NLS::Core::Assets::AssetMeta m_meta;
    Inspector::InspectorTargetHandle m_target;
    Undo::EditorUndoService* m_undo;
    uint32_t m_width = 0,m_height = 0;
    uintmax_t m_sourceBytes = 0;
    std::filesystem::file_time_type m_sourceTime{};
    std::vector<uint8_t> m_rgba;
    std::string m_error;
};
}
