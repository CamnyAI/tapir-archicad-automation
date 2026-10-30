#pragma once

#include "CommandBase.hpp"

// Type-specific details of GetDetailsOfElements for the element types its own switch did not
// cover (Skylight, Camera, CamSet, ChangeMarker, Picture) and the Bezier handles of a Spline. Each
// writes the fields of its schema definition (SkylightDetails, CameraDetails, CameraSetDetails,
// ChangeMarkerDetails, PictureDetails, SplineDetails in CommonSchemaDefinitions.json) into
// `typeSpecificDetails`.
// Every memo they read is checked against the size of its handle before it is indexed.
namespace ElementDetailsReaders {

// The placement and opening settings of a skylight. The library part and the owner roof or
// shell are added by the caller, the same way as for windows and doors.
void AddSkylightDetails (const API_Element& elem, const Stories& stories, GS::ObjectState& typeSpecificDetails);

// The perspective of a camera, in the shape Set3DProjectionSettings takes, and its place in its
// camera set (path).
void AddCameraDetails (const API_Element& elem, GS::ObjectState& typeSpecificDetails);

// The name, the cameras and the path options of a camera set.
void AddCameraSetDetails (const API_Element& elem, GS::ObjectState& typeSpecificDetails);

// The linked change, the marker and the cloud polygon of a change marker.
void AddChangeMarkerDetails (const API_Element& elem, GS::ObjectState& typeSpecificDetails);

// The box, rotation, mirroring, size and format of a placed picture (Figure tool). The image
// data itself is not read.
void AddPictureDetails (const API_Element& elem, GS::ObjectState& typeSpecificDetails);

// autoSmooth and the Bezier handle of every spline point (memo bezierDirs), next to the
// coordinates the Spline case already reads.
void AddSplineBezierDirections (const API_Element& elem, GS::ObjectState& typeSpecificDetails);

}
