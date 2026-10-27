#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace NLS::Editor::Icons
{
    /**
    * How an icon is produced at draw time.
    */
    enum class IconKind
    {
        Monochrome, // White/alpha asset tinted by the active editor theme.
        Color,      // Pre-colored asset drawn as-is (file types, brands).
        Glyph,      // Font codepoint merged into the UI font atlas.
        Generated,  // Produced at runtime (missing placeholder).
        Raster      // Declared bitmap exception; no SVG source.
    };

    /**
    * Logical (UI-scale independent) icon sizes. Panels use tokens instead of
    * raw pixel constants so DPI handling stays in IconPainter.
    */
    enum class IconToken
    {
        Tiny,    // 12
        Small,   // 16
        Default, // 20
        Medium,  // 24
        Large,   // 32
        Huge     // 48
    };

    /**
    * Stable semantic icon identity. Built-ins are generated constants; plugins
    * use IconId::FromRuntime with registry-owned strings.
    */
    struct IconId
    {
        std::string_view value;

        constexpr explicit IconId(std::string_view p_value) : value(p_value) {}
        static constexpr IconId FromRuntime(std::string_view p_value) { return IconId{ p_value }; }

        friend bool operator==(const IconId&, const IconId&) = default;
    };

    /**
    * Metadata owned by the registry. Strings are owned so plugin registrations
    * do not depend on caller lifetime; built-ins are registered once at startup.
    */
    struct IconDefinition
    {
        std::string id;
        IconKind kind = IconKind::Color;
        IconToken defaultToken = IconToken::Small;
        bool themeable = false;
        std::string module = "editor";
        char32_t glyph = 0; // Only for IconKind::Glyph.
        std::vector<std::string> aliases;
        std::vector<std::string> tags;
        std::vector<std::uint32_t> tiers; // Available raster pixel sizes.
        std::string deprecatedBy;
    };

    /**
    * Immutable-after-registration icon table. Built-ins come from
    * DefaultDefinitions(); plugins append/remove their own module at runtime.
    * Access is expected from the editor/UI thread only.
    */
    class IconRegistry
    {
    public:
        static constexpr std::string_view IdPrefix = "editor.icon.";

        bool Register(IconDefinition p_definition);
        bool UnregisterModule(std::string_view p_module);

        const IconDefinition* Find(std::string_view p_id) const;
        const IconDefinition* Find(const IconId p_id) const { return Find(p_id.value); }
        bool Contains(std::string_view p_id) const { return Find(p_id) != nullptr; }

        const std::deque<IconDefinition>& GetDefinitions() const { return m_definitions; }
        std::uint32_t Generation() const { return m_generation; }
        std::vector<char32_t> UsedGlyphCodepoints() const;

        static bool IsValidId(std::string_view p_id);
        static const std::vector<IconDefinition>& DefaultDefinitions();
        static IconRegistry CreateDefault();

    private:
        void RebuildIndex();

        std::deque<IconDefinition> m_definitions;
        std::unordered_map<std::string, std::size_t> m_indexById;
        std::uint32_t m_generation = 1;
    };

    // Logical size in UI points (unscaled).
    std::uint32_t IconTokenToLogicalPixels(IconToken p_token);
    const char* IconTokenName(IconToken p_token);

    // Picks the smallest tier >= requested, else the largest available tier.
    // An empty tier list returns the requested size unchanged.
    std::uint32_t SelectIconTier(
        std::uint32_t p_requestedPixels,
        const std::vector<std::uint32_t>& p_availableTiers);

    // Encodes a codepoint as UTF-8 into a 5-byte buffer (always null terminated).
    void EncodeIconGlyphUtf8(char32_t p_codepoint, char (&p_out)[5]);

    // Codepoints from the default icon set, for UIManager::SetFontIconCodepoints.
    std::vector<char32_t> DefaultGlyphCodepoints();
}
