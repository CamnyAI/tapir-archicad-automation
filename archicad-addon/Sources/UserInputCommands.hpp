#pragma once

#include "CommandBase.hpp"

// Starts one of the "click in Archicad" commands (GetPointFromUser, GetElementFromUser,
// GetLineFromUser, GetPolygonFromUser) without keeping its JSON command waiting for the click:
// the input runs from Archicad's event loop, and its result is delivered to the notification
// clients that set notifyOnUserInput, and kept for GetUserInputResult.
class StartUserInputCommand : public CommandBase
{
public:
    StartUserInputCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;

    // From FreeData: stops the timer thread of a waiting input.
    static void StopTimer ();
};

class GetUserInputResultCommand : public CommandBase
{
public:
    GetUserInputResultCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};
