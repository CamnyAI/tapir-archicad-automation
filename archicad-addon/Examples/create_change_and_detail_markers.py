import aclib

# A revision cloud around a wall, linked to a new Change.
walls = aclib.RunTapirCommand ('CreateWalls', {
    'wallsData': [{'begCoordinate': {'x': 0.0, 'y': 0.0}, 'endCoordinate': {'x': 5.0, 'y': 0.0}, 'zCoordinate': 0.0, 'height': 3.0, 'thickness': 0.3}]
})['elements']

cloud = [{'x': -1.0, 'y': -2.0}, {'x': 6.0, 'y': -2.0}, {'x': 6.0, 'y': 2.0}, {'x': -1.0, 'y': 2.0}]

changeMarkers = aclib.RunTapirCommand ('CreateChangeMarkers', {
    'changeMarkersData': [{
        'position': {'x': 2.5, 'y': 0.0},
        'polygonCoordinates': cloud,
        'markerPosition': {'x': 7.0, 'y': 3.0},
        'changeId': 'C-01',
        'changeName': 'Wall moved'
    }]
})['elements']

aclib.RunTapirCommand ('GetRevisionChanges')

# A Detail marker around the wall: Archicad creates a new Detail database with this name and
# reference ID, and puts the marker's reference point at the center of the polygon. Deleting the marker deletes the database. The same goes for CreateWorksheetMarkers.
detailMarkers = aclib.RunTapirCommand ('CreateDetailMarkers', {
    'detailMarkersData': [
        {'position': {'x': 2.5, 'y': 0.0}, 'polygonCoordinates': cloud, 'markerPosition': {'x': 7.0, 'y': -3.0}, 'name': 'Wall detail', 'referenceId': 'D-01'}
    ]
})['elements']

aclib.RunTapirCommand ('GetDetailsOfElements', {'elements': detailMarkers})
aclib.RunTapirCommand ('GetDatabases', {'types': ['Detail']})

aclib.RunTapirCommand ('DeleteElements', {'elements': detailMarkers + changeMarkers + walls})
