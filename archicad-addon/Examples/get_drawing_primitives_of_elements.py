import aclib
from collections import Counter

# GetDrawingPrimitivesOfElements works from any window: it reads the floor plan even when a
# section, a layout or the 3D window is shown.
elements = []
for elementType in ['Wall', 'Slab', 'Zone', 'PolyLine', 'Text']:
    elements += aclib.RunTapirCommand ('GetElementsByType', {'elementType': elementType}, debug=False)['elements'][:3]
if not elements:
    print ('No walls, slabs, zones, polylines or texts in the project.')
    raise SystemExit (0)

result = aclib.RunTapirCommand ('GetDrawingPrimitivesOfElements', {
    'elements': elements
}, debug=False)
if result is None or 'drawingPrimitivesOfElements' not in result:
    print ('Failed: {}'.format (result))
    raise SystemExit (1)

for element, drawingPrimitives in zip (elements, result['drawingPrimitivesOfElements']):
    print ('-' * 30)
    print ('Element {}'.format (element['elementId']['guid']))
    if 'error' in drawingPrimitives:
        print ('Error:\n{}'.format (aclib.JsonDumpDictionary (drawingPrimitives['error'])))
        continue
    primitives = drawingPrimitives['primitives']
    print ('Primitives by type: {}'.format (dict (Counter (p['type'] for p in primitives))))
    for primitive in primitives:
        if primitive['type'] == 'Line':
            print ('Line from ({0:.3f}, {1:.3f}) to ({2:.3f}, {3:.3f}), pen {4}'.format (
                primitive['c1']['x'], primitive['c1']['y'], primitive['c2']['x'], primitive['c2']['y'], primitive['pen']))
        elif primitive['type'] == 'PLine':
            print ('Polyline with {} vertices and {} curved segments'.format (
                len (primitive.get ('coordinates', [])), len (primitive.get ('arcs', []))))
        elif primitive['type'] == 'Poly':
            for polygon in primitive.get ('polygons', []):
                print ('Polygon with {} vertices and {} curved segments'.format (
                    len (polygon['coordinates']), len (polygon.get ('arcs', []))))
        elif primitive['type'] == 'Text':
            print ('Text "{}" at ({:.3f}, {:.3f})'.format (primitive.get ('text', ''), primitive['loc']['x'], primitive['loc']['y']))
