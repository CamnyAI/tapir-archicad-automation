import aclib

# ModifyDimensions changes a linear dimension in place: the dimension line, the appearance and the
# text settings, for every witness point. GetDimensionData returns the settings in the same format.
dimension = aclib.RunTapirCommand ('CreateAssociativeDimensions', {
    'dimensionsData': [{
        'referencePoint': {'x': 0.0, 'y': -2.0},
        'direction': {'x': 1.0, 'y': 0.0},
        'witnessPoints': [
            {'coordinate': {'x': 0.0, 'y': 0.0}},
            {'coordinate': {'x': 3.0, 'y': 0.0}},
            {'coordinate': {'x': 6.0, 'y': 0.0}}]
    }]
})['elements'][0]['elementId']

aclib.RunTapirCommand ('ModifyDimensions', {'dimensionsWithDetails': [{
    'elementId': dimension,
    'referencePoint': {'x': 0.0, 'y': -3.0},
    'appearance': 'Cumulative',
    'linePen': 11,
    'markerType': 'FullArrow30',
    'markerSize': 3.0,
    'witnessForm': 'Fix',
    'witnessVal': 0.5,
    'textHeight': 3.5,
    'textPen': 21,
    'textFrame': True,
    'textPosition': 'Below'}]})

data = aclib.RunTapirCommand ('GetDimensionData', {'elements': [{'elementId': dimension}]})['dimensionsData'][0]
print (aclib.JsonDumpDictionary (data['settings']))
print ([round (w['dimensionValue'], 3) for w in data['witnessPoints']])
