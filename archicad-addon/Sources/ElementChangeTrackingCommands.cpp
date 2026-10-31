#include "ElementChangeTrackingCommands.hpp"
#include "MigrationHelper.hpp"

#include "FileSystem.hpp"

namespace {

// A state captured by CaptureElementState: the generator type it was captured with and the file
// the generator wrote it to. ACAPI_DifferenceGenerator_GenerateDifference takes one type for both
// of its states, so two states captured with different types cannot be compared. The states are
// kept in files (APIDiffState_InFile), as the DevKit's Plan_Dump example does for every type: the
// modification stamp based generator "operates only with file" (APIdefs_ElementDifferenceGenerator.h).
struct StoredElementState {
    API_ElemDifferenceGeneratorTypeID type = APIDiff_ModificationStampBased;
    IO::Location                      fileLocation;
};

// Keyed by the stateId handed out by CaptureElementState. Every command runs on the main thread
// (CommandBase::GetExecutionPolicy) and so does the project event handler, so no lock is needed.
GS::HashTable<API_Guid, StoredElementState>& StoredStates ()
{
    static GS::HashTable<API_Guid, StoredElementState> states;
    return states;
}

const StoredElementState* FindStoredState (const API_Guid& stateId)
{
    return StoredStates ().GetPtr (stateId);
}

// <temporary folder>/Tapir/ElementStates/<stateId>: the stateId is a new GUID, so the states of
// other Archicad instances never share a file.
GSErrCode GetStateFileLocation (const API_Guid& stateId, API_ElemDifferenceGeneratorTypeID type, IO::Location& fileLocation)
{
    GSErrCode err = IO::fileSystem.GetSpecialLocation (IO::FileSystem::TemporaryFolder, &fileLocation);
    if (err != NoError) {
        return err;
    }
    fileLocation.AppendToLocal (IO::Name ("Tapir"));
    fileLocation.AppendToLocal (IO::Name ("ElementStates"));
    err = IO::fileSystem.CreateFolderTree (fileLocation);
    if (err != NoError) {
        return err;
    }
    // The modification stamp based state is a project file (Plan_Dump names it ForCompare.pln).
    GS::UniString fileName = APIGuidToString (stateId);
    if (type == APIDiff_ModificationStampBased) {
        fileName += ".pln";
    }
    fileLocation.AppendToLocal (IO::Name (fileName));
    return NoError;
}

void DeleteStateFile (const StoredElementState& stored)
{
    IO::fileSystem.Delete (stored.fileLocation);
}

bool ParseStateType (const GS::UniString& str, API_ElemDifferenceGeneratorTypeID& type)
{
    if (str == "ModificationStamp") {
        type = APIDiff_ModificationStampBased;
        return true;
    }
    if (str == "3DModel") {
        type = APIDiff_3DModelBased;
        return true;
    }
    return false;
}

GS::UniString StateTypeToString (API_ElemDifferenceGeneratorTypeID type)
{
    switch (type) {
        case APIDiff_3DModelBased:  return "3DModel";
        default:
        case APIDiff_ModificationStampBased: return "ModificationStamp";
    }
}

GS::ObjectState UnknownStateIdError (const char* parameterName, const API_Guid& stateId)
{
    return CreateErrorResponse (APIERR_BADID,
        GS::UniString::Printf ("Unknown %s %T: it was not captured by CaptureElementState in this Archicad session, or it was released - by ReleaseElementState, by closing the project or by restarting Archicad.",
            parameterName, APIGuidToString (stateId).ToPrintf ()));
}

template <typename ElementGuids>
void AddElementList (GS::ObjectState& response, const char* fieldName, const ElementGuids& guids)
{
    const auto& elements = response.AddList<GS::ObjectState> (fieldName);
    for (const API_Guid& guid : guids) {
        elements (CreateElementIdObjectState (guid));
    }
}

// The 3D model based generator compares the 3D model of the 3D window, and Archicad builds and
// updates that model only while the 3D window is in front ("you get the model which can be seen in
// the 3D window", ACAPI_ModelAccess_GetComponent in ACAPinc.h). Tested in Archicad 30: with the
// floor plan in front every 3DModel comparison was empty, also with the 3D window open behind it;
// a model generated in a sight of our own (ACAPI_Sight_CreateSight,
// ACAPI_ModelAccess_GenerateModelWithSeparateComponents) and ACAPI_Database_RebuildCurrentDatabase
// on the 3D database did not change that, and ACAPI_Sight_Switch3DSight crashed Archicad while the
// 3D window had never been open. So while a 3DModel state is captured or compared with the current
// project, the 3D window is brought to the front (and rebuilt if the Work Environment does not
// rebuild it when it is brought to the front), and the window and database that were current
// before are brought back when this guard goes out of scope. ACAPI_Window_CloseWindow only closes
// an add-on's own windows, so the 3D window stays open behind them.
class Front3DWindow
{
public:
    Front3DWindow () = default;
    Front3DWindow (const Front3DWindow&) = delete;
    Front3DWindow& operator= (const Front3DWindow&) = delete;

    GSErrCode Activate ()
    {
        API_WindowInfo currentWindow {};
        GSErrCode err = ACAPI_Window_GetCurrentWindow (&currentWindow);
        if (err != NoError) {
            return err;
        }
        if (currentWindow.typeID == APIWind_3DModelID) {
            return NoError;
        }

        previousWindow = currentWindow;
        hasPreviousDatabase = ACAPI_Database_GetCurrentDatabase (&previousDatabase) == NoError;

        API_WindowInfo window3D {};
        window3D.typeID = APIWind_3DModelID;
        err = ACAPI_Window_ChangeWindow (&window3D);
        if (err != NoError) {
            return err;
        }
        switched = true;

        API_ImagingAndCalcPrefs imagingPrefs {};
        if (ACAPI_ProjectSetting_GetPreferences (&imagingPrefs, APIPrefs_ImagingAndCalcID) == NoError && !imagingPrefs.autoRebuild3D) {
            err = ACAPI_View_Rebuild (nullptr);
        }
        return err;
    }

    ~Front3DWindow ()
    {
        if (!switched) {
            return;
        }
        ACAPI_Window_ChangeWindow (&previousWindow);
        if (hasPreviousDatabase) {
            ACAPI_Database_ChangeCurrentDatabase (&previousDatabase);
        }
    }

private:
    bool             switched = false;
    bool             hasPreviousDatabase = false;
    API_WindowInfo   previousWindow {};
    API_DatabaseInfo previousDatabase {};
};

GS::ObjectState Front3DWindowError (GSErrCode err)
{
    return CreateErrorResponse (err, "The 3DModel state is the 3D model of the 3D window, which Archicad only updates while the 3D window is in front, and the 3D window could not be brought to the front (or rebuilt there).");
}

GSErrCode ProjectEventHandler (API_NotifyEventID notifID, Int32 /*param*/)
{
    switch (notifID) {
        case APINotify_New:
        case APINotify_Open:
        case APINotify_Close:
        case APINotify_Quit:
            ReleaseAllElementStates ();
            break;
        default:
            break;
    }
    return NoError;
}

} // namespace

GSErrCode InstallElementStateProjectEventHandler ()
{
    return ACAPI_ProjectOperation_CatchProjectEvent (APINotify_New | APINotify_Open | APINotify_Close | APINotify_Quit, ProjectEventHandler);
}

void ReleaseAllElementStates ()
{
    StoredStates ().EnumerateValues ([] (const StoredElementState& stored) {
        DeleteStateFile (stored);
    });
    StoredStates ().Clear ();
}

CaptureElementStateCommand::CaptureElementStateCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String CaptureElementStateCommand::GetName () const
{
    return "CaptureElementState";
}

GS::Optional<GS::UniString> CaptureElementStateCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "type": {
                "type": "string",
                "description": "How GetElementChangesSince finds the changed elements between this state and another one. ModificationStamp (the default) compares the modification stamps of the elements, so any change to an element counts. 3DModel compares the 3D model of the 3D window, so only a change that alters the 3D model counts, and only the elements the 3D window shows are covered (all of them after Show All in 3D; a selection shown in 3D and the 3D filter settings limit them). Archicad updates that model only while the 3D window is in front, so capturing a 3DModel state, and comparing one with the current project, brings the 3D window to the front for the call and then the window that was in front back; the 3D window stays open behind it. Two states can only be compared when they were captured with the same type.",
                "enum": [
                    "ModificationStamp",
                    "3DModel"
                ]
            }
        },
        "additionalProperties": false,
        "required": []
    })";
}

GS::Optional<GS::UniString> CaptureElementStateCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "stateId": {
                "type": "object",
                "description": "The identifier of the captured state, accepted by GetElementChangesSince and ReleaseElementState. It is only valid in the Archicad session and project it was captured in.",
                "properties": {
                    "guid": {
                        "$ref": "#/Guid"
                    }
                },
                "additionalProperties": false,
                "required": [
                    "guid"
                ]
            },
            "type": {
                "type": "string",
                "description": "The type the state was captured with.",
                "enum": [
                    "ModificationStamp",
                    "3DModel"
                ]
            }
        },
        "additionalProperties": false,
        "required": [
            "stateId",
            "type"
        ]
    })";
}

GS::ObjectState CaptureElementStateCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::UniString typeStr = "ModificationStamp";
    parameters.Get ("type", typeStr);

    StoredElementState stored;
    if (!ParseStateType (typeStr, stored.type)) {
        return CreateErrorResponse (APIERR_BADPARS,
            GS::UniString::Printf ("Invalid type '%T'. Use ModificationStamp or 3DModel.", typeStr.ToPrintf ()));
    }

    GS::Guid guid;
    guid.Generate ();
    const API_Guid stateId = GSGuid2APIGuid (guid);

    GSErrCode err = GetStateFileLocation (stateId, stored.type, stored.fileLocation);
    if (err != NoError) {
        return CreateErrorResponse (err, "Failed to create the folder of the element states in the temporary folder.");
    }

    Front3DWindow front3DWindow;
    if (stored.type == APIDiff_3DModelBased) {
        err = front3DWindow.Activate ();
        if (err != NoError) {
            DeleteStateFile (stored);
            return Front3DWindowError (err);
        }
    }

    API_ElemDifferenceGeneratorState state;
    state.stateType = APIDiffState_InFile;
    state.fileLocation = stored.fileLocation;
    err = ACAPI_DifferenceGenerator_GetState (stored.type, &state);
    if (err != NoError) {
        DeleteStateFile (stored);
        return CreateErrorResponse (err, GS::UniString::Printf ("Failed to capture the %T based element state.", StateTypeToString (stored.type).ToPrintf ()));
    }
    StoredStates ().Add (stateId, stored);

    GS::ObjectState response;
    response.Add ("stateId", CreateGuidObjectState (stateId));
    response.Add ("type", StateTypeToString (stored.type));
    return response;
}

GetElementChangesSinceCommand::GetElementChangesSinceCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String GetElementChangesSinceCommand::GetName () const
{
    return "GetElementChangesSince";
}

GS::Optional<GS::UniString> GetElementChangesSinceCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "stateId": {
                "type": "object",
                "description": "The state to compare from, as returned by CaptureElementState.",
                "properties": {
                    "guid": {
                        "$ref": "#/Guid"
                    }
                },
                "additionalProperties": false,
                "required": [
                    "guid"
                ]
            },
            "toStateId": {
                "type": "object",
                "description": "The state to compare to, as returned by CaptureElementState with the same type as stateId. If omitted, the current state of the project is used, so the response lists what changed since stateId was captured; for a 3DModel state that brings the 3D window to the front for the call, as CaptureElementState does.",
                "properties": {
                    "guid": {
                        "$ref": "#/Guid"
                    }
                },
                "additionalProperties": false,
                "required": [
                    "guid"
                ]
            }
        },
        "additionalProperties": false,
        "required": [
            "stateId"
        ]
    })";
}

GS::Optional<GS::UniString> GetElementChangesSinceCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "newElements": {
                "$ref": "#/Elements",
                "description": "The elements that exist in the later state but not in the earlier one."
            },
            "modifiedElements": {
                "$ref": "#/Elements",
                "description": "The elements that exist in both states and changed between them."
            },
            "deletedElements": {
                "$ref": "#/Elements",
                "description": "The elements that exist in the earlier state but not in the later one. They cannot be read any more, only their identifiers are known."
            }
        },
        "additionalProperties": false,
        "required": [
            "newElements",
            "modifiedElements",
            "deletedElements"
        ]
    })";
}

GS::ObjectState GetElementChangesSinceCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    const GS::ObjectState* stateIdOS = parameters.Get ("stateId");
    if (stateIdOS == nullptr) {
        return CreateErrorResponse (APIERR_BADPARS, "stateId is required.");
    }
    const API_Guid fromStateId = GetGuidFromObjectState (*stateIdOS);
    const StoredElementState* from = FindStoredState (fromStateId);
    if (from == nullptr) {
        return UnknownStateIdError ("stateId", fromStateId);
    }

    API_ElemDifferenceGeneratorState fromState;
    fromState.stateType = APIDiffState_InFile;
    fromState.fileLocation = from->fileLocation;

    // Without toStateId the comparison runs against the project as it is now
    // (APIDiffState_CurrentProject, which needs no ACAPI_DifferenceGenerator_GetState call).
    API_ElemDifferenceGeneratorState toState;
    toState.stateType = APIDiffState_CurrentProject;
    const GS::ObjectState* toStateIdOS = parameters.Get ("toStateId");
    if (toStateIdOS != nullptr) {
        const API_Guid toStateId = GetGuidFromObjectState (*toStateIdOS);
        const StoredElementState* to = FindStoredState (toStateId);
        if (to == nullptr) {
            return UnknownStateIdError ("toStateId", toStateId);
        }
        if (to->type != from->type) {
            return CreateErrorResponse (APIERR_BADPARS,
                GS::UniString::Printf ("stateId was captured with type %T and toStateId with type %T; only states of the same type can be compared.",
                    StateTypeToString (from->type).ToPrintf (), StateTypeToString (to->type).ToPrintf ()));
        }
        toState.stateType = APIDiffState_InFile;
        toState.fileLocation = to->fileLocation;
    }

    // Two stored states need no current 3D model; the current project's does.
    Front3DWindow front3DWindow;
    if (from->type == APIDiff_3DModelBased && toState.stateType == APIDiffState_CurrentProject) {
        const GSErrCode err = front3DWindow.Activate ();
        if (err != NoError) {
            return Front3DWindowError (err);
        }
    }

    API_ElemDifference difference {};
    const GSErrCode err = ACAPI_DifferenceGenerator_GenerateDifference (from->type, &fromState, &toState, difference);
    if (err != NoError) {
        return CreateErrorResponse (err, "Failed to generate the difference between the two element states.");
    }

    GS::ObjectState response;
    AddElementList (response, "newElements", difference.newElements);
    AddElementList (response, "modifiedElements", difference.modifiedElements);
    AddElementList (response, "deletedElements", difference.deletedElements);
    return response;
}

ReleaseElementStateCommand::ReleaseElementStateCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String ReleaseElementStateCommand::GetName () const
{
    return "ReleaseElementState";
}

GS::Optional<GS::UniString> ReleaseElementStateCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "stateId": {
                "type": "object",
                "description": "The state to free, as returned by CaptureElementState.",
                "properties": {
                    "guid": {
                        "$ref": "#/Guid"
                    }
                },
                "additionalProperties": false,
                "required": [
                    "guid"
                ]
            }
        },
        "additionalProperties": false,
        "required": [
            "stateId"
        ]
    })";
}

GS::Optional<GS::UniString> ReleaseElementStateCommand::GetRawResponseSchema () const
{
    return R"({
        "$ref": "#/ExecutionResult"
    })";
}

GS::ObjectState ReleaseElementStateCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    const GS::ObjectState* stateIdOS = parameters.Get ("stateId");
    if (stateIdOS == nullptr) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "stateId is required.");
    }
    const API_Guid stateId = GetGuidFromObjectState (*stateIdOS);
    const StoredElementState* stored = FindStoredState (stateId);
    if (stored == nullptr) {
        return CreateFailedExecutionResult (UnknownStateIdError ("stateId", stateId));
    }

    DeleteStateFile (*stored);
    StoredStates ().Delete (stateId);
    return CreateSuccessfulExecutionResult ();
}
