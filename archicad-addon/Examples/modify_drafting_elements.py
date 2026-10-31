import aclib

# ModifyLines, ModifyArcs, ModifyCircles, ModifyPolylines and ModifySplines change drafting elements
# in place: the elements keep their GUIDs, and with them their links, properties and
# classifications. They take the keys of the matching Create commands, and only change the fields
# they are given.
def NewElement (command, arrayName, data):
    return aclib.RunTapirCommand (command, {arrayName: [data]})['elements'][0]['elementId']

line = NewElement ('CreateLineElements', 'linesData', {'begCoordinate': {'x': 0.0, 'y': 0.0}, 'endCoordinate': {'x': 5.0, 'y': 0.0}})
arc = NewElement ('CreateArcs', 'arcsData', {'origin': {'x': 10.0, 'y': 0.0}, 'radius': 2.0, 'begAngle': 0.0, 'endAngle': 1.5})
circle = NewElement ('CreateCircles', 'circlesData', {'origin': {'x': 20.0, 'y': 0.0}, 'radius': 1.0})
polyline = NewElement ('CreatePolylines', 'polylinesData', {'coordinates': [{'x': 0.0, 'y': 10.0}, {'x': 5.0, 'y': 10.0}, {'x': 5.0, 'y': 15.0}]})
spline = NewElement ('CreateSplines', 'splinesData', {'coordinates': [{'x': 0.0, 'y': 20.0}, {'x': 3.0, 'y': 22.0}, {'x': 6.0, 'y': 20.0}]})

lineTypes = aclib.RunCommand ('API.GetAttributesByType', {'attributeType': 'Line'})['attributeIds']

aclib.RunTapirCommand ('ModifyLines', {'linesWithDetails': [{
    'elementId': line,
    'begCoordinate': {'x': 1.0, 'y': 1.0},
    'endCoordinate': {'x': 7.0, 'y': 3.0},
    'linePenIndex': 20,
    'lineTypeId': lineTypes[-1]['attributeId']}]})

aclib.RunTapirCommand ('ModifyArcs', {'arcsWithDetails': [{
    'elementId': arc, 'radius': 3.0, 'begAngle': 0.5, 'endAngle': 2.5}]})

# A circle made by CreateCircles is kept as an arc spanning the whole circle; ModifyCircles takes it.
aclib.RunTapirCommand ('ModifyCircles', {'circlesWithDetails': [{
    'elementId': circle, 'origin': {'x': 21.0, 'y': 2.0}, 'radius': 4.0}]})

# The polyline gets a new, closed geometry with one curved segment (indices into coordinates).
aclib.RunTapirCommand ('ModifyPolylines', {'polylinesWithDetails': [{
    'elementId': polyline,
    'coordinates': [{'x': 0.0, 'y': 10.0}, {'x': 4.0, 'y': 10.0}, {'x': 4.0, 'y': 14.0}, {'x': 0.0, 'y': 14.0}, {'x': 0.0, 'y': 10.0}],
    'arcs': [{'begIndex': 1, 'endIndex': 2, 'arcAngle': 1.0}]}]})

# The spline gets its own handles (one per point), which turns automatic smoothing off.
aclib.RunTapirCommand ('ModifySplines', {'splinesWithDetails': [{
    'elementId': spline,
    'bezierDirections': [
        {'lenPrev': 1.0, 'lenNext': 1.5, 'dirAng': 0.25},
        {'lenPrev': 0.5, 'lenNext': 0.75, 'dirAng': 1.25},
        {'lenPrev': 2.0, 'lenNext': 1.0, 'dirAng': -0.5}]}]})

details = aclib.RunTapirCommand ('GetDetailsOfElements', {'elements': [
    {'elementId': e} for e in [line, arc, circle, polyline, spline]]})
for d in details['detailsOfElements']:
    print (d['type'], aclib.JsonDumpDictionary (d['details']))
