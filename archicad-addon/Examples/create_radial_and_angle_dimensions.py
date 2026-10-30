import aclib
import math

# A radial dimension on an arc: it follows the arc when the arc changes.
arcs = aclib.RunTapirCommand ('CreateArcs', {
    'arcsData': [{'origin': {'x': 0.0, 'y': 0.0}, 'radius': 2.5, 'begAngle': 0.0, 'endAngle': 2.0}]
})['elements']

radialDimensions = aclib.RunTapirCommand ('CreateRadialDimensions', {
    'radialDimensionsData': [{
        'elementId': arcs[0]['elementId'],
        # The dimension line points from the center through the arc to this point.
        'endCoordinate': {'x': 4.0 * math.cos (1.0), 'y': 4.0 * math.sin (1.0)},
        'prefix': 'R'
    }]
})['elements']

# A static angle dimension between two lines that meet at (10, 0): the arc goes through arcPoint.
angleDimensions = aclib.RunTapirCommand ('CreateAngleDimensions', {
    'angleDimensionsData': [{
        'firstLine': {'begCoordinate': {'x': 10.0, 'y': 0.0}, 'endCoordinate': {'x': 15.0, 'y': 0.0}},
        'secondLine': {'begCoordinate': {'x': 10.0, 'y': 0.0}, 'endCoordinate': {'x': 15.0, 'y': 5.0}},
        'arcPoint': {'x': 10.0 + 3.0 * math.cos (0.3), 'y': 3.0 * math.sin (0.3)}
    }]
})['elements']

# The texts Archicad draws for them show the measured radius and angle.
aclib.RunTapirCommand ('GetDrawingPrimitivesOfElements', {'elements': radialDimensions + angleDimensions})

aclib.RunTapirCommand ('DeleteElements', {'elements': radialDimensions + angleDimensions + arcs})
