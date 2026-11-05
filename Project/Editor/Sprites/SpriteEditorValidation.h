#pragma once

#if defined(NLS_ENABLE_TEST_HOOKS)
#include <memory>

namespace NLS::Editor::Sprites
{
class SpriteDocument;
// Explicitly enabled by the validation runner; never injects desktop input.
class SpriteEditorValidation final
{
public:
    SpriteEditorValidation();
    ~SpriteEditorValidation();
    void Tick(SpriteDocument& document);
private:
    struct State;
    std::unique_ptr<State> m;
};
}
#endif
