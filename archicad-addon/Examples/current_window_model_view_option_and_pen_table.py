import aclib

# The model view options and the pen table of the current window, e.g. the floor plan. Each model
# window (the floor plan, the 3D window, each section and elevation) has its own; layouts have
# neither. The three commands are available from Archicad 27; on older versions they return an error.
current = aclib.RunTapirCommand ('GetCurrentModelViewOption')
currentPenTable = aclib.RunTapirCommand ('GetCurrentPenTable')

if current is not None and 'error' not in current:
    # Switch to the first model view option combination of the project ...
    combinations = aclib.RunTapirCommand ('GetModelViewOptions')['modelViewOptions']
    aclib.RunTapirCommand ('SetCurrentModelViewOption', {
        'name': combinations[0]['name']
    })

    # ... hide the doors and the zone stamps on top of it (index 0: custom settings) ...
    aclib.RunTapirCommand ('SetCurrentModelViewOption', {
        'settings': {
            'doorDisplay': 'HideOnPlan',
            'hideZoneStamps': True
        }
    })
    aclib.RunTapirCommand ('GetCurrentModelViewOption')

    # ... and draw it with another pen table.
    penTables = aclib.RunTapirCommand ('GetAttributesByType', {'attributeType': 'PenTable'})['attributes']
    aclib.RunTapirCommand ('SetCurrentPenTable', {
        'attributeId': penTables[0]['attributeId']
    })
    aclib.RunTapirCommand ('GetCurrentPenTable')

    # Restore what the user saw before.
    if 'name' in current:
        aclib.RunTapirCommand ('SetCurrentModelViewOption', {'name': current['name']})
    else:
        aclib.RunTapirCommand ('SetCurrentModelViewOption', {'settings': current['settings']})
    if currentPenTable.get ('isPenTableAttribute'):
        aclib.RunTapirCommand ('SetCurrentPenTable', {'attributeId': currentPenTable['attributeId']})
