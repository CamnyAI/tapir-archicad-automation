#pragma once

#include "ElementCreationCommands.hpp"

// Creation commands for the element types that the other Create* commands do not cover: shells,
// radial and angle dimensions, skylights, cameras, pictures, change markers and detail / worksheet markers.
// They take the same per-item fields as the other Create* commands (favoriteName, id, layerIndex)
// and answer in the same shape (elements: one element id or error per item), in one undo step.

class CreateShellsCommand : public CreateElementsCommandBase
{
public:
    CreateShellsCommand ();
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::ObjectState> SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, const Stories& stories, const GS::ObjectState& parameters) const override;
};

class CreateRadialDimensionsCommand : public CreateElementsCommandBase
{
public:
    CreateRadialDimensionsCommand ();
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::ObjectState> SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, const Stories& stories, const GS::ObjectState& parameters) const override;
};

class CreateAngleDimensionsCommand : public CreateElementsCommandBase
{
public:
    CreateAngleDimensionsCommand ();
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::ObjectState> SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, const Stories& stories, const GS::ObjectState& parameters) const override;
};

class CreateSkylightsCommand : public CreateElementsCommandBase
{
public:
    CreateSkylightsCommand ();
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::ObjectState> SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, const Stories& stories, const GS::ObjectState& parameters) const override;
};

class CreateCamerasCommand : public CreateElementsCommandBase
{
public:
    CreateCamerasCommand ();
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::ObjectState> SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, const Stories& stories, const GS::ObjectState& parameters) const override;
};

class CreatePicturesCommand : public CreateElementsCommandBase
{
public:
    CreatePicturesCommand ();
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::ObjectState> SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, const Stories& stories, const GS::ObjectState& parameters) const override;
};

// The base of the creation commands of elements that are placed together with a marker symbol
// (ACAPI_Element_CreateExt with a Main Marker sub-element): change markers, detail and worksheet
// markers. Same per-item handling as CreateElementsCommandBase (favoriteName with the tool
// defaults put back afterwards, id, layerIndex through SetTypeSpecificParameters, one undo step,
// element notifications), with the marker of the favorite or of the tool defaults.
class CreateMarkerElementsCommandBase : public CommandBase
{
public:
    CreateMarkerElementsCommandBase (const GS::String& commandName, API_ElemTypeID elemTypeID, const GS::String& arrayFieldName);
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
protected:
    virtual GS::Optional<GS::ObjectState> SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, API_SubElement& marker, const GS::ObjectState& parameters) const = 0;
protected:
    GS::String     commandName;
    API_ElemTypeID elemTypeID;
    GS::String     arrayFieldName;
    // The subtype of elemTypeID, e.g. the Wall End or Corner Window variation of a Window.
    API_ElemVariationID variationID;
};

class CreateChangeMarkersCommand : public CreateMarkerElementsCommandBase
{
public:
    CreateChangeMarkersCommand ();
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
protected:
    virtual GS::Optional<GS::ObjectState> SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, API_SubElement& marker, const GS::ObjectState& parameters) const override;
};

class CreateDetailMarkersCommand : public CreateMarkerElementsCommandBase
{
public:
    CreateDetailMarkersCommand ();
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
protected:
    virtual GS::Optional<GS::ObjectState> SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, API_SubElement& marker, const GS::ObjectState& parameters) const override;
};

class CreateWorksheetMarkersCommand : public CreateMarkerElementsCommandBase
{
public:
    CreateWorksheetMarkersCommand ();
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
protected:
    virtual GS::Optional<GS::ObjectState> SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, API_SubElement& marker, const GS::ObjectState& parameters) const override;
};

// Wall Ends and Corner Windows are Window elements of their own subtype (API_ElemVariationID
// APIVarId_WallEnd / APIVarId_CornerWindow), placed in a wall like a window. Archicad 26 or later.
class CreateWallEndsOrCornerWindowsCommandBase : public CreateMarkerElementsCommandBase
{
public:
    CreateWallEndsOrCornerWindowsCommandBase (const GS::String& commandName, API_ElemVariationID variationID, const GS::String& arrayFieldName);
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
protected:
    virtual GS::Optional<GS::ObjectState> SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, API_SubElement& marker, const GS::ObjectState& parameters) const override;
};

class CreateWallEndsCommand : public CreateWallEndsOrCornerWindowsCommandBase
{
public:
    CreateWallEndsCommand ();
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
};

class CreateCornerWindowsCommand : public CreateWallEndsOrCornerWindowsCommandBase
{
public:
    CreateCornerWindowsCommand ();
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
};
