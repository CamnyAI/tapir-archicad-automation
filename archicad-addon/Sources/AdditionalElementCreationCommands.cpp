#include "AdditionalElementCreationCommands.hpp"
#include "MigrationHelper.hpp"
#include "NotificationCommands.hpp"
#include "File.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace {

constexpr double Pi = 3.14159265358979323846;

void SetElemType (API_Element& element, API_ElemTypeID typeID, API_ElemVariationID variationID = APIVarId_Generic)
{
#ifdef ServerMainVers_2600
    element.header.type = API_ElemType (typeID, variationID);
#else
    element.header.typeID = typeID;
    element.header.variationID = variationID;
#endif
}

API_ElemVariationID GetVariationId (const API_Elem_Head& header)
{
#ifdef ServerMainVers_2600
    return header.type.variationID;
#else
    return header.variationID;
#endif
}

void SetBaseType (API_Base& base, API_ElemTypeID typeID)
{
#ifdef ServerMainVers_2600
    base.type = API_ElemType (typeID);
#else
    base.typeID = typeID;
#endif
}

API_Tranmat IdentityTranmat ()
{
    API_Tranmat tranmat = {};
    tranmat.tmx[0] = 1.0;
    tranmat.tmx[5] = 1.0;
    tranmat.tmx[10] = 1.0;
    return tranmat;
}

void SetUniString (GS::uchar_t* target, USize targetSize, const GS::UniString& value)
{
    GS::ucsncpy (target, value.ToUStr ().Get (), targetSize - 1);
    target[targetSize - 1] = 0;
}

// The profile of a shell is a polyline. Archicad keeps it as a closed polygon whose last point
// repeats the first one, with a body flag per edge: true for the edges of the profile, false for
// the edge that only closes the polygon (as in the DevKit's shell examples). An open profile of
// n points has n - 1 profile edges and one closing edge; a closed one has n profile edges.
GS::Optional<GS::UniString> BuildShellShape (const GS::ObjectState& profile, const char* fieldName, API_Polygon& poly, API_ShellShapeData& shape)
{
    GS::Array<GS::ObjectState> coordinates;
    profile.Get ("coordinates", coordinates);
    GS::Array<GS::ObjectState> arcs;
    profile.Get ("arcs", arcs);
    bool closed = false;
    profile.Get ("closed", closed);

    USize n = coordinates.GetSize ();
    if (n >= 3 && IsSame2DCoordinate (coordinates[0], coordinates[n - 1])) {
        // The first point repeated at the end closes the profile.
        --n;
        closed = true;
    }
    if (n < 2) {
        return GS::UniString::Printf ("'%s' needs at least 2 coordinates.", fieldName);
    }
    if (closed && n < 3) {
        return GS::UniString::Printf ("A closed '%s' needs at least 3 different coordinates.", fieldName);
    }
    for (UIndex i = 0; i + 1 < n; ++i) {
        if (IsSame2DCoordinate (coordinates[i], coordinates[i + 1])) {
            return GS::UniString::Printf ("'%s' has two identical consecutive coordinates (index %u).", fieldName, i);
        }
    }

    const GS::Array<API_PolyArc> polyArcs = GetPolyArcs (arcs, 1);
    if (polyArcs.GetSize () != arcs.GetSize ()) {
        return GS::UniString::Printf ("Every arc of '%s' needs begIndex, endIndex and arcAngle.", fieldName);
    }
    const Int32 lastEdgeEnd = closed ? static_cast<Int32> (n) + 1 : static_cast<Int32> (n);
    for (const API_PolyArc& arc : polyArcs) {
        if (arc.endIndex != arc.begIndex + 1 || arc.begIndex < 1 || arc.endIndex > lastEdgeEnd) {
            return GS::UniString::Printf ("An arc of '%s' must join two consecutive points of the profile (endIndex = begIndex + 1, 0-based).", fieldName);
        }
    }

    poly.nCoords = static_cast<Int32> (n) + 1;
    poly.nSubPolys = 1;
    poly.nArcs = static_cast<Int32> (polyArcs.GetSize ());

    shape.coords = reinterpret_cast<API_Coord**> (BMAllocateHandle ((poly.nCoords + 1) * sizeof (API_Coord), ALLOCATE_CLEAR, 0));
    shape.pends = reinterpret_cast<Int32**> (BMAllocateHandle ((poly.nSubPolys + 1) * sizeof (Int32), ALLOCATE_CLEAR, 0));
    // No arc handle without arcs: Archicad answers APIERR_BADPOLY for a profile with an empty one.
    if (poly.nArcs > 0) {
        shape.parcs = reinterpret_cast<API_PolyArc**> (BMAllocateHandle (poly.nArcs * sizeof (API_PolyArc), ALLOCATE_CLEAR, 0));
    }
    shape.bodyFlags = reinterpret_cast<bool**> (BMAllocateHandle ((poly.nCoords + 1) * sizeof (bool), ALLOCATE_CLEAR, 0));
    if (shape.coords == nullptr || shape.pends == nullptr || (poly.nArcs > 0 && shape.parcs == nullptr) || shape.bodyFlags == nullptr) {
        return GS::UniString ("Not enough memory for the shell profile.");
    }

    for (UIndex i = 0; i < n; ++i) {
        (*shape.coords)[i + 1] = Get2DCoordinateFromObjectState (coordinates[i]);
        (*shape.bodyFlags)[i + 1] = true;
    }
    (*shape.coords)[n + 1] = (*shape.coords)[1];
    (*shape.bodyFlags)[n] = closed;
    (*shape.bodyFlags)[n + 1] = (*shape.bodyFlags)[1];
    (*shape.pends)[0] = 0;
    (*shape.pends)[1] = poly.nCoords;
    for (UIndex i = 0; i < polyArcs.GetSize (); ++i) {
        (*shape.parcs)[i] = polyArcs[i];
    }
    return {};
}

GS::Optional<GS::UniString> ApplyShellStructure (const GS::ObjectState& parameters, API_ShellBaseType& shellBase)
{
    GS::UniString structureType;
    const bool hasStructureType = parameters.Get ("structureType", structureType);
    const GS::ObjectState* buildingMaterialId = parameters.Get ("buildingMaterialId");
    const GS::ObjectState* compositeId = parameters.Get ("compositeId");

    if (hasStructureType && structureType != "Basic" && structureType != "Composite") {
        return GS::UniString ("'structureType' must be 'Basic' or 'Composite'.");
    }
    if (buildingMaterialId != nullptr && compositeId != nullptr) {
        return GS::UniString ("Give either 'buildingMaterialId' or 'compositeId', not both.");
    }
    if ((hasStructureType && structureType == "Basic" && compositeId != nullptr) ||
        (hasStructureType && structureType == "Composite" && buildingMaterialId != nullptr)) {
        return GS::UniString ("'buildingMaterialId' goes with the 'Basic' and 'compositeId' with the 'Composite' structure.");
    }

    if ((hasStructureType && structureType == "Basic") || buildingMaterialId != nullptr) {
        shellBase.modelElemStructureType = API_BasicStructure;
    } else if ((hasStructureType && structureType == "Composite") || compositeId != nullptr) {
        shellBase.modelElemStructureType = API_CompositeStructure;
    }

    if (buildingMaterialId != nullptr) {
        const API_AttributeIndex index = GetAttributeIndexFromGuid (API_BuildingMaterialID, GetGuidFromObjectState (*buildingMaterialId));
        if (index == APIInvalidAttributeIndex) {
            return GS::UniString ("'buildingMaterialId' does not refer to an existing building material.");
        }
        shellBase.buildingMaterial = index;
    }
    if (compositeId != nullptr) {
        const API_AttributeIndex index = GetAttributeIndexFromGuid (API_CompWallID, GetGuidFromObjectState (*compositeId));
        if (index == APIInvalidAttributeIndex) {
            return GS::UniString ("'compositeId' does not refer to an existing composite.");
        }
        shellBase.composite = index;
    }

    double thickness = 0.0;
    if (parameters.Get ("thickness", thickness)) {
        if (thickness <= 0.0) {
            return GS::UniString ("'thickness' must be greater than zero.");
        }
        shellBase.thickness = thickness;
    }
    return {};
}

GS::Optional<GS::ObjectState> BadParameter (const GS::UniString& message)
{
    return CreateErrorResponse (APIERR_BADPARS, message);
}

GS::Optional<GS::ObjectState> BadParameterFrom (const GS::Optional<GS::UniString>& message)
{
    return CreateErrorResponse (APIERR_BADPARS, message.Get ());
}

void ApplyNoteStyle (const GS::ObjectState& parameters, API_NoteType& note)
{
    double textHeight = 0.0;
    if (parameters.Get ("textHeight", textHeight) && textHeight > 0.0) {
        note.noteSize = textHeight;
    }
    short textPen = 0;
    if (parameters.Get ("textPen", textPen) && textPen > 0) {
        note.notePen = textPen;
    }
}

const char* ShellPropertiesSchema = R"(
    "favoriteName": {
        "type": "string",
        "description": "Optional name of a Shell favorite to base the new shell on. Its settings are applied first, then the explicitly given fields override them."
    },
    "id": {
        "type": "string",
        "description": "Optional Element ID of the new element. When omitted the element gets the next automatic ID of its tool."
    },
    "layerIndex": {
        "type": "integer",
        "minimum": 1,
        "description": "Optional layer attribute index to place the new element on. It must be an existing, unlocked layer. When omitted the element gets the layer of the favorite or of the tool defaults."
    },
    "floorIndex": {
        "type": "integer",
        "description": "Optional home story of the shell. It does not move the shell: its geometry is in project coordinates (z from the project zero, not from the story level). When omitted, the story of the tool defaults (the current story)."
    },
    "shellClass": {
        "type": "string",
        "enum": ["Extruded"],
        "description": "Optional geometry method of the shell. Only Extruded shells can be created: Revolved and Ruled shells made through ACAPI_Element_Create have no body in Archicad 30 (tested with the values of the DevKit's own examples). Default: Extruded."
    },
    "profile": {
        "type": "object",
        "description": "The profile of the shell: a polyline drawn at right angles to the extrusion, its x axis horizontal (see shapeDirection) and its y axis up.",
        "properties": {
            "coordinates": {
                "type": "array",
                "items": { "$ref": "#/Coordinate2D" },
                "minItems": 2,
                "description": "The points of the polyline. A last point equal to the first closes the profile."
            },
            "arcs": {
                "type": "array",
                "items": { "$ref": "#/PolyArc" },
                "description": "Optional arcs of the polyline: begIndex and endIndex are 0-based indices of consecutive points."
            },
            "closed": {
                "type": "boolean",
                "description": "Optional: the profile is closed (its last point is joined to the first), e.g. for a tube. Default: false, or true when the last point equals the first."
            }
        },
        "additionalProperties": false,
        "required": ["coordinates"]
    },
    "begCoordinate": {
        "$ref": "#/Coordinate3D",
        "description": "The point the extrusion starts from, in project coordinates (z from the project zero). The profile's origin is at this point."
    },
    "extrusionVector": {
        "$ref": "#/Coordinate3D",
        "description": "The direction and length of the extrusion."
    },
    "shapeDirection": {
        "$ref": "#/Coordinate2D",
        "description": "Optional direction of the profile's x axis. Default: from the favorite or the tool defaults."
    },
    "slantAngle": {
        "type": "number",
        "description": "Optional angle of the extrusion vector from the horizontal plane in radians. Default: from the favorite or the tool defaults."
    },
    "shapePlaneTilt": {
        "type": "number",
        "description": "Optional tilt of the profile's plane in radians. Default: from the favorite or the tool defaults."
    },
    "begPlaneTilt": {
        "type": "number",
        "description": "Optional tilt of the start plane of the extrusion in radians. Default: from the favorite or the tool defaults."
    },
    "endPlaneTilt": {
        "type": "number",
        "description": "Optional tilt of the end plane of the extrusion in radians. Default: from the favorite or the tool defaults."
    },
    "isFlipped": {
        "type": "boolean",
        "description": "Optional: apply the thickness on the other side of the shell's surface."
    },
    "thickness": {
        "type": "number",
        "exclusiveMinimum": 0.0,
        "description": "Optional thickness of a Basic shell. A Composite shell takes its thickness from the composite."
    },
    "structureType": {
        "type": "string",
        "enum": ["Basic", "Composite"],
        "description": "Optional structure. Without it the shell keeps the structure of the favorite or of the tool defaults, unless buildingMaterialId (Basic) or compositeId (Composite) is given."
    },
    "buildingMaterialId": {
        "$ref": "#/AttributeId",
        "description": "Optional building material of a Basic shell."
    },
    "compositeId": {
        "$ref": "#/AttributeId",
        "description": "Optional composite of a Composite shell. It must be a composite usable for shells."
    }
)";

} // namespace

// ---------------------------------------------------------------------------------------------
// Shells
// ---------------------------------------------------------------------------------------------

CreateShellsCommand::CreateShellsCommand () :
    CreateElementsCommandBase ("CreateShells", API_ShellID, "shellsData")
{
}

GS::Optional<GS::UniString> CreateShellsCommand::GetInputParametersSchema () const
{
    return GS::UniString (R"({
        "type": "object",
        "properties": {
            "shellsData": {
                "type": "array",
                "description": "Array of data to create Shells.",
                "items": {
                    "type": "object",
                    "description": "The parameters of the new Extruded Shell: a profile extruded from begCoordinate along extrusionVector. Lengths are in meters, angles in radians.",
                    "properties": {)") + ShellPropertiesSchema + R"(},
                    "additionalProperties": false,
                    "required": [
                        "profile",
                        "begCoordinate",
                        "extrusionVector"
                    ]
                }
            }
        },
        "additionalProperties": false,
        "required": [
            "shellsData"
        ]
    })";
}

GS::Optional<GS::ObjectState> CreateShellsCommand::SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, const Stories& /*stories*/, const GS::ObjectState& parameters) const
{
    GS::UniString shellClassName ("Extruded");
    parameters.Get ("shellClass", shellClassName);
    if (shellClassName != "Extruded") {
        return BadParameter ("Only 'Extruded' shells can be created.");
    }

    // The defaults of the geometry method, as the DevKit's shell examples read them: the union
    // holds the data of one class only. The memo is built from scratch below.
    ACAPI_DisposeElemMemoHdls (&memo);
    memo = {};
    element = {};
    SetElemType (element, API_ShellID);
    element.shell.shellClass = API_ExtrudedShellID;
    GSErrCode err = ACAPI_Element_GetDefaults (&element, nullptr);
    if (err != NoError) {
        return CreateErrorResponse (err, "Failed to read the Shell defaults.");
    }
    if (element.shell.shellClass != API_ExtrudedShellID) {
        // Defaults of another geometry method: their union data means nothing for this one.
        element.shell.shellClass = API_ExtrudedShellID;
        std::memset (&element.shell.u, 0, sizeof (element.shell.u));
    }

    const GS::Optional<GS::ObjectState> layerError = ApplyLayerIndexFromParameters (parameters, element);
    if (layerError.HasValue ()) {
        return layerError;
    }
    short floorIndex = 0;
    if (parameters.Get ("floorIndex", floorIndex)) {
        element.header.floorInd = floorIndex;
    }

    const GS::Optional<GS::UniString> structureError = ApplyShellStructure (parameters, element.shell.shellBase);
    if (structureError.HasValue ()) {
        return BadParameterFrom (structureError);
    }
    parameters.Get ("isFlipped", element.shell.isFlipped);

    // No body contour and no holes: the whole surface the profile describes. The base plane is the
    // floor plan's coordinate system; the extrusion is placed by begCoordinate and extrusionVector.
    element.shell.hasContour = false;
    element.shell.numHoles = 0;
    element.shell.basePlane = IdentityTranmat ();

    const GS::ObjectState* profile = parameters.Get ("profile");
    const GS::ObjectState* begCoordinate = parameters.Get ("begCoordinate");
    const GS::ObjectState* extrusionVector = parameters.Get ("extrusionVector");
    if (profile == nullptr || begCoordinate == nullptr || extrusionVector == nullptr) {
        return BadParameter ("'profile', 'begCoordinate' and 'extrusionVector' are required.");
    }

    API_ExtrudedShellData& extruded = element.shell.u.extrudedShell;
    extruded.begC = Get3DCoordinateFromObjectState (*begCoordinate);
    const API_Coord3D vector = Get3DCoordinateFromObjectState (*extrusionVector);
    if (std::sqrt (vector.x * vector.x + vector.y * vector.y + vector.z * vector.z) < 1e-6) {
        return BadParameter ("'extrusionVector' must not be a zero vector.");
    }
    extruded.extrusionVector.x = vector.x;
    extruded.extrusionVector.y = vector.y;
    extruded.extrusionVector.z = vector.z;
    const GS::ObjectState* shapeDirection = parameters.Get ("shapeDirection");
    if (shapeDirection != nullptr) {
        const API_Coord direction = Get2DCoordinateFromObjectState (*shapeDirection);
        extruded.shapeDirection.x = direction.x;
        extruded.shapeDirection.y = direction.y;
    }
    parameters.Get ("slantAngle", extruded.slantAngle);
    parameters.Get ("shapePlaneTilt", extruded.shapePlaneTilt);
    parameters.Get ("begPlaneTilt", extruded.begPlaneTilt);
    parameters.Get ("endPlaneTilt", extruded.endPlaneTilt);

    const GS::Optional<GS::UniString> error = BuildShellShape (*profile, "profile", extruded.shellShape, memo.shellShapes[0]);
    if (error.HasValue ()) {
        return BadParameterFrom (error);
    }
    return {};
}

// ---------------------------------------------------------------------------------------------
// Radial dimensions
// ---------------------------------------------------------------------------------------------

CreateRadialDimensionsCommand::CreateRadialDimensionsCommand () :
    CreateElementsCommandBase ("CreateRadialDimensions", API_RadialDimensionID, "radialDimensionsData")
{
}

GS::Optional<GS::UniString> CreateRadialDimensionsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "radialDimensionsData": {
                "type": "array",
                "description": "Array of data to create Radial Dimensions.",
                "items": {
                    "type": "object",
                    "description": "The parameters of the new Radial Dimension. It dimensions the radius of an arc or circle element and follows the element when it changes.",
                    "properties": {
                        "favoriteName": {
                            "type": "string",
                            "description": "Optional name of a Radial Dimension favorite to base the new dimension on. Its settings are applied first, then the explicitly given fields override them."
                        },
                        "layerIndex": {
                            "type": "integer",
                            "minimum": 1,
                            "description": "Optional layer attribute index to place the new element on. It must be an existing, unlocked layer."
                        },
                        "floorIndex": {
                            "type": "integer",
                            "description": "Optional story of the dimension. Default: the story of the dimensioned element."
                        },
                        "elementId": {
                            "$ref": "#/ElementId",
                            "description": "The dimensioned Arc or Circle element."
                        },
                        "pointOnArc": {
                            "$ref": "#/Coordinate2D",
                            "description": "Optional point of the arc the dimension points to. Default: the point of the arc towards endCoordinate from its center."
                        },
                        "endCoordinate": {
                            "$ref": "#/Coordinate2D",
                            "description": "The end of the dimension line, where the dimension text goes."
                        },
                        "inIndex": {
                            "type": "integer",
                            "description": "Optional index of the element's part the dimension refers to. Default: 1."
                        },
                        "showOrigin": {
                            "type": "boolean",
                            "description": "Optional: show the arc's center."
                        },
                        "prefix": {
                            "type": "string",
                            "maxLength": 7,
                            "description": "Optional prefix before the number, e.g. 'R'."
                        },
                        "linePen": {
                            "type": "integer",
                            "minimum": 1,
                            "maximum": 255,
                            "description": "Optional pen of the dimension line."
                        },
                        "textHeight": {
                            "type": "number",
                            "description": "Optional height of the dimension text in millimeters (paper size)."
                        },
                        "textPen": {
                            "type": "integer",
                            "minimum": 1,
                            "maximum": 255,
                            "description": "Optional pen of the dimension text."
                        }
                    },
                    "additionalProperties": false,
                    "required": [
                        "elementId",
                        "endCoordinate"
                    ]
                }
            }
        },
        "additionalProperties": false,
        "required": [
            "radialDimensionsData"
        ]
    })";
}

GS::Optional<GS::ObjectState> CreateRadialDimensionsCommand::SetTypeSpecificParameters (API_Element& element, API_ElementMemo& /*memo*/, const Stories& /*stories*/, const GS::ObjectState& parameters) const
{
    const GS::Optional<GS::ObjectState> layerError = ApplyLayerIndexFromParameters (parameters, element);
    if (layerError.HasValue ()) {
        return layerError;
    }

    const GS::ObjectState* elementId = parameters.Get ("elementId");
    const GS::ObjectState* endCoordinate = parameters.Get ("endCoordinate");
    if (elementId == nullptr || endCoordinate == nullptr) {
        return BadParameter ("'elementId' and 'endCoordinate' are required.");
    }

    API_Element arcElement = {};
    arcElement.header.guid = GetGuidFromObjectState (*elementId);
    if (arcElement.header.guid == APINULLGuid || ACAPI_Element_Get (&arcElement) != NoError) {
        return CreateErrorResponse (APIERR_BADID, "'elementId' does not refer to an existing element.");
    }
    const API_ElemTypeID arcType = GetElemTypeId (arcElement.header);
    const API_Coord endC = Get2DCoordinateFromObjectState (*endCoordinate);

    API_Coord pointOnArc = {};
    const GS::ObjectState* pointOnArcOs = parameters.Get ("pointOnArc");
    if (arcType == API_ArcID || arcType == API_CircleID) {
        const API_ArcType& arc = arcType == API_ArcID ? arcElement.arc : arcElement.circle;
        if (std::fabs (arc.ratio - 1.0) > 1e-6 && arc.ratio != 0.0) {
            return BadParameter ("An elliptic arc has no radius to dimension.");
        }
        element.radialDimension.dimVal = arc.r;
        if (pointOnArcOs != nullptr) {
            pointOnArc = Get2DCoordinateFromObjectState (*pointOnArcOs);
        } else {
            const double dx = endC.x - arc.origC.x;
            const double dy = endC.y - arc.origC.y;
            const double length = std::sqrt (dx * dx + dy * dy);
            if (length < 1e-6) {
                return BadParameter ("'endCoordinate' must not be the center of the arc.");
            }
            pointOnArc.x = arc.origC.x + arc.r * dx / length;
            pointOnArc.y = arc.origC.y + arc.r * dy / length;
        }
    } else {
        if (pointOnArcOs == nullptr) {
            return BadParameter ("'pointOnArc' is required for elements other than Arcs and Circles.");
        }
        pointOnArc = Get2DCoordinateFromObjectState (*pointOnArcOs);
    }

    short floorIndex = arcElement.header.floorInd;
    parameters.Get ("floorIndex", floorIndex);
    element.header.floorInd = floorIndex;

    API_Base& base = element.radialDimension.base.base;
    SetBaseType (base, arcType);
    base.guid = arcElement.header.guid;
    base.line = true;
    base.inIndex = 1;
    parameters.Get ("inIndex", base.inIndex);
    element.radialDimension.base.loc = pointOnArc;
    element.radialDimension.base.loc3D.x = pointOnArc.x;
    element.radialDimension.base.loc3D.y = pointOnArc.y;
    element.radialDimension.endC = endC;

    parameters.Get ("showOrigin", element.radialDimension.showOrigo);
    GS::UniString prefix;
    if (parameters.Get ("prefix", prefix)) {
        const GS::String prefixUtf8 = prefix.ToCStr (CC_UTF8).Get ();
        CHTruncate (prefixUtf8.ToCStr (), element.radialDimension.prefix, sizeof (element.radialDimension.prefix));
    }
    short linePen = 0;
    if (parameters.Get ("linePen", linePen) && linePen > 0) {
        element.radialDimension.linPen = linePen;
    }
    ApplyNoteStyle (parameters, element.radialDimension.note);
    return {};
}

// ---------------------------------------------------------------------------------------------
// Angle dimensions
// ---------------------------------------------------------------------------------------------

CreateAngleDimensionsCommand::CreateAngleDimensionsCommand () :
    CreateElementsCommandBase ("CreateAngleDimensions", API_AngleDimensionID, "angleDimensionsData")
{
}

GS::Optional<GS::UniString> CreateAngleDimensionsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "angleDimensionsData": {
                "type": "array",
                "description": "Array of data to create Angle Dimensions.",
                "items": {
                    "type": "object",
                    "description": "The parameters of the new Angle Dimension: the angle between two lines, measured around their intersection. The dimension is static: it is not associated with elements.",
                    "properties": {
                        "favoriteName": {
                            "type": "string",
                            "description": "Optional name of an Angle Dimension favorite to base the new dimension on. Its settings are applied first, then the explicitly given fields override them."
                        },
                        "layerIndex": {
                            "type": "integer",
                            "minimum": 1,
                            "description": "Optional layer attribute index to place the new element on. It must be an existing, unlocked layer."
                        },
                        "floorIndex": {
                            "type": "integer",
                            "description": "Optional story of the dimension. Default: the story of the tool defaults (the current story)."
                        },
                        "firstLine": {
                            "type": "object",
                            "properties": {
                                "begCoordinate": { "$ref": "#/Coordinate2D" },
                                "endCoordinate": { "$ref": "#/Coordinate2D" }
                            },
                            "additionalProperties": false,
                            "required": ["begCoordinate", "endCoordinate"],
                            "description": "The first leg of the angle."
                        },
                        "secondLine": {
                            "type": "object",
                            "properties": {
                                "begCoordinate": { "$ref": "#/Coordinate2D" },
                                "endCoordinate": { "$ref": "#/Coordinate2D" }
                            },
                            "additionalProperties": false,
                            "required": ["begCoordinate", "endCoordinate"],
                            "description": "The second leg of the angle. It must not be parallel to the first."
                        },
                        "arcPoint": {
                            "$ref": "#/Coordinate2D",
                            "description": "A point the dimension arc goes through: it gives the radius of the arc and which of the angles between the lines is dimensioned."
                        },
                        "smallArc": {
                            "type": "boolean",
                            "description": "Optional: dimension the smaller arc between the legs (true, the default) or the larger one."
                        },
                        "linePen": {
                            "type": "integer",
                            "minimum": 1,
                            "maximum": 255,
                            "description": "Optional pen of the dimension and witness lines."
                        },
                        "textHeight": {
                            "type": "number",
                            "description": "Optional height of the dimension text in millimeters (paper size)."
                        },
                        "textPen": {
                            "type": "integer",
                            "minimum": 1,
                            "maximum": 255,
                            "description": "Optional pen of the dimension text."
                        }
                    },
                    "additionalProperties": false,
                    "required": [
                        "firstLine",
                        "secondLine",
                        "arcPoint"
                    ]
                }
            }
        },
        "additionalProperties": false,
        "required": [
            "angleDimensionsData"
        ]
    })";
}

GS::Optional<GS::ObjectState> CreateAngleDimensionsCommand::SetTypeSpecificParameters (API_Element& element, API_ElementMemo& /*memo*/, const Stories& /*stories*/, const GS::ObjectState& parameters) const
{
    const GS::Optional<GS::ObjectState> layerError = ApplyLayerIndexFromParameters (parameters, element);
    if (layerError.HasValue ()) {
        return layerError;
    }
    short floorIndex = 0;
    if (parameters.Get ("floorIndex", floorIndex)) {
        element.header.floorInd = floorIndex;
    }

    API_Coord points[4] = {};
    const char* lineNames[2] = { "firstLine", "secondLine" };
    for (UIndex lineIndex = 0; lineIndex < 2; ++lineIndex) {
        const GS::ObjectState* line = parameters.Get (lineNames[lineIndex]);
        const GS::ObjectState* beg = line != nullptr ? line->Get ("begCoordinate") : nullptr;
        const GS::ObjectState* end = line != nullptr ? line->Get ("endCoordinate") : nullptr;
        if (beg == nullptr || end == nullptr) {
            return BadParameter (GS::UniString::Printf ("'%s' needs 'begCoordinate' and 'endCoordinate'.", lineNames[lineIndex]));
        }
        points[2 * lineIndex] = Get2DCoordinateFromObjectState (*beg);
        points[2 * lineIndex + 1] = Get2DCoordinateFromObjectState (*end);
        if (IsSame2DCoordinate (points[2 * lineIndex], points[2 * lineIndex + 1])) {
            return BadParameter (GS::UniString::Printf ("'%s' has zero length.", lineNames[lineIndex]));
        }
    }
    const GS::ObjectState* arcPoint = parameters.Get ("arcPoint");
    if (arcPoint == nullptr) {
        return BadParameter ("'arcPoint' is required.");
    }

    // The center of the dimension arc: the intersection of the two lines.
    const double d1x = points[1].x - points[0].x;
    const double d1y = points[1].y - points[0].y;
    const double d2x = points[3].x - points[2].x;
    const double d2y = points[3].y - points[2].y;
    const double denominator = d1x * d2y - d1y * d2x;
    const double len1 = std::sqrt (d1x * d1x + d1y * d1y);
    const double len2 = std::sqrt (d2x * d2x + d2y * d2y);
    if (std::fabs (denominator) < 1e-9 * len1 * len2) {
        return BadParameter ("The two lines are parallel: they have no angle to dimension.");
    }
    const double t = ((points[2].x - points[0].x) * d2y - (points[2].y - points[0].y) * d2x) / denominator;
    API_Coord origo = { points[0].x + t * d1x, points[0].y + t * d1y };

    API_AngleDimensionType& dim = element.angleDimension;
    for (UIndex i = 0; i < 4; ++i) {
        dim.base[i] = {};
        SetBaseType (dim.base[i].base, API_ZombieElemID);
        dim.base[i].loc = points[i];
        dim.base[i].loc3D.x = points[i].x;
        dim.base[i].loc3D.y = points[i].y;
    }
    dim.pos = Get2DCoordinateFromObjectState (*arcPoint);
    dim.origo = origo;
    dim.radius = std::sqrt ((dim.pos.x - origo.x) * (dim.pos.x - origo.x) + (dim.pos.y - origo.y) * (dim.pos.y - origo.y));
    if (dim.radius < 1e-6) {
        return BadParameter ("'arcPoint' must not be the intersection of the lines.");
    }
    dim.smallArc = true;
    parameters.Get ("smallArc", dim.smallArc);
    short linePen = 0;
    if (parameters.Get ("linePen", linePen) && linePen > 0) {
        dim.linPen = linePen;
    }
    ApplyNoteStyle (parameters, dim.note);
    return {};
}

// ---------------------------------------------------------------------------------------------
// Skylights
// ---------------------------------------------------------------------------------------------

CreateSkylightsCommand::CreateSkylightsCommand () :
    CreateElementsCommandBase ("CreateSkylights", API_SkylightID, "skylightsData")
{
}

GS::Optional<GS::UniString> CreateSkylightsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "skylightsData": {
                "type": "array",
                "description": "Array of data to create Skylights.",
                "items": {
                    "type": "object",
                    "description": "The parameters of the new Skylight.",
                    "properties": {
                        "favoriteName": {
                            "type": "string",
                            "description": "Optional name of a Skylight favorite to base the new skylight on. Its settings are applied first, then the explicitly given fields override them."
                        },
                        "id": {
                            "type": "string",
                            "description": "Optional Element ID of the new element."
                        },
                        "layerIndex": {
                            "type": "integer",
                            "minimum": 1,
                            "description": "Optional layer attribute index to place the new element on. It must be an existing, unlocked layer."
                        },
                        "libraryPartName": {
                            "type": "string",
                            "description": "Optional name of the skylight library part to place, as GetLibraryParts lists it. Default: the library part of the favorite or of the tool defaults."
                        },
                        "ownerElementId": {
                            "$ref": "#/ElementId",
                            "description": "The Roof or Shell element the skylight is placed in."
                        },
                        "anchorPosition": {
                            "$ref": "#/Coordinate2D",
                            "description": "The floor plan position of the skylight's anchor point. It must be on the owner element."
                        },
                        "anchorLevel": {
                            "type": "number",
                            "description": "Optional level of the anchor point, as ACAPI_Element_Create takes it. Archicad puts the skylight onto the owner's surface at anchorPosition."
                        },
                        "anchorPoint": {
                            "type": "string",
                            "enum": ["BottomCenter", "BottomLeft", "BottomRight", "TopCenter", "TopLeft", "TopRight"],
                            "description": "Optional: which point of the skylight anchorPosition is."
                        },
                        "azimuthAngle": {
                            "type": "number",
                            "description": "Optional rotation in the horizontal plane in radians, measured clockwise."
                        },
                        "width": {
                            "type": "number",
                            "exclusiveMinimum": 0.0,
                            "description": "Optional width (A) of the skylight."
                        },
                        "height": {
                            "type": "number",
                            "exclusiveMinimum": 0.0,
                            "description": "Optional height (B) of the skylight."
                        }
                    },
                    "additionalProperties": false,
                    "required": [
                        "ownerElementId",
                        "anchorPosition"
                    ]
                }
            }
        },
        "additionalProperties": false,
        "required": [
            "skylightsData"
        ]
    })";
}

GS::Optional<GS::ObjectState> CreateSkylightsCommand::SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, const Stories& /*stories*/, const GS::ObjectState& parameters) const
{
    const GS::Optional<GS::ObjectState> layerError = ApplyLayerIndexFromParameters (parameters, element);
    if (layerError.HasValue ()) {
        return layerError;
    }

    const GS::ObjectState* ownerId = parameters.Get ("ownerElementId");
    const GS::ObjectState* anchorPosition = parameters.Get ("anchorPosition");
    if (ownerId == nullptr || anchorPosition == nullptr) {
        return BadParameter ("'ownerElementId' and 'anchorPosition' are required.");
    }
    API_Elem_Head ownerHeader = {};
    ownerHeader.guid = GetGuidFromObjectState (*ownerId);
    if (ownerHeader.guid == APINULLGuid || ACAPI_Element_GetHeader (&ownerHeader) != NoError) {
        return CreateErrorResponse (APIERR_BADID, "'ownerElementId' does not refer to an existing element.");
    }
    const API_ElemTypeID ownerType = GetElemTypeId (ownerHeader);
    if (ownerType != API_RoofID && ownerType != API_ShellID) {
        return BadParameter ("The owner of a skylight must be a Roof or a Shell.");
    }

    GS::UniString libraryPartName;
    if (parameters.Get ("libraryPartName", libraryPartName) && !libraryPartName.IsEmpty ()) {
        API_LibPart libPart = {};
        libPart.typeID = APILib_SkylightID;
        GS::ucscpy (libPart.docu_UName, libraryPartName.ToUStr ());
        GSErrCode err = ACAPI_LibraryPart_Search (&libPart, false, true);
        delete libPart.location;
        if (err != NoError) {
            return CreateErrorResponse (err, "No skylight library part has the name given in 'libraryPartName'.");
        }
        double a = 0.0;
        double b = 0.0;
        Int32 addParNum = 0;
        API_AddParType** addPars = nullptr;
        err = ACAPI_LibraryPart_GetParams (libPart.index, &a, &b, &addParNum, &addPars);
        if (err != NoError) {
            return CreateErrorResponse (err, "Failed to read the parameters of the library part given in 'libraryPartName'.");
        }
        if (memo.params != nullptr) {
            ACAPI_DisposeAddParHdl (&memo.params);
        }
        memo.params = addPars;
        element.skylight.openingBase.libInd = libPart.index;
        if (a > 0.0) {
            element.skylight.openingBase.width = a;
        }
        if (b > 0.0) {
            element.skylight.openingBase.height = b;
        }
    }

    element.header.floorInd = ownerHeader.floorInd;
    element.skylight.owner = ownerHeader.guid;
    element.skylight.anchorPosition = Get2DCoordinateFromObjectState (*anchorPosition);
    parameters.Get ("anchorLevel", element.skylight.anchorLevel);
    parameters.Get ("azimuthAngle", element.skylight.azimuthAngle);

    GS::UniString anchorPoint;
    if (parameters.Get ("anchorPoint", anchorPoint)) {
        if (anchorPoint == "BottomCenter")      element.skylight.anchorPoint = APISkylightAnchor_BC;
        else if (anchorPoint == "BottomLeft")   element.skylight.anchorPoint = APISkylightAnchor_BL;
        else if (anchorPoint == "BottomRight")  element.skylight.anchorPoint = APISkylightAnchor_BR;
        else if (anchorPoint == "TopCenter")    element.skylight.anchorPoint = APISkylightAnchor_TC;
        else if (anchorPoint == "TopLeft")      element.skylight.anchorPoint = APISkylightAnchor_TL;
        else if (anchorPoint == "TopRight")     element.skylight.anchorPoint = APISkylightAnchor_TR;
        else return BadParameter ("Unknown 'anchorPoint'.");
    }

    double width = 0.0;
    if (parameters.Get ("width", width)) {
        element.skylight.openingBase.width = width;
    }
    double height = 0.0;
    if (parameters.Get ("height", height)) {
        element.skylight.openingBase.height = height;
    }
    return {};
}

// ---------------------------------------------------------------------------------------------
// Cameras
// ---------------------------------------------------------------------------------------------

namespace {

GS::Optional<API_Guid> FindActiveCameraSet ()
{
    GS::Array<API_Guid> camSets;
    ACAPI_Element_GetElemList (API_CamSetID, &camSets);
    for (const API_Guid& guid : camSets) {
        API_Element camSet = {};
        camSet.header.guid = guid;
        if (ACAPI_Element_Get (&camSet) == NoError && camSet.camset.active) {
            return guid;
        }
    }
    return GS::NoValue;
}

// The last camera of a camera set, following the nextCam links from its first camera.
API_Guid FindLastCameraOfSet (const API_Guid& camSetGuid)
{
    API_Element camSet = {};
    camSet.header.guid = camSetGuid;
    if (ACAPI_Element_Get (&camSet) != NoError) {
        return APINULLGuid;
    }
    API_Guid last = APINULLGuid;
    API_Guid current = camSet.camset.firstCam;
    GS::HashSet<API_Guid> visited;
    while (current != APINULLGuid && !visited.Contains (current)) {
        visited.Add (current);
        API_Element camera = {};
        camera.header.guid = current;
        if (ACAPI_Element_Get (&camera) != NoError) {
            break;
        }
        last = current;
        current = camera.camera.perspCam.nextCam;
    }
    return last;
}

}

CreateCamerasCommand::CreateCamerasCommand () :
    CreateElementsCommandBase ("CreateCameras", API_CameraID, "camerasData")
{
}

GS::Optional<GS::UniString> CreateCamerasCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "camerasData": {
                "type": "array",
                "description": "Array of data to create perspective Cameras.",
                "items": {
                    "type": "object",
                    "description": "The parameters of the new Camera. It is added at the end of its camera set (path).",
                    "properties": {
                        "favoriteName": {
                            "type": "string",
                            "description": "Optional name of a Camera favorite to base the new camera on. Its settings are applied first, then the explicitly given fields override them."
                        },
                        "layerIndex": {
                            "type": "integer",
                            "minimum": 1,
                            "description": "Optional layer attribute index to place the new element on. It must be an existing, unlocked layer."
                        },
                        "cameraSetId": {
                            "$ref": "#/ElementId",
                            "description": "Optional camera set (path, a CamSet element) to add the camera to. Default: the active camera set; when the project has none, a new camera set is created and made active."
                        },
                        "cameraSetName": {
                            "type": "string",
                            "description": "Optional name of the camera set that is created when cameraSetId is not given and the project has no camera set. Default: 'Camera Path'."
                        },
                        "position": {
                            "$ref": "#/Coordinate3D",
                            "description": "The position of the camera."
                        },
                        "target": {
                            "$ref": "#/Coordinate3D",
                            "description": "The point the camera looks at."
                        },
                        "viewCone": {
                            "type": "number",
                            "exclusiveMinimum": 0.0,
                            "description": "Optional angle of the view cone in radians."
                        },
                        "rollAngle": {
                            "type": "number",
                            "description": "Optional roll angle of the camera in radians."
                        },
                        "waitFrames": {
                            "type": "integer",
                            "minimum": 0,
                            "description": "Optional number of frames the animation waits at this camera."
                        }
                    },
                    "additionalProperties": false,
                    "required": [
                        "position",
                        "target"
                    ]
                }
            }
        },
        "additionalProperties": false,
        "required": [
            "camerasData"
        ]
    })";
}

GS::Optional<GS::ObjectState> CreateCamerasCommand::SetTypeSpecificParameters (API_Element& element, API_ElementMemo& /*memo*/, const Stories& /*stories*/, const GS::ObjectState& parameters) const
{
    const GS::Optional<GS::ObjectState> layerError = ApplyLayerIndexFromParameters (parameters, element);
    if (layerError.HasValue ()) {
        return layerError;
    }

    const GS::ObjectState* positionOs = parameters.Get ("position");
    const GS::ObjectState* targetOs = parameters.Get ("target");
    if (positionOs == nullptr || targetOs == nullptr) {
        return BadParameter ("'position' and 'target' are required.");
    }
    const API_Coord3D position = Get3DCoordinateFromObjectState (*positionOs);
    const API_Coord3D target = Get3DCoordinateFromObjectState (*targetOs);
    if (IsSame3DCoordinate (position, target)) {
        return BadParameter ("'position' and 'target' must differ.");
    }

    API_Guid camSetGuid = APINULLGuid;
    const GS::ObjectState* cameraSetId = parameters.Get ("cameraSetId");
    if (cameraSetId != nullptr) {
        camSetGuid = GetGuidFromObjectState (*cameraSetId);
        if (!DoesElementExist (camSetGuid, API_CamSetID)) {
            return CreateErrorResponse (APIERR_BADID, "'cameraSetId' does not refer to an existing camera set.");
        }
    } else {
        const GS::Optional<API_Guid> active = FindActiveCameraSet ();
        if (active.HasValue ()) {
            camSetGuid = active.Get ();
        } else {
            // Camera sets have no tool defaults (ACAPI_Element_GetDefaults refuses them), so the
            // set is filled in as the DevKit's Do_CreateCamset does.
            API_Element camSet = {};
            SetElemType (camSet, API_CamSetID);
            GS::UniString cameraSetName ("Camera Path");
            parameters.Get ("cameraSetName", cameraSetName);
            SetUniString (camSet.camset.name, API_UniLongNameLen, cameraSetName);
            camSet.camset.perspPars.openedPath = true;
            camSet.camset.perspPars.bezierPath = false;
            camSet.camset.perspPars.smoothTarget = false;
            camSet.camset.perspPars.pen = element.camera.perspCam.pen > 0 ? element.camera.perspCam.pen : 1;
            camSet.camset.perspPars.inFrames = 10;
            camSet.camset.active = true;
            const GSErrCode err = ACAPI_Element_Create (&camSet, nullptr);
            if (err != NoError) {
                return CreateErrorResponse (err, "Failed to create a camera set for the camera.");
            }
            camSetGuid = camSet.header.guid;
        }
    }

    API_PerspCamPars& cam = element.camera.perspCam;
    element.camera.camSetGuid = camSetGuid;
    cam.persp.pos.x = position.x;
    cam.persp.pos.y = position.y;
    cam.persp.cameraZ = position.z;
    cam.persp.target.x = target.x;
    cam.persp.target.y = target.y;
    cam.persp.targetZ = target.z;
    const double dx = target.x - position.x;
    const double dy = target.y - position.y;
    cam.persp.distance = std::sqrt (dx * dx + dy * dy);
    cam.persp.azimuth = std::atan2 (dy, dx);
    parameters.Get ("viewCone", cam.persp.viewCone);
    if (cam.persp.viewCone <= 0.0) {
        cam.persp.viewCone = 60.0 * Pi / 180.0;
    }
    parameters.Get ("rollAngle", cam.persp.rollAngle);
    short waitFrames = 0;
    if (parameters.Get ("waitFrames", waitFrames)) {
        cam.waitFrames = waitFrames;
    }
    cam.prevCam = FindLastCameraOfSet (camSetGuid);
    cam.nextCam = APINULLGuid;
    cam.active = true;
    return {};
}

// ---------------------------------------------------------------------------------------------
// Pictures
// ---------------------------------------------------------------------------------------------

CreatePicturesCommand::CreatePicturesCommand () :
    CreateElementsCommandBase ("CreatePictures", API_PictureID, "picturesData")
{
}

GS::Optional<GS::UniString> CreatePicturesCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "picturesData": {
                "type": "array",
                "description": "Array of data to create Picture (Figure) elements from image files.",
                "items": {
                    "type": "object",
                    "description": "The parameters of the new Picture.",
                    "properties": {
                        "favoriteName": {
                            "type": "string",
                            "description": "Optional name of a Figure favorite to base the new picture on. Its settings are applied first, then the explicitly given fields override them."
                        },
                        "layerIndex": {
                            "type": "integer",
                            "minimum": 1,
                            "description": "Optional layer attribute index to place the new element on. It must be an existing, unlocked layer."
                        },
                        "floorIndex": {
                            "type": "integer",
                            "description": "Optional story of the picture, on the floor plan. Default: the current story."
                        },
                        "filePath": {
                            "type": "string",
                            "description": "The full path of the image file: a PNG, JPEG, GIF, TIFF or BMP file. Its content is stored in the project."
                        },
                        "position": {
                            "$ref": "#/Coordinate2D",
                            "description": "The bottom left corner of the picture, before rotation."
                        },
                        "width": {
                            "type": "number",
                            "exclusiveMinimum": 0.0,
                            "description": "The width of the picture in model units (meters)."
                        },
                        "height": {
                            "type": "number",
                            "exclusiveMinimum": 0.0,
                            "description": "The height of the picture in model units (meters)."
                        },
                        "angle": {
                            "type": "number",
                            "description": "Optional rotation in radians."
                        },
                        "isMirrored": {
                            "type": "boolean",
                            "description": "Optional: mirror the picture about its vertical axis."
                        },
                        "isTransparent": {
                            "type": "boolean",
                            "description": "Optional: draw the white pixels of the picture transparent."
                        },
                        "name": {
                            "type": "string",
                            "description": "Optional name of the picture. Default: the file name."
                        }
                    },
                    "additionalProperties": false,
                    "required": [
                        "filePath",
                        "position",
                        "width",
                        "height"
                    ]
                }
            }
        },
        "additionalProperties": false,
        "required": [
            "picturesData"
        ]
    })";
}

GS::Optional<GS::ObjectState> CreatePicturesCommand::SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, const Stories& /*stories*/, const GS::ObjectState& parameters) const
{
    const GS::Optional<GS::ObjectState> layerError = ApplyLayerIndexFromParameters (parameters, element);
    if (layerError.HasValue ()) {
        return layerError;
    }
    short floorIndex = 0;
    if (parameters.Get ("floorIndex", floorIndex)) {
        element.header.floorInd = floorIndex;
    }

    GS::UniString filePath;
    const GS::ObjectState* position = parameters.Get ("position");
    double width = 0.0;
    double height = 0.0;
    if (!parameters.Get ("filePath", filePath) || filePath.IsEmpty () || position == nullptr ||
        !parameters.Get ("width", width) || !parameters.Get ("height", height)) {
        return BadParameter ("'filePath', 'position', 'width' and 'height' are required.");
    }
    if (width <= 0.0 || height <= 0.0) {
        return BadParameter ("'width' and 'height' must be greater than zero.");
    }

    GS::UniString extension = filePath;
    const UIndex dot = filePath.FindLast ('.');
    extension = dot != MaxUIndex ? filePath.GetSubstring (dot + 1, filePath.GetLength () - dot - 1) : GS::UniString ();
    extension.SetToLowerCase ();
    API_PictureFormat format = APIPictForm_Default;
    if (extension == "png") {
        format = APIPictForm_PNG;
    } else if (extension == "jpg" || extension == "jpeg") {
        format = APIPictForm_JPEG;
    } else if (extension == "gif") {
        format = APIPictForm_GIF;
    } else if (extension == "tif" || extension == "tiff") {
        format = APIPictForm_TIFF;
    } else if (extension == "bmp") {
        format = APIPictForm_Bitmap;
    } else {
        return BadParameter ("The image file must be a .png, .jpg, .jpeg, .gif, .tif, .tiff or .bmp file.");
    }

    IO::Location location (filePath);
    IO::File file (location, IO::File::OnNotFound::Fail);
    if (file.GetStatus () != NoError || file.Open (IO::File::ReadMode) != NoError) {
        return CreateErrorResponse (APIERR_BADPARS, "Failed to open the image file '" + filePath + "'.");
    }
    const GS::OnExit closeFile ([&file] () { file.Close (); });
    USize length = 0;
    if (file.GetDataLength (&length) != NoError || length == 0) {
        return CreateErrorResponse (APIERR_BADPARS, "The image file '" + filePath + "' is empty or cannot be read.");
    }
    if (memo.pictHdl != nullptr) {
        BMKillHandle (&memo.pictHdl);
    }
    memo.pictHdl = BMAllocateHandle (length, ALLOCATE_CLEAR, 0);
    if (memo.pictHdl == nullptr) {
        return CreateErrorResponse (APIERR_MEMFULL, "Not enough memory for the image file.");
    }
    USize read = 0;
    if (file.ReadBin (*memo.pictHdl, length, &read) != NoError || read != length) {
        return CreateErrorResponse (APIERR_BADPARS, "Failed to read the image file '" + filePath + "'.");
    }

    API_PictureType& picture = element.picture;
    const API_Coord origin = Get2DCoordinateFromObjectState (*position);
    picture.usePixelSize = false;
    picture.destBox.xMin = origin.x;
    picture.destBox.yMin = origin.y;
    picture.destBox.xMax = origin.x + width;
    picture.destBox.yMax = origin.y + height;
    picture.anchorPoint = APIAnc_LB;
    picture.storageFormat = format;
    picture.rotAngle = 0.0;
    parameters.Get ("angle", picture.rotAngle);
    parameters.Get ("isMirrored", picture.mirrored);
    parameters.Get ("isTransparent", picture.transparent);
    GS::UniString name;
    if (!parameters.Get ("name", name)) {
        IO::Name lastLocalName;
        if (location.GetLastLocalName (&lastLocalName) == NoError) {
            name = lastLocalName.ToString ();
        }
    }
    SetUniString (picture.pictName, API_UniLongNameLen, name);
    return {};
}

// ---------------------------------------------------------------------------------------------
// Elements with a marker: change markers, detail and worksheet markers
// ---------------------------------------------------------------------------------------------

namespace {

bool HasMarkerParameters (const API_SubElement& marker)
{
    return marker.memo.params != nullptr && BMGetHandleSize (reinterpret_cast<GSHandle> (marker.memo.params)) >= static_cast<GSSize> (sizeof (API_AddParType));
}

// Reads the tool defaults of a marker-bearing element with its Main Marker. Without marker
// parameters in the tool defaults (a tool never opened in the project) the marker parent's
// library part is used, as the DevKit's Do_CreateDetail does: CreateExt refuses a marker without
// parameters.
GSErrCode GetMarkerElementDefaults (API_ElemTypeID typeID, API_ElemVariationID variationID, API_Element& element, API_ElementMemo& memo, API_SubElement& marker)
{
    element = {};
    marker = {};
    SetElemType (element, typeID, variationID);
    marker.subType = APISubElement_MainMarker;
    GSErrCode err = ACAPI_Element_GetDefaultsExt (&element, &memo, 1UL, &marker);
    if (err != NoError) {
        return err;
    }
    const GSSize markerParamNum = marker.memo.params != nullptr
        ? BMGetHandleSize (reinterpret_cast<GSHandle> (marker.memo.params)) / sizeof (API_AddParType)
        : 0;
    if (markerParamNum > 0) {
        return NoError;
    }

    API_LibPart libPart = {};
#ifdef ServerMainVers_2700
    err = ACAPI_LibraryPart_GetMarkerParent (element.header.type, libPart);
#elif defined (ServerMainVers_2600)
    err = ACAPI_Goodies_GetMarkerParent (element.header.type, libPart);
#else
    err = ACAPI_Goodies (APIAny_GetMarkerParentID, (void*) &element.header.typeID, (void*) &libPart);
#endif
    if (err != NoError) {
        return NoError;
    }
    err = ACAPI_LibraryPart_Search (&libPart, false, true);
    delete libPart.location;
    if (err != NoError) {
        return NoError;
    }
    double a = 0.0;
    double b = 0.0;
    Int32 addParNum = 0;
    API_AddParType** markAddPars = nullptr;
    if (ACAPI_LibraryPart_GetParams (libPart.index, &a, &b, &addParNum, &markAddPars) != NoError) {
        return NoError;
    }
    if (marker.memo.params != nullptr) {
        ACAPI_DisposeAddParHdl (&marker.memo.params);
    }
    marker.memo.params = markAddPars;
    marker.subElem.object.libInd = libPart.index;
    marker.subElem.object.useObjPens = true;
    return NoError;
}

// Applies a favorite, with its marker, to the tool defaults (ACAPI_Element_ChangeDefaultsExt, as
// for windows and doors). APIERR_REFUSEDPAR: a favorite of another element type.
GSErrCode ApplyMarkerFavoriteToDefaults (const GS::UniString& favoriteName, API_ElemTypeID expectedTypeId, API_ElemVariationID expectedVariationId)
{
    API_Favorite favorite;
    favorite.name = favoriteName;
    favorite.memo.New ();
    favorite.elementMarker.New ();
    favorite.memoMarker.New ();
    favorite.properties.New ();
    favorite.classifications.New ();
    favorite.elemCategoryValues.New ();

    GSErrCode err = ACAPI_Favorite_Get (&favorite);
    const GS::OnExit disposeFavoriteMemos ([&] () {
        ACAPI_DisposeElemMemoHdls (&favorite.memo.Get ());
        if (favorite.memoMarker.HasValue ()) {
            ACAPI_DisposeElemMemoHdls (&favorite.memoMarker.Get ());
        }
    });
    if (err != NoError) {
        return err;
    }
    if (GetElemTypeId (favorite.element.header) != expectedTypeId || GetVariationId (favorite.element.header) != expectedVariationId) {
        return APIERR_REFUSEDPAR;
    }

    API_Element mask;
    ACAPI_ELEMENT_MASK_SETFULL (mask);
    API_SubElement markerSubElement = {};
    UInt32 nSubElems = 0;
    API_SubElement* subElems = nullptr;
    if (favorite.elementMarker.HasValue () && favorite.memoMarker.HasValue ()) {
        markerSubElement.subType = APISubElement_MainMarker;
        markerSubElement.subElem = favorite.elementMarker.Get ();
        markerSubElement.memo = favorite.memoMarker.Get ();
        ACAPI_ELEMENT_MASK_SETFULL (markerSubElement.mask);
        nSubElems = 1;
        subElems = &markerSubElement;
    }
    err = ACAPI_Element_ChangeDefaultsExt (&favorite.element, favorite.memo.GetPtr (), &mask, nSubElems, subElems);
    if (err != NoError) {
        return err;
    }

    for (const GS::Pair<API_Guid, API_Guid>& pair : *favorite.classifications) {
        TAPIR_Element_AddClassificationItemDefault (favorite.element.header, pair.second);
    }
    for (const API_ElemCategoryValue& categoryValue : *favorite.elemCategoryValues) {
        TAPIR_Element_SetCategoryValueDefault (favorite.element.header, categoryValue);
    }
    TAPIR_Element_SetPropertiesOfDefaultElem (favorite.element.header, *favorite.properties);
    return NoError;
}

// The tool defaults of a marker-bearing element, kept to put them back after favorites were applied.
class MarkerDefaultsSnapshot {
public:
    MarkerDefaultsSnapshot (API_ElemTypeID typeID, API_ElemVariationID variationID)
    {
        GetMarkerElementDefaults (typeID, variationID, element, memo, marker);
    }
    ~MarkerDefaultsSnapshot ()
    {
        ACAPI_DisposeElemMemoHdls (&memo);
        ACAPI_DisposeElemMemoHdls (&marker.memo);
    }
    void Restore ()
    {
        API_Element mask;
        ACAPI_ELEMENT_MASK_SETFULL (mask);
        API_SubElement restoreMarker = marker;
        ACAPI_ELEMENT_MASK_SETFULL (restoreMarker.mask);
        ACAPI_Element_ChangeDefaultsExt (&element, &memo, &mask, HasMarkerParameters (marker) ? 1UL : 0UL, &restoreMarker);
    }
private:
    API_Element     element = {};
    API_ElementMemo memo = {};
    API_SubElement  marker = {};
};

GSErrCode SetPolygonMemo (const GS::Array<GS::ObjectState>& coordinates, const GS::Array<GS::ObjectState>& arcs, API_Polygon& poly, API_ElementMemo& memo)
{
    const GS::Array<API_PolyArc> polyArcs = GetPolyArcs (arcs, 1);
    USize n = coordinates.GetSize ();
    if (n >= 2 && IsSame2DCoordinate (coordinates[0], coordinates[n - 1])) {
        --n;
    }
    poly.nCoords = static_cast<Int32> (n) + 1;
    poly.nSubPolys = 1;
    poly.nArcs = static_cast<Int32> (polyArcs.GetSize ());
    if (memo.coords != nullptr) {
        BMKillHandle (reinterpret_cast<GSHandle*> (&memo.coords));
    }
    if (memo.pends != nullptr) {
        BMKillHandle (reinterpret_cast<GSHandle*> (&memo.pends));
    }
    if (memo.parcs != nullptr) {
        BMKillHandle (reinterpret_cast<GSHandle*> (&memo.parcs));
    }
    if (memo.vertexIDs != nullptr) {
        BMKillHandle (reinterpret_cast<GSHandle*> (&memo.vertexIDs));
    }
    memo.coords = reinterpret_cast<API_Coord**> (BMAllocateHandle ((poly.nCoords + 1) * sizeof (API_Coord), ALLOCATE_CLEAR, 0));
    memo.pends = reinterpret_cast<Int32**> (BMAllocateHandle ((poly.nSubPolys + 1) * sizeof (Int32), ALLOCATE_CLEAR, 0));
    if (poly.nArcs > 0) {
        memo.parcs = reinterpret_cast<API_PolyArc**> (BMAllocateHandle (poly.nArcs * sizeof (API_PolyArc), ALLOCATE_CLEAR, 0));
    }
    if (memo.coords == nullptr || memo.pends == nullptr || (poly.nArcs > 0 && memo.parcs == nullptr)) {
        return APIERR_MEMFULL;
    }
    for (UIndex i = 0; i < n; ++i) {
        (*memo.coords)[i + 1] = Get2DCoordinateFromObjectState (coordinates[i]);
    }
    (*memo.coords)[n + 1] = (*memo.coords)[1];
    (*memo.pends)[0] = 0;
    (*memo.pends)[1] = poly.nCoords;
    for (UIndex i = 0; i < polyArcs.GetSize (); ++i) {
        (*memo.parcs)[i] = polyArcs[i];
    }
    return NoError;
}

// The optional polygon of a change marker or detail / worksheet marker: an error message for a
// polygon that is given but not usable, or nothing.
GS::Optional<GS::UniString> ApplyMarkerPolygon (const GS::ObjectState& parameters, API_Polygon& poly, API_ElementMemo& memo, bool& hasPolygon)
{
    GS::Array<GS::ObjectState> coordinates;
    hasPolygon = parameters.Get ("polygonCoordinates", coordinates);
    if (!hasPolygon) {
        poly = {};
        return {};
    }
    USize n = coordinates.GetSize ();
    if (n >= 2 && IsSame2DCoordinate (coordinates[0], coordinates[n - 1])) {
        --n;
    }
    if (n < 3) {
        return GS::UniString ("'polygonCoordinates' needs at least 3 different coordinates.");
    }
    GS::Array<GS::ObjectState> arcs;
    parameters.Get ("polygonArcs", arcs);
    if (SetPolygonMemo (coordinates, arcs, poly, memo) != NoError) {
        return GS::UniString ("Not enough memory for the polygon.");
    }
    return {};
}

void ApplyMarkerPlacement (const GS::ObjectState& parameters, const API_Coord& defaultPosition, API_SubElement& marker, double& angle, bool& horizontalMarker)
{
    const GS::ObjectState* markerPosition = parameters.Get ("markerPosition");
    marker.subElem.object.pos = markerPosition != nullptr ? Get2DCoordinateFromObjectState (*markerPosition) : defaultPosition;
    parameters.Get ("angle", angle);
    parameters.Get ("isHorizontalMarker", horizontalMarker);
}

const char* MarkerCommonSchemaProperties = R"(
    "layerIndex": {
        "type": "integer",
        "minimum": 1,
        "description": "Optional layer attribute index to place the new element on. It must be an existing, unlocked layer."
    },
    "floorIndex": {
        "type": "integer",
        "description": "Optional story to place the marker on, on the floor plan. Default: the current story."
    },
    "markerPosition": {
        "$ref": "#/Coordinate2D",
        "description": "Optional position of the marker symbol. Default: position."
    },
    "angle": {
        "type": "number",
        "description": "Optional rotation of the marker symbol in radians."
    },
    "isHorizontalMarker": {
        "type": "boolean",
        "description": "Optional: the marker symbol is always horizontal."
    },
    "polygonArcs": {
        "type": "array",
        "items": { "$ref": "#/PolyArc" },
        "description": "Optional arcs of the polygon (0-based indices into polygonCoordinates)."
    }
)";

}

CreateMarkerElementsCommandBase::CreateMarkerElementsCommandBase (const GS::String& commandNameIn, API_ElemTypeID elemTypeIDIn, const GS::String& arrayFieldNameIn)
    : CommandBase (CommonSchema::Used)
    , commandName (commandNameIn)
    , elemTypeID (elemTypeIDIn)
    , arrayFieldName (arrayFieldNameIn)
    , variationID (APIVarId_Generic)
{
}

GS::String CreateMarkerElementsCommandBase::GetName () const
{
    return commandName;
}

GS::Optional<GS::UniString> CreateMarkerElementsCommandBase::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elements": {
                "$ref": "#/ElementIdsOrErrors"
            }
        },
        "additionalProperties": false,
        "required": [
            "elements"
        ]
    })";
}

GS::ObjectState CreateMarkerElementsCommandBase::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> dataArray;
    parameters.Get (arrayFieldName, dataArray);

    GS::ObjectState response;
    const auto& elements = response.AddList<GS::ObjectState> ("elements");
    const GS::UniString elemTypeName = variationID == APIVarId_WallEnd ? GS::UniString ("Wall End")
        : variationID == APIVarId_CornerWindow ? GS::UniString ("Corner Window")
        : GetElementTypeNonLocalizedName (elemTypeID);

    API_NotifyElementType notification = {};
    notification.notifID = APINotifyElement_BeginEvents;
    AddElementNotificationClientCommand::ElementEventHandlerProc (&notification);

    ACAPI_CallUndoableCommand ("Create " + elemTypeName, [&] () -> GSErrCode {
        // Favorites go through the tool defaults, which are global: the defaults are kept and put
        // back before an item without a favorite and at the end, as in CreateElementsCommandBase.
        const bool anyFavoriteName = [&] () {
            for (const GS::ObjectState& data : dataArray) {
                GS::UniString name;
                if (data.Get ("favoriteName", name) && !name.IsEmpty ()) {
                    return true;
                }
            }
            return false;
        } ();
        std::unique_ptr<MarkerDefaultsSnapshot> snapshot;
        if (anyFavoriteName) {
            snapshot.reset (new MarkerDefaultsSnapshot (elemTypeID, variationID));
        }
        bool favoriteApplied = false;

        for (const GS::ObjectState& data : dataArray) {
            if (anyFavoriteName) {
                GS::UniString favoriteName;
                if (data.Get ("favoriteName", favoriteName) && !favoriteName.IsEmpty ()) {
                    const GSErrCode favoriteErr = ApplyMarkerFavoriteToDefaults (favoriteName, elemTypeID, variationID);
                    if (favoriteErr != NoError) {
                        const GS::UniString reason = favoriteErr == APIERR_REFUSEDPAR
                            ? GS::UniString ("it is not a " + elemTypeName + " favorite")
                            : GS::UniString ("no favorite with this exact name was found");
                        elements (CreateErrorResponse (favoriteErr,
                            "Failed to apply favoriteName '" + favoriteName + "' to the " + elemTypeName + " defaults: " + reason + "."));
                        continue;
                    }
                    favoriteApplied = true;
                } else if (favoriteApplied) {
                    snapshot->Restore ();
                    favoriteApplied = false;
                }
            }

            API_Element element = {};
            API_ElementMemo memo = {};
            API_SubElement marker = {};
            const GS::OnExit guard ([&] () {
                ACAPI_DisposeElemMemoHdls (&memo);
                ACAPI_DisposeElemMemoHdls (&marker.memo);
            });
            GSErrCode err = GetMarkerElementDefaults (elemTypeID, variationID, element, memo, marker);
            if (err != NoError) {
                elements (CreateErrorResponse (err, "Failed to read the " + elemTypeName + " defaults."));
                continue;
            }

            const GS::Optional<GS::ObjectState> layerError = ApplyLayerIndexFromParameters (data, element);
            if (layerError.HasValue ()) {
                elements (*layerError);
                continue;
            }
            short floorIndex = 0;
            if (data.Get ("floorIndex", floorIndex)) {
                element.header.floorInd = floorIndex;
            }

            const GS::Optional<GS::ObjectState> os = SetTypeSpecificParameters (element, memo, marker, data);
            if (os.HasValue ()) {
                elements (*os);
                continue;
            }

            marker.subType = APISubElement_MainMarker;
            // An element type without a marker symbol (or a project without its marker library
            // part) is created without the sub-element: CreateExt refuses a marker without parameters.
            err = ACAPI_Element_CreateExt (&element, &memo, HasMarkerParameters (marker) ? 1UL : 0UL, &marker);
            if (err != NoError) {
                elements (CreateErrorResponse (err, "Failed to create new " + elemTypeName));
                continue;
            }

            GS::UniString id;
            if (data.Get ("id", id)) {
                err = ChangeElementId (element.header.guid, id);
                if (err != NoError) {
                    ACAPI_Element_Delete ({ element.header.guid });
                    elements (CreateErrorResponse (err, "Failed to set the id of the new " + elemTypeName + ", so it was not created"));
                    continue;
                }
            }

            notification = {};
            notification.notifID = APINotifyElement_New;
            notification.elemHead = element.header;
            AddElementNotificationClientCommand::ElementEventHandlerProc (&notification);

            elements (CreateElementIdObjectState (element.header.guid));
        }

        if (favoriteApplied) {
            snapshot->Restore ();
        }
        return NoError;
    });

    notification = {};
    notification.notifID = APINotifyElement_EndEvents;
    AddElementNotificationClientCommand::ElementEventHandlerProc (&notification);

    return response;
}

// ---------------------------------------------------------------------------------------------
// Change markers
// ---------------------------------------------------------------------------------------------

namespace {

bool DoesRevisionChangeExist (const GS::UniString& changeId)
{
    GS::Array<API_RVMChange> changes;
    if (ACAPI_Revision_GetRVMChanges (&changes) != NoError) {
        return false;
    }
    for (const API_RVMChange& change : changes) {
        if (change.id == changeId) {
            return true;
        }
    }
    return false;
}

}

CreateChangeMarkersCommand::CreateChangeMarkersCommand () :
    CreateMarkerElementsCommandBase ("CreateChangeMarkers", API_ChangeMarkerID, "changeMarkersData")
{
}

GS::Optional<GS::UniString> CreateChangeMarkersCommand::GetInputParametersSchema () const
{
    return GS::UniString (R"({
        "type": "object",
        "properties": {
            "changeMarkersData": {
                "type": "array",
                "description": "Array of data to create Change Markers (revision clouds).",
                "items": {
                    "type": "object",
                    "description": "The parameters of the new Change Marker.",
                    "properties": {
                        "favoriteName": {
                            "type": "string",
                            "description": "Optional name of a Change Marker favorite to base the new marker on. Its settings and marker are applied first, then the explicitly given fields override them."
                        },
                        "linkType": {
                            "type": "string",
                            "enum": ["CreateNewChange", "Linked", "Unlinked"],
                            "description": "Optional: 'CreateNewChange' creates a new Change with changeId and changeName and links the marker to it; 'Linked' links the marker to the existing Change with changeId; 'Unlinked' places a marker without a Change. Default: 'CreateNewChange' when changeName is given, 'Linked' when only changeId is given, otherwise the setting of the tool defaults."
                        },
                        "changeId": {
                            "type": "string",
                            "description": "The ID of the Change (required for 'CreateNewChange' and 'Linked')."
                        },
                        "changeName": {
                            "type": "string",
                            "description": "The name of the new Change (for 'CreateNewChange')."
                        },
                        "position": {
                            "$ref": "#/Coordinate2D",
                            "description": "The reference point the marker points to (when it has no polygon)."
                        },
                        "polygonCoordinates": {
                            "type": "array",
                            "items": { "$ref": "#/Coordinate2D" },
                            "minItems": 3,
                            "description": "Optional polygon of the cloud."
                        },
                        "markerComponent": {
                            "type": "string",
                            "enum": ["HeadOnly", "CloudOnly", "HeadAndCloud"],
                            "description": "Optional: which parts of the Change Marker are shown. Default: HeadAndCloud with polygonCoordinates, HeadOnly without."
                        },)") + MarkerCommonSchemaProperties + R"(},
                    "additionalProperties": false,
                    "required": [
                        "position"
                    ]
                }
            }
        },
        "additionalProperties": false,
        "required": [
            "changeMarkersData"
        ]
    })";
}

GS::Optional<GS::ObjectState> CreateChangeMarkersCommand::SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, API_SubElement& marker, const GS::ObjectState& parameters) const
{
    API_ChangeMarkerType& changeMarker = element.changeMarker;
    const GS::ObjectState* position = parameters.Get ("position");
    if (position == nullptr) {
        return BadParameter ("'position' is required.");
    }
    changeMarker.pos = Get2DCoordinateFromObjectState (*position);

    GS::UniString changeId;
    GS::UniString changeName;
    const bool hasChangeId = parameters.Get ("changeId", changeId) && !changeId.IsEmpty ();
    const bool hasChangeName = parameters.Get ("changeName", changeName) && !changeName.IsEmpty ();
    GS::UniString linkType;
    if (!parameters.Get ("linkType", linkType)) {
        linkType = hasChangeName ? "CreateNewChange" : (hasChangeId ? "Linked" : "");
    }
    if (linkType == "CreateNewChange") {
        if (!hasChangeId || !hasChangeName) {
            return BadParameter ("'CreateNewChange' needs 'changeId' and 'changeName'.");
        }
        changeMarker.linkType = APICMLT_CreateNewChange;
    } else if (linkType == "Linked") {
        if (!hasChangeId || !DoesRevisionChangeExist (changeId)) {
            // Archicad accepts a marker linked to a Change that does not exist (tested in
            // Archicad 30), which leaves a marker linked to nothing.
            return BadParameter ("'Linked' needs the 'changeId' of an existing Change (see GetRevisionChanges).");
        }
        changeMarker.linkType = APICMLT_Linked;
    } else if (linkType == "Unlinked") {
        changeMarker.linkType = APICMLT_Unlinked;
    } else if (!linkType.IsEmpty ()) {
        return BadParameter ("'linkType' must be 'CreateNewChange', 'Linked' or 'Unlinked'.");
    }
    if (hasChangeId) {
        SetUniString (changeMarker.changeId, API_UniLongNameLen, changeId);
    }
    if (hasChangeName) {
        SetUniString (changeMarker.changeName, API_UniLongNameLen, changeName);
    }

    bool hasPolygon = false;
    const GS::Optional<GS::UniString> polygonError = ApplyMarkerPolygon (parameters, changeMarker.poly, memo, hasPolygon);
    if (polygonError.HasValue ()) {
        return BadParameterFrom (polygonError);
    }
    GS::UniString markerComponent;
    if (parameters.Get ("markerComponent", markerComponent)) {
        if (markerComponent == "HeadOnly") {
            changeMarker.markerComponent = APICMCT_HeadOnly;
        } else if (markerComponent == "CloudOnly") {
            changeMarker.markerComponent = APICMCT_CloudOnly;
        } else if (markerComponent == "HeadAndCloud") {
            changeMarker.markerComponent = APICMCT_HeadAndCloud;
        } else {
            return BadParameter ("'markerComponent' must be 'HeadOnly', 'CloudOnly' or 'HeadAndCloud'.");
        }
    } else {
        changeMarker.markerComponent = hasPolygon ? APICMCT_HeadAndCloud : APICMCT_HeadOnly;
    }
    if (changeMarker.markerComponent != APICMCT_HeadOnly && !hasPolygon) {
        return BadParameter ("A Change Marker with a cloud needs 'polygonCoordinates'.");
    }

    ApplyMarkerPlacement (parameters, changeMarker.pos, marker, changeMarker.angle, changeMarker.horizontalMarker);
    return {};
}

// ---------------------------------------------------------------------------------------------
// Detail and worksheet markers
// ---------------------------------------------------------------------------------------------

namespace {

GS::UniString DetailMarkerSchema (const char* arrayFieldName, const char* typeName, const char* databaseTypeName)
{
    return GS::UniString::Printf (R"({
        "type": "object",
        "properties": {
            "%s": {
                "type": "array",
                "description": "Array of data to create %s markers.",
                "items": {
                    "type": "object",
                    "description": "The parameters of the new %s marker. It is a source marker: Archicad creates a new %s database for it, with the name and reference ID given here. Deleting the marker deletes the database.",
                    "properties": {
                        "favoriteName": {
                            "type": "string",
                            "description": "Optional name of a %s favorite to base the new marker on. Its settings and marker are applied first, then the explicitly given fields override them."
                        },
                        "name": {
                            "type": "string",
                            "description": "Optional name of the %s (of the new database of a source marker)."
                        },
                        "referenceId": {
                            "type": "string",
                            "description": "Optional reference ID of the %s. A '/' splits it between the beginning and end marks."
                        },
                        "position": {
                            "$ref": "#/Coordinate2D",
                            "description": "The reference point of the marker, where the origin of a new %s database is. For a source marker with polygonCoordinates Archicad puts it at the center of the polygon's bounding box instead."
                        },
                        "polygonCoordinates": {
                            "type": "array",
                            "items": { "$ref": "#/Coordinate2D" },
                            "minItems": 3,
                            "description": "Optional boundary polygon of the marker (the region the detail or worksheet shows)."
                        },
)", arrayFieldName, typeName, typeName, databaseTypeName, typeName, databaseTypeName, databaseTypeName, databaseTypeName)
        + MarkerCommonSchemaProperties + GS::UniString::Printf (R"(},
                    "additionalProperties": false,
                    "required": [
                        "position"
                    ]
                }
            }
        },
        "additionalProperties": false,
        "required": [
            "%s"
        ]
    })", arrayFieldName);
}

GS::Optional<GS::ObjectState> SetDetailMarkerParameters (API_DetailType& detail, API_ElementMemo& memo, API_SubElement& marker, const GS::ObjectState& parameters)
{
    const GS::ObjectState* position = parameters.Get ("position");
    if (position == nullptr) {
        return BadParameter ("'position' is required.");
    }
    detail.pos = Get2DCoordinateFromObjectState (*position);

    GS::UniString name;
    if (parameters.Get ("name", name)) {
        SetUniString (detail.detailName, API_UniLongNameLen, name);
    }
    GS::UniString referenceId;
    if (parameters.Get ("referenceId", referenceId)) {
        SetUniString (detail.detailIdStr, API_UniLongNameLen, referenceId);
    }

    bool hasPolygon = false;
    const GS::Optional<GS::UniString> polygonError = ApplyMarkerPolygon (parameters, detail.poly, memo, hasPolygon);
    if (polygonError.HasValue ()) {
        return BadParameterFrom (polygonError);
    }

    // A source marker: Archicad creates its database. The tool defaults hold the database of the
    // last detail or worksheet made in the project (CreateDetails and CreateWorksheets leave it
    // there, tested in Archicad 30); it is cleared, so that Archicad makes a new one.
    //
    // Linked and unlinked markers are not offered. Tested in Archicad 30, ACAPI_Element_CreateExt
    // makes a database for every detail marker: with linkData referring to a Project Map view point
    // (ReferredToViewPoint) it makes a second database with the id of the referred one and Archicad
    // ends ("IdSpace already contains an object with the same Id", class VBES::Detail); referring
    // to a View Map view (ReferredToView) it makes a copy of the viewed database, and an unlinked
    // one (customMarker) makes an empty database. None of them comes back with the link it was
    // given.
    detail.databaseID = {};
    detail.linkData = {};
    detail.linkData.sourceMarker = true;

    ApplyMarkerPlacement (parameters, detail.pos, marker, detail.angle, detail.horizontalMarker);
    return {};
}

}

CreateDetailMarkersCommand::CreateDetailMarkersCommand () :
    CreateMarkerElementsCommandBase ("CreateDetailMarkers", API_DetailID, "detailMarkersData")
{
}

GS::Optional<GS::UniString> CreateDetailMarkersCommand::GetInputParametersSchema () const
{
    return DetailMarkerSchema ("detailMarkersData", "Detail", "Detail");
}

GS::Optional<GS::ObjectState> CreateDetailMarkersCommand::SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, API_SubElement& marker, const GS::ObjectState& parameters) const
{
    return SetDetailMarkerParameters (element.detail, memo, marker, parameters);
}

CreateWorksheetMarkersCommand::CreateWorksheetMarkersCommand () :
    CreateMarkerElementsCommandBase ("CreateWorksheetMarkers", API_WorksheetID, "worksheetMarkersData")
{
}

GS::Optional<GS::UniString> CreateWorksheetMarkersCommand::GetInputParametersSchema () const
{
    return DetailMarkerSchema ("worksheetMarkersData", "Worksheet", "Worksheet");
}

GS::Optional<GS::ObjectState> CreateWorksheetMarkersCommand::SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, API_SubElement& marker, const GS::ObjectState& parameters) const
{
    return SetDetailMarkerParameters (element.worksheet, memo, marker, parameters);
}

// ---------------------------------------------------------------------------------------------
// Wall ends and corner windows
// ---------------------------------------------------------------------------------------------

namespace {

GS::UniString WallEndOrCornerWindowSchema (const char* arrayFieldName, const char* typeName, const char* placementDescription)
{
    return GS::UniString::Printf (R"({
        "type": "object",
        "properties": {
            "%s": {
                "type": "array",
                "description": "Array of data to create %ss.",
                "items": {
                    "type": "object",
                    "description": "The parameters of the new %s. %s",
                    "properties": {
                        "favoriteName": {
                            "type": "string",
                            "description": "Optional name of a %s favorite to base the new element on. Its settings are applied first, then the explicitly given fields override them."
                        },
                        "id": {
                            "type": "string",
                            "description": "Optional Element ID of the new element."
                        },
                        "layerIndex": {
                            "type": "integer",
                            "minimum": 1,
                            "description": "Optional layer attribute index to place the new element on. It must be an existing, unlocked layer."
                        },
                        "ownerWallId": {
                            "$ref": "#/ElementId",
                            "description": "The wall the element is placed in."
                        },
                        "atWallEnd": {
                            "type": "string",
                            "enum": ["Begin", "End"],
                            "description": "Optional: place the element at the begin or end of the wall's reference line. Not together with centerOffset."
                        },
                        "centerOffset": {
                            "type": "number",
                            "description": "Optional distance of the element's anchor from the begin of the wall's reference line, as CreateWindows takes it. Not together with atWallEnd."
                        },
                        "sillHeight": {
                            "type": "number",
                            "description": "Optional sill height (the lower edge from the wall's base)."
                        },
                        "width": {
                            "type": "number",
                            "exclusiveMinimum": 0.0,
                            "description": "Optional width."
                        },
                        "height": {
                            "type": "number",
                            "exclusiveMinimum": 0.0,
                            "description": "Optional height."
                        },
                        "reflected": {
                            "type": "boolean",
                            "description": "Optional: mirror the element along the wall."
                        },
                        "oSide": {
                            "type": "boolean",
                            "description": "Optional: open the element to the other side of the wall."
                        }
                    },
                    "additionalProperties": false,
                    "required": [
                        "ownerWallId"
                    ]
                }
            }
        },
        "additionalProperties": false,
        "required": [
            "%s"
        ]
    })", arrayFieldName, typeName, typeName, placementDescription, typeName, arrayFieldName);
}

}

CreateWallEndsOrCornerWindowsCommandBase::CreateWallEndsOrCornerWindowsCommandBase (const GS::String& commandNameIn, API_ElemVariationID variationIDIn, const GS::String& arrayFieldNameIn) :
    CreateMarkerElementsCommandBase (commandNameIn, API_WindowID, arrayFieldNameIn)
{
    variationID = variationIDIn;
}

GS::ObjectState CreateWallEndsOrCornerWindowsCommandBase::Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const
{
#ifdef ServerMainVers_2600
    // Like windows, they are created with the floor plan as the current database (#532).
    API_DatabaseInfo previousDatabase = {};
    bool databaseSwitched = false;
    const GSErrCode databaseErr = SwitchCurrentDatabaseToFloorPlan (previousDatabase, databaseSwitched);
    const GS::OnExit restoreDatabase ([&] () {
        if (databaseSwitched) {
            ACAPI_Database_ChangeCurrentDatabase (&previousDatabase);
        }
    });
    if (databaseErr != NoError) {
        return CreateErrorResponse (databaseErr, "Failed to make the floor plan the current database.");
    }
    return CreateMarkerElementsCommandBase::Execute (parameters, processControl);
#else
    UNUSED_PARAMETER (parameters);
    UNUSED_PARAMETER (processControl);
    return CreateErrorResponse (APIERR_NOTSUPPORTED, commandName + " needs Archicad 26 or later.");
#endif
}

GS::Optional<GS::ObjectState> CreateWallEndsOrCornerWindowsCommandBase::SetTypeSpecificParameters (API_Element& element, API_ElementMemo& /*memo*/, API_SubElement& /*marker*/, const GS::ObjectState& parameters) const
{
    const GS::ObjectState* ownerWallId = parameters.Get ("ownerWallId");
    if (ownerWallId == nullptr) {
        return BadParameter ("'ownerWallId' is required.");
    }
    API_Element wall = {};
    wall.header.guid = GetGuidFromObjectState (*ownerWallId);
    if (wall.header.guid == APINULLGuid || ACAPI_Element_Get (&wall) != NoError || GetElemTypeId (wall.header) != API_WallID) {
        return CreateErrorResponse (APIERR_BADID, "'ownerWallId' does not refer to an existing wall.");
    }

    GS::UniString atWallEnd;
    const bool hasAtWallEnd = parameters.Get ("atWallEnd", atWallEnd);
    double centerOffset = 0.0;
    const bool hasCenterOffset = parameters.Get ("centerOffset", centerOffset);
    if (hasAtWallEnd && hasCenterOffset) {
        return BadParameter ("Give either 'atWallEnd' or 'centerOffset', not both.");
    }
    if (hasAtWallEnd) {
        if (atWallEnd == "Begin") {
            centerOffset = 0.0;
        } else if (atWallEnd == "End") {
            const double dx = wall.wall.endC.x - wall.wall.begC.x;
            const double dy = wall.wall.endC.y - wall.wall.begC.y;
            centerOffset = std::sqrt (dx * dx + dy * dy);
        } else {
            return BadParameter ("'atWallEnd' must be 'Begin' or 'End'.");
        }
    }

    element.header.floorInd = wall.header.floorInd;
    element.window.owner = wall.header.guid;
    element.window.objLoc = centerOffset;
    parameters.Get ("sillHeight", element.window.lower);
    double width = 0.0;
    if (parameters.Get ("width", width)) {
        element.window.openingBase.width = width;
    }
    double height = 0.0;
    if (parameters.Get ("height", height)) {
        element.window.openingBase.height = height;
    }
    parameters.Get ("reflected", element.window.openingBase.reflected);
    parameters.Get ("oSide", element.window.openingBase.oSide);
    return {};
}

CreateWallEndsCommand::CreateWallEndsCommand () :
    CreateWallEndsOrCornerWindowsCommandBase ("CreateWallEnds", APIVarId_WallEnd, "wallEndsData")
{
}

GS::Optional<GS::UniString> CreateWallEndsCommand::GetInputParametersSchema () const
{
    return WallEndOrCornerWindowSchema ("wallEndsData", "Wall End",
        "A Wall End is a library part placed at the end of a wall (the Wall End tool), from the Wall End tool defaults or a favorite.");
}

CreateCornerWindowsCommand::CreateCornerWindowsCommand () :
    CreateWallEndsOrCornerWindowsCommandBase ("CreateCornerWindows", APIVarId_CornerWindow, "cornerWindowsData")
{
}

GS::Optional<GS::UniString> CreateCornerWindowsCommand::GetInputParametersSchema () const
{
    return WallEndOrCornerWindowSchema ("cornerWindowsData", "Corner Window",
        "A Corner Window is a window placed at the corner of two connected walls (the Corner Window tool), from the Corner Window tool defaults or a favorite.");
}
