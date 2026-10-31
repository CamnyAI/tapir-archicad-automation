#pragma once

#include "APIEnvir.h"
#include "ACAPinc.h"

#include <functional>

// Runs work later, from Archicad's main event loop, through a command of the Add-On itself
// (ACAPI_AddOnAddOnCommunication_CallFromEventLoop). The work runs with the full Archicad API,
// after the notification or JSON command that posted it has returned: the selection
// notifications use it to send one event for a burst of selection changes, and StartUserInput
// to start an input without keeping its JSON command waiting.
namespace EventLoopTasks {

// From RegisterInterface: announces the command the calls go through.
GSErrCode RegisterService ();

// From Initialize: installs the handler of that command.
GSErrCode InstallHandler ();

// Schedules the task. False when it cannot be scheduled (the service is missing, or the Add-On's
// own identifier could not be read); the caller then does the work at once or reports an error.
bool Post (const std::function<void ()>& task);

}
