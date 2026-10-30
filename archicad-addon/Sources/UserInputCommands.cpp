#include "UserInputCommands.hpp"

#include "ApplicationCommands.hpp"
#include "EventLoopTasks.hpp"
#include "MigrationHelper.hpp"
#include "NotificationCommands.hpp"

#include "GSProcessControl.hpp"
#include "MessageLoopExecutor.hpp"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

// While an input of the Archicad API waits for the user (ACAPI_UserInput_GetPoint and the
// others), Archicad's JSON interface refuses every command at once with error 4001, "Invalid
// program status (ongoing user input)", and runs no add-on command called from the event loop
// either (ACAPI_AddOnAddOnCommunication_CallFromEventLoop): a command that polls for the result,
// or that cancels the input, cannot reach the Add-On. So the result is pushed to the notification
// clients when the input ends, and the input can end on its own after a timeout: a timer thread
// posts ACAPI_UserInput_CancelUserInput to the message loop, which keeps running during the input.

namespace {

enum class InputStatus {
    NoInput,
    Scheduled,  // posted to the event loop, not started yet
    Running,    // Archicad waits for the user
    Finished
};

struct UserInput {
    InputStatus     status = InputStatus::NoInput;
    UInt32          inputId = 0;
    GS::UniString   command;
    GS::ObjectState parameters;
    double          timeout = 0.0;
    bool            timedOut = false;
    GS::ObjectState result;
};

UserInput currentInput;
UInt32    lastInputId = 0;

std::mutex              timerMutex;
std::condition_variable timerCondition;
std::thread             timerThread;
bool                    timerStopRequested = false;
GS::RunnableTask        pendingCancelTask;    // guarded by timerMutex, with cancelTaskPending
bool                    cancelTaskPending = false;

std::unique_ptr<CommandBase> CreateInputCommand (const GS::UniString& name)
{
    if (name == "GetPointFromUser") {
        return std::unique_ptr<CommandBase> (new GetPointFromUserCommand ());
    } else if (name == "GetElementFromUser") {
        return std::unique_ptr<CommandBase> (new GetElementFromUserCommand ());
    } else if (name == "GetLineFromUser") {
        return std::unique_ptr<CommandBase> (new GetLineFromUserCommand ());
    } else if (name == "GetPolygonFromUser") {
        return std::unique_ptr<CommandBase> (new GetPolygonFromUserCommand ());
    }
    return nullptr;
}

GS::UniString StatusName (InputStatus status)
{
    switch (status) {
        case InputStatus::Scheduled:
        case InputStatus::Running:  return "Waiting";
        case InputStatus::Finished: return "Finished";
        default:                    return "NoInput";
    }
}

void CancelOnTimeout (UInt32 inputId)
{
    if (currentInput.inputId == inputId && currentInput.status == InputStatus::Running) {
        currentInput.timedOut = true;
        ACAPI_UserInput_CancelUserInput ();
    }
}

// Posted to the message loop, which Archicad keeps running while the input waits; the event loop
// calls of EventLoopTasks only run after the input.
class CancelOnTimeoutTask : public GS::Runnable {
    UInt32 inputId;
public:
    explicit CancelOnTimeoutTask (UInt32 inputId) : inputId (inputId) {}
    virtual void Run () override
    {
        {
            std::lock_guard<std::mutex> lock (timerMutex);
            cancelTaskPending = false;
        }
        CancelOnTimeout (inputId);
    }
};

void StartTimer (UInt32 inputId, double seconds)
{
    StartUserInputCommand::StopTimer ();
    timerThread = std::thread ([inputId, seconds] () {
        std::unique_lock<std::mutex> lock (timerMutex);
        if (timerCondition.wait_for (lock, std::chrono::duration<double> (seconds), [] () { return timerStopRequested; })) {
            return;
        }
        pendingCancelTask = GS::RunnableTask (new CancelOnTimeoutTask (inputId));
        cancelTaskPending = true;
        const GS::RunnableTask task = pendingCancelTask;
        lock.unlock ();
        GS::MessageLoopExecutor ().Execute (task);
    });
}

void FinishInput (const GS::ObjectState& result)
{
    {
        std::lock_guard<std::mutex> lock (timerMutex);
        timerStopRequested = true;
    }
    timerCondition.notify_all ();

    currentInput.result = result;
    currentInput.status = InputStatus::Finished;

    GS::ObjectState event (
        "inputId", currentInput.inputId,
        "command", currentInput.command,
        "timedOut", currentInput.timedOut,
        "result", currentInput.result);
    AddElementNotificationClientCommand::SendUserInputFinishedEvent (event);
}

// Runs from the event loop, outside of any JSON command: the command that started it has
// answered already.
void RunScheduledInput ()
{
    if (currentInput.status != InputStatus::Scheduled) {
        return;
    }
    currentInput.status = InputStatus::Running;
    if (currentInput.timeout > 0.0) {
        StartTimer (currentInput.inputId, currentInput.timeout);
    }

    GS::ObjectState result;
    try {
        const std::unique_ptr<CommandBase> command = CreateInputCommand (currentInput.command);
        GS::NonInterruptibleProcessControl processControl;
        const GS::ObjectState parameters = currentInput.parameters;
        result = command->Execute (parameters, processControl);
    } catch (...) {
        result = CreateErrorResponse (APIERR_GENERAL, "The input failed with an exception.");
    }
    FinishInput (result);
}

}

// ---------------------------------------------------------------------------
// StartUserInput
// ---------------------------------------------------------------------------

StartUserInputCommand::StartUserInputCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String StartUserInputCommand::GetName () const
{
    return "StartUserInput";
}

void StartUserInputCommand::StopTimer ()
{
    {
        std::lock_guard<std::mutex> lock (timerMutex);
        timerStopRequested = true;
    }
    timerCondition.notify_all ();
    if (timerThread.joinable ()) {
        timerThread.join ();
    }
    std::lock_guard<std::mutex> lock (timerMutex);
    timerStopRequested = false;
    if (cancelTaskPending) {
        // Not run yet: when Archicad quits, the message loop could destroy it after the Add-On is
        // unloaded. One reference is kept for good, so that never happens.
        new GS::RunnableTask (pendingCancelTask);
        cancelTaskPending = false;
    }
}

GS::Optional<GS::UniString> StartUserInputCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "command": {
                "type": "string",
                "enum": ["GetPointFromUser", "GetElementFromUser", "GetLineFromUser", "GetPolygonFromUser"],
                "description": "The input command to start."
            },
            "parameters": {
                "type": "object",
                "description": "The parameters of that command, as the command itself takes them. Optional."
            },
            "timeout": {
                "type": "number",
                "exclusiveMinimum": 0,
                "description": "Seconds after which the input is cancelled, as if the user pressed Escape, when the user has not finished it. Optional; without it the input waits until the user finishes or cancels it."
            }
        },
        "additionalProperties": false,
        "required": [
            "command"
        ]
    })";
}

GS::Optional<GS::UniString> StartUserInputCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "inputId": {
                "type": "integer",
                "description": "Identifies this input in GetUserInputResult and in the userInputFinished notification."
            }
        },
        "additionalProperties": false,
        "required": [
            "inputId"
        ]
    })";
}

GS::ObjectState StartUserInputCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::UniString commandName;
    parameters.Get ("command", commandName);
    if (CreateInputCommand (commandName) == nullptr) {
        return CreateErrorResponse (APIERR_BADPARS, GS::UniString ("Unknown input command: ") + commandName);
    }
    if (currentInput.status == InputStatus::Scheduled || currentInput.status == InputStatus::Running) {
        return CreateErrorResponse (APIERR_REFUSEDCMD, GS::UniString::Printf ("Input %u has not finished yet.", currentInput.inputId));
    }

    double timeout = 0.0;
    parameters.Get ("timeout", timeout);

    GS::ObjectState inputParameters;
    parameters.Get ("parameters", inputParameters);

    // Checked here, so a mistake is answered now rather than in the result of the input.
    GS::Array<GS::UniString> elementTypeNames;
    if (commandName == "GetElementFromUser" && inputParameters.Get ("elementTypes", elementTypeNames)) {
        for (const GS::UniString& elementTypeName : elementTypeNames) {
            if (GetElementTypeFromNonLocalizedName (elementTypeName) == API_ZombieElemID) {
                return CreateErrorResponse (APIERR_BADPARS, GS::UniString ("Unknown element type: ") + elementTypeName);
            }
        }
    }

    currentInput = UserInput ();
    currentInput.status = InputStatus::Scheduled;
    currentInput.inputId = ++lastInputId;
    currentInput.command = commandName;
    currentInput.parameters = inputParameters;
    currentInput.timeout = timeout;

    if (!EventLoopTasks::Post (RunScheduledInput)) {
        currentInput.status = InputStatus::NoInput;
        return CreateErrorResponse (APIERR_GENERAL, "The input could not be scheduled on Archicad's event loop.");
    }

    return GS::ObjectState ("inputId", currentInput.inputId);
}

// ---------------------------------------------------------------------------
// GetUserInputResult
// ---------------------------------------------------------------------------

GetUserInputResultCommand::GetUserInputResultCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String GetUserInputResultCommand::GetName () const
{
    return "GetUserInputResult";
}

GS::Optional<GS::UniString> GetUserInputResultCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "inputId": {
                "type": "integer",
                "description": "The input StartUserInput returned. Optional; an id other than that of the input started last is an error."
            }
        },
        "additionalProperties": false,
        "required": []
    })";
}

GS::Optional<GS::UniString> GetUserInputResultCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "status": {
                "type": "string",
                "enum": ["NoInput", "Waiting", "Finished"],
                "description": "NoInput when StartUserInput was not used in this Archicad session, Waiting before the input started, Finished when it ended. While Archicad waits for the user it refuses this command, like every other, with error 4001."
            },
            "inputId": {
                "type": "integer",
                "description": "The input started last."
            },
            "command": {
                "type": "string",
                "description": "Its command."
            },
            "timedOut": {
                "type": "boolean",
                "description": "True when the timeout given to StartUserInput cancelled it."
            },
            "result": {
                "type": "object",
                "description": "When finished: the answer the command gives when it is called directly. A cancelled input gives {\"cancelled\": true} for GetElementFromUser, GetLineFromUser and GetPolygonFromUser, and an error object for GetPointFromUser."
            }
        },
        "additionalProperties": false,
        "required": [
            "status"
        ]
    })";
}

GS::ObjectState GetUserInputResultCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    UInt32 inputId = 0;
    if (parameters.Get ("inputId", inputId) && inputId != currentInput.inputId) {
        return CreateErrorResponse (APIERR_BADID, GS::UniString::Printf (
            "Unknown inputId %u: only the input started last (%u) is kept.", inputId, currentInput.inputId));
    }

    GS::ObjectState response ("status", StatusName (currentInput.status));
    if (currentInput.status == InputStatus::NoInput) {
        return response;
    }
    response.Add ("inputId", currentInput.inputId);
    response.Add ("command", currentInput.command);
    response.Add ("timedOut", currentInput.timedOut);
    if (currentInput.status == InputStatus::Finished) {
        response.Add ("result", currentInput.result);
    }
    return response;
}
