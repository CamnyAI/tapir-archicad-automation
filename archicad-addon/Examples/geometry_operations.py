"""
The geometry operations of Archicad's Edit menu and pet palette: Split, Intersect, Adjust,
Fillet, Chamfer, Offset Edge, Resize and Multiply.

Every block creates the elements it works on far from the origin, so it does not touch
anything else in the project, and deletes them at the end.
"""

import math
import aclib

def run (command, parameters):
    return aclib.RunTapirCommand (command, parameters)

def elementId (response, index = 0):
    return response['elements'][index]['elementId']

created = []

# --- Split ------------------------------------------------------------------------------------
# A wall with a window on each half, split in the middle. The wall keeps its guid and becomes the
# first piece; the second piece is a new wall with the same settings, classifications, properties
# and Element ID, and the window on that half moves into it.
wall = elementId (run ('CreateWalls', {'wallsData': [{
    'begCoordinate': {'x': 1000.0, 'y': 0.0}, 'endCoordinate': {'x': 1010.0, 'y': 0.0},
    'zCoordinate': 0.0, 'height': 3.0, 'thickness': 0.3}]}))
run ('CreateWindows', {'windowsData': [
    {'ownerWallId': wall, 'centerOffset': 2.0, 'width': 1.0, 'height': 1.5},
    {'ownerWallId': wall, 'centerOffset': 7.0, 'width': 1.0, 'height': 1.5}]})
polyline = elementId (run ('CreatePolylines', {'polylinesData': [{'coordinates': [
    {'x': 1000.0, 'y': 5.0}, {'x': 1004.0, 'y': 9.0}, {'x': 1010.0, 'y': 5.0}]}]}))

pieces = run ('SplitElements', {
    'elements': [{'elementId': wall}, {'elementId': polyline}],
    'splitLine': {'begCoordinate': {'x': 1005.0, 'y': -100.0}, 'endCoordinate': {'x': 1005.0, 'y': 100.0}}
})['piecesOfElements']
for piecesOfElement in pieces:
    created += [piece['elementId'] for piece in piecesOfElement.get ('elements', [])]

# --- Intersect and Adjust ---------------------------------------------------------------------
# Two lines that do not meet are extended to the intersection of their lines.
line1 = elementId (run ('CreateLineElements', {'linesData': [{'begCoordinate': {'x': 1020.0, 'y': 0.0}, 'endCoordinate': {'x': 1024.0, 'y': 0.0}}]}))
line2 = elementId (run ('CreateLineElements', {'linesData': [{'begCoordinate': {'x': 1026.0, 'y': 1.0}, 'endCoordinate': {'x': 1026.0, 'y': 5.0}}]}))
created += [line1, line2]
run ('IntersectElements', {'elementPairs': [{'elementId1': line1, 'elementId2': line2}]})

# A wall is extended to the line x = 1040.
wall2 = elementId (run ('CreateWalls', {'wallsData': [{
    'begCoordinate': {'x': 1030.0, 'y': 0.0}, 'endCoordinate': {'x': 1035.0, 'y': 0.0},
    'zCoordinate': 0.0, 'height': 3.0, 'thickness': 0.3}]}))
created.append (wall2)
run ('AdjustElements', {
    'elements': [{'elementId': wall2}],
    'targetLine': {'begCoordinate': {'x': 1040.0, 'y': 0.0}, 'endCoordinate': {'x': 1040.0, 'y': 1.0}}
})

# --- Fillet and Chamfer -----------------------------------------------------------------------
# The corner of two walls rounded with a radius of 2 m: a new curved wall joins them.
wall3 = elementId (run ('CreateWalls', {'wallsData': [{
    'begCoordinate': {'x': 1050.0, 'y': 0.0}, 'endCoordinate': {'x': 1060.0, 'y': 0.0},
    'zCoordinate': 0.0, 'height': 3.0, 'thickness': 0.3}]}))
wall4 = elementId (run ('CreateWalls', {'wallsData': [{
    'begCoordinate': {'x': 1060.0, 'y': 0.0}, 'endCoordinate': {'x': 1060.0, 'y': 10.0},
    'zCoordinate': 0.0, 'height': 3.0, 'thickness': 0.3}]}))
created += [wall3, wall4]
fillet = run ('FilletElements', {'elementPairs': [{'elementId1': wall3, 'elementId2': wall4, 'radius': 2.0}]})
created += [item['elementId'] for item in fillet['connectingElements'] if 'elementId' in item]

# The corner of two lines cut 1 m and 2 m from the corner: a new line joins them.
line3 = elementId (run ('CreateLineElements', {'linesData': [{'begCoordinate': {'x': 1070.0, 'y': 0.0}, 'endCoordinate': {'x': 1080.0, 'y': 0.0}}]}))
line4 = elementId (run ('CreateLineElements', {'linesData': [{'begCoordinate': {'x': 1080.0, 'y': 0.0}, 'endCoordinate': {'x': 1080.0, 'y': 10.0}}]}))
created += [line3, line4]
chamfer = run ('ChamferElements', {'elementPairs': [{'elementId1': line3, 'elementId2': line4, 'distance1': 1.0, 'distance2': 2.0}]})
created += [item['elementId'] for item in chamfer['connectingElements'] if 'elementId' in item]

# --- Offset Edge ------------------------------------------------------------------------------
# The right edge (edge 1, from vertex 1 to vertex 2) of a slab moved out by 0.5 m.
slab = elementId (run ('CreateSlabs', {'slabsData': [{'level': 0.0, 'polygonCoordinates': [
    {'x': 1090.0, 'y': 0.0}, {'x': 1096.0, 'y': 0.0}, {'x': 1096.0, 'y': 4.0}, {'x': 1090.0, 'y': 4.0}]}]}))
created.append (slab)
run ('OffsetEdges', {'edgesToOffset': [{'elementId': slab, 'edgeIndex': 1, 'offset': 0.5}]})

# --- Resize and Multiply ----------------------------------------------------------------------
# A resized copy of the slab at half size, and four copies of a line, each 1 m further and
# rotated by 15 degrees more about the line's start.
resized = run ('ResizeElements', {'elements': [{'elementId': slab}], 'origin': {'x': 1090.0, 'y': 0.0}, 'ratio': 0.5, 'copy': True})
created += [item['elementId'] for item in resized['resizedElements'] if 'elementId' in item]

line5 = elementId (run ('CreateLineElements', {'linesData': [{'begCoordinate': {'x': 1100.0, 'y': 0.0}, 'endCoordinate': {'x': 1102.0, 'y': 0.0}}]}))
created.append (line5)
copies = run ('MultiplyElements', {
    'elements': [{'elementId': line5}],
    'numberOfCopies': 4,
    'rotation': {'origin': {'x': 1100.0, 'y': 0.0}, 'angle': math.radians (15.0)},
    'moveVector': {'x': 0.0, 'y': 1.0, 'z': 0.0}
})
for copiesOfElement in copies['copiesOfElements']:
    created += [copy['elementId'] for copy in copiesOfElement.get ('elements', [])]

aclib.RunTapirCommand ('DeleteElements', {'elements': [{'elementId': elementId} for elementId in created]})
