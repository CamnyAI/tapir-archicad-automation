"""
Receives the selection after each change, and the project events, without polling
GetSelectedElements: a notification client registered with notifyOnSelectionChange and
notifyOnProjectEvents. The element events are switched off, so this client only gets those.
"""

from http.server import BaseHTTPRequestHandler, HTTPServer
import aclib, threading, json, time

HOST = '127.0.0.1'
received = []

class NotificationHandler (BaseHTTPRequestHandler):
    def do_POST (self):
        length = int (self.headers.get ('Content-Length') or 0)
        notification = json.loads (self.rfile.read (length))
        if 'selectionChanged' in notification:
            selection = notification['selectionChanged']
            # selectedCount is the full number; elements lists at most maxSelectedElementIds.
            received.append ((selection['sequence'], selection['selectedCount'], len (selection['elements']), selection['truncated']))
        if 'projectEvent' in notification:
            print ('Project event: ' + notification['projectEvent']['event'])
        self.send_response (200)
        self.end_headers ()

    def log_message (self, *args):
        pass

server = HTTPServer ((HOST, 0), NotificationHandler)
port = server.server_address[1]
threading.Thread (target=server.serve_forever, daemon=True).start ()

aclib.RunTapirCommand ('SetElementNotificationClient', {
    'host': HOST,
    'port': port,
    'notifyOnNewElement': False,
    'notifyOnModificationOfAnElement': False,
    'notifyOnReservationChanges': False,
    'notifyOnSelectionChange': True,
    'maxSelectedElementIds': 2,
    'notifyOnProjectEvents': True
}, debug=False)

walls = aclib.RunTapirCommand ('CreateWalls', {
    'wallsData': [
        { 'begCoordinate': { 'x': 3.0 * i, 'y': 50.0 }, 'endCoordinate': { 'x': 3.0 * i + 2.0, 'y': 50.0 }, 'height': 3.0, 'thickness': 0.2 }
        for i in range (3)
    ]
}, debug=False)
wallIds = [{ 'elementId': wall['elementId'] } for wall in walls['elements']]

# One message per change, not one per selected element.
aclib.RunTapirCommand ('ChangeSelectionOfElements', { 'addElementsToSelection': wallIds }, debug=False)
time.sleep (1)
aclib.RunTapirCommand ('ChangeSelectionOfElements', { 'removeElementsFromSelection': wallIds }, debug=False)
time.sleep (1)

for sequence, selectedCount, listed, truncated in received:
    print ('Selection: {} selected, {} listed, truncated: {}'.format (selectedCount, listed, truncated))

aclib.RunTapirCommand ('RemoveElementNotificationClient', { 'host': HOST, 'port': port }, debug=False)
aclib.RunTapirCommand ('DeleteElements', { 'elements': wallIds }, debug=False)
