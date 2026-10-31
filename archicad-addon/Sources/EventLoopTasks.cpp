#include "EventLoopTasks.hpp"

#include "MigrationHelper.hpp"
#include "RS.hpp"

#include <mutex>
#include <vector>

namespace EventLoopTasks {

namespace {

const GSType EventLoopCommandId      = 'TELT';
const Int32  EventLoopCommandVersion = 1;

// The 'MDID' resource Archicad identifies the Add-On by (RFIX/AddOnFix.grc).
const GSResID OwnModuleIdResourceId = 32500;

std::mutex                         queueMutex;
std::vector<std::function<void ()>> queue;
bool                               callScheduled = false;

UInt32 SwapBytes (UInt32 value)
{
    return ((value & 0x000000FFu) << 24) | ((value & 0x0000FF00u) << 8) |
           ((value & 0x00FF0000u) >> 8)  | ((value & 0xFF000000u) >> 24);
}

// Read from the resource rather than written in here, so a build with its own identifier
// (an own 'MDID') keeps calling itself. The stored byte order is not documented, so both are
// tried; ACAPI_AddOnAddOnCommunication_Test tells which one is this Add-On.
const API_ModulID* GetOwnModuleId ()
{
    static bool        looked = false;
    static bool        found = false;
    static API_ModulID ownId = {};
    if (looked) {
        return found ? &ownId : nullptr;
    }
    looked = true;

    GSHandle handle = RSLoadResource ('MDID', ACAPI_GetOwnResModule (), OwnModuleIdResourceId);
    if (handle == nullptr) {
        return nullptr;
    }
    if (BMGetHandleSize (handle) >= (GSSize) (2 * sizeof (UInt32))) {
        const UInt32* values = reinterpret_cast<const UInt32*> (*handle);
        const API_ModulID candidates[] = {
            { (GSType) values[0], (GSType) values[1] },
            { (GSType) SwapBytes (values[0]), (GSType) SwapBytes (values[1]) }
        };
        for (const API_ModulID& candidate : candidates) {
            if (ACAPI_AddOnAddOnCommunication_Test (&candidate, EventLoopCommandId, EventLoopCommandVersion) == NoError) {
                ownId = candidate;
                found = true;
                break;
            }
        }
    }
    BMKillHandle (&handle);
    return found ? &ownId : nullptr;
}

GSErrCode EventLoopCommandHandler (GSHandle /*params*/, GSPtr /*resultData*/, bool /*silentMode*/)
{
    std::vector<std::function<void ()>> tasks;
    {
        std::lock_guard<std::mutex> lock (queueMutex);
        tasks.swap (queue);
        callScheduled = false;
    }
    for (const auto& task : tasks) {
        try {
            task ();
        } catch (...) {
        }
    }
    return NoError;
}

}

GSErrCode RegisterService ()
{
    return ACAPI_AddOnAddOnCommunication_RegisterSupportedService (EventLoopCommandId, EventLoopCommandVersion);
}

GSErrCode InstallHandler ()
{
    return ACAPI_AddOnIntegration_InstallModulCommandHandler (EventLoopCommandId, EventLoopCommandVersion, EventLoopCommandHandler);
}

bool Post (const std::function<void ()>& task)
{
    const API_ModulID* ownId = GetOwnModuleId ();
    if (ownId == nullptr) {
        return false;
    }

    // Posted and run on the main thread; the lock only keeps the queue consistent. One call is
    // scheduled at a time, and it runs every task posted until it starts.
    {
        std::lock_guard<std::mutex> lock (queueMutex);
        queue.push_back (task);
        if (callScheduled) {
            return true;
        }
        callScheduled = true;
    }
    if (ACAPI_AddOnAddOnCommunication_CallFromEventLoop (ownId, EventLoopCommandId, EventLoopCommandVersion, nullptr, true, nullptr) != NoError) {
        std::lock_guard<std::mutex> lock (queueMutex);
        queue.clear ();
        callScheduled = false;
        return false;
    }
    return true;
}

}
