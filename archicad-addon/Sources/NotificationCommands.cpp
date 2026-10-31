#include "NotificationCommands.hpp"

#include "HTTP/Client/ClientConnection.hpp"
#include "HTTP/Client/Request.hpp"
#include "HTTP/Client/Response.hpp"
#include "HTTP/Client/Authentication/BasicAuthentication.hpp"
#include "JSON/JDOMParser.hpp"
#include "IBinaryChannelUtilities.hpp"
#include "IOBinProtocolXs.hpp"
#include "IChannelX.hpp"
#include "ObjectStateJSONConversion.hpp"

#include <string>

#include "MigrationHelper.hpp"
#include "EventLoopTasks.hpp"

static
GS::String ElementEventTypeToString (AddElementNotificationClientCommand::ElementEventType eventType)
{
    switch (eventType) {
        case AddElementNotificationClientCommand::ElementEventType::New:
            return "newElements";
        case AddElementNotificationClientCommand::ElementEventType::Changed:
            return "changedElements";
        case AddElementNotificationClientCommand::ElementEventType::Deleted:
            return "deletedElements";
        case AddElementNotificationClientCommand::ElementEventType::Reserved:
            return "reservedElements";
        case AddElementNotificationClientCommand::ElementEventType::Released:
            return "releasedElements";
        default:
            return "unknown";
    }
}

class MessageSenderTask : public GS::Runnable {
    AddElementNotificationClientCommand::EventQueue eventQueue;
public:
    MessageSenderTask (AddElementNotificationClientCommand::EventQueue& q)
    {
        std::swap (eventQueue, q);
    }
    virtual void Run ()
    {
        for (auto& kv : AddElementNotificationClientCommand::clients) {
            auto& client = kv.second;

            bool isEmptyMessage = true;
            GS::ObjectState message;
            for (const auto& kv : eventQueue) {
                if (!client.acceptsNotificationsForEventType (kv.first)) {
                    continue;
                }

                const auto& events = message.AddList<GS::ObjectState> (ElementEventTypeToString (kv.first));
                for (const auto& os : kv.second) {
                    events (os);
                    isEmptyMessage = false;
                }
            }

            if (isEmptyMessage) {
                continue;
            }

            AddElementNotificationClientCommand::SendMessageToNotificationClient (client, message);
        }
    }
};

std::map<GS::UniString, AddElementNotificationClientCommand::Client> AddElementNotificationClientCommand::clients;
bool AddElementNotificationClientCommand::hasClientToNotifyOnNew = false;
bool AddElementNotificationClientCommand::hasClientToNotifyOnModification = false;
bool AddElementNotificationClientCommand::hasClientToNotifyOnReservationChanges = false;
bool AddElementNotificationClientCommand::hasClientToNotifyOnSelectionChange = false;
bool AddElementNotificationClientCommand::hasClientToNotifyOnProjectEvents = false;
std::unique_ptr<AddElementNotificationClientCommand::EventQueue> AddElementNotificationClientCommand::queuedEvents;
GS::HashTable<API_Guid, GS::Pair<API_ElemTypeID, API_Guid>> AddElementNotificationClientCommand::trackedHostedElements;
bool AddElementNotificationClientCommand::selectionEventScheduled = false;
bool AddElementNotificationClientCommand::hasLastSentSelection = false;
UInt64 AddElementNotificationClientCommand::lastSentSelectionFingerprint = 0;
UInt32 AddElementNotificationClientCommand::eventSequence = 0;
GS::Thread AddElementNotificationClientCommand::messageSenderThread;

void AddElementNotificationClientCommand::Client::Send(HTTP::Client::Request& request, const GS::UniString* body)
{
    try {
        HTTP::Client::ClientConnection clientConnection (url);
        clientConnection.SetKeepAlive (false);
        clientConnection.SetTimeout (100); // 0.1 seconds
        clientConnection.Connect ();
        // The length in bytes of the UTF-8 text, not in characters: a body with non-ASCII text
        // (a Teamwork user name) was cut short.
        const std::string utf8Body (body->ToCStr (CC_UTF8).Get ());
        clientConnection.Send (request, utf8Body.c_str (), (USize) utf8Body.size ());
        clientConnection.Close (true);
    } catch (const GS::Exception& e) {
        e.Print (dbChannel);
    } catch (...) {
    }
}

AddElementNotificationClientCommand::AddElementNotificationClientCommand () :
    CommandBase (CommonSchema::NotUsed)
{
}

GS::String AddElementNotificationClientCommand::GetName () const
{
    return "SetElementNotificationClient";
}

GS::Optional<GS::UniString> AddElementNotificationClientCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "host": {
                "type": "string",
                "description": "The host address of the notification client. If not provided, localhost is used."
            },
            "port": {
                "type": "integer",
                "description": "The port number of the notification client."
            },
            "notifyOnNewElement": {
                "type": "boolean",
                "description": "Notify on creation of a new element. Optional parameter, by default true."
            },
            "notifyOnModificationOfAnElement": {
                "type": "boolean",
                "description": "Notify on modification/deletion of an element. Optional parameter, by default true."
            },
            "notifyOnReservationChanges": {
                "type": "boolean",
                "description": "Notify on reservation changes of an element. Optional parameter, by default true."
            },
            "notifyOnSelectionChange": {
                "type": "boolean",
                "description": "Notify when the selection changes: the message has a selectionChanged object with a sequence number, the number of selected elements (selectedCount), the ids and types of the selected elements, at most maxSelectedElementIds of them, and truncated when more are selected. A burst of changes (a command that selects many elements, a marquee) gives one message with the selection after it, and a change that leaves the same elements selected gives none. Optional parameter, by default false. A client that only wants these sets notifyOnNewElement, notifyOnModificationOfAnElement and notifyOnReservationChanges to false."
            },
            "maxSelectedElementIds": {
                "type": "integer",
                "minimum": 0,
                "description": "The most element ids a selectionChanged message lists; selectedCount always gives the full number. Optional parameter, by default 1000."
            },
            "notifyOnProjectEvents": {
                "type": "boolean",
                "description": "Notify on project events: the message has a projectEvent object with a sequence number and the event, one of New, NewAndReset, Open, PreSave, Save, Close, Quit, SendChanges, ReceiveChanges (Teamwork), ChangeProjectDB (with a cause: Other, StoriesEdited or Merge), ChangeWindow, ChangeFloor (another story), ChangeLibrary, AllInputFinished (an open or merge finished). Autosaves are not reported: Archicad runs one after almost every change. Optional parameter, by default false."
            },
            "notifyOnUserInput": {
                "type": "boolean",
                "description": "Notify when an input started by StartUserInput finishes: the message has a userInputFinished object with a sequence number, the inputId, the command and its result. Optional parameter, by default false."
            }
        },
        "additionalProperties": false,
        "required": [
            "port"
        ]
    })";
}

GS::Optional<GS::UniString> AddElementNotificationClientCommand::GetRawResponseSchema () const
{
    return R"({
        "$ref": "#/ExecutionResult"
    })";
}

GS::ObjectState AddElementNotificationClientCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    Int32 port = -1;
    if (parameters.Get ("port", port)) {
        if (port <= 0 || port > 65535) {
            return CreateFailedExecutionResult (APIERR_BADPARS, "Invalid port number.");
        }
    } else {
        return CreateFailedExecutionResult (APIERR_BADPARS, "Port number is required.");
    }

    GS::UniString host = "localhost";
    parameters.Get ("host", host);

    bool notifyOnNew = true;
    parameters.Get ("notifyOnNewElement", notifyOnNew);

    bool notifyOnModification = true;
    parameters.Get ("notifyOnModificationOfAnElement", notifyOnModification);

    bool notifyOnReservationChanges = true;
    parameters.Get ("notifyOnReservationChanges", notifyOnReservationChanges);

    bool notifyOnSelectionChange = false;
    parameters.Get ("notifyOnSelectionChange", notifyOnSelectionChange);

    Int32 maxSelectedElementIds = 1000;
    parameters.Get ("maxSelectedElementIds", maxSelectedElementIds);
    if (maxSelectedElementIds < 0) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "maxSelectedElementIds cannot be negative.");
    }

    bool notifyOnProjectEvents = false;
    parameters.Get ("notifyOnProjectEvents", notifyOnProjectEvents);

    bool notifyOnUserInput = false;
    parameters.Get ("notifyOnUserInput", notifyOnUserInput);

    const GS::UniString connectionUrlStr = GS::UniString::Printf ("http://%T:%d/", host.ToPrintf (), port);
    const IO::URI::URI connectionUrl (connectionUrlStr);
    auto result = clients.emplace(std::piecewise_construct, std::forward_as_tuple(connectionUrlStr), std::forward_as_tuple(connectionUrl, notifyOnNew, notifyOnModification, notifyOnReservationChanges));
    Client& client = result.first->second;
    client.notifyOnSelectionChange = notifyOnSelectionChange;
    client.maxSelectedElementIds = maxSelectedElementIds;
    client.notifyOnProjectEvents = notifyOnProjectEvents;
    client.notifyOnUserInput = notifyOnUserInput;
    if (!result.second) {
        result.first->second.notifyOnNew = notifyOnNew;
        result.first->second.notifyOnModification = notifyOnModification;
        result.first->second.notifyOnReservationChanges = notifyOnReservationChanges;
        // A client registered again without modification notifications may have
        // been the last one that wanted them; detach the observers in that case,
        // as RemoveElementNotificationClient does.
        DetachElementObserversIfNotNeeded ();
    }

    if (notifyOnNew && !hasClientToNotifyOnNew) {
        ACAPI_Element_CatchNewElement (nullptr, ElementEventHandlerProc);
        hasClientToNotifyOnNew = true;
    }

    if (notifyOnModification && !hasClientToNotifyOnModification) {
        AttachObserversToAllElements ();
        hasClientToNotifyOnModification = true;
    }

    if (notifyOnReservationChanges && !hasClientToNotifyOnReservationChanges) {
        ACAPI_Notification_CatchElementReservationChange (ElementReservationChangeHandler);
        hasClientToNotifyOnReservationChanges = true;
    }

    ACAPI_Element_InstallElementObserver (ElementEventHandlerProc);

    if (notifyOnSelectionChange) {
        // A client registered now gets the next selection even when it is the one sent last.
        hasLastSentSelection = false;
    }
    UpdateSelectionAndProjectEventHandlers ();

    return CreateSuccessfulExecutionResult ();
}

void
AddElementNotificationClientCommand::AttachObserversToAllElements ()
{
    GS::Array<API_Guid> elementIds;
    ACAPI_Element_GetElemList (API_ZombieElemID, &elementIds);
    for (const auto& elemId : elementIds) {
        ACAPI_Element_AttachObserver (elemId);
    }

    trackedHostedElements.Clear ();
    for (API_ElemTypeID typeID : { API_DoorID, API_WindowID, API_SkylightID }) {
        GS::Array<API_Guid> hostedIds;
        ACAPI_Element_GetElemList (typeID, &hostedIds);
        for (const auto& elemId : hostedIds) {
            TrackHostedElement (elemId, typeID);
        }
    }
}

void
AddElementNotificationClientCommand::DetachElementObserversIfNotNeeded ()
{
    if (!hasClientToNotifyOnModification) {
        return;
    }

    for (const auto& kv : clients) {
        if (kv.second.notifyOnModification) {
            return;
        }
    }

    // Without this the observer stays attached to every element for the rest of the
    // session after the last client was removed (see issue #762).
    GS::Array<API_Guid> elementIds;
    ACAPI_Element_GetElemList (API_ZombieElemID, &elementIds);
    for (const auto& elemId : elementIds) {
        ACAPI_Element_DetachObserver (elemId);
    }
    trackedHostedElements.Clear ();
    hasClientToNotifyOnModification = false;
}

void
AddElementNotificationClientCommand::SendEventToNotificationClient (ElementEventType eventType, const GS::ObjectState& os)
{
    if (queuedEvents) {
        (*queuedEvents)[eventType].push_back (os);
        return;
    }

    GS::ObjectState message;
    const auto& events = message.AddList<GS::ObjectState> (ElementEventTypeToString (eventType));
    events (os);

    for (auto& kv : clients) {
        auto& client = kv.second;
        if (!client.acceptsNotificationsForEventType (eventType)) {
            continue;
        }

        SendMessageToNotificationClient (client, message);
    }
}

void
AddElementNotificationClientCommand::SendQueuedEventsToNotificationClient ()
{
    if (!queuedEvents) {
        return;
    }

    messageSenderThread = GS::Thread (new MessageSenderTask (*queuedEvents), "ElementNotifications");
    messageSenderThread.Start ();
    queuedEvents.reset ();
}

void
AddElementNotificationClientCommand::SendMessageToNotificationClient (Client& client, const GS::ObjectState& os)
{
    HTTP::Client::Request postRequest (HTTP::MessageHeader::Method::Post, "/element_notification");

    GS::UniString postBody;
    JSON::CreateFromObjectState (os, postBody);
    if (postBody.GetLast () != '\n') {
        postBody.Append ("\n");
    }

    postRequest.GetRequestHeaderFieldCollection ().Add (HTTP::MessageHeader::HeaderFieldName::ContentType, "application/json");

    client.Send (postRequest, &postBody);
}

GSErrCode
AddElementNotificationClientCommand::ElementEventHandlerProc (const API_NotifyElementType *elemType)
{
    if (elemType->notifID == APINotifyElement_BeginEvents) {
        SendQueuedEventsToNotificationClient ();
        queuedEvents.reset (new EventQueue ());
    } else if (elemType->notifID == APINotifyElement_EndEvents) {
        SendQueuedEventsToNotificationClient ();
    } else {
        // Only the header of the notified element is used here. Building the parent
        // element with ACAPI_Notification_GetParentElement runs inside the command's
        // post-processing, and for a library part it also runs its 2D script; a script
        // that requests a property (REQUEST "Properties_Of_Parent" or
        // "Property_Value_Of_Parent") hits an ODB assertion at that point and takes
        // Archicad down (see issue #762).
        const API_ElemTypeID typeID = GetElemTypeId (elemType->elemHead);
        GS::ObjectState message (
            "elementId", CreateGuidObjectState (elemType->elemHead.guid),
            "elementType", GetElementTypeNonLocalizedName (typeID));

        switch (elemType->notifID) {
            case APINotifyElement_New:
            case APINotifyElement_Copy:
            case APINotifyElement_Undo_Deleted:
            case APINotifyElement_Redo_Created:
                SendEventToNotificationClient (ElementEventType::New, message);
                if (hasClientToNotifyOnModification) {
                    ACAPI_Element_AttachObserver (elemType->elemHead.guid);
                    TrackHostedElement (elemType->elemHead.guid, typeID);
                }
                break;

            case APINotifyElement_Change:
            case APINotifyElement_Edit:
            case APINotifyElement_Undo_Modified:
            case APINotifyElement_Redo_Modified:
            case APINotifyElement_PropertyValueChange:
            case APINotifyElement_ClassificationChange:
                SendEventToNotificationClient (ElementEventType::Changed, message);
                // An undone deletion of a wall brings its doors and windows back as changed.
                if (hasClientToNotifyOnModification && !trackedHostedElements.ContainsKey (elemType->elemHead.guid)) {
                    TrackHostedElement (elemType->elemHead.guid, typeID);
                }
                break;

            case APINotifyElement_Delete:
            case APINotifyElement_Undo_Created:
            case APINotifyElement_Redo_Deleted:
                SendEventToNotificationClient (ElementEventType::Deleted, message);
                trackedHostedElements.Delete (elemType->elemHead.guid);
                SendDeletedHostedElements (elemType->elemHead.guid);
                break;

            default:
                SendEventToNotificationClient (ElementEventType::Unknown, message);
                break;
        }
    }

    return NoError;
}

static API_ElemTypeID
GetElemTypeID (const API_Guid& elemGuid)
{
    API_Elem_Head elemHead = {};
    elemHead.guid = elemGuid;
    ACAPI_Element_GetHeader (&elemHead);
    return GetElemTypeId (elemHead);
}

GSErrCode
AddElementNotificationClientCommand::ElementReservationChangeHandler (
    const GS::HashTable<API_Guid, short>& reserved,
    const GS::HashSet<API_Guid>&          released,
    const GS::HashSet<API_Guid>&          /*deleted*/)
{
    std::map<short, GS::UniString> userIdToNameMap;
    for (const auto& kv : reserved) {
#ifdef ServerMainVers_2800
        const auto& userId = kv.value;
#else
        const auto& userId = *kv.value;
#endif
        ACAPI_Teamwork_GetUsernameFromId (userId, &userIdToNameMap[userId]);
    }
    for (const auto& kv : reserved) {
#ifdef ServerMainVers_2800
        const auto& guid = kv.key;
        const auto& userId = kv.value;
#else
        const auto& guid = *kv.key;
        const auto& userId = *kv.value;
#endif
        GS::ObjectState message (
            "elementId", CreateGuidObjectState (guid),
            "elementType", GetElementTypeNonLocalizedName (GetElemTypeID (guid)),
            "reservedBy", userIdToNameMap[userId]);

        SendEventToNotificationClient (ElementEventType::Reserved, message);
    }
    for (const auto& guid : released) {
        GS::ObjectState message (
            "elementId", CreateGuidObjectState (guid),
            "elementType", GetElementTypeNonLocalizedName (GetElemTypeID (guid)));

        SendEventToNotificationClient (ElementEventType::Released, message);
    }

    return NoError;
}

RemoveElementNotificationClientCommand::RemoveElementNotificationClientCommand () :
    CommandBase (CommonSchema::NotUsed)
{
}

GS::String RemoveElementNotificationClientCommand::GetName () const
{
    return "RemoveElementNotificationClient";
}

GS::Optional<GS::UniString> RemoveElementNotificationClientCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "host": {
                "type": "string",
                "description": "The host address of the notification client. If not provided, localhost is used."
            },
            "port": {
                "type": "integer",
                "description": "The port number of the notification client."
            }
        },
        "additionalProperties": false,
        "required": [
            "port"
        ]
    })";
}

GS::Optional<GS::UniString> RemoveElementNotificationClientCommand::GetRawResponseSchema () const
{
    return R"({
        "$ref": "#/ExecutionResult"
    })";
}

GS::ObjectState RemoveElementNotificationClientCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    Int32 port = -1;
    if (parameters.Get ("port", port)) {
        if (port <= 0 || port > 65535) {
            return CreateFailedExecutionResult (APIERR_BADPARS, "Invalid port number.");
        }
    } else {
        return CreateFailedExecutionResult (APIERR_BADPARS, "Port number is required.");
    }

    GS::UniString host = "localhost";
    parameters.Get ("host", host);

    const GS::UniString connectionUrlStr = GS::UniString::Printf ("http://%T:%d/", host.ToPrintf (), port);

    if (AddElementNotificationClientCommand::clients.erase (connectionUrlStr) == 0) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "No such notification client registered.");
    }

    AddElementNotificationClientCommand::DetachElementObserversIfNotNeeded ();
    AddElementNotificationClientCommand::UpdateSelectionAndProjectEventHandlers ();

    return CreateSuccessfulExecutionResult ();
}

// ---------------------------------------------------------------------------
// Doors, windows, skylights and openings deleted with their host element
// ---------------------------------------------------------------------------

bool
AddElementNotificationClientCommand::IsHostedElementType (API_ElemTypeID typeID)
{
    return typeID == API_DoorID || typeID == API_WindowID || typeID == API_SkylightID;
}

void
AddElementNotificationClientCommand::TrackHostedElement (const API_Guid& guid, API_ElemTypeID typeID)
{
    if (!IsHostedElementType (typeID)) {
        return;
    }
    API_Element element = {};
    element.header.guid = guid;
    if (ACAPI_Element_Get (&element) != NoError) {
        return;
    }
    // A door is a window in the API (API_DoorType is API_WindowType).
    const API_Guid owner = (typeID == API_SkylightID) ? element.skylight.owner : element.window.owner;
    if (owner != APINULLGuid) {
        trackedHostedElements.Put (guid, GS::Pair<API_ElemTypeID, API_Guid> (typeID, owner));
    }
}

void
AddElementNotificationClientCommand::SendDeletedHostedElements (const API_Guid& deletedHostGuid)
{
    // Archicad deletes the doors, windows and skylights of a deleted wall, roof or shell with it,
    // but notifies their observers of nothing. They are reported as deleted with their host.
    GS::Array<GS::Pair<API_Guid, API_ElemTypeID>> hosted;
    for (const auto& kv : trackedHostedElements) {
#ifdef ServerMainVers_2800
        const API_Guid& guid = kv.key;
        const GS::Pair<API_ElemTypeID, API_Guid>& typeAndOwner = kv.value;
#else
        const API_Guid& guid = *kv.key;
        const GS::Pair<API_ElemTypeID, API_Guid>& typeAndOwner = *kv.value;
#endif
        if (typeAndOwner.second == deletedHostGuid) {
            hosted.Push (GS::Pair<API_Guid, API_ElemTypeID> (guid, typeAndOwner.first));
        }
    }

    for (const auto& guidAndType : hosted) {
        trackedHostedElements.Delete (guidAndType.first);
        GS::ObjectState message (
            "elementId", CreateGuidObjectState (guidAndType.first),
            "elementType", GetElementTypeNonLocalizedName (guidAndType.second));
        SendEventToNotificationClient (ElementEventType::Deleted, message);
    }
}

// ---------------------------------------------------------------------------
// Selection, project and user input events
// ---------------------------------------------------------------------------

// Not APINotify_TempSave: Archicad autosaves after almost every change.
static const GSFlags NotifiedProjectEvents =
    APINotify_New | APINotify_NewAndReset | APINotify_Open | APINotify_PreSave | APINotify_Save |
    APINotify_Close | APINotify_Quit | APINotify_SendChanges | APINotify_ReceiveChanges |
    APINotify_ChangeProjectDB | APINotify_ChangeWindow | APINotify_ChangeFloor | APINotify_ChangeLibrary |
    APINotify_AllInputFinished;

static GS::UniString
ProjectEventName (API_NotifyEventID notifID)
{
    switch (notifID) {
        case APINotify_New:              return "New";
        case APINotify_NewAndReset:      return "NewAndReset";
        case APINotify_Open:             return "Open";
        case APINotify_PreSave:          return "PreSave";
        case APINotify_Save:             return "Save";
        case APINotify_Close:            return "Close";
        case APINotify_Quit:             return "Quit";
        case APINotify_SendChanges:      return "SendChanges";
        case APINotify_ReceiveChanges:   return "ReceiveChanges";
        case APINotify_ChangeProjectDB:  return "ChangeProjectDB";
        case APINotify_ChangeWindow:     return "ChangeWindow";
        case APINotify_ChangeFloor:      return "ChangeFloor";
        case APINotify_ChangeLibrary:    return "ChangeLibrary";
        case APINotify_AllInputFinished: return "AllInputFinished";
        default:                         return GS::UniString ();
    }
}

// Order independent, so the same elements selected in another order count as the same selection.
static UInt64
SelectionFingerprint (const GS::Array<API_Guid>& guids)
{
    UInt64 sum = 0;
    for (const API_Guid& guid : guids) {
        const unsigned char* bytes = reinterpret_cast<const unsigned char*> (&guid);
        UInt64 hash = 14695981039346656037ULL;
        for (size_t i = 0; i < sizeof (API_Guid); ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ULL;
        }
        sum += hash;
    }
    return sum ^ (static_cast<UInt64> (guids.GetSize ()) * 0x9E3779B97F4A7C15ULL);
}

UInt32
AddElementNotificationClientCommand::NextEventSequence ()
{
    return ++eventSequence;
}

void
AddElementNotificationClientCommand::UpdateSelectionAndProjectEventHandlers ()
{
    bool wantsSelection = false;
    bool wantsProjectEvents = false;
    for (const auto& kv : clients) {
        wantsSelection = wantsSelection || kv.second.notifyOnSelectionChange;
        wantsProjectEvents = wantsProjectEvents || kv.second.notifyOnProjectEvents;
    }

    if (wantsSelection != hasClientToNotifyOnSelectionChange) {
        ACAPI_Notification_CatchSelectionChange (wantsSelection ? SelectionChangeHandlerProc : nullptr);
        hasClientToNotifyOnSelectionChange = wantsSelection;
        hasLastSentSelection = false;
    }
    hasClientToNotifyOnProjectEvents = wantsProjectEvents;
}

GSFlags
AddElementNotificationClientCommand::ProjectEventsToCatch ()
{
    return NotifiedProjectEvents;
}

GSErrCode
AddElementNotificationClientCommand::SelectionChangeHandlerProc (const API_Neig* /*selElemNeig*/)
{
    // Archicad notifies every step of a selection change (each element a command or a marquee
    // selects). The event is sent once, from the event loop after those steps, with the selection
    // they left.
    if (!hasClientToNotifyOnSelectionChange || selectionEventScheduled) {
        return NoError;
    }
    selectionEventScheduled = true;
    const bool posted = EventLoopTasks::Post ([] () {
        selectionEventScheduled = false;
        SendSelectionChangedEvent ();
    });
    if (!posted) {
        selectionEventScheduled = false;
        SendSelectionChangedEvent ();
    }
    return NoError;
}

void
AddElementNotificationClientCommand::SendSelectionChangedEvent ()
{
    if (!hasClientToNotifyOnSelectionChange) {
        return;
    }

    API_SelectionInfo   selectionInfo = {};
    GS::Array<API_Neig> selectedNeigs;
    const GSErrCode err = ACAPI_Selection_Get (&selectionInfo, &selectedNeigs, false);
    BMKillHandle ((GSHandle*) &selectionInfo.marquee.coords);
    if (err != NoError && err != APIERR_NOSEL) {
        return;
    }

    // The same ids as GetSelectedElements: a selected section element stands for its model element.
    GS::Array<API_Guid>   selectedIds;
    GS::HashSet<API_Guid> seen;
    if (err == NoError && selectionInfo.typeID != API_SelEmpty) {
        for (const API_Neig& neig : selectedNeigs) {
            const API_Guid guid = GetParentElemOfSectElem (neig.guid);
            if (guid != APINULLGuid && !seen.Contains (guid)) {
                seen.Add (guid);
                selectedIds.Push (guid);
            }
        }
    }

    const UInt64 fingerprint = SelectionFingerprint (selectedIds);
    if (hasLastSentSelection && fingerprint == lastSentSelectionFingerprint) {
        return;
    }
    hasLastSentSelection = true;
    lastSentSelectionFingerprint = fingerprint;

    USize listedCount = 0;
    for (const auto& kv : clients) {
        if (kv.second.notifyOnSelectionChange) {
            listedCount = GS::Max (listedCount, GS::Min (selectedIds.GetSize (), (USize) kv.second.maxSelectedElementIds));
        }
    }
    GS::Array<GS::ObjectState> listedElements;
    for (UIndex i = 0; i < listedCount; ++i) {
        listedElements.Push (GS::ObjectState (
            "elementId", CreateGuidObjectState (selectedIds[i]),
            "elementType", GetElementTypeNonLocalizedName (GetElemTypeID (selectedIds[i]))));
    }

    const UInt32 sequence = NextEventSequence ();
    for (auto& kv : clients) {
        Client& client = kv.second;
        if (!client.notifyOnSelectionChange) {
            continue;
        }
        const USize count = GS::Min (selectedIds.GetSize (), (USize) client.maxSelectedElementIds);
        GS::ObjectState event ("sequence", sequence, "selectedCount", (UInt32) selectedIds.GetSize ());
        const auto& elements = event.AddList<GS::ObjectState> ("elements");
        for (UIndex i = 0; i < count; ++i) {
            elements (listedElements[i]);
        }
        event.Add ("truncated", count < selectedIds.GetSize ());
        SendMessageToNotificationClient (client, GS::ObjectState ("selectionChanged", event));
    }
}

GSErrCode
AddElementNotificationClientCommand::ProjectEventHandlerProc (API_NotifyEventID notifID, Int32 param)
{
    // Caught for the whole session: the events also keep the observers and the tracked hosted
    // elements in step with the project that is open.
    switch (notifID) {
        case APINotify_Close:
            trackedHostedElements.Clear ();
            hasLastSentSelection = false;
            break;
        case APINotify_New:
        case APINotify_NewAndReset:
        case APINotify_Open:
            // The observers belong to the elements of the project that was open before.
            hasLastSentSelection = false;
            if (hasClientToNotifyOnModification) {
                AttachObserversToAllElements ();
            }
            break;
        default:
            break;
    }

    if (!hasClientToNotifyOnProjectEvents) {
        return NoError;
    }
    const GS::UniString eventName = ProjectEventName (notifID);
    if (eventName.IsEmpty ()) {
        return NoError;
    }

    GS::ObjectState event ("sequence", NextEventSequence (), "event", eventName);
    if (notifID == APINotify_ChangeProjectDB) {
        event.Add ("cause", GS::UniString (param == 1 ? "StoriesEdited" : (param == 2 ? "Merge" : "Other")));
    }
    const GS::ObjectState message ("projectEvent", event);
    for (auto& kv : clients) {
        if (kv.second.notifyOnProjectEvents) {
            SendMessageToNotificationClient (kv.second, message);
        }
    }
    return NoError;
}

void
AddElementNotificationClientCommand::SendUserInputFinishedEvent (GS::ObjectState event)
{
    event.Add ("sequence", NextEventSequence ());
    const GS::ObjectState message ("userInputFinished", event);
    for (auto& kv : clients) {
        if (kv.second.notifyOnUserInput) {
            SendMessageToNotificationClient (kv.second, message);
        }
    }
}
