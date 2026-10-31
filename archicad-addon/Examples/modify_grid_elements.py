import aclib

# Requires Archicad 26 or newer.

# ModifyGridElements changes grid elements in place, with the keys CreateGridElements takes. A
# point that is not given stays where it is.
grid = aclib.RunTapirCommand ('CreateGridElements', {
    'gridElements': [{'begCoordinate': {'x': 0.0, 'y': -2.0}, 'endCoordinate': {'x': 0.0, 'y': 18.0}}]
})['elements'][0]['elementId']

# Move the grid line and turn it: both points given.
aclib.RunTapirCommand ('ModifyGridElements', {'gridElementsWithDetails': [{
    'elementId': grid, 'begCoordinate': {'x': -2.0, 'y': 0.0}, 'endCoordinate': {'x': 18.0, 'y': 0.0}}]})

# Lengthen it: only the end point given. Relabel it with the marker's GDL parameter.
result = aclib.RunTapirCommand ('ModifyGridElements', {'gridElementsWithDetails': [{
    'elementId': grid,
    'endCoordinate': {'x': 28.0, 'y': 0.0},
    'parameters': [{'name': 'AC_MarkerText_1', 'value': 'A'}]}]})
print (f"Warnings: {result['warnings']}")

for g in aclib.RunTapirCommand ('GetGridElements', {})['gridElements']:
    if g['elementId']['guid'] == grid['guid']:
        print (f"Grid at ({g['pos']['x']:.2f}, {g['pos']['y']:.2f}), angle {g['angle']:.4f} rad, length {g['length']:.2f} m")
