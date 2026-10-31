import aclib

# Creates a wall with an opening in it, then changes the opening's size and position with
# ModifyOpenings and reads the result back. ModifyOpenings takes the fields CreateOpenings takes:
# width, height and basePoint, and a new basePoint puts the opening where CreateOpenings puts one
# of its size for that point. A new height keeps the bottom edge where it is. Up to Archicad 28
# the command writes API_OpeningType, from Archicad 29 it goes through
# ACAPI::Element::Opening::Modify.
#
# From Archicad 29 CreateOpenings makes a custom (polygonal) opening, which ModifyOpenings resizes
# by scaling its polygon. GetOpeningDetails (Archicad 29 or newer) reads it back: the polygon gives
# the size and the anchor point the position. Where the opening sits around its anchor point is
# set by its anchor (with the stock TopCenter anchor it hangs from the point).

walls = aclib.RunTapirCommand ('CreateWalls', {
    'wallsData': [
        {
            'begCoordinate': { 'x': 0.0, 'y': -30.0 },
            'endCoordinate': { 'x': 8.0, 'y': -30.0 },
            'height': 4.0,
            'thickness': 0.3
        }
    ]
})['elements']

openings = aclib.RunTapirCommand ('CreateOpenings', {
    'openingsData': [
        {
            'ownerElementId': walls[0]['elementId'],
            'basePoint': { 'x': 2.0, 'y': -30.0, 'z': 1.0 },
            'width': 0.8,
            'height': 0.8
        }
    ]
})['elements']

archicadVersion = aclib.RunCommand ('API.GetProductInfo')['version']

def PrintOpening (label):
    boundingBox = aclib.RunTapirCommand ('Get3DBoundingBoxes', {
        'elements': openings
    }, debug = False)['boundingBoxes3D'][0]['boundingBox3D']
    line = '{}: width along the wall={:.3f}'.format (label, boundingBox['xMax'] - boundingBox['xMin'])
    if archicadVersion >= 29:
        opening = aclib.RunTapirCommand ('GetOpeningDetails', {
            'elements': openings
        }, debug = False)['openings'][0]
        polygon = opening['geometry'].get ('polygon')
        if polygon:
            width = max (p['x'] for p in polygon) - min (p['x'] for p in polygon)
            height = max (p['y'] for p in polygon) - min (p['y'] for p in polygon)
        else:
            width = opening['extrusionParameters']['width']
            height = opening['extrusionParameters']['height']
        anchorPoint = opening['geometry']['anchorPoint']
        line += ' size={:.3f} x {:.3f} anchor=({:.3f}, {:.3f}, {:.3f}) {}'.format (
            width, height, anchorPoint['x'], anchorPoint['y'], anchorPoint['z'], opening['extrusionParameters']['anchor'])
    print (line)

PrintOpening ('created')

# Resize only: the bottom edge stays where it is, so a top anchor rises by the added height.
result = aclib.RunTapirCommand ('ModifyOpenings', {
    'openingsWithDetails': [
        {
            'elementId': openings[0]['elementId'],
            'width': 1.2,
            'height': 1.5
        }
    ]
}, debug = False)
print ('resize: success={}'.format (result['executionResults'][0]['success']))
PrintOpening ('resized')

# Move: the opening goes where CreateOpenings puts an opening of its size for this base point.
result = aclib.RunTapirCommand ('ModifyOpenings', {
    'openingsWithDetails': [
        {
            'elementId': openings[0]['elementId'],
            'basePoint': { 'x': 5.0, 'y': -30.0, 'z': 1.8 }
        }
    ]
}, debug = False)
print ('move: success={}'.format (result['executionResults'][0]['success']))
PrintOpening ('moved')

# An item without any field to change fails; other items of the same call are still applied.
result = aclib.RunTapirCommand ('ModifyOpenings', {
    'openingsWithDetails': [
        { 'elementId': openings[0]['elementId'] }
    ]
}, debug = False)
print ('no fields: success={}'.format (result['executionResults'][0]['success']))

aclib.RunTapirCommand ('DeleteElements', {'elements': openings + walls})
