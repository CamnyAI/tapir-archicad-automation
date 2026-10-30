"""
StartUserInput asks the designer for a point like GetPointFromUser, but answers at once: the
script does not keep a request open until the click. The result is pushed to a notification
client registered with notifyOnUserInput when the input ends.

While Archicad waits for the click it refuses every JSON command at once with error 4001
("ongoing user input"), so the result is not polled: it is waited for here.
"""

from http.server import BaseHTTPRequestHandler, HTTPServer
import aclib, json, queue, sys, threading

HOST = '127.0.0.1'
finishedInputs = queue.Queue ()

class NotificationHandler (BaseHTTPRequestHandler):
    def do_POST (self):
        length = int (self.headers.get ('Content-Length') or 0)
        notification = json.loads (self.rfile.read (length))
        if 'userInputFinished' in notification:
            finishedInputs.put (notification['userInputFinished'])
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
    'notifyOnUserInput': True
}, debug=False)

parameters = {
    'command': 'GetPointFromUser',
    'parameters': { 'prompt': 'Click the corner of the building' }
}
if 'silent' in sys.argv:
    # Nobody clicks in the silent test run: the input is cancelled after a second, as if
    # Escape was pressed.
    parameters['timeout'] = 1.0

inputId = aclib.RunTapirCommand ('StartUserInput', parameters)['inputId']

finished = finishedInputs.get ()
if finished['inputId'] == inputId:
    if finished['timedOut']:
        print ('Nobody clicked.')
    else:
        print (json.dumps (finished['result'], indent=4))

aclib.RunTapirCommand ('RemoveElementNotificationClient', { 'host': HOST, 'port': port }, debug=False)
