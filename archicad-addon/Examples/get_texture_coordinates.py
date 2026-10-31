import aclib

# GetTextureCoordinates returns the texture coordinates (u, v) at every vertex of every face of the
# 3D model of an element, and the material (surface and texture) of each face, for example to map
# the textures when the geometry is exported to a renderer. The bodies, faces and vertexIds are the
# same as Get3DGeometryOfElements answers (including the parts of a Stair, Railing or Curtain Wall
# and the segments of a Column or Beam), so the two answers can be joined by index; with
# includeVertices: true the vertices come with this answer and it is enough on its own.
walls = aclib.RunTapirCommand (
    'GetElementsByType', {
        'elementType': 'Wall'
    }, debug = False)['elements']

# The first few walls are enough to show the shape of the response.
walls = walls[:3]

textureCoordinates = aclib.RunTapirCommand (
    'GetTextureCoordinates', {
        'elements': walls,
        'includeVertices': True
    }, debug = False)['textureCoordinates']

def TextureSpace (uv, texture):
    # (u, v) are lengths in the plane of the texture, in meters. The surface's own texture
    # transformation is not in them: scale them by its size (rotationAngle and mirrorX/mirrorY,
    # also in texture, are left out here) to get repetitions of the picture.
    return (uv['u'] / texture['xSize'], uv['v'] / texture['ySize'])

for wall, textureCoordinatesOfWall in zip (walls, textureCoordinates):
    print ('Wall {}'.format (wall['elementId']['guid']))
    if 'error' in textureCoordinatesOfWall:
        print ('  Error:\n{}'.format (aclib.JsonDumpDictionary (textureCoordinatesOfWall['error'])))
        continue

    materials = textureCoordinatesOfWall['materials']
    for material in materials:
        texture = material.get ('texture')
        print ('  Material {}: texture {}'.format (material['name'], texture['name'] if texture else None))

    for body in textureCoordinatesOfWall['bodies']:
        if 'error' in body:
            print ('  Body without texture information')
            continue
        vertices = body['vertices']
        faces = [face for face in body['polygons'] if 'error' not in face]
        print ('  Body with {} faces, {} with texture coordinates'.format (len (body['polygons']), len (faces)))
        for face in faces[:2]:
            material = materials[face['materialIndex']] if 'materialIndex' in face else {}
            texture = material.get ('texture')
            for vertexId, uv in zip (face['vertexIds'], face['uvs']):
                vertex = vertices[vertexId]
                line = '    ({:.3f}, {:.3f}, {:.3f}): u {:.3f}, v {:.3f}'.format (vertex['x'], vertex['y'], vertex['z'], uv['u'], uv['v'])
                if texture:
                    line += ', picture {:.3f}, {:.3f}'.format (*TextureSpace (uv, texture))
                print (line)
