#pragma once

#include <string>

#include "Object.h"
#include "Macros.h"
#include "Reflection/MetaParserFieldMethodSample.generated.h"

namespace NLS::meta
{
    CLASS(MetaParserFieldMethodSample) : public NLS::Object
    {
    public:
        GENERATED_BODY()
        PROPERTY(DisplayName("Value"), Getter(GetValue), Setter(SetValue))
        int value = 7;

        PROPERTY(
            DisplayName("Inspector Value"),
            Tooltip("Exercises generated native Inspector metadata."),
            Header("Inspector Metadata"),
            Space(4.0f),
            Range(-2.5f, 3.25f),
            Min(-5.0f),
            Max(5.0f),
            ReadOnly)
        float inspectorMetadataValue = 0.0f;

        PROPERTY(Multiline)
        std::string inspectorNotes;

        PROPERTY(HideInInspector)
        int inspectorHiddenValue = 0;

        PROPERTY(Flags)
        int inspectorFlagValue = 0;

        PROPERTY(ColorUsage)
        int inspectorColorValue = 0;

        PROPERTY(ReferenceTypeConstraint("NLS::Object"))
        int inspectorReferenceValue = 0;

        FUNCTION()
        int GetValue() const
        {
            return value;
        }

        FUNCTION()
        void SetValue(int v)
        {
            value = v;
        }
    };
}
