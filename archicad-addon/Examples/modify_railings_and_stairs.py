import aclib

# ModifyRailings and ModifyStairs change railings and stairs in place, with the keys of
# CreateRailings and CreateStairs (except the reference line and the baseline, which Archicad does
# not change through the API). The elements keep their GUIDs, their IDs, properties and
# classifications.
railing = aclib.RunTapirCommand ('CreateRailings', {
    'railingsData': [{'coordinates': [{'x': 0.0, 'y': 0.0}, {'x': 4.0, 'y': 0.0}, {'x': 4.0, 'y': 4.0}], 'height': 1.1}]
})['elements'][0]['elementId']

# One height for every segment, then one height per segment, in the order of segmentHeights.
aclib.RunTapirCommand ('ModifyRailings', {'railingsWithDetails': [{'elementId': railing, 'height': 0.9, 'bottomOffset': 0.2}]})
aclib.RunTapirCommand ('ModifyRailings', {'railingsWithDetails': [{'elementId': railing, 'segmentHeights': [1.2, 0.8]}]})

stair = aclib.RunTapirCommand ('CreateStairs', {
    'stairsData': [{'baseLinePoints': [{'x': 10.0, 'y': 0.0}, {'x': 16.0, 'y': 0.0}], 'zCoordinate': 0.0, 'totalHeight': 3.0, 'stepNum': 20, 'flightWidth': 1.2}]
})['elements'][0]['elementId']

# Archicad applies the stair rules as the Stair Settings do: the riser height follows the total
# height and the number of risers, and a riser height alone picks the closest number of risers.
aclib.RunTapirCommand ('ModifyStairs', {'stairsWithDetails': [{'elementId': stair, 'flightWidth': 1.5, 'totalHeight': 2.7}]})
aclib.RunTapirCommand ('ModifyStairs', {'stairsWithDetails': [{'elementId': stair, 'riserHeight': 0.17}]})

details = aclib.RunTapirCommand ('GetDetailsOfElements', {'elements': [{'elementId': railing}, {'elementId': stair}]})
for d in details['detailsOfElements']:
    print (d['type'], aclib.JsonDumpDictionary (d['details']))
