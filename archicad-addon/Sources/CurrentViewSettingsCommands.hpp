#pragma once

#include "CommandBase.hpp"

// The Project Map item (story, section, the generic perspective or axonometry, ...) the given
// window shows. Archicad 27 and later.
GSErrCode FindProjectMapItemOfCurrentWindow (const API_WindowInfo& window, API_NavigatorItem& item);

class GetCurrentModelViewOptionCommand : public CommandBase
{
public:
    GetCurrentModelViewOptionCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class SetCurrentModelViewOptionCommand : public CommandBase
{
public:
    SetCurrentModelViewOptionCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class SetCurrentPenTableCommand : public CommandBase
{
public:
    SetCurrentPenTableCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};
