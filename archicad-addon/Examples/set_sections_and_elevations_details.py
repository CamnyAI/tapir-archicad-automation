import aclib

# Changes the line pen and the marker text pen of every section of the project,
# and numbers them S-1, S-2, ... (the Project Map viewpoints follow the new IDs).
sections = aclib.RunTapirCommand ('GetElementsByType', {'elementType': 'CutPlane'})['elements']

print (aclib.RunTapirCommand ('SetSectionsAndElevationsDetails', {
    'elementsWithDetails': [{
        'elementId': section['elementId'],
        'details': {'linePenIndex': 5, 'textPenIndex': 5, 'referenceId': f'S-{index + 1}'}
    } for index, section in enumerate (sections)]
}))
