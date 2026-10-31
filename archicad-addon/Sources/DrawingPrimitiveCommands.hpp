#pragma once

#include "CommandBase.hpp"

// Drawing primitive commands: read the 2D drawing primitives (lines, arcs, polygons, texts, ...)
// Archicad draws an element from. Read only, they change nothing in the project.

class GetDrawingPrimitivesOfElementsCommand : public CommandBase
{
public:
    GetDrawingPrimitivesOfElementsCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class GetStoryControlPrimitivesCommand : public CommandBase
{
public:
    GetStoryControlPrimitivesCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

// The primitive converter of GetDrawingPrimitivesOfElements, for the commands of other files.
// Decomposes one element of the current database into `primitives`, each item in the format of
// one item of GetDrawingPrimitivesOfElements' primitives list.
GSErrCode GetDrawingPrimitivesOfElement (const API_Elem_Head& elemHead, GS::Array<GS::ObjectState>& primitives);
// The JSON schema of one item of a primitives list.
GS::UniString DrawingPrimitiveItemSchema ();
