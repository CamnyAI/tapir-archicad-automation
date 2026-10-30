#pragma once

#include "CommandBase.hpp"

// Reads the 2D drawing primitives of every element of a section, elevation, interior elevation,
// detail, worksheet, 3D document, layout or master layout database, in the format of
// GetDrawingPrimitivesOfElements. Read only: it changes nothing in the project, and the window and
// the database that were current before are current again afterwards.

class GetDrawingPrimitivesOfDatabaseCommand : public CommandBase
{
public:
    GetDrawingPrimitivesOfDatabaseCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};
