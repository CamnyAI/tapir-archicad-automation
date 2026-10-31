#pragma once

#include "CommandBase.hpp"
#include "Pair.hpp"
#include "Thread.hpp"
#include "URI.hpp"
#include "HTTP/MessageHeader/StatusCode.hpp"

#include <set>
#include <map>
#include <atomic>
#include <vector>

namespace HTTP {
namespace Client {
class Response;
class Request;
}
}

class AddElementNotificationClientCommand : public CommandBase
{
public:
    enum class ElementEventType {
        New,
        Changed,
        Deleted,
        Reserved,
        Released,
        Unknown
    };

    struct Client {
        Client(
            const IO::URI::URI& url,
            bool onNew, bool onMod, bool onResChanges)
            : url(url), notifyOnNew(onNew), notifyOnModification(onMod), notifyOnReservationChanges(onResChanges) {}

        IO::URI::URI url;
        bool         notifyOnNew;
        bool         notifyOnModification;
        bool         notifyOnReservationChanges;
        // Opt-in, so a client registered without them keeps receiving exactly the element events.
        bool         notifyOnSelectionChange = false;
        Int32        maxSelectedElementIds = 1000;
        bool         notifyOnProjectEvents = false;
        bool         notifyOnUserInput = false;

        bool acceptsNotificationsForEventType (ElementEventType eventType) const
        {
            if (eventType == ElementEventType::New) {
                return notifyOnNew;
            } else if (eventType == ElementEventType::Changed || eventType == ElementEventType::Deleted) {
                return notifyOnModification;
            } else if (eventType == ElementEventType::Reserved || eventType == ElementEventType::Released) {
                return notifyOnReservationChanges;
            }
            return false;
        }

        void Send(HTTP::Client::Request& request, const GS::UniString* body);
    };

public:
    AddElementNotificationClientCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;

    static void DetachElementObserversIfNotNeeded ();
    static void SendEventToNotificationClient (ElementEventType eventType, const GS::ObjectState& os);
    static void SendQueuedEventsToNotificationClient ();
    static void SendMessageToNotificationClient (Client& client, const GS::ObjectState& os);
    static GSErrCode ElementEventHandlerProc (const API_NotifyElementType *elemType);
    static GSErrCode ElementReservationChangeHandler (const GS::HashTable<API_Guid, short>& reserved,
                                                      const GS::HashSet<API_Guid>&          released,
                                                      const GS::HashSet<API_Guid>&          deleted);

    // Installs or removes the selection event handler after the clients changed. Project events
    // reach ProjectEventHandlerProc through the add-on's single project event handler, which
    // AddOnMain.cpp installs for the whole session with ProjectEventsToCatch.
    static void UpdateSelectionAndProjectEventHandlers ();
    static GSErrCode SelectionChangeHandlerProc (const API_Neig* selElemNeig);
    static GSErrCode ProjectEventHandlerProc (API_NotifyEventID notifID, Int32 param);
    static GSFlags ProjectEventsToCatch ();
    static void SendSelectionChangedEvent ();
    // Delivers the result of an input started by StartUserInput to the clients that asked for it.
    static void SendUserInputFinishedEvent (GS::ObjectState event);

private:
    // Doors, windows and skylights are deleted with their host element without notifications of
    // their own; these remember the observed ones with their host, so they can be reported.
    static void AttachObserversToAllElements ();
    static void TrackHostedElement (const API_Guid& guid, API_ElemTypeID typeID);
    static void SendDeletedHostedElements (const API_Guid& deletedHostGuid);
    static bool IsHostedElementType (API_ElemTypeID typeID);
    static UInt32 NextEventSequence ();

    friend class MessageSenderTask;
    friend class RemoveElementNotificationClientCommand;
    static std::map<GS::UniString, Client> clients;
    static bool hasClientToNotifyOnNew;
    static bool hasClientToNotifyOnModification;
    static bool hasClientToNotifyOnReservationChanges;
    static bool hasClientToNotifyOnSelectionChange;
    static bool hasClientToNotifyOnProjectEvents;

    using EventQueue = std::map<ElementEventType, std::vector<GS::ObjectState>>;
    static std::unique_ptr<EventQueue> queuedEvents;

    // Hosted element -> its type and its host.
    static GS::HashTable<API_Guid, GS::Pair<API_ElemTypeID, API_Guid>> trackedHostedElements;

    static bool   selectionEventScheduled;
    static bool   hasLastSentSelection;
    static UInt64 lastSentSelectionFingerprint;
    static UInt32 eventSequence;

    static GS::Thread messageSenderThread;
};

class RemoveElementNotificationClientCommand : public CommandBase
{
public:
    RemoveElementNotificationClientCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};
