#pragma once

#include "CommandBase.hpp"

// The geometry operations of the Edit menu and the pet palette that the Archicad API has no
// function for (Split, Intersect, Adjust, Fillet, Chamfer, Offset Edge), computed here and
// written with ACAPI_Element_Change / ACAPI_Element_Create, plus Resize and Multiply, which
// ACAPI_Element_Edit can do.

class SplitElementsCommand : public CommandBase
{
public:
    SplitElementsCommand ();
    virtual GS::String                          GetName () const override;
    virtual GS::Optional<GS::UniString>         GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString>         GetRawResponseSchema () const override;
    virtual GS::ObjectState                     Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class IntersectElementsCommand : public CommandBase
{
public:
    IntersectElementsCommand ();
    virtual GS::String                          GetName () const override;
    virtual GS::Optional<GS::UniString>         GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString>         GetRawResponseSchema () const override;
    virtual GS::ObjectState                     Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class AdjustElementsCommand : public CommandBase
{
public:
    AdjustElementsCommand ();
    virtual GS::String                          GetName () const override;
    virtual GS::Optional<GS::UniString>         GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString>         GetRawResponseSchema () const override;
    virtual GS::ObjectState                     Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class FilletElementsCommand : public CommandBase
{
public:
    FilletElementsCommand ();
    virtual GS::String                          GetName () const override;
    virtual GS::Optional<GS::UniString>         GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString>         GetRawResponseSchema () const override;
    virtual GS::ObjectState                     Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class ChamferElementsCommand : public CommandBase
{
public:
    ChamferElementsCommand ();
    virtual GS::String                          GetName () const override;
    virtual GS::Optional<GS::UniString>         GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString>         GetRawResponseSchema () const override;
    virtual GS::ObjectState                     Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class OffsetEdgesCommand : public CommandBase
{
public:
    OffsetEdgesCommand ();
    virtual GS::String                          GetName () const override;
    virtual GS::Optional<GS::UniString>         GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString>         GetRawResponseSchema () const override;
    virtual GS::ObjectState                     Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class ResizeElementsCommand : public CommandBase
{
public:
    ResizeElementsCommand ();
    virtual GS::String                          GetName () const override;
    virtual GS::Optional<GS::UniString>         GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString>         GetRawResponseSchema () const override;
    virtual GS::ObjectState                     Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class MultiplyElementsCommand : public CommandBase
{
public:
    MultiplyElementsCommand ();
    virtual GS::String                          GetName () const override;
    virtual GS::Optional<GS::UniString>         GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString>         GetRawResponseSchema () const override;
    virtual GS::ObjectState                     Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};
