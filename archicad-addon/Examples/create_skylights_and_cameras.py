import aclib

# A single-plane roof with two skylights in it. The skylights come from the Skylight tool defaults;
# libraryPartName or favoriteName pick another skylight.
roofs = aclib.RunTapirCommand ('CreateRoofs', {
    'roofsData': [{
        'level': 3.0,
        'polygonCoordinates': [{'x': 0.0, 'y': 0.0}, {'x': 10.0, 'y': 0.0}, {'x': 10.0, 'y': 8.0}, {'x': 0.0, 'y': 8.0}],
        'pivotLine': {'begCoordinate': {'x': 0.0, 'y': 0.0}, 'endCoordinate': {'x': 10.0, 'y': 0.0}},
        'angle': 0.5
    }]
})['elements']

skylights = aclib.RunTapirCommand ('CreateSkylights', {
    'skylightsData': [
        {'ownerElementId': roofs[0]['elementId'], 'anchorPosition': {'x': 3.0, 'y': 4.0}},
        {'ownerElementId': roofs[0]['elementId'], 'anchorPosition': {'x': 7.0, 'y': 4.0}, 'width': 0.8, 'height': 1.2}
    ]
})['elements']

aclib.RunTapirCommand ('Get3DBoundingBoxes', {'elements': skylights})

# Two cameras looking at the roof. Without cameraSetId they are added to the active camera set
# (path); a project without one gets a new camera set.
cameras = aclib.RunTapirCommand ('CreateCameras', {
    'camerasData': [
        {'position': {'x': 5.0, 'y': -20.0, 'z': 1.7}, 'target': {'x': 5.0, 'y': 4.0, 'z': 3.0}},
        {'position': {'x': 25.0, 'y': -10.0, 'z': 10.0}, 'target': {'x': 5.0, 'y': 4.0, 'z': 3.0}, 'viewCone': 1.0}
    ]
})['elements']

aclib.RunTapirCommand ('GetElementsByType', {'elementType': 'CamSet'})

aclib.RunTapirCommand ('DeleteElements', {'elements': cameras + skylights + roofs})
