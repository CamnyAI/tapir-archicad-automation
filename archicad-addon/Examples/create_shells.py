import aclib

# Two extruded shells. The profile is drawn at right angles to the extrusion, its x axis horizontal
# and its y axis up, with its origin at begCoordinate. Lengths are in meters, angles in radians.
vault = {
    'coordinates': [
        {'x': -2.0, 'y': 0.0},
        {'x': -2.5, 'y': 2.0},
        {'x': 2.5, 'y': 2.0},
        {'x': 2.0, 'y': 0.0}
    ],
    # The edge between the 2nd and 3rd points is an arc.
    'arcs': [{'begIndex': 1, 'endIndex': 2, 'arcAngle': -1.0}]
}

tube = {
    'coordinates': [{'x': -1.0, 'y': 0.0}, {'x': 1.0, 'y': 0.0}, {'x': 1.0, 'y': 1.0}, {'x': -1.0, 'y': 1.0}],
    'closed': True
}

shells = aclib.RunTapirCommand ('CreateShells', {
    'shellsData': [
        {
            'profile': vault,
            'begCoordinate': {'x': 0.0, 'y': 0.0, 'z': 0.0},
            'extrusionVector': {'x': 10.0, 'y': 0.0, 'z': 0.0},
            'structureType': 'Basic',
            'thickness': 0.2
        },
        {
            'profile': tube,
            'begCoordinate': {'x': 20.0, 'y': 0.0, 'z': 0.0},
            'extrusionVector': {'x': 0.0, 'y': 5.0, 'z': 0.0},
            'structureType': 'Composite'
        }
    ]
})['elements']

aclib.RunTapirCommand ('Get3DBoundingBoxes', {'elements': shells})
aclib.RunTapirCommand ('GetQuantitiesOfElements', {'elements': shells})

aclib.RunTapirCommand ('DeleteElements', {'elements': shells})
