import aclib

# Skylights, cameras, change markers and pictures report their type-specific details
# like the other element types: where a skylight sits in its roof, the view of each
# camera (in the shape Set3DProjectionSettings takes), the change and the cloud of a
# change marker, and the box of a placed picture. Spline details also carry the
# Bezier handle of every point.

for elementType in ['Skylight', 'Camera', 'ChangeMarker', 'Picture', 'Spline']:
    elements = aclib.RunTapirCommand ('GetElementsByType', {'elementType': elementType}, debug=False)['elements']
    if not elements:
        continue

    detailsOfElements = aclib.RunTapirCommand ('GetDetailsOfElements', {
        'elements': elements,
        'fields': ['type', 'details']
    }, debug=False)['detailsOfElements']

    for detailsOfElement in detailsOfElements:
        if 'details' not in detailsOfElement:
            continue
        details = detailsOfElement['details']
        if elementType == 'Skylight':
            print ('Skylight {} x {} in a {} at ({:.2f}, {:.2f}), level {:.2f}'.format (
                details['width'], details['height'], details.get ('ownerElementType'),
                details['anchorPosition']['x'], details['anchorPosition']['y'], details['anchorLevel']))
        elif elementType == 'Camera':
            perspective = details['perspective']
            print ('Camera at {} looking at {}'.format (perspective['cameraPosition'], perspective['targetPosition']))
        elif elementType == 'ChangeMarker':
            print ('Change marker of change {} with {} cloud points'.format (
                details.get ('changeId', '(unlinked)'), len (details.get ('cloudPolygon', []))))
        elif elementType == 'Picture':
            print ('Picture {} ({} x {} px) in box {}'.format (
                details['name'], details['pixelWidth'], details['pixelHeight'], details['box']))
        else:
            print ('Spline with {} points and {} Bezier handles'.format (
                len (details['coordinates']), len (details.get ('bezierDirections', []))))
