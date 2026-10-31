import aclib
from collections import Counter

# GetDrawingPrimitivesOfDatabase reads the vector drawing of a section, elevation, interior
# elevation, detail, worksheet, 3D document or layout in the background: the window in front
# stays as it is. Here: the first section of the project, or the first elevation.
databases = aclib.RunTapirCommand ('GetDatabases', {
    'types': ['Section', 'Elevation']
}, debug=False)['databases']
if not databases:
    print ('No section or elevation in the project.')
    raise SystemExit (0)
database = databases[0]

result = aclib.RunTapirCommand ('GetDrawingPrimitivesOfDatabase', {
    'databaseId': database['databaseId']
}, debug=False)
if result is None or 'elements' not in result:
    print ('Failed: {}'.format (result))
    raise SystemExit (1)

print ('{} {} at 1:{:g}{}'.format (
    result['database']['windowType'], result['database']['title'], result.get ('drawingScale', 0),
    ' (its window had to be opened once)' if result['windowOpened'] else ''))
if 'extent' in result:
    extent = result['extent']
    print ('Extent: ({:.3f}, {:.3f}) - ({:.3f}, {:.3f})'.format (extent['xMin'], extent['yMin'], extent['xMax'], extent['yMax']))

# Section elements carry the model element they were cut from.
print ('Elements by type: {}'.format (dict (Counter (
    element.get ('ownerElementType', element['elementType']) for element in result['elements']))))

for element in result['elements']:
    if element.get ('ownerElementType') != 'Wall':
        continue
    print ('-' * 30)
    print ('Wall {} drawn as section element {}'.format (element['ownerElementId']['guid'], element['elementId']['guid']))
    primitives = element.get ('primitives', [])
    print ('Primitives by type: {}'.format (dict (Counter (p['type'] for p in primitives))))
    for primitive in primitives:
        if primitive['type'] == 'Poly':
            for polygon in primitive.get ('polygons', []):
                xs = [c['x'] for c in polygon['coordinates']]
                ys = [c['y'] for c in polygon['coordinates']]
                print ('Fill polygon from ({:.3f}, {:.3f}) to ({:.3f}, {:.3f})'.format (min (xs), min (ys), max (xs), max (ys)))
